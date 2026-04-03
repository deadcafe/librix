/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flow_table.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", (msg)); return 1; } while (0)
#define FAILF(fmt, ...) do { fprintf(stderr, "FAIL: " fmt "\n", __VA_ARGS__); return 1; } while (0)

static void *
test_bucket_alloc(size_t size, size_t align, void *arg __attribute__((unused)))
{
    return aligned_alloc(align, size);
}

static void
test_bucket_free(void *ptr, size_t size __attribute__((unused)),
                 size_t align __attribute__((unused)),
                 void *arg __attribute__((unused)))
{
    free(ptr);
}

static void *
test_aligned_calloc(size_t count, size_t size, size_t align)
{
    size_t total = count * size;
    void *ptr = aligned_alloc(align, total);

    if (ptr != NULL)
        memset(ptr, 0, total);
    return ptr;
}

static struct ft_table_config
test_cfg(unsigned start_nb_bk, unsigned max_nb_bk, unsigned grow_fill_pct)
{
    struct ft_table_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.start_nb_bk = start_nb_bk;
    cfg.max_nb_bk = max_nb_bk;
    cfg.grow_fill_pct = grow_fill_pct;
    cfg.bucket_alloc.alloc = test_bucket_alloc;
    cfg.bucket_alloc.free = test_bucket_free;
    cfg.bucket_alloc.arg = NULL;
    return cfg;
}

static struct flow4_key
test_key(unsigned i)
{
    struct flow4_key k;

    memset(&k, 0, sizeof(k));
    k.family   = 2u;
    k.proto    = (uint8_t)(6u + (i & 1u));
    k.src_port = (uint16_t)(1024u + (i & 0x7fffu));
    k.dst_port = (uint16_t)(2048u + ((i >> 11) & 0x7fffu));
    k.vrfid    = 1u + (i >> 24);
    k.src_ip   = UINT32_C(0x0a000000) | (i & 0x00ffffffu);
    k.dst_ip   = UINT32_C(0x14000000)
               | ((i * UINT32_C(2654435761)) & 0x00ffffffu);
    return k;
}

