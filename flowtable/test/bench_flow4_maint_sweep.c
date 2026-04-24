/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_flow4_maint_sweep.c - maintain sweep matrix for classic vs
 * slot_extra flow4.  Iterates over (N_ENTRIES, fill%, expire%) per
 * spec section 9.3 and reports per-bucket cycle cost of one
 * ft_table_maintain pass for each variant.
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <rix/rix_defs_private.h>
#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>

#include "flow4_table.h"
#include "flow4_extra_table.h"
#include "flow_key.h"

#define TS_SHIFT 4u

static RIX_FORCE_INLINE u64
tsc_start(void)
{
    u32 lo, hi;

    __asm__ volatile ("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static RIX_FORCE_INLINE u64
tsc_end(void)
{
    u32 lo, hi;

    __asm__ volatile ("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((u64)hi << 32) | lo;
}

static void *
hugealloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED)
        return NULL;
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

static void
hugefree(void *p, size_t bytes)
{
    if (p != NULL)
        munmap(p, bytes);
}

static void
fill_classic_keys(struct flow4_entry *pool, unsigned n)
{
    unsigned i;

    for (i = 0u; i < n; i++) {
        memset(&pool[i].key, 0, sizeof(pool[i].key));
        pool[i].key.family   = 4u;
        pool[i].key.proto    = 6u;
        pool[i].key.src_ip   = 0x0A000000u | i;
        pool[i].key.dst_ip   = 0x0B000000u | (i + 1u);
        pool[i].key.src_port = (u16)(i * 7u);
        pool[i].key.dst_port = (u16)(i * 11u);
    }
}

static void
fill_extra_keys(struct flow4_extra_entry *pool, unsigned n)
{
    unsigned i;

    for (i = 0u; i < n; i++) {
        memset(&pool[i].key, 0, sizeof(pool[i].key));
        pool[i].key.family   = 4u;
        pool[i].key.proto    = 6u;
        pool[i].key.src_addr = 0x0A000000u | i;
        pool[i].key.dst_addr = 0x0B000000u | (i + 1u);
        pool[i].key.src_port = (u16)(i * 7u);
        pool[i].key.dst_port = (u16)(i * 11u);
    }
}

static void
age_classic(struct flow4_entry *pool, unsigned n_total,
            unsigned stale_n, u64 now, u64 old)
{
    u32 enc_old = (u32)flow_timestamp_encode(old, TS_SHIFT);
    u32 enc_now = (u32)flow_timestamp_encode(now, TS_SHIFT);
    unsigned i;

    for (i = 0u; i < stale_n; i++) {
        if (pool[i].meta.timestamp != 0u)
            pool[i].meta.timestamp = enc_old;
    }
    for (; i < n_total; i++) {
        if (pool[i].meta.timestamp != 0u)
            pool[i].meta.timestamp = enc_now;
    }
}

static void
age_extra(struct ft_table_extra *ft,
          struct flow4_extra_entry *pool, unsigned n_total,
          unsigned stale_n, u64 now, u64 old)
{
    u32 enc_old = flow_extra_timestamp_encode(old, TS_SHIFT);
    u32 enc_now = flow_extra_timestamp_encode(now, TS_SHIFT);
    unsigned bk_mask = ft_table_extra_nb_bk(ft) - 1u;
    unsigned i;

    for (i = 0u; i < stale_n; i++) {
        unsigned bk_idx = pool[i].meta.cur_hash & bk_mask;
        unsigned slot   = (unsigned)pool[i].meta.slot;

        if (ft->buckets[bk_idx].extra[slot] != 0u)
            flow_extra_ts_set(&ft->buckets[bk_idx], slot, enc_old);
    }
    for (; i < n_total; i++) {
        unsigned bk_idx = pool[i].meta.cur_hash & bk_mask;
        unsigned slot   = (unsigned)pool[i].meta.slot;

        if (ft->buckets[bk_idx].extra[slot] != 0u)
            flow_extra_ts_set(&ft->buckets[bk_idx], slot, enc_now);
    }
}

/* Round up to next power of two.  u32 domain is plenty for our sweep. */
static unsigned
roundup_pow2(unsigned x)
{
    unsigned v = 1u;

    if (x <= 1u)
        return 1u;
    while (v < x)
        v <<= 1;
    return v;
}

/* Compute bucket count targeting approximately `fill_pct` load once
 * `n_insert` entries are live.  Clamps to FT minimums inside
 * ft_table_bucket_size() via ceil + pow2 rounding here. */
static unsigned
pick_nb_bk(unsigned n_insert, unsigned fill_pct)
{
    /* desired: n_insert / (nb_bk * 16) = fill/100
     *       => nb_bk = n_insert * 100 / (16 * fill)
     * Round up before pow2 rounding to keep fill% at or below target.
     */
    unsigned num = n_insert * 100u;
    unsigned den = 16u * fill_pct;
    unsigned nb  = (num + den - 1u) / den;

    if (nb < 4096u)              /* FT_TABLE_MIN_NB_BK */
        nb = 4096u;
    return roundup_pow2(nb);
}

static int
run_one(unsigned n_entries, unsigned fill_pct, unsigned expire_pct)
{
    size_t pool_c_mem;
    size_t pool_e_mem;
    size_t bk_c_mem;
    size_t bk_e_mem;
    struct flow4_entry       *pool_c = NULL;
    struct flow4_extra_entry *pool_e = NULL;
    void                     *bk_c   = NULL;
    void                     *bk_e   = NULL;
    u32                      *expired = NULL;
    struct ft_table        ft_c;
    struct ft_table_extra  ft_e;
    struct ft_table_config       cfg_c = { .ts_shift = TS_SHIFT };
    struct ft_table_extra_config cfg_e = { .ts_shift = TS_SHIFT };
    unsigned insert_n = (unsigned)((u64)n_entries * fill_pct / 100u);
    unsigned stale_n  = (unsigned)((u64)insert_n * expire_pct / 100u);
    unsigned nb_bk    = pick_nb_bk(insert_n, fill_pct);
    const u64 now = 1000000u;
    const u64 old = 0u;
    const u64 tmo = 1u << 18;
    u64 t0;
    u64 t1;
    u64 c_cy = 0;
    u64 e_cy = 0;
    unsigned next;
    int rc;
    unsigned i;

    if (insert_n == 0u)
        return 0;

    pool_c_mem = (size_t)n_entries * sizeof(struct flow4_entry);
    pool_e_mem = (size_t)n_entries * sizeof(struct flow4_extra_entry);
    bk_c_mem   = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    bk_e_mem   = (size_t)nb_bk * sizeof(struct rix_hash_bucket_extra_s);

    pool_c  = hugealloc(pool_c_mem);
    pool_e  = hugealloc(pool_e_mem);
    bk_c    = hugealloc(bk_c_mem);
    bk_e    = hugealloc(bk_e_mem);
    expired = calloc(insert_n, sizeof(*expired));
    if (pool_c == NULL || pool_e == NULL || bk_c == NULL ||
        bk_e == NULL || expired == NULL) {
        fprintf(stderr, "  skip N=%u fill=%u expire=%u (alloc failed)\n",
                n_entries, fill_pct, expire_pct);
        goto out;
    }

    fill_classic_keys(pool_c, n_entries);
    fill_extra_keys(pool_e, n_entries);

    rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, n_entries,
                                   struct flow4_entry, key,
                                   bk_c, bk_c_mem, &cfg_c);
    if (rc != 0) {
        fprintf(stderr, "  skip N=%u fill=%u expire=%u (classic init rc=%d)\n",
                n_entries, fill_pct, expire_pct, rc);
        goto out;
    }
    rc = flow4_extra_table_init(&ft_e, pool_e, n_entries,
                                bk_e, bk_e_mem, &cfg_e);
    if (rc != 0) {
        fprintf(stderr, "  skip N=%u fill=%u expire=%u (extra init rc=%d)\n",
                n_entries, fill_pct, expire_pct, rc);
        ft_flow4_table_destroy(&ft_c);
        goto out;
    }

    for (i = 0u; i < insert_n; i++) {
        (void)ft_flow4_table_add_idx(&ft_c, i + 1u, now);
        (void)flow4_extra_table_add(&ft_e, &pool_e[i], now);
    }

    age_classic(pool_c, insert_n, stale_n, now, old);
    age_extra(&ft_e, pool_e, insert_n, stale_n, now, old);

    {
        struct ft_maint_ctx mc = {
            .buckets     = ft_c.buckets,
            .rhh_nb      = &ft_c.ht_head.rhh_nb,
            .pool_base   = (const unsigned char *)pool_c,
            .stats       = &ft_c.stats,
            .pool_stride = sizeof(*pool_c),
            .meta_off    = offsetof(struct flow4_entry, meta),
            .max_entries = n_entries,
            .rhh_mask    = ft_c.ht_head.rhh_mask,
            .ts_shift    = TS_SHIFT,
        };
        struct ft_maint_extra_ctx me = {
            .buckets     = ft_e.buckets,
            .rhh_nb      = &ft_e.ht_head.rhh_nb,
            .stats       = &ft_e.stats,
            .pool_base   = (unsigned char *)pool_e,
            .pool_stride = sizeof(*pool_e),
            .meta_off    = offsetof(struct flow4_extra_entry, meta),
            .max_entries = n_entries,
            .rhh_mask    = ft_e.ht_head.rhh_mask,
            .ts_shift    = TS_SHIFT,
        };

        next = 0u;
        t0 = tsc_start();
        (void)ft_table_maintain(&mc, 0u, now, tmo,
                                expired, insert_n, 0u, &next);
        t1 = tsc_end();
        c_cy = t1 - t0;

        next = 0u;
        t0 = tsc_start();
        (void)ft_table_extra_maintain(&me, 0u, now, tmo,
                                      expired, insert_n, 0u, &next);
        t1 = tsc_end();
        e_cy = t1 - t0;
    }

    {
        double nbk = (double)nb_bk;
        double cb  = (double)c_cy / nbk;
        double eb  = (double)e_cy / nbk;
        double ratio = (cb > 0.0) ? (eb / cb) : 0.0;

        printf("  %8u %5u%% %5u%% %8u %10.2f %10.2f %8.3f\n",
               n_entries, fill_pct, expire_pct, nb_bk,
               cb, eb, ratio);
    }

    ft_flow4_table_destroy(&ft_c);
    ft_table_extra_destroy(&ft_e);

out:
    hugefree(pool_c, pool_c_mem);
    hugefree(pool_e, pool_e_mem);
    hugefree(bk_c,   bk_c_mem);
    hugefree(bk_e,   bk_e_mem);
    free(expired);
    return 0;
}

int
main(void)
{
    const unsigned N_list[]      = { 16384u, 65536u, 262144u, 1048576u,
                                     4194304u };
    const unsigned fill_list[]   = { 25u, 50u, 75u, 90u };
    const unsigned expire_list[] = { 10u, 50u, 90u };
    unsigned ni;
    unsigned fi;
    unsigned ei;

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    ft_arch_init(FT_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);

    printf("flow4 maintain sweep (cy/bucket, single pass)\n");
    printf("  %8s %5s  %5s  %8s %10s %10s %8s\n",
           "N", "fill", "exp", "nb_bk", "classic", "extra", "e/c");

    for (ni = 0u; ni < sizeof(N_list) / sizeof(N_list[0]); ni++) {
        for (fi = 0u; fi < sizeof(fill_list) / sizeof(fill_list[0]); fi++) {
            for (ei = 0u; ei < sizeof(expire_list) / sizeof(expire_list[0]);
                 ei++) {
                (void)run_one(N_list[ni], fill_list[fi], expire_list[ei]);
            }
        }
    }

    return 0;
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
