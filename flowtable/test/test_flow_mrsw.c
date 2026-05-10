/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "flow_table.h"

#define TEST_ASSERT(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr, "%s:%d: assertion failed: %s\n",                 \
                    __FILE__, __LINE__, #expr);                               \
            abort();                                                          \
        }                                                                     \
    } while (0)

struct rec4 {
    struct flow4_mrsw_entry flow;
    u32 marker;
};

struct rec6 {
    struct flow6_mrsw_entry flow;
    u32 marker;
};

struct recu {
    struct flowu_mrsw_entry flow;
    u32 marker;
};

static void *
xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);

    TEST_ASSERT(p != NULL);
    return p;
}

static struct flow4_key
key4(unsigned i)
{
    struct flow4_key k;

    memset(&k, 0, sizeof(k));
    k.family = 4u;
    k.proto = 17u;
    k.src_port = (u16)(1000u + i);
    k.dst_port = (u16)(2000u + i);
    k.vrfid = i & 7u;
    k.src_ip = UINT32_C(0x0a000001) + i;
    k.dst_ip = UINT32_C(0x0a010001) + i;
    return k;
}

static struct flow6_key
key6(unsigned i)
{
    struct flow6_key k;

    memset(&k, 0, sizeof(k));
    k.family = 6u;
    k.proto = 6u;
    k.src_port = (u16)(3000u + i);
    k.dst_port = (u16)(4000u + i);
    k.vrfid = i & 15u;
    k.src_ip[15] = (u8)i;
    k.dst_ip[15] = (u8)(i + 1u);
    return k;
}

static struct flowu_key
keyu(unsigned i)
{
    struct flowu_key k;

    memset(&k, 0, sizeof(k));
    k.family = (i & 1u) ? 6u : 4u;
    k.proto = 17u;
    k.src_port = (u16)(5000u + i);
    k.dst_port = (u16)(6000u + i);
    k.vrfid = i & 31u;
    if (k.family == 4u) {
        k.addr.v4.src = UINT32_C(0xc0a80001) + i;
        k.addr.v4.dst = UINT32_C(0xc0a80101) + i;
    } else {
        k.addr.v6.src[15] = (u8)i;
        k.addr.v6.dst[15] = (u8)(i + 1u);
    }
    return k;
}

static void
test_mrsw_bucket_size_guidance(void)
{
    size_t bsz_1m = ft_mrsw_table_bucket_size(1048576u);

    TEST_ASSERT(ft_mrsw_table_bucket_size(21u)
                == FT_TABLE_MIN_NB_BK * sizeof(struct rix_hash_mrsw_bucket_s));
    TEST_ASSERT(ft_mrsw_table_bucket_size(22u)
                == FT_TABLE_MIN_NB_BK * sizeof(struct rix_hash_mrsw_bucket_s));
    TEST_ASSERT(bsz_1m == 131072u * sizeof(struct rix_hash_mrsw_bucket_s));
}

