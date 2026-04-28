/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * test_flow4_extra.c - Functional tests for the flow4 slot_extra variant.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>

#include "flow_table.h"

#define N_MAX 1024u

static unsigned
parse_arch(const char *name)
{
    if (name == NULL || strcmp(name, "auto") == 0)
        return FT_ARCH_AUTO;
    if (strcmp(name, "gen") == 0)
        return FT_ARCH_GEN;
    if (strcmp(name, "sse") == 0)
        return FT_ARCH_SSE;
    if (strcmp(name, "avx2") == 0)
        return FT_ARCH_AVX2;
    if (strcmp(name, "avx512") == 0)
        return FT_ARCH_AVX512;
    fprintf(stderr, "usage: ft_test_extra [gen|sse|avx2|avx512|auto]\n");
    abort();
}

struct test_any_flow4_record {
    struct flow4_entry entry;
};

struct test_any_flow4_extra_record {
    struct flow4_extra_entry entry;
};

static int
count_walk_cb(u32 entry_idx, void *arg)
{
    unsigned *count = arg;

    assert(entry_idx != 0u);
    (*count)++;
    return 0;
}

static void *
hugealloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

static struct flow4_extra_key
mk_key(unsigned i)
{
    struct flow4_extra_key k = { 0 };

    k.family   = 4u;
    k.proto    = 6u;
    k.src_addr = 0x0A000000u | i;
    k.dst_addr = 0x0B000000u | (i + 1u);
    k.src_port = (u16)(i * 7u);
    k.dst_port = (u16)(i * 11u);
    return k;
}

static struct flow6_extra_key
mk_key6(unsigned i)
{
    struct flow6_extra_key k = { 0 };
    u32 w;

    k.family   = 10u;
    k.proto    = 6u;
    k.src_port = (u16)(1000u + i);
    k.dst_port = (u16)(2000u + i);
    k.vrfid    = 1u;
    w = i;
    memcpy(&k.src_ip[0], &w, sizeof(w));
    w = i ^ UINT32_C(0x13579bdf);
    memcpy(&k.src_ip[4], &w, sizeof(w));
    w = i ^ UINT32_C(0x2468ace0);
    memcpy(&k.dst_ip[0], &w, sizeof(w));
    w = ~i;
    memcpy(&k.dst_ip[4], &w, sizeof(w));
    w = i * UINT32_C(0x9e3779b1);
    memcpy(&k.src_ip[8], &w, sizeof(w));
    w = (i << 1) ^ UINT32_C(0xa5a5a5a5);
    memcpy(&k.dst_ip[8], &w, sizeof(w));
    w = i + UINT32_C(0x01020304);
    memcpy(&k.src_ip[12], &w, sizeof(w));
    w = i ^ UINT32_C(0x5a5a5a5a);
    memcpy(&k.dst_ip[12], &w, sizeof(w));
    return k;
}

static struct flowu_extra_key
mk_keyu(unsigned i)
{
    struct flowu_extra_key k = { 0 };
    u32 w;

    k.proto    = 6u;
    k.src_port = (u16)(1000u + i);
    k.dst_port = (u16)(2000u + i);
    k.vrfid    = 1u;
    if ((i & 1u) == 0u) {
        k.family = 2u;
        k.addr.v4.src = UINT32_C(0x0a000001) ^ i;
        k.addr.v4.dst = UINT32_C(0x0a000101) ^ (i * UINT32_C(0x9e3779b1));
    } else {
        k.family = 10u;
        w = i;
        memcpy(&k.addr.v6.src[0], &w, sizeof(w));
        w = i ^ UINT32_C(0x13579bdf);
        memcpy(&k.addr.v6.src[4], &w, sizeof(w));
        w = i * UINT32_C(0x9e3779b1);
        memcpy(&k.addr.v6.src[8], &w, sizeof(w));
        w = i + UINT32_C(0x01020304);
        memcpy(&k.addr.v6.src[12], &w, sizeof(w));
        w = ~i;
        memcpy(&k.addr.v6.dst[0], &w, sizeof(w));
        w = i ^ UINT32_C(0x2468ace0);
        memcpy(&k.addr.v6.dst[4], &w, sizeof(w));
        w = (i << 1) ^ UINT32_C(0xa5a5a5a5);
        memcpy(&k.addr.v6.dst[8], &w, sizeof(w));
        w = i ^ UINT32_C(0x5a5a5a5a);
        memcpy(&k.addr.v6.dst[12], &w, sizeof(w));
    }
    return k;
}

static struct flow4_key
mk_classic_key(unsigned i)
{
    struct flow4_key k = { 0 };

    k.family   = 4u;
    k.proto    = 6u;
    k.src_ip   = 0x0A000000u | i;
    k.dst_ip   = 0x0B000000u | (i + 1u);
    k.src_port = (u16)(i * 7u);
    k.dst_port = (u16)(i * 11u);
    k.vrfid    = 1u;
    return k;
}