struct test_user_record {
    struct flow4_entry entry;
    uint32_t cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct test_user_record6 {
    struct flow6_entry entry;
    uint32_t cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct test_user_recordu {
    struct flowu_entry entry;
    uint32_t cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

union test_any_table {
    struct ft_flow4_table flow4;
    struct ft_flow6_table flow6;
    struct ft_flowu_table flowu;
};

union test_any_key {
    struct flow4_key flow4;
    struct flow6_key flow6;
    struct flowu_key flowu;
};

static struct flow6_key
test_key6(unsigned i)
{
    struct flow6_key k;
    uint32_t w;

    memset(&k, 0, sizeof(k));
    k.family   = 10u;
    k.proto    = 6u;
    k.src_port = (uint16_t)(1000u + i);
    k.dst_port = (uint16_t)(2000u + i);
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

static struct flowu_key
test_keyu(unsigned i)
{
    struct flowu_key k;
    uint32_t w;

    memset(&k, 0, sizeof(k));
    k.proto    = 6u;
    k.src_port = (uint16_t)(1000u + i);
    k.dst_port = (uint16_t)(2000u + i);
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

struct test_variant_ops {
    const char *name;
    size_t table_size;
    size_t key_size;
    size_t record_size;
    size_t entry_offset;
    size_t cookie_offset;
    unsigned default_min_nb_bk;
    int (*init)(void *ft, void *records, unsigned max_entries,
                const struct ft_table_config *cfg);
    void (*destroy)(void *ft);
    void (*flush)(void *ft);
    unsigned (*nb_entries)(const void *ft);
    unsigned (*nb_bk)(const void *ft);
    unsigned (*need_grow)(const void *ft);
    void (*stats)(const void *ft, struct ft_table_stats *out);
    uint32_t (*find)(void *ft, const void *key);
    uint32_t (*add_idx)(void *ft, uint32_t entry_idx);
    uint32_t (*del_key)(void *ft, const void *key);
    uint32_t (*del_entry_idx)(void *ft, uint32_t entry_idx);
    void (*find_bulk)(void *ft, const void *keys, unsigned nb_keys,
                      struct ft_table_result *results);
    void (*add_idx_bulk)(void *ft, const uint32_t *entry_idxv,
                         unsigned nb_keys,
                         struct ft_table_result *results);
    void (*del_entry_idx_bulk)(void *ft, const uint32_t *entry_idxv,
                               unsigned nb_keys);
    int (*walk)(void *ft, int (*cb)(uint32_t entry_idx, void *arg), void *arg);
    int (*grow_2x)(void *ft);
    int (*reserve)(void *ft, unsigned min_entries);
    void *(*record_ptr)(void *ft, uint32_t entry_idx);
    void *(*entry_ptr)(void *ft, uint32_t entry_idx);
    void (*make_key)(void *out, unsigned i);
};

#define TEST_VARIANT_WRAPPERS(tag, prefix, key_t, record_t, entry_t, init_ex_fn, \
                              destroy_fn, flush_fn, nb_entries_fn, nb_bk_fn,   \
                              need_grow_fn, stats_fn, find_fn, add_idx_fn,      \
                              del_key_fn, del_entry_idx_fn, find_bulk_fn,       \
                              add_idx_bulk_fn,                                 \
                              del_entry_idx_bulk_fn,                           \
                              walk_fn, grow_2x_fn,                              \
                              reserve_fn, record_ptr_fn, entry_ptr_fn,          \
                              make_key_fn, default_min_nb_bk_v)                 \
static int                                                                 \
testv_init_##tag(void *ft, void *records, unsigned max_entries,            \
                 const struct ft_table_config *cfg)                        \
{                                                                          \
    return init_ex_fn((struct ft_##prefix##_table *)ft, records,          \
                      max_entries, sizeof(record_t),                       \
                      offsetof(record_t, entry), cfg);                     \
}                                                                          \
static void                                                                 \
testv_destroy_##tag(void *ft)                                              \
{                                                                          \
    destroy_fn((struct ft_##prefix##_table *)ft);                          \
}                                                                          \
static void                                                                 \
testv_flush_##tag(void *ft)                                                \
{                                                                          \
    flush_fn((struct ft_##prefix##_table *)ft);                            \
}                                                                          \
static unsigned                                                             \
testv_nb_entries_##tag(const void *ft)                                      \
{                                                                          \
    return nb_entries_fn((const struct ft_##prefix##_table *)ft);          \
}                                                                          \
static unsigned                                                             \
testv_nb_bk_##tag(const void *ft)                                           \
{                                                                          \
    return nb_bk_fn((const struct ft_##prefix##_table *)ft);               \
}                                                                          \
static unsigned                                                             \
testv_need_grow_##tag(const void *ft)                                       \
{                                                                          \
    return need_grow_fn((const struct ft_##prefix##_table *)ft);           \
}                                                                          \
static void                                                                 \
testv_stats_##tag(const void *ft, struct ft_table_stats *out)               \
{                                                                          \
    stats_fn((const struct ft_##prefix##_table *)ft, out);                 \
}                                                                          \
static uint32_t                                                             \
testv_find_##tag(void *ft, const void *key)                                 \
{                                                                          \
    return find_fn((struct ft_##prefix##_table *)ft,                        \
                   (const key_t *)key);                                     \
}                                                                          \
static uint32_t                                                             \
testv_add_idx_##tag(void *ft, uint32_t entry_idx)                           \
{                                                                          \
    return add_idx_fn((struct ft_##prefix##_table *)ft, entry_idx);        \
}                                                                          \
static uint32_t                                                             \
testv_del_key_##tag(void *ft, const void *key)                              \
{                                                                          \
    return del_key_fn((struct ft_##prefix##_table *)ft,                     \
                      (const key_t *)key);                                  \
}                                                                          \
static uint32_t                                                             \
testv_del_entry_idx_##tag(void *ft, uint32_t entry_idx)                     \
{                                                                          \
    return del_entry_idx_fn((struct ft_##prefix##_table *)ft, entry_idx);  \
}                                                                          \
static void                                                                 \
testv_find_bulk_##tag(void *ft, const void *keys, unsigned nb_keys,         \
                      struct ft_table_result *results)                       \
{                                                                          \
    find_bulk_fn((struct ft_##prefix##_table *)ft,                          \
                 (const key_t *)keys, nb_keys, results);                    \
}                                                                          \
static void                                                                 \
testv_add_idx_bulk_##tag(void *ft, const uint32_t *entry_idxv,              \
                         unsigned nb_keys,                                   \
                         struct ft_table_result *results)                    \
{                                                                          \
    add_idx_bulk_fn((struct ft_##prefix##_table *)ft,                       \
                    entry_idxv, nb_keys, results);                          \
}                                                                          \
static void                                                                 \
testv_del_entry_idx_bulk_##tag(void *ft, const uint32_t *entry_idxv,        \
                               unsigned nb_keys)                             \
{                                                                          \
    del_entry_idx_bulk_fn((struct ft_##prefix##_table *)ft,                 \
                          entry_idxv, nb_keys);                             \
}                                                                          \
static int                                                                  \
testv_walk_##tag(void *ft, int (*cb)(uint32_t entry_idx, void *arg),        \
                 void *arg)                                                  \
{                                                                          \
    return walk_fn((struct ft_##prefix##_table *)ft, cb, arg);              \
}                                                                          \
static int                                                                  \
testv_grow_2x_##tag(void *ft)                                               \
{                                                                          \
    return grow_2x_fn((struct ft_##prefix##_table *)ft);                    \
}                                                                          \
static int                                                                  \
testv_reserve_##tag(void *ft, unsigned min_entries)                         \
{                                                                          \
    return reserve_fn((struct ft_##prefix##_table *)ft, min_entries);       \
}                                                                          \
static void *                                                               \
testv_record_ptr_##tag(void *ft, uint32_t entry_idx)                        \
{                                                                          \
    return record_ptr_fn((struct ft_##prefix##_table *)ft, entry_idx);      \
}                                                                          \
static void *                                                               \
testv_entry_ptr_##tag(void *ft, uint32_t entry_idx)                         \
{                                                                          \
    return entry_ptr_fn((struct ft_##prefix##_table *)ft, entry_idx);       \
}                                                                          \
static void                                                                 \
testv_make_key_##tag(void *out, unsigned i)                                  \
{                                                                          \
    *(key_t *)out = make_key_fn(i);                                         \
}                                                                          \
static const struct test_variant_ops test_ops_##tag = {                     \
    .name = #prefix,                                                        \
    .table_size = sizeof(struct ft_##prefix##_table),                       \
    .key_size = sizeof(key_t),                                              \
    .record_size = sizeof(record_t),                                        \
    .entry_offset = offsetof(record_t, entry),                              \
    .cookie_offset = offsetof(record_t, cookie),                            \
    .default_min_nb_bk = (default_min_nb_bk_v),                             \
    .init = testv_init_##tag,                                               \
    .destroy = testv_destroy_##tag,                                         \
    .flush = testv_flush_##tag,                                             \
    .nb_entries = testv_nb_entries_##tag,                                   \
    .nb_bk = testv_nb_bk_##tag,                                             \
    .need_grow = testv_need_grow_##tag,                                     \
    .stats = testv_stats_##tag,                                             \
    .find = testv_find_##tag,                                               \
    .add_idx = testv_add_idx_##tag,                                         \
    .del_key = testv_del_key_##tag,                                         \
    .del_entry_idx = testv_del_entry_idx_##tag,                             \
    .find_bulk = testv_find_bulk_##tag,                                     \
    .add_idx_bulk = testv_add_idx_bulk_##tag,                               \
    .del_entry_idx_bulk = testv_del_entry_idx_bulk_##tag,                   \
    .walk = testv_walk_##tag,                                               \
    .grow_2x = testv_grow_2x_##tag,                                         \
    .reserve = testv_reserve_##tag,                                         \
    .record_ptr = testv_record_ptr_##tag,                                   \
    .entry_ptr = testv_entry_ptr_##tag,                                     \
    .make_key = testv_make_key_##tag                                        \
}

TEST_VARIANT_WRAPPERS(flow6, flow6, struct flow6_key, struct test_user_record6,
                      struct flow6_entry, ft_flow6_table_init_ex,
                      ft_flow6_table_destroy, ft_flow6_table_flush,
                      ft_flow6_table_nb_entries, ft_flow6_table_nb_bk,
                      ft_flow6_table_need_grow, ft_flow6_table_stats,
                      ft_flow6_table_find, ft_flow6_table_add_entry_idx,
                      ft_flow6_table_del_key, ft_flow6_table_del_entry_idx,
                      ft_flow6_table_find_bulk, ft_flow6_table_add_idx_bulk,
                      ft_flow6_table_del_entry_idx_bulk, ft_flow6_table_walk,
                      ft_flow6_table_grow_2x, ft_flow6_table_reserve,
                      ft_flow6_table_record_ptr, ft_flow6_table_entry_ptr,
                      test_key6, FT_FLOW6_DEFAULT_MIN_NB_BK);

TEST_VARIANT_WRAPPERS(flowu, flowu, struct flowu_key, struct test_user_recordu,
                      struct flowu_entry, ft_flowu_table_init_ex,
                      ft_flowu_table_destroy, ft_flowu_table_flush,
                      ft_flowu_table_nb_entries, ft_flowu_table_nb_bk,
                      ft_flowu_table_need_grow, ft_flowu_table_stats,
                      ft_flowu_table_find, ft_flowu_table_add_entry_idx,
                      ft_flowu_table_del_key, ft_flowu_table_del_entry_idx,
                      ft_flowu_table_find_bulk, ft_flowu_table_add_idx_bulk,
                      ft_flowu_table_del_entry_idx_bulk, ft_flowu_table_walk,
                      ft_flowu_table_grow_2x, ft_flowu_table_reserve,
                      ft_flowu_table_record_ptr, ft_flowu_table_entry_ptr,
                      test_keyu, FT_FLOWU_DEFAULT_MIN_NB_BK);

#define TEST_KEY_AT(base, ops, i) \
    ((void *)((unsigned char *)(base) + (size_t)(i) * (ops)->key_size))

static uint32_t
test_add_idx_key(struct ft_flow4_table *ft, uint32_t entry_idx,
                 const struct flow4_key *key)
{
    struct flow4_entry *entry = ft_flow4_table_entry_ptr(ft, entry_idx);

    if (entry == NULL)
        return 0u;
    entry->key = *key;
    return ft_flow4_table_add_idx(ft, entry_idx);
}

struct walk_ctx {
    unsigned count;
    uint64_t sum;
    unsigned stop_after;
};

static int
walk_count_cb(uint32_t entry_idx, void *arg)
{
    struct walk_ctx *ctx = (struct walk_ctx *)arg;

    ctx->count++;
    ctx->sum += entry_idx;
    if (ctx->stop_after != 0u && ctx->count >= ctx->stop_after)
        return 1;
    return 0;
}

struct fail_bucket_alloc_ctx {
    unsigned call_count;
    unsigned fail_after;
};

static void *
fail_bucket_alloc(size_t size, size_t align, void *arg)
{
    struct fail_bucket_alloc_ctx *ctx = (struct fail_bucket_alloc_ctx *)arg;

    ctx->call_count++;
    if (ctx->call_count > ctx->fail_after)
        return NULL;
    return aligned_alloc(align, size);
}

static int
test_basic_add_find_del(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    struct flow4_key k1 = test_key(1u);
    struct flow4_key k2 = test_key(2u);
    uint32_t idx1, idx2;

    printf("[T] flow4 table basic add/find/del\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, &cfg) != 0)
        FAIL("ft_flow4_table_init failed");
    idx1 = test_add_idx_key(&ft, 1u, &k1);
    idx2 = test_add_idx_key(&ft, 2u, &k2);
    if (idx1 != 1u || idx2 != 2u || idx1 == idx2)
        FAIL("basic add returned invalid indices");
    if (ft_flow4_table_find(&ft, &k1) != idx1)
        FAIL("find should return idx1");
    if (ft_flow4_table_find(&ft, &k2) != idx2)
        FAIL("find should return idx2");
    if (test_add_idx_key(&ft, 1u, &k1) != idx1)
        FAIL("duplicate add should return existing idx");
    if (ft_flow4_table_nb_entries(&ft) != 2u)
        FAIL("duplicate add should not increase count");
    if (ft_flow4_table_del_key(&ft, &k1) != idx1)
        FAIL("del should return idx1");
    if (ft_flow4_table_find(&ft, &k1) != 0u)
        FAIL("deleted key should miss");
    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_init_ex_and_mapping(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *users;
    struct flow4_key key = test_key(10u);
    uint32_t idx;

    printf("[T] flow4 table init_ex mapping\n");
    users = test_aligned_calloc(64u, sizeof(*users), FT_TABLE_CACHE_LINE_SIZE);
    if (users == NULL)
        FAIL("calloc users");
    for (unsigned i = 0; i < 64u; i++)
        users[i].cookie = UINT32_C(0xabc00000) + i;
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, users, 64u,
                                  struct test_user_record, entry, &cfg) != 0)
        FAIL("FT_FLOW4_TABLE_INIT_TYPED failed");
    idx = test_add_idx_key(&ft, 1u, &key);
    if (idx != 1u)
        FAIL("init_ex add failed");
    if (FT_FLOW4_TABLE_RECORD_FROM_ENTRY(struct test_user_record, entry,
                                         ft_flow4_table_entry_ptr(&ft, idx))
        != &users[idx - 1u])
        FAIL("record_from_entry mismatch");
    if (FT_FLOW4_TABLE_ENTRY_FROM_RECORD(&users[idx - 1u], entry)
        != ft_flow4_table_entry_ptr(&ft, idx))
        FAIL("entry_from_record mismatch");
    if (users[idx - 1u].cookie != UINT32_C(0xabc00000) + (idx - 1u))
        FAIL("user record payload mismatch");
    ft_flow4_table_destroy(&ft);
    free(users);
    return 0;
}

static int
test_manual_grow_preserves_entries(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    uint32_t idxs[256];

    printf("[T] flow4 table grow_2x preserves entries\n");
    pool = test_aligned_calloc(4096u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 4096u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 256u; i++) {
        struct flow4_key key = test_key(i + 100u);

        idxs[i] = test_add_idx_key(&ft, i + 1u, &key);
        if (idxs[i] != i + 1u)
            FAILF("add before grow failed at %u", i);
    }
    if (ft_flow4_table_grow_2x(&ft) != 0)
        FAIL("grow_2x failed");
    for (unsigned i = 0; i < 256u; i++) {
        struct flow4_key key = test_key(i + 100u);

        if (ft_flow4_table_find(&ft, &key) != idxs[i])
            FAILF("find after grow mismatch at %u", i);
    }
    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_need_grow_and_reserve(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;

    printf("[T] flow4 table need_grow/reserve\n");
    pool = test_aligned_calloc(200000u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 200000u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");
    if (ft_flow4_table_nb_bk(&ft) != FT_FLOW4_DEFAULT_MIN_NB_BK)
        FAILF("unexpected initial nb_bk=%u", ft_flow4_table_nb_bk(&ft));
    if (ft_flow4_table_reserve(&ft, 160000u) != 0)
        FAIL("reserve failed");
    if (ft_flow4_table_nb_bk(&ft) != (FT_FLOW4_DEFAULT_MIN_NB_BK << 1))
        FAILF("reserve should grow to %u, got %u",
              FT_FLOW4_DEFAULT_MIN_NB_BK << 1, ft_flow4_table_nb_bk(&ft));

    ft_flow4_table_destroy(&ft);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 200000u, struct test_user_record, entry, &cfg) != 0)
        FAIL("reinit failed");
    for (unsigned i = 0; i < 160000u; i++) {
        struct flow4_key key = test_key(i + 1000u);

        if (test_add_idx_key(&ft, i + 1u, &key) == 0u)
            break;
    }
    if (ft_flow4_table_need_grow(&ft) == 0u)
        FAIL("need_grow should be set after threshold crossing");
    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_bulk_ops_and_stats(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    struct flow4_key keys[8];
    uint32_t entry_idxv[8];
    struct ft_table_result results[8];
    struct ft_table_stats stats;

    printf("[T] flow4 table bulk ops and stats\n");
    pool = test_aligned_calloc(256u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 256u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 2000u);
        pool[i].entry.key = keys[i];
        entry_idxv[i] = i + 1u;
    }

    ft_flow4_table_add_idx_bulk(&ft, entry_idxv, 8u, results);
    for (unsigned i = 0; i < 8u; i++) {
        if (results[i].entry_idx != entry_idxv[i])
            FAILF("add_bulk failed at %u", i);
    }

    memset(results, 0, sizeof(results));
    ft_flow4_table_find_bulk(&ft, keys, 8u, results);
    for (unsigned i = 0; i < 8u; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("find_bulk miss at %u", i);
    }

    ft_flow4_table_add_idx_bulk(&ft, entry_idxv, 8u, results);
    for (unsigned i = 0; i < 8u; i++) {
        if (results[i].entry_idx != entry_idxv[i])
            FAILF("duplicate add_bulk failed at %u", i);
    }

    for (unsigned i = 0; i < 4u; i++) {
        if (ft_flow4_table_del_key(&ft, &keys[i]) == 0u)
            FAILF("del_key failed at %u", i);
    }

    memset(results, 0, sizeof(results));
    ft_flow4_table_find_bulk(&ft, keys, 8u, results);
    for (unsigned i = 0; i < 4u; i++) {
        if (results[i].entry_idx != 0u)
            FAILF("deleted key should miss at %u", i);
    }
    for (unsigned i = 4u; i < 8u; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("remaining key should hit at %u", i);
    }

    ft_flow4_table_stats(&ft, &stats);
    if (stats.adds != 8u)
        FAILF("adds=%llu", (unsigned long long)stats.adds);
    if (stats.add_existing != 8u)
        FAILF("add_existing=%llu", (unsigned long long)stats.add_existing);
    if (stats.lookups != 16u)
        FAILF("lookups=%llu", (unsigned long long)stats.lookups);
    if (stats.hits != 12u)
        FAILF("hits=%llu", (unsigned long long)stats.hits);
    if (stats.misses != 4u)
        FAILF("misses=%llu", (unsigned long long)stats.misses);
    if (stats.dels != 4u)
        FAILF("dels=%llu", (unsigned long long)stats.dels);
    if (stats.del_miss != 0u)
        FAILF("del_miss=%llu", (unsigned long long)stats.del_miss);

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_walk_flush_and_stats_reset(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    struct walk_ctx ctx;
    struct ft_table_stats stats;

    printf("[T] flow4 table walk/flush/stats\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 16u; i++) {
        struct flow4_key key = test_key(i + 5000u);

        if (test_add_idx_key(&ft, i + 1u, &key) == 0u)
            FAILF("add failed at %u", i);
    }

    memset(&ctx, 0, sizeof(ctx));
    if (ft_flow4_table_walk(&ft, walk_count_cb, &ctx) != 0)
        FAIL("walk failed");
    if (ctx.count != 16u)
        FAILF("walk count=%u", ctx.count);

    ft_flow4_table_stats(&ft, &stats);
    if (stats.adds != 16u)
        FAILF("adds before flush=%llu", (unsigned long long)stats.adds);

    ft_flow4_table_flush(&ft);
    if (ft_flow4_table_nb_entries(&ft) != 0u)
        FAIL("flush should clear entries");
    if (ft_flow4_table_need_grow(&ft) != 0u)
        FAIL("flush should clear need_grow");

    memset(&ctx, 0, sizeof(ctx));
    if (ft_flow4_table_walk(&ft, walk_count_cb, &ctx) != 0)
        FAIL("walk after flush failed");
    if (ctx.count != 0u)
        FAILF("walk after flush count=%u", ctx.count);

    ft_flow4_table_stats(&ft, &stats);
    if (stats.adds != 16u)
        FAILF("stats should be preserved across flush, adds=%llu",
              (unsigned long long)stats.adds);

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_allocator_failure_and_max_bucket_limit(void)
{
    struct fail_bucket_alloc_ctx alloc_ctx = { 0u, 1u };
    struct ft_table_config cfg;
    struct ft_flow4_table ft;
    struct test_user_record *pool;

    printf("[T] flow4 table allocator failure/max bucket limit\n");
    cfg = test_cfg(FT_FLOW4_DEFAULT_MIN_NB_BK,
                   FT_FLOW4_DEFAULT_MIN_NB_BK, 60u);
    cfg.bucket_alloc.alloc = fail_bucket_alloc;
    cfg.bucket_alloc.free = test_bucket_free;
    cfg.bucket_alloc.arg = &alloc_ctx;

    pool = test_aligned_calloc(200000u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 200000u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");
    if (ft_flow4_table_reserve(&ft, 160000u) == 0)
        FAIL("reserve should fail when max_nb_bk is reached");
    if (ft_flow4_table_grow_2x(&ft) == 0)
        FAIL("grow_2x should fail at max_nb_bk");
    ft_flow4_table_destroy(&ft);

    alloc_ctx.call_count = 0u;
    alloc_ctx.fail_after = 0u;
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 200000u, struct test_user_record, entry, &cfg) == 0) {
        ft_flow4_table_destroy(&ft);
        FAIL("init should fail when allocator returns NULL");
    }

    free(pool);
    return 0;
}

static int
test_duplicate_and_delete_miss_stats(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    struct ft_table_stats stats;
    struct flow4_key key = test_key(6000u);
    struct flow4_key miss = test_key(6001u);
    uint32_t idx;

    printf("[T] flow4 table duplicate add/del miss stats\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    idx = test_add_idx_key(&ft, 1u, &key);
    if (idx != 1u)
        FAIL("initial add failed");
    if (test_add_idx_key(&ft, 1u, &key) != idx)
        FAIL("duplicate add should return existing idx");
    if (ft_flow4_table_del_key(&ft, &miss) != 0u)
        FAIL("delete miss should return 0");

    ft_flow4_table_stats(&ft, &stats);
    if (stats.adds != 1u)
        FAILF("adds=%llu", (unsigned long long)stats.adds);
    if (stats.add_existing != 1u)
        FAILF("add_existing=%llu", (unsigned long long)stats.add_existing);
    if (stats.del_miss != 1u)
        FAILF("del_miss=%llu", (unsigned long long)stats.del_miss);
    if (stats.add_failed != 0u)
        FAILF("add_failed=%llu", (unsigned long long)stats.add_failed);

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_walk_early_stop(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    struct walk_ctx ctx;

    printf("[T] flow4 table walk early stop\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 16u; i++) {
        struct flow4_key key = test_key(i + 7000u);

        if (test_add_idx_key(&ft, i + 1u, &key) == 0u)
            FAILF("add failed at %u", i);
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.stop_after = 5u;
    if (ft_flow4_table_walk(&ft, walk_count_cb, &ctx) != 1)
        FAIL("walk should stop early");
    if (ctx.count != 5u)
        FAILF("walk early-stop count=%u", ctx.count);

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_grow_failure_preserves_table(void)
{
    struct fail_bucket_alloc_ctx alloc_ctx = { 0u, 1u };
    struct ft_table_config cfg;
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    uint32_t idxs[64];
    unsigned old_nb_bk;

    printf("[T] flow4 table grow failure preserves table\n");
    cfg = test_cfg(FT_FLOW4_DEFAULT_MIN_NB_BK,
                   FT_FLOW4_DEFAULT_MIN_NB_BK << 1, 60u);
    cfg.bucket_alloc.alloc = fail_bucket_alloc;
    cfg.bucket_alloc.free = test_bucket_free;
    cfg.bucket_alloc.arg = &alloc_ctx;

    pool = test_aligned_calloc(2048u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 2048u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 64u; i++) {
        struct flow4_key key = test_key(i + 8000u);

        idxs[i] = test_add_idx_key(&ft, i + 1u, &key);
        if (idxs[i] != i + 1u)
            FAILF("add failed at %u", i);
    }

    old_nb_bk = ft_flow4_table_nb_bk(&ft);
    if (ft_flow4_table_grow_2x(&ft) == 0)
        FAIL("grow_2x should fail");
    if (ft_flow4_table_nb_bk(&ft) != old_nb_bk)
        FAIL("nb_bk changed after failed grow");

    for (unsigned i = 0; i < 64u; i++) {
        struct flow4_key key = test_key(i + 8000u);

        if (ft_flow4_table_find(&ft, &key) != idxs[i])
            FAILF("find after failed grow mismatch at %u", i);
    }

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_config_rounding_and_clamp(void)
{
    struct ft_table_config cfg = test_cfg(9000u, 9000u, 0u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;

    printf("[T] flow4 table config rounding/clamp\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    if (ft_flow4_table_nb_bk(&ft) != FT_FLOW4_DEFAULT_MIN_NB_BK)
        FAILF("rounded/clamped nb_bk=%u", ft_flow4_table_nb_bk(&ft));
    if (ft_flow4_table_grow_2x(&ft) == 0)
        FAIL("grow_2x should fail because max_nb_bk clamps to start");

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

/*
 * High fill: push to ~94% of bucket capacity.
 * 64 buckets × 16 slots = 1024 capacity; insert 960.
 * Verifies all entries findable, then removes every other entry
 * and verifies remaining entries still findable.
 */
static int
test_high_fill(void)
{
    const unsigned nb_bk = 64u;
    const unsigned capacity = nb_bk * 16u; /* 1024 */
    const unsigned target = (capacity * 15u) / 16u; /* 960 */
    struct ft_table_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    unsigned inserted = 0u;

    printf("[T] flow4 table high fill (94%%)\n");
    pool = test_aligned_calloc(capacity + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, capacity, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < target; i++) {
        struct flow4_key key = test_key(i + 10000u);
        uint32_t idx = test_add_idx_key(&ft, i + 1u, &key);

        if (idx == 0u)
            break;
        inserted++;
    }
    printf("  inserted %u / %u (%.1f%%)\n",
           inserted, capacity,
           (double)inserted * 100.0 / (double)capacity);
    if (inserted < target)
        FAILF("could not reach target fill: %u < %u", inserted, target);

    /* verify all findable */
    for (unsigned i = 0; i < inserted; i++) {
        struct flow4_key key = test_key(i + 10000u);

        if (ft_flow4_table_find(&ft, &key) != i + 1u)
            FAILF("find failed at high fill i=%u", i);
    }

    /* remove every other entry, verify remaining */
    for (unsigned i = 0; i < inserted; i += 2u) {
        struct flow4_key key = test_key(i + 10000u);

        if (ft_flow4_table_del_key(&ft, &key) != i + 1u)
            FAILF("del at high fill failed i=%u", i);
    }
    for (unsigned i = 1u; i < inserted; i += 2u) {
        struct flow4_key key = test_key(i + 10000u);

        if (ft_flow4_table_find(&ft, &key) != i + 1u)
            FAILF("find after partial del failed i=%u", i);
    }
    /* deleted keys must miss */
    for (unsigned i = 0; i < inserted; i += 2u) {
        struct flow4_key key = test_key(i + 10000u);

        if (ft_flow4_table_find(&ft, &key) != 0u)
            FAILF("deleted key still found i=%u", i);
    }

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

/*
 * Max fill: insert until first kickout failure.
 * 128 buckets × 16 = 2048 capacity.
 * Verifies all inserted entries are findable at max fill.
 */
static int
test_max_fill(void)
{
    const unsigned nb_bk = 128u;
    const unsigned capacity = nb_bk * 16u; /* 2048 */
    struct ft_table_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    unsigned inserted = 0u;

    printf("[T] flow4 table max fill\n");
    pool = test_aligned_calloc(capacity + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, capacity, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < capacity; i++) {
        struct flow4_key key = test_key(i + 20000u);
        uint32_t idx = test_add_idx_key(&ft, i + 1u, &key);

        if (idx == 0u)
            break;
        inserted++;
    }
    printf("  inserted %u / %u (%.1f%%)\n",
           inserted, capacity,
           (double)inserted * 100.0 / (double)capacity);
    if (inserted == 0u)
        FAIL("could not insert any entries");

    /* all inserted entries must be findable */
    for (unsigned i = 0; i < inserted; i++) {
        struct flow4_key key = test_key(i + 20000u);

        if (ft_flow4_table_find(&ft, &key) != i + 1u)
            FAILF("find at max fill failed i=%u", i);
    }

    /* walk count must match */
    {
        struct walk_ctx ctx;

        memset(&ctx, 0, sizeof(ctx));
        if (ft_flow4_table_walk(&ft, walk_count_cb, &ctx) != 0)
            FAIL("walk at max fill failed");
        if (ctx.count != inserted)
            FAILF("walk count=%u expected=%u", ctx.count, inserted);
    }

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

/*
 * Kickout safety: verify no previously-inserted entries become
 * unreachable after failed inserts (kickout exhaustion).
 *
 * Phase 1: Fill greedily until first rejection.
 * Phase 2: Continue inserting beyond first failure.
 * Check: all Phase-1 entries still findable.
 */
static int
test_kickout_safety(void)
{
    const unsigned nb_bk = 32u;
    const unsigned capacity = nb_bk * 16u; /* 512 */
    const unsigned overflow = 64u;
    struct ft_table_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    unsigned phase1_count = 0u;
    unsigned lost = 0u;

    printf("[T] flow4 table kickout safety\n");
    pool = test_aligned_calloc(capacity + overflow + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, capacity + overflow,
                                  struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    /* Phase 1: fill until first rejection */
    for (unsigned i = 0; i < capacity; i++) {
        struct flow4_key key = test_key(i + 30000u);
        uint32_t idx = test_add_idx_key(&ft, i + 1u, &key);

        if (idx == 0u)
            break;
        phase1_count++;
    }
    if (phase1_count == 0u)
        FAIL("phase1 inserted nothing");

    /* Phase 2: attempt more inserts (some will fail via kickout exhaustion) */
    for (unsigned i = 0; i < overflow; i++) {
        struct flow4_key key = test_key(capacity + i + 30000u);

        test_add_idx_key(&ft, phase1_count + i + 1u, &key);
    }

    /* Check: all phase-1 entries must still be findable */
    for (unsigned i = 0; i < phase1_count; i++) {
        struct flow4_key key = test_key(i + 30000u);

        if (ft_flow4_table_find(&ft, &key) != i + 1u)
            lost++;
    }
    printf("  phase1=%u lost=%u\n", phase1_count, lost);
    if (lost != 0u)
        FAILF("kickout caused %u victim(s) to become unreachable", lost);

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

/*
 * del_idx: delete by entry index rather than by key.
 * Verifies del_idx returns the correct index and find misses afterward.
 */
static int
test_del_idx(void)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    struct flow4_key keys[8];
    uint32_t idxs[8];

    printf("[T] flow4 table del_idx\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 40000u);
        idxs[i] = test_add_idx_key(&ft, i + 1u, &keys[i]);
        if (idxs[i] != i + 1u)
            FAILF("add failed at %u", i);
    }

    /* del_idx for even entries */
    for (unsigned i = 0; i < 8u; i += 2u) {
        if (ft_flow4_table_del_entry_idx(&ft, idxs[i]) != idxs[i])
            FAILF("del_idx failed at idx=%u", idxs[i]);
    }
    /* deleted entries miss, remaining entries hit */
    for (unsigned i = 0; i < 8u; i++) {
        uint32_t found = ft_flow4_table_find(&ft, &keys[i]);

        if ((i & 1u) == 0u) {
            if (found != 0u)
                FAILF("del_idx entry still found i=%u", i);
        } else {
            if (found != idxs[i])
                FAILF("remaining entry not found i=%u", i);
        }
    }
    /* double del_idx should return 0 */
    if (ft_flow4_table_del_entry_idx(&ft, idxs[0]) != 0u)
        FAIL("double del_idx should return 0");

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

/*
 * Fuzz: random insert/find/delete with internal model validation.
 * Detects correctness issues under unpredictable access patterns.
 */
static int
test_fuzz(unsigned seed, unsigned n, unsigned nb_bk, unsigned ops)
{
    struct ft_table_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct test_user_record *pool;
    uint8_t *in_table; /* 1 if entry i is in table */
    unsigned in_count = 0u;

    printf("[T] flow4 table fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           seed, n, nb_bk, ops);
    pool = test_aligned_calloc(n + 1u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    in_table = calloc(n, 1u);
    if (pool == NULL || in_table == NULL)
        FAIL("alloc failed");
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, n, struct test_user_record, entry, &cfg) != 0)
        FAIL("init failed");

    for (unsigned op = 0; op < ops; op++) {
        seed = seed * 1103515245u + 12345u;
        unsigned idx0 = (seed >> 16) % n;
        unsigned action = (seed >> 8) & 3u;
        struct flow4_key key = test_key(idx0 + 50000u);
        uint32_t entry_idx = idx0 + 1u;

        switch (action) {
        case 0: /* insert */
            if (!in_table[idx0]) {
                uint32_t ret = test_add_idx_key(&ft, entry_idx, &key);

                if (ret == entry_idx) {
                    in_table[idx0] = 1u;
                    in_count++;
                }
                /* ret == 0 means table full, acceptable */
            }
            break;
        case 1: /* find */
        {
            uint32_t ret = ft_flow4_table_find(&ft, &key);

            if (in_table[idx0] && ret != entry_idx)
                FAILF("fuzz find miss: op=%u idx0=%u", op, idx0);
            if (!in_table[idx0] && ret != 0u)
                FAILF("fuzz find ghost: op=%u idx0=%u", op, idx0);
            break;
        }
        case 2: /* delete */
            if (in_table[idx0]) {
                uint32_t ret = ft_flow4_table_del_key(&ft, &key);

                if (ret != entry_idx)
                    FAILF("fuzz del mismatch: op=%u idx0=%u ret=%u",
                           op, idx0, ret);
                in_table[idx0] = 0u;
                in_count--;
            }
            break;
        default: /* find (bias toward reads) */
        {
            uint32_t ret = ft_flow4_table_find(&ft, &key);

            if (in_table[idx0] && ret != entry_idx)
                FAILF("fuzz find2 miss: op=%u idx0=%u", op, idx0);
            break;
        }
        }
    }

    /* final walk count must match model */
    {
        struct walk_ctx ctx;

        memset(&ctx, 0, sizeof(ctx));
        ft_flow4_table_walk(&ft, walk_count_cb, &ctx);
        if (ctx.count != in_count)
            FAILF("fuzz walk count=%u model=%u", ctx.count, in_count);
    }

    /* verify all model entries findable */
    for (unsigned i = 0; i < n; i++) {
        if (in_table[i]) {
            struct flow4_key key = test_key(i + 50000u);

            if (ft_flow4_table_find(&ft, &key) != i + 1u)
                FAILF("fuzz final find miss i=%u", i);
        }
    }

    ft_flow4_table_destroy(&ft);
    free(pool);
    free(in_table);
    return 0;
}

static void
testv_bind_key(const struct test_variant_ops *ops, void *ft,
               uint32_t entry_idx, const void *key)
{
    void *entry = ops->entry_ptr(ft, entry_idx);

    if (entry == NULL) {
        fprintf(stderr, "FAIL: %s bind_key failed at idx=%u\n",
                ops->name, entry_idx);
        exit(1);
    }
    memcpy(entry, key, ops->key_size);
}

static uint32_t
testv_add_idx_key(const struct test_variant_ops *ops, void *ft,
                  uint32_t entry_idx, const void *key)
{
    testv_bind_key(ops, ft, entry_idx, key);
    return ops->add_idx(ft, entry_idx);
}

static int
testv_basic_add_find_del(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    union test_any_table ft;
    void *pool = test_aligned_calloc(128u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key k1, k2;
    uint32_t idx1, idx2;

    printf("[T] %s table basic add/find/del\n", ops->name);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ops->init(&ft, pool, 128u, &cfg) != 0)
        FAIL("variant init failed");
    ops->make_key(&k1, 1u);
    ops->make_key(&k2, 2u);
    idx1 = testv_add_idx_key(ops, &ft, 1u, &k1);
    idx2 = testv_add_idx_key(ops, &ft, 2u, &k2);
    if (idx1 != 1u || idx2 != 2u || idx1 == idx2)
        FAIL("basic add returned invalid indices");
    if (ops->find(&ft, &k1) != idx1)
        FAIL("find should return idx1");
    if (ops->find(&ft, &k2) != idx2)
        FAIL("find should return idx2");
    if (testv_add_idx_key(ops, &ft, 1u, &k1) != idx1)
        FAIL("duplicate add should return existing idx");
    if (ops->nb_entries(&ft) != 2u)
        FAIL("duplicate add should not increase count");
    if (ops->del_key(&ft, &k1) != idx1)
        FAIL("del should return idx1");
    if (ops->find(&ft, &k1) != 0u)
        FAIL("deleted key should miss");
    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_init_ex_and_mapping(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    union test_any_table ft;
    void *users = test_aligned_calloc(64u, ops->record_size,
                                      FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key;
    uint32_t idx;

    printf("[T] %s table init_ex mapping\n", ops->name);
    if (users == NULL)
        FAIL("calloc users");
    for (unsigned i = 0; i < 64u; i++) {
        uint32_t cookie = UINT32_C(0xabc00000) + i;
        memcpy((unsigned char *)users
                   + (size_t)i * ops->record_size + ops->cookie_offset,
               &cookie, sizeof(cookie));
    }
    if (ops->init(&ft, users, 64u, &cfg) != 0)
        FAIL("variant init_ex failed");
    ops->make_key(&key, 10u);
    idx = testv_add_idx_key(ops, &ft, 1u, &key);
    if (idx != 1u)
        FAIL("init_ex add failed");
    if (ops->record_ptr(&ft, idx) != users)
        FAIL("record_ptr mismatch");
    if (ops->entry_ptr(&ft, idx)
        != (void *)((unsigned char *)users + ops->entry_offset))
        FAIL("entry_ptr mismatch");
    {
        uint32_t cookie = 0u;

        memcpy(&cookie, (unsigned char *)users + ops->cookie_offset,
               sizeof(cookie));
        if (cookie != UINT32_C(0xabc00000))
            FAIL("user record payload mismatch");
    }
    ops->destroy(&ft);
    free(users);
    return 0;
}

static int
testv_bulk_ops_and_stats(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    union test_any_table ft;
    void *pool = test_aligned_calloc(256u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(8u, ops->key_size);
    uint32_t entry_idxv[8];
    struct ft_table_result results[8];
    struct ft_table_stats stats;

    printf("[T] %s table bulk ops and stats\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, 256u, &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 8u; i++) {
        ops->make_key(TEST_KEY_AT(keys, ops, i), i + 2000u);
        testv_bind_key(ops, &ft, i + 1u, TEST_KEY_AT(keys, ops, i));
        entry_idxv[i] = i + 1u;
    }

    ops->add_idx_bulk(&ft, entry_idxv, 8u, results);
    for (unsigned i = 0; i < 8u; i++) {
        if (results[i].entry_idx != entry_idxv[i])
            FAILF("add_bulk failed at %u", i);
    }

    memset(results, 0, sizeof(results));
    ops->find_bulk(&ft, keys, 8u, results);
    for (unsigned i = 0; i < 8u; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("find_bulk miss at %u", i);
    }

    ops->add_idx_bulk(&ft, entry_idxv, 8u, results);
    for (unsigned i = 0; i < 8u; i++) {
        if (results[i].entry_idx != entry_idxv[i])
            FAILF("duplicate add_bulk failed at %u", i);
    }

    for (unsigned i = 0; i < 4u; i++) {
        if (ops->del_key(&ft, TEST_KEY_AT(keys, ops, i)) == 0u)
            FAILF("del_key failed at %u", i);
    }

    memset(results, 0, sizeof(results));
    ops->find_bulk(&ft, keys, 8u, results);
    for (unsigned i = 0; i < 4u; i++) {
        if (results[i].entry_idx != 0u)
            FAILF("deleted key should miss at %u", i);
    }
    for (unsigned i = 4u; i < 8u; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("remaining key should hit at %u", i);
    }

    ops->stats(&ft, &stats);
    if (stats.adds != 8u || stats.add_existing != 8u || stats.lookups != 16u
        || stats.hits != 12u || stats.misses != 4u || stats.dels != 4u
        || stats.del_miss != 0u)
        FAIL("bulk stats mismatch");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_manual_grow_preserves_entries(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    union test_any_table ft;
    void *pool = test_aligned_calloc(4096u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key;
    uint32_t idxs[256];

    printf("[T] %s table grow_2x preserves entries\n", ops->name);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ops->init(&ft, pool, 4096u, &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 256u; i++) {
        ops->make_key(&key, i + 100u);
        idxs[i] = testv_add_idx_key(ops, &ft, i + 1u, &key);
        if (idxs[i] != i + 1u)
            FAILF("add before grow failed at %u", i);
    }
    if (ops->grow_2x(&ft) != 0)
        FAIL("grow_2x failed");
    for (unsigned i = 0; i < 256u; i++) {
        ops->make_key(&key, i + 100u);
        if (ops->find(&ft, &key) != idxs[i])
            FAILF("find after grow mismatch at %u", i);
    }
    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_need_grow_and_reserve(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    union test_any_table ft;
    void *pool = test_aligned_calloc(200000u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key;

    printf("[T] %s table need_grow/reserve\n", ops->name);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ops->init(&ft, pool, 200000u, &cfg) != 0)
        FAIL("init failed");
    if (ops->nb_bk(&ft) != ops->default_min_nb_bk)
        FAIL("unexpected initial nb_bk");
    if (ops->reserve(&ft, 160000u) != 0)
        FAIL("reserve failed");
    if (ops->nb_bk(&ft) != (ops->default_min_nb_bk << 1))
        FAIL("reserve should double nb_bk");

    ops->destroy(&ft);
    if (ops->init(&ft, pool, 200000u, &cfg) != 0)
        FAIL("reinit failed");
    for (unsigned i = 0; i < 160000u; i++) {
        ops->make_key(&key, i + 1000u);
        if (testv_add_idx_key(ops, &ft, i + 1u, &key) == 0u)
            break;
    }
    if (ops->need_grow(&ft) == 0u)
        FAIL("need_grow should be set");
    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_walk_flush_and_del_idx(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg(0u, 0u, 60u);
    union test_any_table ft;
    void *pool = test_aligned_calloc(128u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    struct walk_ctx ctx;
    union test_any_key key;

    printf("[T] %s table walk/flush/del_idx\n", ops->name);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ops->init(&ft, pool, 128u, &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 16u; i++) {
        ops->make_key(&key, i + 5000u);
        if (testv_add_idx_key(ops, &ft, i + 1u, &key) == 0u)
            FAILF("add failed at %u", i);
    }
    memset(&ctx, 0, sizeof(ctx));
    if (ops->walk(&ft, walk_count_cb, &ctx) != 0 || ctx.count != 16u)
        FAIL("walk count mismatch");
    if (ops->del_entry_idx(&ft, 2u) != 2u)
        FAIL("del_idx failed");
    ops->make_key(&key, 5001u);
    if (ops->find(&ft, &key) != 0u)
        FAIL("del_idx entry still found");
    ops->flush(&ft);
    if (ops->nb_entries(&ft) != 0u)
        FAIL("flush should clear entries");
    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_fuzz(const struct test_variant_ops *ops,
           unsigned seed, unsigned n, unsigned nb_bk, unsigned ops_n)
{
    struct ft_table_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    union test_any_table ft;
    void *pool = test_aligned_calloc(n + 1u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    uint8_t *in_table = calloc(n, 1u);
    unsigned in_count = 0u;
    union test_any_key key;

    printf("[T] %s table fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           ops->name, seed, n, nb_bk, ops_n);
    if (pool == NULL || in_table == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, n, &cfg) != 0)
        FAIL("init failed");

    for (unsigned op = 0; op < ops_n; op++) {
        uint32_t ret;
        seed = seed * 1103515245u + 12345u;
        unsigned idx0 = (seed >> 16) % n;
        unsigned action = (seed >> 8) & 3u;
        uint32_t entry_idx = idx0 + 1u;

        ops->make_key(&key, idx0 + 50000u);
        switch (action) {
        case 0:
            if (!in_table[idx0]) {
                ret = testv_add_idx_key(ops, &ft, entry_idx, &key);
                if (ret == entry_idx) {
                    in_table[idx0] = 1u;
                    in_count++;
                }
            }
            break;
        case 1:
        case 3:
            ret = ops->find(&ft, &key);
            if (in_table[idx0] && ret != entry_idx)
                FAIL("fuzz find miss");
            if (!in_table[idx0] && action == 1u && ret != 0u)
                FAIL("fuzz find ghost");
            break;
        default:
            if (in_table[idx0]) {
                ret = ops->del_key(&ft, &key);
                if (ret != entry_idx)
                    FAIL("fuzz del mismatch");
                in_table[idx0] = 0u;
                in_count--;
            }
            break;
        }
    }
    {
        struct walk_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ops->walk(&ft, walk_count_cb, &ctx);
        if (ctx.count != in_count)
            FAIL("fuzz walk count mismatch");
    }
    for (unsigned i = 0; i < n; i++) {
        if (in_table[i]) {
            ops->make_key(&key, i + 50000u);
            if (ops->find(&ft, &key) != i + 1u)
                FAIL("fuzz final find miss");
        }
    }
    ops->destroy(&ft);
    free(in_table);
    free(pool);
    return 0;
}

static int
test_variant_suite(const struct test_variant_ops *ops)
{
    if (testv_basic_add_find_del(ops) != 0)
        return 1;
    if (testv_init_ex_and_mapping(ops) != 0)
        return 1;
    if (testv_bulk_ops_and_stats(ops) != 0)
        return 1;
    if (testv_manual_grow_preserves_entries(ops) != 0)
        return 1;
    if (testv_need_grow_and_reserve(ops) != 0)
        return 1;
    if (testv_walk_flush_and_del_idx(ops) != 0)
        return 1;
    if (testv_fuzz(ops, 3237998097u, 512u, 64u, 200000u) != 0)
        return 1;
    return 0;
}

int
main(void)
{
    if (test_basic_add_find_del() != 0)
        return 1;
    if (test_duplicate_and_delete_miss_stats() != 0)
        return 1;
    if (test_bulk_ops_and_stats() != 0)
        return 1;
    if (test_init_ex_and_mapping() != 0)
        return 1;
    if (test_walk_early_stop() != 0)
        return 1;
    if (test_manual_grow_preserves_entries() != 0)
        return 1;
    if (test_grow_failure_preserves_table() != 0)
        return 1;
    if (test_need_grow_and_reserve() != 0)
        return 1;
    if (test_walk_flush_and_stats_reset() != 0)
        return 1;
    if (test_allocator_failure_and_max_bucket_limit() != 0)
        return 1;
    if (test_config_rounding_and_clamp() != 0)
        return 1;
    if (test_high_fill() != 0)
        return 1;
    if (test_max_fill() != 0)
        return 1;
    if (test_kickout_safety() != 0)
        return 1;
    if (test_del_idx() != 0)
        return 1;
    if (test_fuzz(3237998097u, 512u, 64u, 200000u) != 0)
        return 1;
    if (test_fuzz(3237998097u, 1000u, 64u, 500000u) != 0)
        return 1;
    if (test_variant_suite(&test_ops_flow6) != 0)
        return 1;
    if (test_variant_suite(&test_ops_flowu) != 0)
        return 1;
    printf("ALL FLOW TABLE TESTS PASSED\n");
    return 0;
}
