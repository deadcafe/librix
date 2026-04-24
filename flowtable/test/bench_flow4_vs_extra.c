/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_flow4_vs_extra.c - matched classic-vs-extra microbench.
 *
 * Same N, same bucket count, same keys, same rep count for both
 * variants.  Runs insert / find_hit / find_miss / touch / remove /
 * maintain(0%|50%|100% stale) / maintain_idx_bulk.  Per-op cycle
 * cost is averaged over REPS * N operations (maintain is per-bucket).
 */

#include <assert.h>
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

#define N_ENTRIES (1u << 16)
#define REPS      8u
#define TS_SHIFT  4u

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
make_miss_classic(struct flow4_key *k, unsigned i)
{
    memset(k, 0, sizeof(*k));
    k->family   = 4u;
    k->proto    = 17u;        /* UDP — never collides with TCP fill */
    k->src_ip   = 0xDEAD0000u | i;
    k->dst_ip   = 0xBEEF0000u | i;
    k->src_port = (u16)(i & 0xFFFFu);
    k->dst_port = (u16)((~i) & 0xFFFFu);
}

static void
make_miss_extra(struct flow4_extra_key *k, unsigned i)
{
    memset(k, 0, sizeof(*k));
    k->family   = 4u;
    k->proto    = 17u;
    k->src_addr = 0xDEAD0000u | i;
    k->dst_addr = 0xBEEF0000u | i;
    k->src_port = (u16)(i & 0xFFFFu);
    k->dst_port = (u16)((~i) & 0xFFFFu);
}

/* Mark first `stale_n` entries as stale (direct TS write). */
static void
age_classic(struct flow4_entry *pool, unsigned stale_n,
            u64 now, u64 old)
{
    unsigned i;
    u32 enc_old = (u32)flow_timestamp_encode(old, TS_SHIFT);
    u32 enc_now = (u32)flow_timestamp_encode(now, TS_SHIFT);

    for (i = 0u; i < stale_n; i++)
        pool[i].meta.timestamp = enc_old;
    for (; i < N_ENTRIES; i++) {
        if (pool[i].meta.timestamp != 0u)
            pool[i].meta.timestamp = enc_now;
    }
}

static void
age_extra(struct ft_table_extra *ft,
          struct flow4_extra_entry *pool, unsigned stale_n,
          u64 now, u64 old)
{
    unsigned i;
    u32 enc_old = flow_extra_timestamp_encode(old, TS_SHIFT);
    u32 enc_now = flow_extra_timestamp_encode(now, TS_SHIFT);
    unsigned bk_mask = ft_table_extra_nb_bk(ft) - 1u;

    for (i = 0u; i < stale_n; i++) {
        unsigned bk_idx = pool[i].meta.cur_hash & bk_mask;
        unsigned slot   = (unsigned)pool[i].meta.slot;

        flow_extra_ts_set(&ft->buckets[bk_idx], slot, enc_old);
    }
    for (; i < N_ENTRIES; i++) {
        unsigned bk_idx = pool[i].meta.cur_hash & bk_mask;
        unsigned slot   = (unsigned)pool[i].meta.slot;

        if (ft->buckets[bk_idx].extra[slot] != 0u)
            flow_extra_ts_set(&ft->buckets[bk_idx], slot, enc_now);
    }
}