static void
test_any_macros_basic(void)
{
    size_t bk_c_sz = ft_table_bucket_size(16u);
    size_t bk_e_sz = ft_table_extra_bucket_size(16u);
    struct test_any_flow4_record *pool_c =
        hugealloc(16u * sizeof(*pool_c));
    struct test_any_flow4_extra_record *pool_e =
        hugealloc(16u * sizeof(*pool_e));
    void *bk_c = hugealloc(bk_c_sz);
    void *bk_e = hugealloc(bk_e_sz);
    struct ft_table ft_c;
    struct ft_table_extra ft_e;
    struct ft_table_config cfg_c = { 0 };
    struct ft_table_extra_config cfg_e = { .ts_shift = 4u };
    struct flow_status status;
    struct ft_table_stats stats_c;
    struct ft_table_extra_stats stats_e;
    struct ft_maint_ctx maint_c;
    struct ft_maint_extra_ctx maint_e;
    u32 idxv[4];
    u32 unused[8];
    unsigned count;
    unsigned n;
    void *new_bk_c;
    void *new_bk_e;
    size_t new_bk_c_sz;
    size_t new_bk_e_sz;

    printf("[T] flow_table generic macros basic\n");
    assert(FT_TABLE_INIT_TYPED(&ft_c, FT_TABLE_VARIANT_FLOW4,
                                   pool_c, 16u,
                                   struct test_any_flow4_record, entry,
                                   bk_c, bk_c_sz, &cfg_c) == 0);
    assert(FT_TABLE_INIT_TYPED(&ft_e, FT_TABLE_VARIANT_FLOW4,
                                   pool_e, 16u,
                                   struct test_any_flow4_extra_record, entry,
                                   bk_e, bk_e_sz, &cfg_e) == 0);

    pool_c[0].entry.key = mk_classic_key(1u);
    pool_e[0].entry.key = mk_key(1u);
    assert(FT_TABLE_ADD_IDX(&ft_c, 1u, 1000u) == 1u);
    assert(FT_TABLE_ADD_IDX(&ft_e, 1u, 1000u) == 1u);
    assert(FT_TABLE_NB_ENTRIES(&ft_c) == 1u);
    assert(FT_TABLE_NB_ENTRIES(&ft_e) == 1u);
    assert(FT_TABLE_NB_BK(&ft_c) > 0u);
    assert(FT_TABLE_NB_BK(&ft_e) > 0u);

    FT_TABLE_STATUS(&ft_c, &status);
    FT_TABLE_STATUS(&ft_e, &status);
    FT_TABLE_STATS(&ft_c, &stats_c);
    FT_TABLE_STATS(&ft_e, &stats_e);

    for (unsigned i = 1u; i < 3u; i++) {
        pool_c[i].entry.key = mk_classic_key(i + 1u);
        pool_e[i].entry.key = mk_key(i + 1u);
        idxv[i - 1u] = i + 1u;
    }
    n = FT_TABLE_ADD_IDX_BULK(&ft_c, idxv, 2u, FT_ADD_IGNORE, 2000u, unused);
    assert(n == 0u);
    idxv[0] = 2u;
    idxv[1] = 3u;
    n = FT_TABLE_ADD_IDX_BULK(&ft_e, idxv, 2u, FT_ADD_IGNORE, 2000u, unused);
    assert(n == 0u);
    assert(FT_TABLE_NB_ENTRIES(&ft_c) == 3u);
    assert(FT_TABLE_NB_ENTRIES(&ft_e) == 3u);

    pool_c[3].entry.key = mk_classic_key(4u);
    pool_e[3].entry.key = mk_key(4u);
    idxv[0] = 4u;
    n = FT_TABLE_ADD_IDX_BULK_MAINT(&ft_c, idxv, 1u, FT_ADD_IGNORE,
                                    3000u, 0u, unused, 1u, 0u);
    assert(n == 0u);
    idxv[0] = 4u;
    n = FT_TABLE_ADD_IDX_BULK_MAINT(&ft_e, idxv, 1u, FT_ADD_IGNORE,
                                    3000u, 0u, unused, 1u, 0u);
    assert(n == 0u);
    assert(FT_TABLE_NB_ENTRIES(&ft_c) == 4u);
    assert(FT_TABLE_NB_ENTRIES(&ft_e) == 4u);
    assert(FT_TABLE_MAINT_CTX_INIT(&ft_c, &maint_c) == 0);
    assert(FT_TABLE_MAINT_CTX_INIT(&ft_e, &maint_e) == 0);
    n = FT_TABLE_MAINTAIN(&maint_c, 0u, 3000u, 0u, unused, 8u, 0u, NULL);
    assert(n == 0u);
    n = FT_TABLE_MAINTAIN(&maint_e, 0u, 3000u, 0u, unused, 8u, 0u, NULL);
    assert(n == 0u);
    n = FT_TABLE_MAINTAIN_IDX_BULK(&maint_c, idxv, 1u, 3000u, 0u,
                                   unused, 8u, 0u, 0);
    assert(n == 0u);
    n = FT_TABLE_MAINTAIN_IDX_BULK(&maint_e, idxv, 1u, 3000u, 0u,
                                   unused, 8u, 0u, 0);
    assert(n == 0u);

    count = 0u;
    assert(FT_TABLE_WALK(&ft_c, count_walk_cb, &count) == 0);
    assert(count == 4u);
    count = 0u;
    assert(FT_TABLE_WALK(&ft_e, count_walk_cb, &count) == 0);
    assert(count == 4u);

    new_bk_c_sz = ft_table_bucket_mem_size(FT_TABLE_NB_BK(&ft_c) * 2u);
    new_bk_e_sz = ft_table_extra_bucket_mem_size(FT_TABLE_NB_BK(&ft_e) * 2u);
    new_bk_c = hugealloc(new_bk_c_sz);
    new_bk_e = hugealloc(new_bk_e_sz);
    assert(FT_TABLE_MIGRATE(&ft_c, new_bk_c, new_bk_c_sz) == 0);
    assert(FT_TABLE_MIGRATE(&ft_e, new_bk_e, new_bk_e_sz) == 0);
    munmap(bk_c, bk_c_sz);
    munmap(bk_e, bk_e_sz);
    bk_c = new_bk_c;
    bk_e = new_bk_e;
    bk_c_sz = new_bk_c_sz;
    bk_e_sz = new_bk_e_sz;

    idxv[0] = 2u;
    idxv[1] = 3u;
    n = FT_TABLE_DEL_IDX_BULK(&ft_c, idxv, 2u, unused);
    assert(n == 2u);
    n = FT_TABLE_DEL_IDX_BULK(&ft_e, idxv, 2u, unused);
    assert(n == 2u);
    assert(FT_TABLE_DEL_IDX(&ft_c, 1u) == 1u);
    assert(FT_TABLE_DEL_IDX(&ft_e, 1u) == 1u);
    FT_TABLE_FLUSH(&ft_c);
    FT_TABLE_FLUSH(&ft_e);
    FT_TABLE_DESTROY(&ft_c);
    FT_TABLE_DESTROY(&ft_e);
    munmap(pool_c, 16u * sizeof(*pool_c));
    munmap(pool_e, 16u * sizeof(*pool_e));
    munmap(bk_c, bk_c_sz);
    munmap(bk_e, bk_e_sz);
}