static void
test_flow4_basic(void)
{
    enum { N = 128u };
    struct rec4 *rec = xcalloc(N, sizeof(*rec));
    size_t bsz = ft_mrsw_table_bucket_size(N);
    size_t alloc_bsz = bsz + FT_MRSW_TABLE_BUCKET_ALIGN;
    size_t new_bsz = ft_mrsw_table_bucket_size(N * 2u);
    size_t new_alloc_bsz = new_bsz + FT_MRSW_TABLE_BUCKET_ALIGN;
    void *buckets = xcalloc(1u, alloc_bsz);
    void *new_buckets = xcalloc(1u, new_alloc_bsz);
    struct ft_mrsw_table ft;
    u32 idxv[4];
    u32 unused[4];
    struct flow4_key keys[4];
    struct ft_table_result results[4];
    struct ft_table_stats stats;
    struct flow_status status;

    TEST_ASSERT(FT_FLOW4_MRSW_TABLE_INIT_TYPED(&ft, rec, N, struct rec4,
                                               flow, buckets, alloc_bsz,
                                               NULL) == 0);
    for (unsigned i = 0u; i < N; i++) {
        rec[i].flow.key = key4(i + 1u);
        rec[i].marker = i + 100u;
    }

    idxv[0] = 1u;
    idxv[1] = 2u;
    TEST_ASSERT(ft_mrsw_table_add_idx_bulk(&ft, idxv, 2u, FT_ADD_IGNORE,
                                           1000u, unused) == 0u);
    TEST_ASSERT(idxv[0] == 1u);
    TEST_ASSERT(idxv[1] == 2u);
    TEST_ASSERT(ft_flow4_mrsw_table_find(&ft, &rec[0].flow.key, 1100u) == 1u);

    idxv[0] = 3u;
    rec[2].flow.key = rec[0].flow.key;
    TEST_ASSERT(ft_mrsw_table_add_idx_bulk(&ft, idxv, 1u, FT_ADD_IGNORE,
                                           1200u, unused) == 1u);
    TEST_ASSERT(idxv[0] == 1u);
    TEST_ASSERT(unused[0] == 3u);
    idxv[0] = 3u;
    TEST_ASSERT(ft_mrsw_table_add_idx_bulk(&ft, idxv, 1u, FT_ADD_UPDATE,
                                           1250u, unused) == 1u);
    TEST_ASSERT(idxv[0] == 3u);
    TEST_ASSERT(unused[0] == 1u);
    TEST_ASSERT(ft_flow4_mrsw_table_find(&ft, &rec[2].flow.key, 1260u) == 3u);
    TEST_ASSERT(ft_flow4_mrsw_table_find(&ft, &rec[0].flow.key, 0u) == 3u);

    idxv[0] = 3u;
    idxv[1] = 2u;
    TEST_ASSERT(ft_mrsw_table_del_idx_bulk(&ft, idxv, 2u, unused) == 2u);
    TEST_ASSERT(ft_flow4_mrsw_table_find(&ft, &rec[0].flow.key, 0u) == RIX_NIL);
    TEST_ASSERT(ft_flow4_mrsw_table_find(&ft, &rec[1].flow.key, 0u) == RIX_NIL);

    idxv[0] = 4u;
    idxv[1] = 5u;
    idxv[2] = 6u;
    TEST_ASSERT(ft_mrsw_table_add_idx_bulk(&ft, idxv, 3u, FT_ADD_IGNORE,
                                           1300u, unused) == 0u);
    keys[0] = rec[3].flow.key;
    keys[1] = rec[4].flow.key;
    keys[2] = rec[5].flow.key;
    ft_flow4_mrsw_table_find_bulk(&ft, keys, 3u, 1400u, results);
    TEST_ASSERT(results[0].entry_idx == 4u);
    TEST_ASSERT(results[1].entry_idx == 5u);
    TEST_ASSERT(results[2].entry_idx == 6u);
    TEST_ASSERT(ft_mrsw_table_migrate(&ft, new_buckets, new_alloc_bsz) == 0);
    TEST_ASSERT(ft_flow4_mrsw_table_find(&ft, &rec[3].flow.key, 0u) == 4u);

    ft_mrsw_table_stats(&ft, &stats);
    TEST_ASSERT(stats.core.adds == 5u);
    TEST_ASSERT(stats.core.add_existing == 2u);
    TEST_ASSERT(stats.core.lookups == 0u);
    TEST_ASSERT(stats.core.hits == 0u);
    TEST_ASSERT(stats.core.misses == 0u);
    ft_mrsw_table_status(&ft, &status);
    TEST_ASSERT(status.entries == 3u);
    TEST_ASSERT(stats.maint_calls == 0u);
    TEST_ASSERT(stats.maint_bucket_checks == 0u);
    TEST_ASSERT(stats.maint_evictions == 0u);
    ft_mrsw_table_flush(&ft);
    TEST_ASSERT(ft_mrsw_table_nb_entries(&ft) == 0u);

    free(new_buckets);
    free(buckets);
    free(rec);
}