int
main(void)
{
    size_t pool_c_mem = (size_t)N_ENTRIES * sizeof(struct flow4_entry);
    size_t pool_e_mem = (size_t)N_ENTRIES * sizeof(struct flow4_extra_entry);
    size_t bk_c_mem   = ft_table_bucket_size(N_ENTRIES);
    size_t bk_e_mem   = flow4_extra_table_bucket_size(N_ENTRIES);
    struct flow4_entry       *pool_c;
    struct flow4_extra_entry *pool_e;
    void                     *bk_c;
    void                     *bk_e;
    struct ft_table        ft_c;
    struct ft_table_extra  ft_e;
    struct ft_table_config       cfg_c = { .ts_shift = TS_SHIFT };
    struct ft_table_extra_config cfg_e = { .ts_shift = TS_SHIFT };
    u32 *expired;
    u64 c_ins = 0, c_find = 0, c_miss = 0, c_touch = 0, c_rm = 0;
    u64 e_ins = 0, e_find = 0, e_miss = 0, e_touch = 0, e_rm = 0;
    u64 c_m0 = 0, c_m50 = 0, c_m100 = 0, c_midx = 0;
    u64 e_m0 = 0, e_m50 = 0, e_m100 = 0, e_midx = 0;
    unsigned r;

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    ft_arch_init(FT_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);

    pool_c = hugealloc(pool_c_mem);
    pool_e = hugealloc(pool_e_mem);
    bk_c   = hugealloc(bk_c_mem);
    bk_e   = hugealloc(bk_e_mem);
    expired = calloc(N_ENTRIES, sizeof(*expired));
    assert(pool_c && pool_e && bk_c && bk_e && expired);

    fill_classic_keys(pool_c, N_ENTRIES);
    fill_extra_keys(pool_e, N_ENTRIES);

    for (r = 0u; r < REPS; r++) {
        const u64 now  = 1000000u;
        const u64 old  = 0u;
        const u64 tmo  = 1u << 18;
        unsigned i;
        u64 t0;
        u64 t1;
        int rc;

        /* --- reset tables --- */
        memset(bk_c, 0, bk_c_mem);
        memset(bk_e, 0, bk_e_mem);
        for (i = 0u; i < N_ENTRIES; i++) {
            memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
            memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
        }
        rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                       struct flow4_entry, key,
                                       bk_c, bk_c_mem, &cfg_c);
        assert(rc == 0);
        rc = flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                    bk_e, bk_e_mem, &cfg_e);
        assert(rc == 0);
        (void)rc;

        /* --- insert --- */
        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++)
            (void)ft_flow4_table_add_idx(&ft_c, i + 1u, now);
        t1 = tsc_end();
        c_ins += (t1 - t0);

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++)
            (void)flow4_extra_table_add(&ft_e, &pool_e[i], now);
        t1 = tsc_end();
        e_ins += (t1 - t0);

        /* --- find_hit --- */
        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++) {
            struct flow4_key k = { 0 };

            k.family   = 4u;
            k.proto    = 6u;
            k.src_ip   = 0x0A000000u | i;
            k.dst_ip   = 0x0B000000u | (i + 1u);
            k.src_port = (u16)(i * 7u);
            k.dst_port = (u16)(i * 11u);
            (void)ft_flow4_table_find(&ft_c, &k, 0u);
        }
        t1 = tsc_end();
        c_find += (t1 - t0);

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++) {
            struct flow4_extra_key k = { 0 };

            k.family   = 4u;
            k.proto    = 6u;
            k.src_addr = 0x0A000000u | i;
            k.dst_addr = 0x0B000000u | (i + 1u);
            k.src_port = (u16)(i * 7u);
            k.dst_port = (u16)(i * 11u);
            (void)flow4_extra_table_find(&ft_e, &k);
        }
        t1 = tsc_end();
        e_find += (t1 - t0);

        /* --- find_miss --- */
        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++) {
            struct flow4_key k;

            make_miss_classic(&k, i);
            (void)ft_flow4_table_find(&ft_c, &k, 0u);
        }
        t1 = tsc_end();
        c_miss += (t1 - t0);

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++) {
            struct flow4_extra_key k;

            make_miss_extra(&k, i);
            (void)flow4_extra_table_find(&ft_e, &k);
        }
        t1 = tsc_end();
        e_miss += (t1 - t0);

        /* --- touch (every entry) --- */
        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++)
            flow_timestamp_touch(&pool_c[i].meta, now, TS_SHIFT);
        t1 = tsc_end();
        c_touch += (t1 - t0);

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++)
            ft_table_extra_touch(&ft_e, i + 1u, now);
        t1 = tsc_end();
        e_touch += (t1 - t0);

        /* --- maintain sweep (0% stale) --- */
        {
            struct ft_maint_ctx mc = {
                .buckets     = ft_c.buckets,
                .rhh_nb      = &ft_c.ht_head.rhh_nb,
                .pool_base   = (const unsigned char *)pool_c,
                .stats       = &ft_c.stats,
                .pool_stride = sizeof(*pool_c),
                .meta_off    = offsetof(struct flow4_entry, meta),
                .max_entries = N_ENTRIES,
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
                .max_entries = N_ENTRIES,
                .rhh_mask    = ft_e.ht_head.rhh_mask,
                .ts_shift    = TS_SHIFT,
            };
            unsigned next = 0u;

            age_classic(pool_c, 0u, now, old);
            age_extra(&ft_e, pool_e, 0u, now, old);

            t0 = tsc_start();
            (void)ft_table_maintain(&mc, 0u, now, tmo,
                                    expired, N_ENTRIES, 0u, &next);
            t1 = tsc_end();
            c_m0 += (t1 - t0);

            t0 = tsc_start();
            (void)ft_table_extra_maintain(&me, 0u, now, tmo,
                                          expired, N_ENTRIES, 0u, &next);
            t1 = tsc_end();
            e_m0 += (t1 - t0);

            /* 50% stale — re-fill both tables first because the 0%
             * pass did not remove anything, but the extra-side path
             * must refresh slot extra[] after age_extra. */
            age_classic(pool_c, N_ENTRIES / 2u, now, old);
            age_extra(&ft_e, pool_e, N_ENTRIES / 2u, now, old);
            t0 = tsc_start();
            (void)ft_table_maintain(&mc, 0u, now, tmo,
                                    expired, N_ENTRIES, 0u, &next);
            t1 = tsc_end();
            c_m50 += (t1 - t0);
            t0 = tsc_start();
            (void)ft_table_extra_maintain(&me, 0u, now, tmo,
                                          expired, N_ENTRIES, 0u, &next);
            t1 = tsc_end();
            e_m50 += (t1 - t0);

            /* The m50 sweep evicted half the entries.  Reinsert and
             * set 100% of remaining entries stale for m100. */
            memset(bk_c, 0, bk_c_mem);
            memset(bk_e, 0, bk_e_mem);
            for (i = 0u; i < N_ENTRIES; i++) {
                memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
                memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
            }
            (void)FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                            struct flow4_entry, key,
                                            bk_c, bk_c_mem, &cfg_c);
            (void)flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                         bk_e, bk_e_mem, &cfg_e);
            mc.rhh_mask = ft_c.ht_head.rhh_mask;
            me.rhh_mask = ft_e.ht_head.rhh_mask;
            mc.buckets  = ft_c.buckets;
            me.buckets  = ft_e.buckets;
            for (i = 0u; i < N_ENTRIES; i++) {
                (void)ft_flow4_table_add_idx(&ft_c, i + 1u, now);
                (void)flow4_extra_table_add(&ft_e, &pool_e[i], now);
            }

            age_classic(pool_c, N_ENTRIES, now, old);
            age_extra(&ft_e, pool_e, N_ENTRIES, now, old);
            t0 = tsc_start();
            (void)ft_table_maintain(&mc, 0u, now, tmo,
                                    expired, N_ENTRIES, 0u, &next);
            t1 = tsc_end();
            c_m100 += (t1 - t0);
            t0 = tsc_start();
            (void)ft_table_extra_maintain(&me, 0u, now, tmo,
                                          expired, N_ENTRIES, 0u, &next);
            t1 = tsc_end();
            e_m100 += (t1 - t0);

            /* --- maintain_idx_bulk --- */
            memset(bk_c, 0, bk_c_mem);
            memset(bk_e, 0, bk_e_mem);
            for (i = 0u; i < N_ENTRIES; i++) {
                memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
                memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
            }
            (void)FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                            struct flow4_entry, key,
                                            bk_c, bk_c_mem, &cfg_c);
            (void)flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                         bk_e, bk_e_mem, &cfg_e);
            mc.rhh_mask = ft_c.ht_head.rhh_mask;
            me.rhh_mask = ft_e.ht_head.rhh_mask;
            mc.buckets  = ft_c.buckets;
            me.buckets  = ft_e.buckets;
            for (i = 0u; i < N_ENTRIES; i++) {
                (void)ft_flow4_table_add_idx(&ft_c, i + 1u, now);
                (void)flow4_extra_table_add(&ft_e, &pool_e[i], now);
            }
            age_classic(pool_c, N_ENTRIES / 2u, now, old);
            age_extra(&ft_e, pool_e, N_ENTRIES / 2u, now, old);

            for (i = 0u; i < N_ENTRIES; i++)
                expired[i] = i + 1u;
            t0 = tsc_start();
            (void)ft_table_maintain_idx_bulk(&mc, expired, N_ENTRIES,
                                             now, tmo,
                                             expired, N_ENTRIES,
                                             0u, 0);
            t1 = tsc_end();
            c_midx += (t1 - t0);

            for (i = 0u; i < N_ENTRIES; i++)
                expired[i] = i + 1u;
            t0 = tsc_start();
            (void)ft_table_extra_maintain_idx_bulk(&me, expired, N_ENTRIES,
                                                   now, tmo,
                                                   expired, N_ENTRIES,
                                                   0u, 0);
            t1 = tsc_end();
            e_midx += (t1 - t0);
        }

        /* --- remove --- */
        memset(bk_c, 0, bk_c_mem);
        memset(bk_e, 0, bk_e_mem);
        for (i = 0u; i < N_ENTRIES; i++) {
            memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
            memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
        }
        (void)FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                        struct flow4_entry, key,
                                        bk_c, bk_c_mem, &cfg_c);
        (void)flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                     bk_e, bk_e_mem, &cfg_e);
        for (i = 0u; i < N_ENTRIES; i++) {
            (void)ft_flow4_table_add_idx(&ft_c, i + 1u, now);
            (void)flow4_extra_table_add(&ft_e, &pool_e[i], now);
        }

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++)
            (void)ft_flow4_table_del_idx(&ft_c, i + 1u);
        t1 = tsc_end();
        c_rm += (t1 - t0);

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++) {
            struct flow4_extra_key k = { 0 };

            k.family   = 4u;
            k.proto    = 6u;
            k.src_addr = 0x0A000000u | i;
            k.dst_addr = 0x0B000000u | (i + 1u);
            k.src_port = (u16)(i * 7u);
            k.dst_port = (u16)(i * 11u);
            (void)flow4_extra_table_del(&ft_e, &k);
        }
        t1 = tsc_end();
        e_rm += (t1 - t0);
    }

    {
        double per_op = (double)N_ENTRIES * REPS;
        double per_bk = (double)ft_table_extra_nb_bk(&ft_e) * REPS;

        printf("matched flow4 bench (N=%u, reps=%u, ts_shift=%u)\n",
               N_ENTRIES, REPS, TS_SHIFT);
        printf("  classic entry : %zu B    extra entry  : %zu B\n",
               sizeof(struct flow4_entry),
               sizeof(struct flow4_extra_entry));
        printf("  classic bucket: %zu B    extra bucket : %zu B\n",
               sizeof(struct rix_hash_bucket_s),
               sizeof(struct rix_hash_bucket_extra_s));
        printf("  %-18s %10s %10s %10s\n",
               "op", "classic", "extra", "delta");
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "insert",
               (double)c_ins / per_op, (double)e_ins / per_op,
               ((double)e_ins - (double)c_ins) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "find_hit",
               (double)c_find / per_op, (double)e_find / per_op,
               ((double)e_find - (double)c_find) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "find_miss",
               (double)c_miss / per_op, (double)e_miss / per_op,
               ((double)e_miss - (double)c_miss) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "touch",
               (double)c_touch / per_op, (double)e_touch / per_op,
               ((double)e_touch - (double)c_touch) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "remove",
               (double)c_rm / per_op, (double)e_rm / per_op,
               ((double)e_rm - (double)c_rm) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f  (cy/bucket)\n",
               "maint_0pct",
               (double)c_m0 / per_bk, (double)e_m0 / per_bk,
               ((double)e_m0 - (double)c_m0) / per_bk);
        printf("  %-18s %10.2f %10.2f %+10.2f  (cy/bucket)\n",
               "maint_50pct",
               (double)c_m50 / per_bk, (double)e_m50 / per_bk,
               ((double)e_m50 - (double)c_m50) / per_bk);
        printf("  %-18s %10.2f %10.2f %+10.2f  (cy/bucket)\n",
               "maint_100pct",
               (double)c_m100 / per_bk, (double)e_m100 / per_bk,
               ((double)e_m100 - (double)c_m100) / per_bk);
        printf("  %-18s %10.2f %10.2f %+10.2f  (cy/idx)\n",
               "maint_idx_bulk_50",
               (double)c_midx / per_op, (double)e_midx / per_op,
               ((double)e_midx - (double)c_midx) / per_op);
    }

    ft_flow4_table_destroy(&ft_c);
    ft_table_extra_destroy(&ft_e);
    free(expired);
    munmap(pool_c, pool_c_mem);
    munmap(pool_e, pool_e_mem);
    munmap(bk_c, bk_c_mem);
    munmap(bk_e, bk_e_mem);
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