static void
test_init_basic(void)
{
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    int rc;

    printf("[T] flow4_extra init basic\n");
    rc = flow4_extra_table_init(&ft, pool, N_MAX, bk, bk_sz, &cfg);
    assert(rc == 0);
    assert(ft_table_extra_nb_entries(&ft) == 0u);
    assert(ft_table_extra_nb_bk(&ft) > 0u);
    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk, bk_sz);
}

static void
test_add_find_del(void)
{
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };

    printf("[T] flow4_extra add/find/del\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk, bk_sz, &cfg) == 0);

    for (unsigned i = 0u; i < 100u; i++)
        pool[i].key = mk_key(i);

    for (unsigned i = 0u; i < 100u; i++) {
        u32 idx = flow4_extra_table_add(&ft, &pool[i], 1000u);

        assert(idx != (u32)RIX_NIL);
    }
    assert(ft_table_extra_nb_entries(&ft) == 100u);
    for (unsigned i = 0u; i < 100u; i++) {
        struct flow4_extra_key k = mk_key(i);
        u32 idx = flow4_extra_table_find(&ft, &k);

        assert(idx != (u32)RIX_NIL);
    }
    for (unsigned i = 0u; i < 100u; i++) {
        struct flow4_extra_key k = mk_key(i);

        assert(flow4_extra_table_del(&ft, &k) != (u32)RIX_NIL);
    }
    assert(ft_table_extra_nb_entries(&ft) == 0u);

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk, bk_sz);
}

static void
test_flow6_extra_add_find_del(void)
{
    size_t bk_sz = flow6_extra_table_bucket_size(N_MAX);
    struct flow6_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    struct flow6_extra_key keys[64];
    u32 results[64];
    u32 idxv[64];
    u32 unused[64];
    unsigned hit;

    printf("[T] flow6_extra add/find/del\n");
    assert(flow6_extra_table_init(&ft, pool, N_MAX, bk, bk_sz, &cfg) == 0);
    for (unsigned i = 0u; i < 64u; i++) {
        pool[i].key = mk_key6(i);
        keys[i] = pool[i].key;
        idxv[i] = i + 1u;
    }
    assert(FT_TABLE_ADD_IDX_BULK(&ft, idxv, 64u, FT_ADD_IGNORE,
                                 1000u, unused) == 0u);
    assert(FT_TABLE_NB_ENTRIES(&ft) == 64u);
    hit = flow6_extra_table_find_bulk(&ft, keys, 64u, 2000u, results);
    assert(hit == 64u);
    for (unsigned i = 0u; i < 64u; i++)
        assert(results[i] == i + 1u);
    for (unsigned i = 0u; i < 64u; i++)
        assert(flow6_extra_table_del(&ft, &keys[i]) == i + 1u);
    assert(FT_TABLE_NB_ENTRIES(&ft) == 0u);

    FT_TABLE_DESTROY(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk, bk_sz);
}