static void
test_flow6_flowu_basic(void)
{
    enum { N = 32u };
    struct rec6 *rec6v = xcalloc(N, sizeof(*rec6v));
    struct recu *recuv = xcalloc(N, sizeof(*recuv));
    size_t bsz = ft_mrsw_table_bucket_size(N);
    size_t alloc_bsz = bsz + FT_MRSW_TABLE_BUCKET_ALIGN;
    void *b6 = xcalloc(1u, alloc_bsz);
    void *bu = xcalloc(1u, alloc_bsz);
    struct ft_mrsw_table ft6;
    struct ft_mrsw_table ftu;
    u32 idx;
    u32 unused;

    TEST_ASSERT(FT_FLOW6_MRSW_TABLE_INIT_TYPED(&ft6, rec6v, N, struct rec6,
                                               flow, b6, alloc_bsz, NULL) == 0);
    TEST_ASSERT(FT_FLOWU_MRSW_TABLE_INIT_TYPED(&ftu, recuv, N, struct recu,
                                               flow, bu, alloc_bsz, NULL) == 0);
    rec6v[0].flow.key = key6(1u);
    recuv[0].flow.key = keyu(1u);
    idx = 1u;
    TEST_ASSERT(ft_mrsw_table_add_idx_bulk(&ft6, &idx, 1u, FT_ADD_IGNORE,
                                           10u, &unused) == 0u);
    TEST_ASSERT(ft_flow6_mrsw_table_find(&ft6, &rec6v[0].flow.key, 11u) == 1u);
    idx = 1u;
    TEST_ASSERT(ft_mrsw_table_add_idx_bulk(&ftu, &idx, 1u, FT_ADD_IGNORE,
                                           10u, &unused) == 0u);
    TEST_ASSERT(ft_flowu_mrsw_table_find(&ftu, &recuv[0].flow.key, 11u) == 1u);

    free(bu);
    free(b6);
    free(recuv);
    free(rec6v);
}

struct stress_ctx {
    struct ft_mrsw_table *ft;
    struct rec4 *rec;
    atomic_uint stop;
    atomic_uint failures;
};

static void *
stress_reader(void *arg)
{
    struct stress_ctx *ctx = arg;

    while (atomic_load_explicit(&ctx->stop, memory_order_acquire) == 0u) {
        for (unsigned i = 1u; i <= 64u; i++) {
            u32 idx = ft_flow4_mrsw_table_find(ctx->ft, &ctx->rec[i - 1u].flow.key,
                                               1000u + i);
            if (idx != i)
                atomic_fetch_add_explicit(&ctx->failures, 1u,
                                          memory_order_relaxed);
        }
    }
    return NULL;
}

static void
test_flow4_mrsw_stress(void)
{
    enum { N = 256u, READERS = 4u, LOOPS = 20000u };
    struct rec4 *rec = xcalloc(N, sizeof(*rec));
    size_t bsz = ft_mrsw_table_bucket_size(N);
    size_t alloc_bsz = bsz + FT_MRSW_TABLE_BUCKET_ALIGN;
    void *buckets = xcalloc(1u, alloc_bsz);
    struct ft_mrsw_table ft;
    struct stress_ctx ctx;
    pthread_t th[READERS];
    u32 unused[N];

    TEST_ASSERT(FT_FLOW4_MRSW_TABLE_INIT_TYPED(&ft, rec, N, struct rec4,
                                               flow, buckets, alloc_bsz,
                                               NULL) == 0);
    for (unsigned i = 0u; i < N; i++)
        rec[i].flow.key = key4(i + 1u);
    for (unsigned i = 1u; i <= 64u; i++) {
        u32 idx = i;
        TEST_ASSERT(ft_mrsw_table_add_idx_bulk(&ft, &idx, 1u, FT_ADD_IGNORE,
                                               100u, unused) == 0u);
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.ft = &ft;
    ctx.rec = rec;
    for (unsigned r = 0u; r < READERS; r++)
        TEST_ASSERT(pthread_create(&th[r], NULL, stress_reader, &ctx) == 0);

    for (unsigned i = 0u; i < LOOPS; i++) {
        u32 idx = 65u + (i & 63u);
        (void)ft_mrsw_table_add_idx_bulk(&ft, &idx, 1u, FT_ADD_IGNORE,
                                         200u + i, unused);
        (void)ft_mrsw_table_del_idx_bulk(&ft, &idx, 1u, unused);
    }

    atomic_store_explicit(&ctx.stop, 1u, memory_order_release);
    for (unsigned r = 0u; r < READERS; r++)
        TEST_ASSERT(pthread_join(th[r], NULL) == 0);
    TEST_ASSERT(atomic_load_explicit(&ctx.failures, memory_order_relaxed) == 0u);

    free(buckets);
    free(rec);
}

int
main(void)
{
    ft_arch_init(FT_ARCH_AUTO);
    test_mrsw_bucket_size_guidance();
    test_flow4_basic();
    test_flow6_flowu_basic();
    test_flow4_mrsw_stress();
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
