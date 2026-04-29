/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_flow4_vs_extra.c - matched pure-vs-extra microbench.
 *
 * Same N, same bucket count, same keys, same rep count for both variants.
 * The first block is a full-table stress pass; the later sections measure
 * representative 75% fill add/maintenance cases.  Per-op cycle cost is
 * averaged over REPS * N operations unless printed otherwise.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <rix/rix_defs_private.h>
#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>

#include "flowtable/flow4_table.h"
#include "flowtable/flow4_extra_table.h"
#include "flowtable/flow_key.h"

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
fill_pure_keys(struct flow4_entry *pool, unsigned n)
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
make_miss_pure(struct flow4_key *k, unsigned i)
{
    memset(k, 0, sizeof(*k));
    k->family   = 4u;
    k->proto    = 17u;        /* UDP - never collides with TCP fill */
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
age_pure(struct flow4_entry *pool, unsigned stale_n,
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
    u32 *idxv;
    u32 *unused_idxv;
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
    expired     = calloc(N_ENTRIES, sizeof(*expired));
    idxv        = calloc(N_ENTRIES, sizeof(*idxv));
    unused_idxv = calloc(N_ENTRIES, sizeof(*unused_idxv));
    assert(pool_c && pool_e && bk_c && bk_e && expired && idxv && unused_idxv);

    fill_pure_keys(pool_c, N_ENTRIES);
    fill_extra_keys(pool_e, N_ENTRIES);

    for (r = 0u; r < REPS; r++) {
        const u64 now  = 1000000u;
        const u64 old  = 1u << TS_SHIFT; /* encodes to 1, not permanent */
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
            (void)ft_table_extra_add_idx(&ft_e, i + 1u, now);
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

            make_miss_pure(&k, i);
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

            age_pure(pool_c, 0u, now, old);
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

            /* 50% stale - re-fill both tables first because the 0%
             * pass did not remove anything, but the extra-side path
             * must refresh slot extra[] after age_extra. */
            age_pure(pool_c, N_ENTRIES / 2u, now, old);
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
                (void)ft_table_extra_add_idx(&ft_e, i + 1u, now);
            }

            age_pure(pool_c, N_ENTRIES, now, old);
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
                (void)ft_table_extra_add_idx(&ft_e, i + 1u, now);
            }
            age_pure(pool_c, N_ENTRIES / 2u, now, old);
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
            (void)ft_table_extra_add_idx(&ft_e, i + 1u, now);
        }

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++)
            (void)ft_flow4_table_del_idx(&ft_c, i + 1u);
        t1 = tsc_end();
        c_rm += (t1 - t0);

        t0 = tsc_start();
        for (i = 0u; i < N_ENTRIES; i++)
            (void)ft_table_extra_del_idx(&ft_e, i + 1u);
        t1 = tsc_end();
        e_rm += (t1 - t0);
    }

    {
        double per_op = (double)N_ENTRIES * REPS;
        double per_bk = (double)ft_table_extra_nb_bk(&ft_e) * REPS;

        printf("matched flow4 full-table stress (N=%u, reps=%u, ts_shift=%u)\n",
               N_ENTRIES, REPS, TS_SHIFT);
        printf("  pure entry : %zu B    extra entry  : %zu B\n",
               sizeof(struct flow4_entry),
               sizeof(struct flow4_extra_entry));
        printf("  pure bucket: %zu B    extra bucket : %zu B\n",
               sizeof(struct rix_hash_bucket_s),
               sizeof(struct rix_hash_bucket_extra_s));
        printf("  %-18s %10s %10s %10s\n",
               "op", "pure", "extra", "delta");
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "insert_idx_full",
               (double)c_ins / per_op, (double)e_ins / per_op,
               ((double)e_ins - (double)c_ins) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "find_hit",
               (double)c_find / per_op, (double)e_find / per_op,
               ((double)e_find - (double)c_find) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "find_miss",
               (double)c_miss / per_op, (double)e_miss / per_op,
               ((double)e_miss - (double)c_miss) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "touch_idx",
               (double)c_touch / per_op, (double)e_touch / per_op,
               ((double)e_touch - (double)c_touch) / per_op);
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "remove_idx_full",
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

    /* --- bulk insert Q-sweep: Q in {32, 64, 128, 256} --- */
    {
        static const unsigned qs[] = { 32u, 64u, 128u, 256u };
        unsigned qi;

        printf("\n  bulk insert to full (cy/entry):\n");
        printf("  %-18s %10s %10s %10s\n", "op", "pure", "extra", "delta");

        for (qi = 0u; qi < sizeof(qs) / sizeof(qs[0]); qi++) {
            unsigned Q = qs[qi];
            u64 c_bk = 0, e_bk = 0;
            char label[32];
            unsigned i;

            for (r = 0u; r < REPS; r++) {
                const u64 now = 1000000u;
                int rc;
                u64 t0, t1;

                /* build fresh index vector */
                for (i = 0u; i < N_ENTRIES; i++)
                    idxv[i] = i + 1u;

                /* pure bulk insert */
                memset(bk_c, 0, bk_c_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
                rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                               struct flow4_entry, key,
                                               bk_c, bk_c_mem, &cfg_c);
                assert(rc == 0);

                t0 = tsc_start();
                for (i = 0u; i < N_ENTRIES; i += Q)
                    (void)ft_flow4_table_add_idx_bulk(&ft_c, &idxv[i], Q,
                                                      FT_ADD_IGNORE, now,
                                                      unused_idxv);
                t1 = tsc_end();
                c_bk += (t1 - t0);

                /* rebuild index vector for extra */
                for (i = 0u; i < N_ENTRIES; i++)
                    idxv[i] = i + 1u;

                /* extra bulk insert */
                memset(bk_e, 0, bk_e_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
                rc = flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                            bk_e, bk_e_mem, &cfg_e);
                assert(rc == 0);
                (void)rc;

                t0 = tsc_start();
                for (i = 0u; i < N_ENTRIES; i += Q)
                    (void)ft_table_extra_add_idx_bulk(&ft_e, &idxv[i], Q,
                                                      FT_ADD_IGNORE, now,
                                                      unused_idxv);
                t1 = tsc_end();
                e_bk += (t1 - t0);
            }

            snprintf(label, sizeof(label), "insert_q%u", Q);
            printf("  %-18s %10.2f %10.2f %+10.2f\n", label,
                   (double)c_bk / (double)(N_ENTRIES * REPS),
                   (double)e_bk / (double)(N_ENTRIES * REPS),
                   ((double)e_bk - (double)c_bk)
                   / (double)(N_ENTRIES * REPS));
        }
    }

    /* --- insert at 75% fill: Q in {1, 32, 64, 128, 256} --- */
    {
        static const unsigned qs[] = { 1u, 32u, 64u, 128u, 256u };
        const unsigned N_FILL = N_ENTRIES * 3u / 4u;   /* 75% of capacity */
        unsigned qi;

        printf("\n  insert @ 75%% fill (N_fill=%u, cy/entry):\n", N_FILL);
        printf("  %-18s %10s %10s %10s\n", "op", "pure", "extra", "delta");

        for (qi = 0u; qi < sizeof(qs) / sizeof(qs[0]); qi++) {
            unsigned Q = qs[qi];
            u64 c_f = 0, e_f = 0;
            char label[32];
            unsigned i;

            for (r = 0u; r < REPS; r++) {
                const u64 now = 1000000u;
                int rc;
                u64 t0, t1;

                /* --- pure --- */
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;

                memset(bk_c, 0, bk_c_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
                rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                               struct flow4_entry, key,
                                               bk_c, bk_c_mem, &cfg_c);
                assert(rc == 0);

                if (Q == 1u) {
                    t0 = tsc_start();
                    for (i = 0u; i < N_FILL; i++)
                        (void)ft_flow4_table_add_idx(&ft_c, idxv[i], now);
                    t1 = tsc_end();
                } else {
                    t0 = tsc_start();
                    for (i = 0u; i < N_FILL; i += Q)
                        (void)ft_flow4_table_add_idx_bulk(&ft_c, &idxv[i], Q,
                                                          FT_ADD_IGNORE, now,
                                                          unused_idxv);
                    t1 = tsc_end();
                }
                c_f += (t1 - t0);

                /* --- extra --- */
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;

                memset(bk_e, 0, bk_e_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
                rc = flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                            bk_e, bk_e_mem, &cfg_e);
                assert(rc == 0);
                (void)rc;

                if (Q == 1u) {
                    t0 = tsc_start();
                    for (i = 0u; i < N_FILL; i++)
                        (void)ft_table_extra_add_idx(&ft_e, idxv[i], now);
                    t1 = tsc_end();
                } else {
                    t0 = tsc_start();
                    for (i = 0u; i < N_FILL; i += Q)
                        (void)ft_table_extra_add_idx_bulk(&ft_e, &idxv[i], Q,
                                                          FT_ADD_IGNORE, now,
                                                          unused_idxv);
                    t1 = tsc_end();
                }
                e_f += (t1 - t0);
            }

            snprintf(label, sizeof(label), "insert_q%u", Q);
            printf("  %-18s %10.2f %10.2f %+10.2f\n", label,
                   (double)c_f / (double)(N_FILL * REPS),
                   (double)e_f / (double)(N_FILL * REPS),
                   ((double)e_f - (double)c_f) / (double)(N_FILL * REPS));
        }
    }

    /* --- insert+maint combined @ 75% fill, 50% turnover (cy/entry) ---
     *
     * Cycle per rep:
     *   1. (untimed) reset, fill N_FILL=49152 entries (75%), age N_STALE=24576
     *      stale (50% of filled).  Pre-flip a key bit on stale entries so
     *      re-insertion after maint produces genuinely new hash positions.
     *   2. (TIMED)   full maint sweep -> frees ~N_STALE slots
     *   3. (TIMED)   insert freed entries (now with modified keys)
     *
     * Report: total_timed_cy / (N_STALE * REPS) = cy per entry turned over.
     */
    {
        const unsigned N_FILL  = N_ENTRIES * 3u / 4u;   /* 49152 */
        const unsigned N_STALE = N_FILL / 2u;            /* 24576 */
        static const unsigned qs[] = { 1u, 32u, 64u, 128u, 256u };
        u32 *freed_c = calloc(N_ENTRIES, sizeof(u32));
        u32 *freed_e = calloc(N_ENTRIES, sizeof(u32));
        unsigned qi;

        assert(freed_c && freed_e);

        printf("\n  insert+maint combined @ 75%% fill, 50%% turnover (cy/entry):\n");
        printf("  %-18s %10s %10s %10s\n", "op", "pure", "extra", "delta");

        for (qi = 0u; qi < sizeof(qs) / sizeof(qs[0]); qi++) {
            unsigned Q = qs[qi];
            u64 c_combo = 0, e_combo = 0;
            char label[32];

            for (r = 0u; r < REPS; r++) {
                const u64 now = 1000000u;
                const u64 old = 1u << TS_SHIFT; /* encodes to 1, not permanent */
                const u64 tmo = 1u << 18;
                unsigned i, n_freed;
                unsigned next;
                int rc;
                u64 t0, t1;

                /* ---- pure setup (not timed) ---- */
                fill_pure_keys(pool_c, N_ENTRIES);
                memset(bk_c, 0, bk_c_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
                rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                               struct flow4_entry, key,
                                               bk_c, bk_c_mem, &cfg_c);
                assert(rc == 0);
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;
                (void)ft_flow4_table_add_idx_bulk(&ft_c, idxv, N_FILL,
                                                  FT_ADD_IGNORE, now,
                                                  unused_idxv);
                age_pure(pool_c, N_STALE, now, old);
                /* pre-flip key bit on stale entries so re-insert is fresh */
                for (i = 0u; i < N_STALE; i++)
                    pool_c[i].key.src_ip ^= 0x01000000u;

                /* ---- pure timed: maint + re-insert ---- */
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
                    next = 0u;
                    t0 = tsc_start();
                    n_freed = ft_table_maintain(&mc, 0u, now, tmo,
                                               freed_c, N_ENTRIES, 0u, &next);
                    if (Q == 1u) {
                        for (i = 0u; i < n_freed; i++)
                            (void)ft_flow4_table_add_idx(&ft_c, freed_c[i], now);
                    } else {
                        for (i = 0u; i < n_freed; i += Q) {
                            unsigned b = (i + Q <= n_freed) ? Q : n_freed - i;
                            (void)ft_flow4_table_add_idx_bulk(&ft_c,
                                &freed_c[i], b, FT_ADD_IGNORE, now,
                                unused_idxv);
                        }
                    }
                    t1 = tsc_end();
                    c_combo += (t1 - t0);
                }

                /* ---- extra setup (not timed) ---- */
                fill_extra_keys(pool_e, N_ENTRIES);
                memset(bk_e, 0, bk_e_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
                rc = flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                            bk_e, bk_e_mem, &cfg_e);
                assert(rc == 0);
                (void)rc;
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;
                (void)ft_table_extra_add_idx_bulk(&ft_e, idxv, N_FILL,
                                                  FT_ADD_IGNORE, now,
                                                  unused_idxv);
                age_extra(&ft_e, pool_e, N_STALE, now, old);
                for (i = 0u; i < N_STALE; i++)
                    pool_e[i].key.src_addr ^= 0x01000000u;

                /* ---- extra timed: maint + re-insert ---- */
                {
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
                    next = 0u;
                    t0 = tsc_start();
                    n_freed = ft_table_extra_maintain(&me, 0u, now, tmo,
                                                      freed_e, N_ENTRIES, 0u,
                                                      &next);
                    if (Q == 1u) {
                        for (i = 0u; i < n_freed; i++)
                            (void)ft_table_extra_add_idx(&ft_e,
                                                         freed_e[i], now);
                    } else {
                        for (i = 0u; i < n_freed; i += Q) {
                            unsigned b = (i + Q <= n_freed) ? Q : n_freed - i;
                            (void)ft_table_extra_add_idx_bulk(&ft_e,
                                &freed_e[i], b, FT_ADD_IGNORE, now,
                                unused_idxv);
                        }
                    }
                    t1 = tsc_end();
                    e_combo += (t1 - t0);
                }
            }

            snprintf(label, sizeof(label), "maint+ins_q%u", Q);
            printf("  %-18s %10.2f %10.2f %+10.2f\n", label,
                   (double)c_combo / (double)(N_STALE * REPS),
                   (double)e_combo / (double)(N_STALE * REPS),
                   ((double)e_combo - (double)c_combo)
                   / (double)(N_STALE * REPS));
        }

        free(freed_c);
        free(freed_e);
    }

    /* --- add+inline-maint: add_idx_bulk_maint (cy/entry) ---
     *
     * Models the actual flow cache pattern: each add also proactively
     * expires stale entries from the same bucket (bk->extra[] for extra,
     * entry->meta for pure).  No separate full-sweep maint needed.
     *
     * Setup: fill 75% with ts_stale=1<<TS_SHIFT (encodes to 1, clearly
     *         expired at now=1000000 with tmo=1<<18).  ts=0 is the permanent
     *         sentinel and must NOT be used here.
     * Timed:  insert N_NEW free-pool entries via add_idx_bulk_maint in
     *         Q-entry batches. Keep N_NEW modest so the phase-1-only row
     *         stays comparable to the 2M-scale case instead of measuring
     *         pathological 100% table fill.
     *
     * pure Phase 2: loads entry->meta per slot from the pool (cache miss).
     * extra   Phase 2: reads bk->extra[s] already warm from Phase 1 prefetch.
     */
    {
        const unsigned N_FILL    = N_ENTRIES * 3u / 4u;  /* 49152 */
        const unsigned N_NEW     = N_ENTRIES / 32u;      /* +3.125% cap */
        const u64 tmo_inline     = 1u << 18;
        const u64 ts_stale       = 1u << TS_SHIFT; /* encodes to 1, never permanent */
        static const unsigned qs[] = { 32u, 64u, 128u, 256u };
        u32 *new_idxv = calloc(N_NEW, sizeof(u32));
        unsigned qi;

        assert(new_idxv);

        printf("\n  add+inline-maint (cy/entry, 75%% fill, all-stale, N_new=%u):\n",
               N_NEW);
        printf("  %-18s %10s %10s %10s\n", "op", "pure", "extra", "delta");

        for (qi = 0u; qi < sizeof(qs) / sizeof(qs[0]); qi++) {
            unsigned Q       = qs[qi];
            unsigned max_uns = Q * 2u;   /* maint_budget = Q */
            u32 *maint_buf_c = calloc(max_uns, sizeof(u32));
            u32 *maint_buf_e = calloc(max_uns, sizeof(u32));
            u64 c_p1 = 0, e_p1 = 0;   /* Phase-1-only (timeout=0) */
            u64 c_am = 0, e_am = 0;   /* Phase 1 + Phase 2 */
            char label[32];
            unsigned i;

            assert(maint_buf_c && maint_buf_e);

            for (r = 0u; r < REPS; r++) {
                const u64 now = 1000000u;
                int rc;
                u64 t0, t1;

                /* ---- pure setup (untimed) ---- */
                fill_pure_keys(pool_c, N_ENTRIES);
                memset(bk_c, 0, bk_c_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
                rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                               struct flow4_entry, key,
                                               bk_c, bk_c_mem, &cfg_c);
                assert(rc == 0);
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;
                (void)ft_flow4_table_add_idx_bulk(&ft_c, idxv, N_FILL,
                                                  FT_ADD_IGNORE, ts_stale,
                                                  unused_idxv);

                /* Phase-1-only pure: call add_idx_bulk directly */
                for (i = 0u; i < N_NEW; i++)
                    new_idxv[i] = N_FILL + 1u + i;
                t0 = tsc_start();
                for (i = 0u; i < N_NEW; i += Q) {
                    unsigned b = (i + Q <= N_NEW) ? Q : N_NEW - i;
                    (void)ft_flow4_table_add_idx_bulk(&ft_c, &new_idxv[i], b,
                                                      FT_ADD_IGNORE, now,
                                                      maint_buf_c);
                }
                t1 = tsc_end();
                c_p1 += (t1 - t0);

                /* Re-fill pure and measure Phase 1 + Phase 2 */
                memset(bk_c, 0, bk_c_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
                rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N_ENTRIES,
                                               struct flow4_entry, key,
                                               bk_c, bk_c_mem, &cfg_c);
                assert(rc == 0);
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;
                (void)ft_flow4_table_add_idx_bulk(&ft_c, idxv, N_FILL,
                                                  FT_ADD_IGNORE, ts_stale,
                                                  unused_idxv);
                for (i = 0u; i < N_NEW; i++)
                    new_idxv[i] = N_FILL + 1u + i;
                t0 = tsc_start();
                for (i = 0u; i < N_NEW; i += Q) {
                    unsigned b = (i + Q <= N_NEW) ? Q : N_NEW - i;
                    (void)ft_table_add_idx_bulk_maint(&ft_c, &new_idxv[i], b,
                                                      FT_ADD_IGNORE, now,
                                                      tmo_inline, maint_buf_c,
                                                      max_uns, 1u);
                }
                t1 = tsc_end();
                c_am += (t1 - t0);

                /* ---- extra setup (untimed) ---- */
                fill_extra_keys(pool_e, N_ENTRIES);
                memset(bk_e, 0, bk_e_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
                rc = flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                            bk_e, bk_e_mem, &cfg_e);
                assert(rc == 0);
                (void)rc;
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;
                (void)ft_table_extra_add_idx_bulk(&ft_e, idxv, N_FILL,
                                                  FT_ADD_IGNORE, ts_stale,
                                                  unused_idxv);

                /* Phase-1-only extra: call add_idx_bulk directly */
                for (i = 0u; i < N_NEW; i++)
                    new_idxv[i] = N_FILL + 1u + i;
                t0 = tsc_start();
                for (i = 0u; i < N_NEW; i += Q) {
                    unsigned b = (i + Q <= N_NEW) ? Q : N_NEW - i;
                    (void)ft_table_extra_add_idx_bulk(&ft_e, &new_idxv[i], b,
                                                      FT_ADD_IGNORE, now,
                                                      maint_buf_e);
                }
                t1 = tsc_end();
                e_p1 += (t1 - t0);

                /* Re-fill extra and measure Phase 1 + Phase 2 */
                memset(bk_e, 0, bk_e_mem);
                for (i = 0u; i < N_ENTRIES; i++)
                    memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
                rc = flow4_extra_table_init(&ft_e, pool_e, N_ENTRIES,
                                            bk_e, bk_e_mem, &cfg_e);
                assert(rc == 0);
                (void)rc;
                for (i = 0u; i < N_FILL; i++)
                    idxv[i] = i + 1u;
                (void)ft_table_extra_add_idx_bulk(&ft_e, idxv, N_FILL,
                                                  FT_ADD_IGNORE, ts_stale,
                                                  unused_idxv);
                for (i = 0u; i < N_NEW; i++)
                    new_idxv[i] = N_FILL + 1u + i;
                t0 = tsc_start();
                for (i = 0u; i < N_NEW; i += Q) {
                    unsigned b = (i + Q <= N_NEW) ? Q : N_NEW - i;
                    (void)ft_table_extra_add_idx_bulk_maint(&ft_e, &new_idxv[i],
                                                            b, FT_ADD_IGNORE,
                                                            now, tmo_inline,
                                                            maint_buf_e,
                                                            max_uns, 1u);
                }
                t1 = tsc_end();
                e_am += (t1 - t0);
            }

            /* phase-1-only row */
            snprintf(label, sizeof(label), "add_only_q%u", Q);
            printf("  %-18s %10.2f %10.2f %+10.2f  (phase1 only)\n", label,
                   (double)c_p1 / (double)(N_NEW * REPS),
                   (double)e_p1 / (double)(N_NEW * REPS),
                   ((double)e_p1 - (double)c_p1) / (double)(N_NEW * REPS));

            /* phase-1+2 row */
            snprintf(label, sizeof(label), "add+maint_q%u", Q);
            printf("  %-18s %10.2f %10.2f %+10.2f\n", label,
                   (double)c_am / (double)(N_NEW * REPS),
                   (double)e_am / (double)(N_NEW * REPS),
                   ((double)e_am - (double)c_am) / (double)(N_NEW * REPS));

            free(maint_buf_c);
            free(maint_buf_e);
        }

        free(new_idxv);
    }

    /* --- 2M-scale add+inline-maint (realistic environment) ---
     *
     * Target: N=2M entries, 75% fill (1.5M stale), N_NEW=64K new flows
     * per batch (~3% of capacity, ~4% of N_FILL).  Pool sizes: pure 72 MB,
     * extra 48 MB.
     * Both pools exceed typical L3; all pool accesses are DRAM-pressure.
     *
     * Pure Phase 2: pool[idx].meta.timestamp per slot -> random DRAM miss
     * Extra  Phase 2: bk->extra[s] already warm from Phase 1 -> ~0 extra cy
     *
     * Pure bucket: 131072 x 128B = 16 MB (fits L3)
     * Extra   bucket: 131072 x 192B = 24 MB (borderline L3)
     */
    {
        const unsigned N_LARGE   = 1u << 21;          /* 2,097,152 */
        const unsigned N_FILL_L  = N_LARGE * 3u / 4u; /* 1,572,864 stale */
        const unsigned N_NEW_L   = 1u << 16;          /* 65,536 new (~3% of N_FILL_L) */
        const unsigned N_USED_L  = N_FILL_L + N_NEW_L + 2u;
        const u64 ts_stale_l     = 1u << TS_SHIFT;
        const u64 tmo_l          = 1u << 18;
        const unsigned REPS_L    = 2u;
        const unsigned Q_L       = 64u;
        const unsigned max_uns_l = Q_L * 2u;

        size_t pc_l_sz = (size_t)N_LARGE * sizeof(struct flow4_entry);
        size_t pe_l_sz = (size_t)N_LARGE * sizeof(struct flow4_extra_entry);
        size_t bc_l_sz = ft_table_bucket_size(N_LARGE);
        size_t be_l_sz = flow4_extra_table_bucket_size(N_LARGE);

        struct flow4_entry       *pool_c_l = hugealloc(pc_l_sz);
        struct flow4_extra_entry *pool_e_l = hugealloc(pe_l_sz);
        void *bk_c_l = hugealloc(bc_l_sz);
        void *bk_e_l = hugealloc(be_l_sz);
        u32 *idxv_l     = calloc(N_LARGE, sizeof(u32));
        u32 *unused_l   = calloc(N_LARGE, sizeof(u32));
        u32 *new_idxv_l = calloc(N_NEW_L, sizeof(u32));
        u32 *mb_c_l     = calloc(max_uns_l, sizeof(u32));
        u32 *mb_e_l     = calloc(max_uns_l, sizeof(u32));
        struct ft_table ft_cl;
        struct ft_table_extra ft_el;
        struct ft_table_config cfg_cl       = { .ts_shift = TS_SHIFT };
        struct ft_table_extra_config cfg_el = { .ts_shift = TS_SHIFT };
        u64 c_p1_l = 0, e_p1_l = 0;
        u64 c_am_l = 0, e_am_l = 0;
        unsigned rl, il;

        assert(pool_c_l && pool_e_l && bk_c_l && bk_e_l);
        assert(idxv_l && unused_l && new_idxv_l && mb_c_l && mb_e_l);

        fill_pure_keys(pool_c_l, N_USED_L);
        fill_extra_keys(pool_e_l, N_USED_L);

        for (rl = 0u; rl < REPS_L; rl++) {
            const u64 now_l = 1000000u;
            int rcl = 0;
            (void)rcl;

            /* ---- pure Phase-1-only ---- */
            for (il = 0u; il < N_USED_L; il++)
                memset(&pool_c_l[il].meta, 0, sizeof(pool_c_l[il].meta));
            memset(bk_c_l, 0, bc_l_sz);
            rcl = FT_FLOW4_TABLE_INIT_TYPED(&ft_cl, pool_c_l, N_LARGE,
                                             struct flow4_entry, key,
                                             bk_c_l, bc_l_sz, &cfg_cl);
            assert(rcl == 0);
            for (il = 0u; il < N_FILL_L; il++) idxv_l[il] = il + 1u;
            (void)ft_flow4_table_add_idx_bulk(&ft_cl, idxv_l, N_FILL_L,
                                              FT_ADD_IGNORE, ts_stale_l, unused_l);
            for (il = 0u; il < N_NEW_L; il++) new_idxv_l[il] = N_FILL_L + 1u + il;
            {
                u64 t0 = tsc_start();
                for (il = 0u; il < N_NEW_L; il += Q_L) {
                    unsigned b = (il + Q_L <= N_NEW_L) ? Q_L : N_NEW_L - il;
                    (void)ft_flow4_table_add_idx_bulk(&ft_cl, &new_idxv_l[il], b,
                                                      FT_ADD_IGNORE, now_l, mb_c_l);
                }
                c_p1_l += tsc_end() - t0;
            }

            /* ---- pure Phase-1+2 ---- */
            for (il = 0u; il < N_USED_L; il++)
                memset(&pool_c_l[il].meta, 0, sizeof(pool_c_l[il].meta));
            memset(bk_c_l, 0, bc_l_sz);
            rcl = FT_FLOW4_TABLE_INIT_TYPED(&ft_cl, pool_c_l, N_LARGE,
                                             struct flow4_entry, key,
                                             bk_c_l, bc_l_sz, &cfg_cl);
            assert(rcl == 0);
            for (il = 0u; il < N_FILL_L; il++) idxv_l[il] = il + 1u;
            (void)ft_flow4_table_add_idx_bulk(&ft_cl, idxv_l, N_FILL_L,
                                              FT_ADD_IGNORE, ts_stale_l, unused_l);
            for (il = 0u; il < N_NEW_L; il++) new_idxv_l[il] = N_FILL_L + 1u + il;
            {
                u64 t0 = tsc_start();
                for (il = 0u; il < N_NEW_L; il += Q_L) {
                    unsigned b = (il + Q_L <= N_NEW_L) ? Q_L : N_NEW_L - il;
                    (void)ft_table_add_idx_bulk_maint(&ft_cl, &new_idxv_l[il], b,
                                                      FT_ADD_IGNORE, now_l, tmo_l,
                                                      mb_c_l, max_uns_l, 1u);
                }
                c_am_l += tsc_end() - t0;
            }

            /* ---- extra Phase-1-only ---- */
            for (il = 0u; il < N_USED_L; il++)
                memset(&pool_e_l[il].meta, 0, sizeof(pool_e_l[il].meta));
            memset(bk_e_l, 0, be_l_sz);
            rcl = flow4_extra_table_init(&ft_el, pool_e_l, N_LARGE,
                                         bk_e_l, be_l_sz, &cfg_el);
            assert(rcl == 0);
            for (il = 0u; il < N_FILL_L; il++) idxv_l[il] = il + 1u;
            (void)ft_table_extra_add_idx_bulk(&ft_el, idxv_l, N_FILL_L,
                                              FT_ADD_IGNORE, ts_stale_l, unused_l);
            for (il = 0u; il < N_NEW_L; il++) new_idxv_l[il] = N_FILL_L + 1u + il;
            {
                u64 t0 = tsc_start();
                for (il = 0u; il < N_NEW_L; il += Q_L) {
                    unsigned b = (il + Q_L <= N_NEW_L) ? Q_L : N_NEW_L - il;
                    (void)ft_table_extra_add_idx_bulk(&ft_el, &new_idxv_l[il], b,
                                                      FT_ADD_IGNORE, now_l, mb_e_l);
                }
                e_p1_l += tsc_end() - t0;
            }

            /* ---- extra Phase-1+2 ---- */
            for (il = 0u; il < N_USED_L; il++)
                memset(&pool_e_l[il].meta, 0, sizeof(pool_e_l[il].meta));
            memset(bk_e_l, 0, be_l_sz);
            rcl = flow4_extra_table_init(&ft_el, pool_e_l, N_LARGE,
                                         bk_e_l, be_l_sz, &cfg_el);
            assert(rcl == 0);
            for (il = 0u; il < N_FILL_L; il++) idxv_l[il] = il + 1u;
            (void)ft_table_extra_add_idx_bulk(&ft_el, idxv_l, N_FILL_L,
                                              FT_ADD_IGNORE, ts_stale_l, unused_l);
            for (il = 0u; il < N_NEW_L; il++) new_idxv_l[il] = N_FILL_L + 1u + il;
            {
                u64 t0 = tsc_start();
                for (il = 0u; il < N_NEW_L; il += Q_L) {
                    unsigned b = (il + Q_L <= N_NEW_L) ? Q_L : N_NEW_L - il;
                    (void)ft_table_extra_add_idx_bulk_maint(&ft_el, &new_idxv_l[il],
                                                            b, FT_ADD_IGNORE,
                                                            now_l, tmo_l,
                                                            mb_e_l, max_uns_l, 1u);
                }
                e_am_l += tsc_end() - t0;
            }
        }

        printf("\n  add+inline-maint 2M-scale (cy/entry, 75%% fill, all-stale, Q=%u):\n", Q_L);
        printf("  (pure pool=72MB  extra pool=48MB  bucket_c=16MB  bucket_e=24MB)\n");
        printf("  %-18s %10s %10s %10s\n", "op", "pure", "extra", "delta");
        printf("  %-18s %10.2f %10.2f %+10.2f  (phase1 only)\n", "add_only",
               (double)c_p1_l / (double)(N_NEW_L * REPS_L),
               (double)e_p1_l / (double)(N_NEW_L * REPS_L),
               ((double)e_p1_l - (double)c_p1_l) / (double)(N_NEW_L * REPS_L));
        printf("  %-18s %10.2f %10.2f %+10.2f\n", "add+maint",
               (double)c_am_l / (double)(N_NEW_L * REPS_L),
               (double)e_am_l / (double)(N_NEW_L * REPS_L),
               ((double)e_am_l - (double)c_am_l) / (double)(N_NEW_L * REPS_L));

        munmap(pool_c_l, pc_l_sz);
        munmap(pool_e_l, pe_l_sz);
        munmap(bk_c_l, bc_l_sz);
        munmap(bk_e_l, be_l_sz);
        free(idxv_l);
        free(unused_l);
        free(new_idxv_l);
        free(mb_c_l);
        free(mb_e_l);
    }

    ft_flow4_table_destroy(&ft_c);
    ft_table_extra_destroy(&ft_e);
    free(expired);
    free(idxv);
    free(unused_idxv);
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