static void
test_flowu_extra_add_find_del(void)
{
    size_t bk_sz = flowu_extra_table_bucket_size(N_MAX);
    struct flowu_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    struct flowu_extra_key keys[64];
    u32 results[64];
    u32 idxv[64];
    u32 unused[64];
    unsigned hit;

    printf("[T] flowu_extra add/find/del\n");
    assert(flowu_extra_table_init(&ft, pool, N_MAX, bk, bk_sz, &cfg) == 0);
    for (unsigned i = 0u; i < 64u; i++) {
        pool[i].key = mk_keyu(i);
        keys[i] = pool[i].key;
        idxv[i] = i + 1u;
    }
    assert(FT_TABLE_ADD_IDX_BULK(&ft, idxv, 64u, FT_ADD_IGNORE,
                                 1000u, unused) == 0u);
    assert(FT_TABLE_NB_ENTRIES(&ft) == 64u);
    hit = flowu_extra_table_find_bulk(&ft, keys, 64u, 2000u, results);
    assert(hit == 64u);
    for (unsigned i = 0u; i < 64u; i++)
        assert(results[i] == i + 1u);
    for (unsigned i = 0u; i < 64u; i++)
        assert(flowu_extra_table_del(&ft, &keys[i]) == i + 1u);
    assert(FT_TABLE_NB_ENTRIES(&ft) == 0u);

    FT_TABLE_DESTROY(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk, bk_sz);
}

#define DEFINE_EXTRA_FAMILY_TESTS(prefix, make_key)                           \
static void                                                                   \
test_##prefix##_ts_in_bucket(void)                                            \
{                                                                             \
    size_t bk_sz = prefix##_extra_table_bucket_size(N_MAX);                   \
    struct prefix##_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));     \
    void *bk_raw = hugealloc(bk_sz);                                          \
    struct ft_table_extra ft;                                                 \
    struct ft_table_extra_config cfg = { .ts_shift = 4u };                    \
    unsigned bk_idx;                                                          \
    unsigned slot;                                                            \
    u32 idx;                                                                  \
                                                                              \
    printf("[T] " #prefix "_extra TS stored in bucket extra[]\n");           \
    assert(prefix##_extra_table_init(&ft, pool, N_MAX, bk_raw,                \
                                     bk_sz, &cfg) == 0);                      \
    pool[0].key = make_key(0u);                                               \
    idx = prefix##_extra_table_add(&ft, &pool[0], 2048u);                     \
    assert(idx != (u32)RIX_NIL);                                              \
    bk_idx = pool[0].meta.cur_hash & (ft_table_extra_nb_bk(&ft) - 1u);        \
    slot = (unsigned)pool[0].meta.slot;                                       \
    assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==                    \
           flow_extra_timestamp_encode(2048u, cfg.ts_shift));                 \
    ft_table_extra_destroy(&ft);                                              \
    munmap(pool, N_MAX * sizeof(*pool));                                      \
    munmap(bk_raw, bk_sz);                                                    \
}                                                                             \
                                                                              \
static void                                                                   \
test_##prefix##_maintain_expires(void)                                        \
{                                                                             \
    size_t bk_sz = prefix##_extra_table_bucket_size(N_MAX);                   \
    struct prefix##_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));     \
    void *bk_raw = hugealloc(bk_sz);                                          \
    struct ft_table_extra ft;                                                 \
    struct ft_table_extra_config cfg = { .ts_shift = 4u };                    \
    struct ft_maint_extra_ctx ctx;                                            \
    u32 expired[64];                                                          \
    unsigned next = 0u;                                                       \
    unsigned n;                                                               \
                                                                              \
    printf("[T] " #prefix "_extra maintain expires stale entries\n");        \
    assert(prefix##_extra_table_init(&ft, pool, N_MAX, bk_raw,                \
                                     bk_sz, &cfg) == 0);                      \
    for (unsigned i = 0u; i < 50u; i++)                                       \
        pool[i].key = make_key(i);                                            \
    for (unsigned i = 0u; i < 50u; i++)                                       \
        assert(prefix##_extra_table_add(&ft, &pool[i], 1000u)                 \
               != (u32)RIX_NIL);                                             \
    assert(ft_table_extra_maint_ctx_init(&ft, &ctx) == 0);                    \
    n = ft_table_extra_maintain(&ctx, 0u, 1000u + (1u << 20),                 \
                                1u << 18, expired, 64u, 0u, &next);           \
    assert(n >= 36u);                                                         \
    (void)next;                                                               \
    ft_table_extra_destroy(&ft);                                              \
    munmap(pool, N_MAX * sizeof(*pool));                                      \
    munmap(bk_raw, bk_sz);                                                    \
}                                                                             \
                                                                              \
static void                                                                   \
test_##prefix##_find_touch_updates_bucket_extra(void)                         \
{                                                                             \
    size_t bk_sz = prefix##_extra_table_bucket_size(N_MAX);                   \
    struct prefix##_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));     \
    void *bk_raw = hugealloc(bk_sz);                                          \
    struct ft_table_extra ft;                                                 \
    struct ft_table_extra_config cfg = { .ts_shift = 4u };                    \
    struct prefix##_extra_key key;                                            \
    unsigned bk_idx;                                                          \
    unsigned slot;                                                            \
    u32 idx;                                                                  \
                                                                              \
    printf("[T] " #prefix "_extra find_touch updates bucket extra[]\n");     \
    assert(prefix##_extra_table_init(&ft, pool, N_MAX, bk_raw,                \
                                     bk_sz, &cfg) == 0);                      \
    pool[0].key = make_key(0u);                                               \
    key = pool[0].key;                                                        \
    idx = prefix##_extra_table_add(&ft, &pool[0], 1000u);                     \
    assert(idx != (u32)RIX_NIL);                                              \
    assert(prefix##_extra_table_find_touch(&ft, &key, 9000u) == idx);         \
    bk_idx = pool[0].meta.cur_hash & ft.ht_head.rhh_mask;                     \
    slot = (unsigned)pool[0].meta.slot;                                       \
    assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==                    \
           flow_extra_timestamp_encode(9000u, cfg.ts_shift));                 \
    ft_table_extra_destroy(&ft);                                              \
    munmap(pool, N_MAX * sizeof(*pool));                                      \
    munmap(bk_raw, bk_sz);                                                    \
}                                                                             \
                                                                              \
static void                                                                   \
test_##prefix##_touch_updates_bucket_extra(void)                              \
{                                                                             \
    size_t bk_sz = prefix##_extra_table_bucket_size(N_MAX);                   \
    struct prefix##_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));     \
    void *bk_raw = hugealloc(bk_sz);                                          \
    struct ft_table_extra ft;                                                 \
    struct ft_table_extra_config cfg = { .ts_shift = 4u };                    \
    unsigned bk_idx;                                                          \
    unsigned slot;                                                            \
    u32 idx;                                                                  \
                                                                              \
    printf("[T] " #prefix "_extra touch updates bucket extra[]\n");          \
    assert(prefix##_extra_table_init(&ft, pool, N_MAX, bk_raw,                \
                                     bk_sz, &cfg) == 0);                      \
    pool[0].key = make_key(0u);                                               \
    idx = prefix##_extra_table_add(&ft, &pool[0], 1000u);                     \
    assert(idx != (u32)RIX_NIL);                                              \
    ft_table_extra_touch(&ft, idx, 5000u);                                    \
    bk_idx = pool[0].meta.cur_hash & (ft_table_extra_nb_bk(&ft) - 1u);        \
    slot = (unsigned)pool[0].meta.slot;                                       \
    assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==                    \
           flow_extra_timestamp_encode(5000u, cfg.ts_shift));                 \
    ft_table_extra_destroy(&ft);                                              \
    munmap(pool, N_MAX * sizeof(*pool));                                      \
    munmap(bk_raw, bk_sz);                                                    \
}                                                                             \
                                                                              \
static void                                                                   \
test_##prefix##_touch_checked_rejects_deleted_idx(void)                       \
{                                                                             \
    size_t bk_sz = prefix##_extra_table_bucket_size(N_MAX);                   \
    struct prefix##_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));     \
    void *bk_raw = hugealloc(bk_sz);                                          \
    struct ft_table_extra ft;                                                 \
    struct ft_table_extra_config cfg = { .ts_shift = 4u };                    \
    u32 idx;                                                                  \
                                                                              \
    printf("[T] " #prefix "_extra touch_checked rejects deleted idx\n");     \
    assert(prefix##_extra_table_init(&ft, pool, N_MAX, bk_raw,                \
                                     bk_sz, &cfg) == 0);                      \
    pool[0].key = make_key(0u);                                               \
    idx = prefix##_extra_table_add(&ft, &pool[0], 1000u);                     \
    assert(idx != (u32)RIX_NIL);                                              \
    assert(prefix##_extra_table_del(&ft, &pool[0].key) == idx);               \
    assert(ft_table_extra_touch_checked(&ft, idx, 5000u) != 0);               \
    ft_table_extra_destroy(&ft);                                              \
    munmap(pool, N_MAX * sizeof(*pool));                                      \
    munmap(bk_raw, bk_sz);                                                    \
}                                                                             \
                                                                              \
static void                                                                   \
test_##prefix##_touch_after_migrate_uses_current_mask(void)                   \
{                                                                             \
    size_t old_bk_sz = prefix##_extra_table_bucket_size(N_MAX);               \
    size_t new_bk_sz = old_bk_sz * 2u;                                        \
    struct prefix##_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));     \
    void *old_bk_raw = hugealloc(old_bk_sz);                                  \
    void *new_bk_raw = hugealloc(new_bk_sz);                                  \
    struct ft_table_extra ft;                                                 \
    struct ft_table_extra_config cfg = { .ts_shift = 4u };                    \
    unsigned old_mask;                                                        \
    unsigned new_mask;                                                        \
    unsigned chosen = 0u;                                                     \
    int found = 0;                                                            \
                                                                              \
    printf("[T] " #prefix "_extra touch after migrate uses current mask\n"); \
    assert(prefix##_extra_table_init(&ft, pool, N_MAX, old_bk_raw,            \
                                     old_bk_sz, &cfg) == 0);                  \
    for (unsigned i = 0u; i < N_MAX; i++) {                                   \
        pool[i].key = make_key(i);                                            \
        assert(prefix##_extra_table_add(&ft, &pool[i], 1000u)                 \
               != (u32)RIX_NIL);                                             \
    }                                                                         \
    old_mask = ft.ht_head.rhh_mask;                                           \
    assert(ft_table_extra_migrate(&ft, new_bk_raw, new_bk_sz) == 0);          \
    new_mask = ft.ht_head.rhh_mask;                                           \
    assert(new_mask > old_mask);                                              \
    for (unsigned i = 0u; i < N_MAX; i++) {                                   \
        unsigned old_bk = pool[i].meta.cur_hash & old_mask;                  \
        unsigned new_bk = pool[i].meta.cur_hash & new_mask;                  \
                                                                              \
        if (old_bk != new_bk) {                                               \
            chosen = i;                                                       \
            found = 1;                                                        \
            break;                                                            \
        }                                                                     \
    }                                                                         \
    assert(found);                                                            \
    assert(ft_table_extra_touch_checked(&ft, chosen + 1u, 7000u) == 0);       \
    {                                                                         \
        unsigned bk_idx = pool[chosen].meta.cur_hash & new_mask;              \
        unsigned slot = (unsigned)pool[chosen].meta.slot;                     \
                                                                              \
        assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==                \
               flow_extra_timestamp_encode(7000u, cfg.ts_shift));             \
    }                                                                         \
    ft_table_extra_destroy(&ft);                                              \
    munmap(pool, N_MAX * sizeof(*pool));                                      \
    munmap(old_bk_raw, old_bk_sz);                                            \
    munmap(new_bk_raw, new_bk_sz);                                            \
}                                                                             \
                                                                              \
static void                                                                   \
test_##prefix##_find_bulk_touches_bucket_extra(void)                          \
{                                                                             \
    enum { NB = 64u };                                                        \
    size_t bk_sz = prefix##_extra_table_bucket_size(N_MAX);                   \
    struct prefix##_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));     \
    void *bk_raw = hugealloc(bk_sz);                                          \
    struct ft_table_extra ft;                                                 \
    struct ft_table_extra_config cfg = { .ts_shift = 4u };                    \
    struct prefix##_extra_key keys[NB];                                       \
    u32 results[NB];                                                          \
    unsigned hit;                                                             \
                                                                              \
    printf("[T] " #prefix "_extra find_bulk path touches bucket extra[]\n"); \
    assert(prefix##_extra_table_init(&ft, pool, N_MAX, bk_raw,                \
                                     bk_sz, &cfg) == 0);                      \
    for (unsigned i = 0u; i < NB; i++) {                                      \
        pool[i].key = make_key(i);                                            \
        keys[i] = pool[i].key;                                                \
        assert(prefix##_extra_table_add(&ft, &pool[i], 1000u)                 \
               != (u32)RIX_NIL);                                             \
    }                                                                         \
    hit = prefix##_extra_table_find_bulk(&ft, keys, NB, 9000u, results);      \
    assert(hit == NB);                                                        \
    for (unsigned i = 0u; i < NB; i++) {                                      \
        unsigned bk_idx = pool[i].meta.cur_hash & ft.ht_head.rhh_mask;        \
        unsigned slot = (unsigned)pool[i].meta.slot;                          \
                                                                              \
        assert(results[i] == i + 1u);                                         \
        assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==                \
               flow_extra_timestamp_encode(9000u, cfg.ts_shift));             \
    }                                                                         \
    ft_table_extra_destroy(&ft);                                              \
    munmap(pool, N_MAX * sizeof(*pool));                                      \
    munmap(bk_raw, bk_sz);                                                    \
}

DEFINE_EXTRA_FAMILY_TESTS(flow6, mk_key6)
DEFINE_EXTRA_FAMILY_TESTS(flowu, mk_keyu)

static void
test_ts_in_bucket(void)
{
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    unsigned bk_idx;
    unsigned slot;
    u32 got;
    u32 want;
    u32 idx;

    printf("[T] flow4_extra TS stored in bucket extra[]\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw, bk_sz, &cfg) == 0);

    pool[0].key = mk_key(0u);
    idx = flow4_extra_table_add(&ft, &pool[0], 2048u);
    assert(idx != (u32)RIX_NIL);

    bk_idx = pool[0].meta.cur_hash & (ft_table_extra_nb_bk(&ft) - 1u);
    slot   = (unsigned)pool[0].meta.slot;
    got    = flow_extra_ts_get(&ft.buckets[bk_idx], slot);
    want   = flow_extra_timestamp_encode(2048u, cfg.ts_shift);
    assert(got == want);

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, bk_sz);
}

static void
test_maintain_expires(void)
{
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    u32 expired[64];
    struct ft_maint_extra_ctx ctx;
    unsigned next = 0u;
    unsigned n;

    printf("[T] flow4_extra maintain expires stale entries\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw, bk_sz, &cfg) == 0);

    for (unsigned i = 0u; i < 50u; i++)
        pool[i].key = mk_key(i);
    for (unsigned i = 0u; i < 50u; i++)
        flow4_extra_table_add(&ft, &pool[i], 1000u);

    assert(ft_table_extra_maint_ctx_init(&ft, &ctx) == 0);

    n = ft_table_extra_maintain(&ctx, 0u,
                                /* now     = */ 1000u + (1u << 20),
                                /* timeout = */ 1u << 18,
                                expired, 64u, 0u, &next);
    assert(n >= 36u);
    (void)next;

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, bk_sz);
}

static void
test_find_touch_updates_bucket_extra(void)
{
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    struct flow4_extra_key key;
    unsigned bk_idx;
    unsigned slot;
    u32 idx;

    printf("[T] flow4_extra find_touch updates bucket extra[]\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw, bk_sz, &cfg) == 0);

    pool[0].key = mk_key(0u);
    key = pool[0].key;
    idx = flow4_extra_table_add(&ft, &pool[0], 1000u);
    assert(idx != (u32)RIX_NIL);
    assert(flow4_extra_table_find_touch(&ft, &key, 9000u) == idx);

    bk_idx = pool[0].meta.cur_hash & ft.ht_head.rhh_mask;
    slot   = (unsigned)pool[0].meta.slot;
    assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==
           flow_extra_timestamp_encode(9000u, cfg.ts_shift));

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, bk_sz);
}

static void
test_touch_updates_bucket_extra(void)
{
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    unsigned bk_idx;
    unsigned slot;
    u32 idx;

    printf("[T] flow4_extra touch updates bucket extra[]\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw, bk_sz, &cfg) == 0);

    pool[0].key = mk_key(0u);
    idx = flow4_extra_table_add(&ft, &pool[0], 1000u);
    assert(idx != (u32)RIX_NIL);

    ft_table_extra_touch(&ft, idx, 5000u);

    bk_idx = pool[0].meta.cur_hash & (ft_table_extra_nb_bk(&ft) - 1u);
    slot   = (unsigned)pool[0].meta.slot;
    assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==
           flow_extra_timestamp_encode(5000u, cfg.ts_shift));

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, bk_sz);
}

static void
test_touch_checked_rejects_deleted_idx(void)
{
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    u32 idx;

    printf("[T] flow4_extra touch_checked rejects deleted idx\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw, bk_sz, &cfg) == 0);

    pool[0].key = mk_key(0u);
    idx = flow4_extra_table_add(&ft, &pool[0], 1000u);
    assert(idx != (u32)RIX_NIL);
    assert(flow4_extra_table_del(&ft, &pool[0].key) == idx);
    assert(ft_table_extra_touch_checked(&ft, idx, 5000u) != 0);

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, bk_sz);
}

static void
test_touch_after_migrate_uses_current_mask(void)
{
    size_t old_bk_sz = flow4_extra_table_bucket_size(N_MAX);
    size_t new_bk_sz = old_bk_sz * 2u;
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *old_bk_raw = hugealloc(old_bk_sz);
    void *new_bk_raw = hugealloc(new_bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    unsigned old_mask;
    unsigned new_mask;
    unsigned chosen = 0u;
    int found = 0;

    printf("[T] flow4_extra touch after migrate uses current mask\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, old_bk_raw,
                                  old_bk_sz, &cfg) == 0);

    for (unsigned i = 0u; i < N_MAX; i++) {
        pool[i].key = mk_key(i);
        assert(flow4_extra_table_add(&ft, &pool[i], 1000u) != (u32)RIX_NIL);
    }
    old_mask = ft.ht_head.rhh_mask;
    assert(ft_table_extra_migrate(&ft, new_bk_raw, new_bk_sz) == 0);
    new_mask = ft.ht_head.rhh_mask;
    assert(new_mask > old_mask);

    for (unsigned i = 0u; i < N_MAX; i++) {
        unsigned old_bk = pool[i].meta.cur_hash & old_mask;
        unsigned new_bk = pool[i].meta.cur_hash & new_mask;

        if (old_bk != new_bk) {
            chosen = i;
            found = 1;
            break;
        }
    }
    assert(found);
    assert(ft_table_extra_touch_checked(&ft, chosen + 1u, 7000u) == 0);
    {
        unsigned bk_idx = pool[chosen].meta.cur_hash & new_mask;
        unsigned slot = (unsigned)pool[chosen].meta.slot;

        assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==
               flow_extra_timestamp_encode(7000u, cfg.ts_shift));
    }

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(old_bk_raw, old_bk_sz);
    munmap(new_bk_raw, new_bk_sz);
}

static void
test_find_bulk_touches_bucket_extra(void)
{
    enum { NB = 64u };
    size_t bk_sz = flow4_extra_table_bucket_size(N_MAX);
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(bk_sz);
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    struct flow4_extra_key keys[NB];
    u32 results[NB];
    unsigned hit;

    printf("[T] flow4_extra find_bulk path touches bucket extra[]\n");
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw, bk_sz, &cfg) == 0);

    for (unsigned i = 0u; i < NB; i++) {
        pool[i].key = mk_key(i);
        keys[i] = pool[i].key;
        assert(flow4_extra_table_add(&ft, &pool[i], 1000u) != (u32)RIX_NIL);
    }
    hit = flow4_extra_table_find_bulk(&ft, keys, NB, 9000u, results);
    assert(hit == NB);
    for (unsigned i = 0u; i < NB; i++) {
        unsigned bk_idx = pool[i].meta.cur_hash & ft.ht_head.rhh_mask;
        unsigned slot = (unsigned)pool[i].meta.slot;

        assert(results[i] == i + 1u);
        assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==
               flow_extra_timestamp_encode(9000u, cfg.ts_shift));
    }

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, bk_sz);
}

static void
test_compact_key_omits_vrfid(void)
{
    struct flow4_key c0 = { 0 };
    struct flow4_key c1 = { 0 };
    struct flow4_extra_key e0 = { 0 };
    struct flow4_extra_key e1 = { 0 };

    printf("[T] flow4_extra compact key omits vrfid by design\n");
    c0.family = c1.family = 4u;
    c0.proto = c1.proto = 6u;
    c0.src_port = c1.src_port = 1000u;
    c0.dst_port = c1.dst_port = 2000u;
    c0.src_ip = c1.src_ip = 0x0A000001u;
    c0.dst_ip = c1.dst_ip = 0x0B000001u;
    c0.vrfid = 1u;
    c1.vrfid = 2u;

    e0.family = e1.family = c0.family;
    e0.proto = e1.proto = c0.proto;
    e0.src_port = e1.src_port = c0.src_port;
    e0.dst_port = e1.dst_port = c0.dst_port;
    e0.src_addr = e1.src_addr = c0.src_ip;
    e0.dst_addr = e1.dst_addr = c0.dst_ip;

    assert(memcmp(&c0, &c1, sizeof(c0)) != 0);
    assert(memcmp(&e0, &e1, sizeof(e0)) == 0);
}

int
main(int argc, char **argv)
{
    unsigned arch_enable;

    assert(argc <= 2);
    arch_enable = parse_arch(argc == 2 ? argv[1] : "auto");
    ft_arch_init(arch_enable);
    ft_arch_extra_init(arch_enable);
    test_any_macros_basic();
    test_init_basic();
    test_add_find_del();
    test_flow6_extra_add_find_del();
    test_flowu_extra_add_find_del();
    test_ts_in_bucket();
    test_maintain_expires();
    test_find_touch_updates_bucket_extra();
    test_touch_updates_bucket_extra();
    test_touch_checked_rejects_deleted_idx();
    test_touch_after_migrate_uses_current_mask();
    test_find_bulk_touches_bucket_extra();
    test_flow6_ts_in_bucket();
    test_flow6_maintain_expires();
    test_flow6_find_touch_updates_bucket_extra();
    test_flow6_touch_updates_bucket_extra();
    test_flow6_touch_checked_rejects_deleted_idx();
    test_flow6_touch_after_migrate_uses_current_mask();
    test_flow6_find_bulk_touches_bucket_extra();
    test_flowu_ts_in_bucket();
    test_flowu_maintain_expires();
    test_flowu_find_touch_updates_bucket_extra();
    test_flowu_touch_updates_bucket_extra();
    test_flowu_touch_checked_rejects_deleted_idx();
    test_flowu_touch_after_migrate_uses_current_mask();
    test_flowu_find_bulk_touches_bucket_extra();
    test_compact_key_omits_vrfid();
    printf("PASS\n");
    return 0;
}
