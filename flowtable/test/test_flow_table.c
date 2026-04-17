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
#include "ft_test_record_allocator.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", (msg)); return 1; } while (0)
#define FAILF(fmt, ...) do { fprintf(stderr, "FAIL: " fmt "\n", __VA_ARGS__); return 1; } while (0)

#define TEST_NOW_ADD   UINT64_C(0x101)
#define TEST_NOW_FIND  UINT64_C(0x202)
#define TEST_NOW_DUP   UINT64_C(0x303)

enum {
    TEST_FORCE_EXPIRE_BUCKET_ENTRIES = 62000u,
    TEST_FORCE_EXPIRE_PHASE1_ENTRIES = 70000u,
};

static void *
test_aligned_calloc(size_t count, size_t size, size_t align)
{
    size_t total = count * size;
    void *ptr = aligned_alloc(align, total);

    if (ptr != NULL)
        memset(ptr, 0, total);
    return ptr;
}

static void *
test_alloc_buckets(unsigned max_entries)
{
    size_t sz = ft_table_bucket_size(max_entries);
    return aligned_alloc(FT_TABLE_BUCKET_ALIGN, sz);
}

static struct ft_table_config
test_cfg(void)
{
    struct ft_table_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    return cfg;
}

static struct flow4_key
test_key(unsigned i)
{
    struct flow4_key k;

    memset(&k, 0, sizeof(k));
    k.family   = 2u;
    k.proto    = (u8)(6u + (i & 1u));
    k.src_port = (u16)(1024u + (i & 0x7fffu));
    k.dst_port = (u16)(2048u + ((i >> 11) & 0x7fffu));
    k.vrfid    = 1u + (i >> 24);
    k.src_ip   = UINT32_C(0x0a000000) | (i & 0x00ffffffu);
    k.dst_ip   = UINT32_C(0x14000000)
               | ((i * UINT32_C(2654435761)) & 0x00ffffffu);
    return k;
}

struct test_user_record {
    struct flow4_entry entry;
    u32 cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct test_user_record6 {
    struct flow6_entry entry;
    u32 cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct test_user_recordu {
    struct flowu_entry entry;
    u32 cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct test_alloc_record {
    struct flow4_entry entry;
    u32 cookie;
    RIX_SLIST_ENTRY(test_alloc_record) free_link;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));


union test_any_table {
    struct ft_table flow4;
    struct ft_table flow6;
    struct ft_table flowu;
};

union test_any_key {
    struct flow4_key flow4;
    struct flow6_key flow6;
    struct flowu_key flowu;
};

static int
test_record_allocator_basic(void)
{
    struct ft_record_allocator alloc;
    struct test_alloc_record *pool;
    u32 idx;

    printf("[T] record allocator basic\n");
    pool = test_aligned_calloc(8u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc allocator pool");
    if (FT_RECORD_ALLOCATOR_INIT_TYPED(&alloc, pool, 8u,
                                       struct test_alloc_record,
                                       free_link) != 0)
        FAIL("allocator init failed");
    if (RIX_SLIST_FIRST(&alloc.free_head, pool) != &pool[0]
        || alloc.free_count != 8u)
        FAIL("allocator init state mismatch");
    if (FT_RECORD_ALLOCATOR_RECORD_PTR_AS(&alloc, struct test_alloc_record, 3u)
        != &pool[2])
        FAIL("allocator record_ptr mismatch");

    idx = FT_RECORD_ALLOCATOR_ALLOC_IDX_TYPED(&alloc, struct test_alloc_record,
                                              free_link);
    if (idx != 1u || RIX_SLIST_FIRST(&alloc.free_head, pool) != &pool[1]
        || alloc.free_count != 7u)
        FAIL("allocator first alloc mismatch");
    idx = FT_RECORD_ALLOCATOR_ALLOC_IDX_TYPED(&alloc, struct test_alloc_record,
                                              free_link);
    if (idx != 2u || RIX_SLIST_FIRST(&alloc.free_head, pool) != &pool[2]
        || alloc.free_count != 6u)
        FAIL("allocator second alloc mismatch");
    if (FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(&alloc, struct test_alloc_record,
                                           free_link, 2u) != 0)
        FAIL("allocator free failed");
    if (RIX_SLIST_FIRST(&alloc.free_head, pool) != &pool[1]
        || alloc.free_count != 7u)
        FAIL("allocator free state mismatch");
    idx = FT_RECORD_ALLOCATOR_ALLOC_IDX_TYPED(&alloc, struct test_alloc_record,
                                              free_link);
    if (idx != 2u || alloc.free_count != 6u)
        FAIL("allocator reuse mismatch");
    if (FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(&alloc, struct test_alloc_record,
                                           free_link, 2u) != 0)
        FAIL("allocator re-free failed");
    if (FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(&alloc, struct test_alloc_record,
                                           free_link, 2u) == 0)
        FAIL("allocator double free accepted");
    if (FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(&alloc, struct test_alloc_record,
                                           free_link, 0u) == 0)
        FAIL("allocator nil free accepted");
    free(pool);
    return 0;
}

static int
test_record_allocator_bulk_and_reset(void)
{
    struct ft_record_allocator alloc;
    struct test_alloc_record *pool;
    u32 idxv[8];
    u32 freev[2];
    unsigned n;

    printf("[T] record allocator bulk/reset\n");
    pool = test_aligned_calloc(6u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc allocator pool");
    if (FT_RECORD_ALLOCATOR_INIT_TYPED(&alloc, pool, 6u,
                                       struct test_alloc_record,
                                       free_link) != 0)
        FAIL("allocator init failed");

    n = FT_RECORD_ALLOCATOR_ALLOC_BULK_TYPED(&alloc, struct test_alloc_record,
                                             free_link, idxv, 4u);
    if (n != 4u)
        FAIL("allocator alloc_bulk count mismatch");
    if (idxv[0] != 1u || idxv[1] != 2u || idxv[2] != 3u || idxv[3] != 4u)
        FAIL("allocator alloc_bulk order mismatch");
    freev[0] = idxv[1];
    freev[1] = idxv[2];
    n = FT_RECORD_ALLOCATOR_FREE_BULK_TYPED(&alloc, struct test_alloc_record,
                                            free_link, freev, 2u);
    if (n != 2u)
        FAIL("allocator free_bulk count mismatch");
    if (RIX_SLIST_FIRST(&alloc.free_head, pool) != &pool[2]
        || alloc.free_count != 4u)
        FAIL("allocator free_bulk state mismatch");
    n = FT_RECORD_ALLOCATOR_ALLOC_BULK_TYPED(&alloc, struct test_alloc_record,
                                             free_link, idxv, 4u);
    if (n != 4u)
        FAIL("allocator second alloc_bulk count mismatch");
    if (idxv[0] != 3u || idxv[1] != 2u || idxv[2] != 5u || idxv[3] != 6u)
        FAIL("allocator second alloc_bulk order mismatch");
    if (FT_RECORD_ALLOCATOR_RESET_TYPED(&alloc, struct test_alloc_record,
                                        free_link) != 0)
        FAIL("allocator reset failed");
    n = FT_RECORD_ALLOCATOR_ALLOC_BULK_TYPED(&alloc, struct test_alloc_record,
                                             free_link, idxv, 6u);
    if (n != 6u)
        FAIL("allocator reset alloc_bulk count mismatch");
    for (unsigned i = 0u; i < 6u; i++) {
        if (idxv[i] != i + 1u)
            FAIL("allocator reset order mismatch");
    }
    free(pool);
    return 0;
}

static struct flow6_key
test_key6(unsigned i)
{
    struct flow6_key k;
    u32 w;

    memset(&k, 0, sizeof(k));
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

static struct flowu_key
test_keyu(unsigned i)
{
    struct flowu_key k;
    u32 w;

    memset(&k, 0, sizeof(k));
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

struct test_variant_ops {
    const char *name;
    size_t table_size;
    size_t key_size;
    size_t record_size;
    size_t entry_offset;
    size_t meta_offset;
    size_t cookie_offset;
    int (*init)(void *ft, void *records, unsigned max_entries,
                const struct ft_table_config *cfg);
    int (*init_with_bucket_entries)(void *ft, void *records,
                                    unsigned max_entries,
                                    unsigned bucket_entries,
                                    const struct ft_table_config *cfg);
    void (*destroy)(void *ft);
    void (*flush)(void *ft);
    unsigned (*nb_entries)(const void *ft);
    unsigned (*nb_bk)(const void *ft);
    void (*stats)(const void *ft, struct ft_table_stats *out);
    void (*status)(const void *ft, struct flow_status *out);
    u32 (*find)(void *ft, const void *key, u64 now);
    u32 (*add_idx)(void *ft, u32 entry_idx, u64 now);
    u32 (*del_key)(void *ft, const void *key); /* single-key convenience: calls del_key_bulk */
    u32 (*del_idx)(void *ft, u32 entry_idx);
    void (*find_bulk)(void *ft, const void *keys, unsigned nb_keys,
                      u64 now,
                      struct ft_table_result *results);
    unsigned (*add_idx_bulk)(void *ft, u32 *entry_idxv,
                             unsigned nb_keys,
                             enum ft_add_policy policy,
                             u64 now,
                             u32 *unused_idxv);
    unsigned (*add_idx_bulk_maint)(void *ft, u32 *entry_idxv,
                                   unsigned nb_keys,
                                   enum ft_add_policy policy,
                                   u64 now,
                                   u64 timeout,
                                   u32 *unused_idxv,
                                   unsigned max_unused,
                                   unsigned min_bk_used);
    int (*set_permanent_idx)(void *ft, u32 entry_idx);
    unsigned (*del_idx_bulk)(void *ft, const u32 *entry_idxv,
                               unsigned nb_keys, u32 *unused_idxv);
    int (*walk)(void *ft, int (*cb)(u32 entry_idx, void *arg), void *arg);
    int (*migrate)(void *ft, void *new_buckets, size_t new_bucket_size);
    unsigned (*maintain_idx_bulk)(void *ft,
                                  const u32 *entry_idxv,
                                  unsigned nb_idx,
                                  u64 now,
                                  u64 expire_tsc,
                                  u32 *expired_idxv,
                                  unsigned max_expired,
                                  unsigned min_bk_entries,
                                  int enable_filter);
    void *(*record_ptr)(void *ft, u32 entry_idx);
    void *(*entry_ptr)(void *ft, u32 entry_idx);
    void (*make_key)(void *out, unsigned i);
};

#define TEST_VARIANT_WRAPPERS(tag, prefix, variant_id, key_t, record_t, entry_t, init_fn, \
                              destroy_fn, flush_fn, nb_entries_fn, nb_bk_fn,   \
                              stats_fn, status_fn, find_fn, add_idx_fn,        \
                              del_key_fn, del_idx_fn, find_bulk_fn,       \
                              add_idx_bulk_fn,                                 \
                              add_idx_bulk_maint_fn,                           \
                              del_idx_bulk_fn,                           \
                              walk_fn, migrate_fn,                              \
                              maintain_idx_bulk_fn,                             \
                              record_ptr_fn, entry_ptr_fn,                      \
                              make_key_fn)                                      \
static int                                                                 \
testv_init_##tag(void *ft, void *records, unsigned max_entries,            \
                 const struct ft_table_config *cfg)                        \
{                                                                          \
    size_t bsz = ft_table_bucket_size(max_entries);                        \
    void *bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);                 \
    int rc;                                                                \
    if (bk == NULL) return -1;                                             \
    rc = init_fn((struct ft_table *)ft, (variant_id), records,             \
                 max_entries, sizeof(record_t),                            \
                 offsetof(record_t, entry), bk, bsz, cfg);                 \
    if (rc != 0)                                                           \
        free(bk);                                                          \
    return rc;                                                             \
}                                                                          \
static int                                                                 \
testv_init_with_bucket_entries_##tag(void *ft, void *records,              \
                                     unsigned max_entries,                 \
                                     unsigned bucket_entries,              \
                                     const struct ft_table_config *cfg)    \
{                                                                          \
    size_t bsz = ft_table_bucket_size(bucket_entries);                     \
    void *bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);                 \
    int rc;                                                                \
    if (bk == NULL) return -1;                                             \
    rc = init_fn((struct ft_table *)ft, (variant_id), records,             \
                 max_entries, sizeof(record_t),                            \
                 offsetof(record_t, entry), bk, bsz, cfg);                 \
    if (rc != 0)                                                           \
        free(bk);                                                          \
    return rc;                                                             \
}                                                                          \
static void                                                                 \
testv_destroy_##tag(void *ft)                                              \
{                                                                          \
    struct ft_table *t = (struct ft_table *)ft;                            \
    void *bk = t->buckets;                                                 \
    destroy_fn(t);                                                         \
    free(bk);                                                              \
}                                                                          \
static void                                                                 \
testv_flush_##tag(void *ft)                                                \
{                                                                          \
    flush_fn((struct ft_table *)ft);                                       \
}                                                                          \
static unsigned                                                             \
testv_nb_entries_##tag(const void *ft)                                      \
{                                                                          \
    return nb_entries_fn((const struct ft_table *)ft);                     \
}                                                                          \
static unsigned                                                             \
testv_nb_bk_##tag(const void *ft)                                           \
{                                                                          \
    return nb_bk_fn((const struct ft_table *)ft);                          \
}                                                                          \
static void                                                                 \
testv_stats_##tag(const void *ft, struct ft_table_stats *out)               \
{                                                                          \
    stats_fn((const struct ft_table *)ft, out);                            \
}                                                                          \
static void                                                                 \
testv_status_##tag(const void *ft, struct flow_status *out)                 \
{                                                                          \
    status_fn((const struct ft_table *)ft, out);                           \
}                                                                          \
static u32                                                             \
testv_find_##tag(void *ft, const void *key, u64 now)                    \
{                                                                          \
    return find_fn((struct ft_table *)ft,                                  \
                   (const key_t *)key, now);                                \
}                                                                          \
static u32                                                             \
testv_add_idx_##tag(void *ft, u32 entry_idx, u64 now)              \
{                                                                          \
    return add_idx_fn((struct ft_table *)ft, entry_idx, now);              \
}                                                                          \
static u32                                                             \
testv_del_key_##tag(void *ft, const void *key)                              \
{                                                                          \
    u32 idx;                                                               \
    unsigned n = del_key_fn((struct ft_table *)ft,                         \
                            (const key_t *)key, 1u, &idx);                  \
    return n > 0u ? idx : 0u;                                              \
}                                                                          \
static u32                                                             \
testv_del_idx_##tag(void *ft, u32 entry_idx)                     \
{                                                                          \
    return del_idx_fn((struct ft_table *)ft, entry_idx);                   \
}                                                                          \
static void                                                                 \
testv_find_bulk_##tag(void *ft, const void *keys, unsigned nb_keys,         \
                      u64 now,                                          \
                      struct ft_table_result *results)                       \
{                                                                          \
    find_bulk_fn((struct ft_table *)ft,                                     \
                 (const key_t *)keys, nb_keys, now, results);               \
}                                                                          \
static unsigned                                                             \
testv_add_idx_bulk_##tag(void *ft, u32 *entry_idxv,                   \
                         unsigned nb_keys,                                  \
                         enum ft_add_policy policy,                         \
                         u64 now,                                       \
                         u32 *unused_idxv)                             \
{                                                                          \
    return add_idx_bulk_fn((struct ft_table *)ft,                           \
                           entry_idxv, nb_keys, policy, now, unused_idxv);  \
}                                                                          \
static unsigned                                                             \
testv_add_idx_bulk_maint_##tag(void *ft, u32 *entry_idxv,                   \
                               unsigned nb_keys,                             \
                               enum ft_add_policy policy,                    \
                               u64 now,                                      \
                               u64 timeout,                                  \
                               u32 *unused_idxv,                             \
                               unsigned max_unused,                          \
                               unsigned min_bk_used)                         \
{                                                                          \
    return add_idx_bulk_maint_fn((struct ft_table *)ft,                      \
                                 entry_idxv, nb_keys, policy, now, timeout, \
                                 unused_idxv, max_unused, min_bk_used);     \
}                                                                          \
static int                                                                  \
testv_set_permanent_idx_##tag(void *ft, u32 entry_idx)                 \
{                                                                          \
    return ft_##prefix##_table_set_permanent_idx(                           \
        (struct ft_table *)ft, entry_idx);                                  \
}                                                                          \
static unsigned                                                             \
testv_del_idx_bulk_##tag(void *ft, const u32 *entry_idxv,        \
                               unsigned nb_keys, u32 *unused_idxv)           \
{                                                                          \
    return del_idx_bulk_fn((struct ft_table *)ft,                            \
                          entry_idxv, nb_keys, unused_idxv);               \
}                                                                          \
static int                                                                  \
testv_walk_##tag(void *ft, int (*cb)(u32 entry_idx, void *arg),        \
                 void *arg)                                                  \
{                                                                          \
    return walk_fn((struct ft_table *)ft, cb, arg);                         \
}                                                                          \
static int                                                                  \
testv_migrate_##tag(void *ft, void *new_buckets, size_t new_bucket_size)    \
{                                                                          \
    return migrate_fn((struct ft_table *)ft,                                \
                      new_buckets, new_bucket_size);                        \
}                                                                          \
static unsigned                                                             \
testv_maintain_idx_bulk_##tag(void *ft, const u32 *entry_idxv,         \
                              unsigned nb_idx, u64 now,                \
                              u64 expire_tsc,                          \
                              u32 *expired_idxv,                        \
                              unsigned max_expired,                          \
                              unsigned min_bk_entries,                       \
                              int enable_filter)                             \
{                                                                          \
    return maintain_idx_bulk_fn((struct ft_table *)ft,                      \
                                entry_idxv, nb_idx, now, expire_tsc,        \
                                expired_idxv, max_expired,                  \
                                min_bk_entries, enable_filter);             \
}                                                                          \
static void *                                                               \
testv_record_ptr_##tag(void *ft, u32 entry_idx)                        \
{                                                                          \
    return record_ptr_fn((struct ft_table *)ft, entry_idx);                 \
}                                                                          \
static void *                                                               \
testv_entry_ptr_##tag(void *ft, u32 entry_idx)                         \
{                                                                          \
    return entry_ptr_fn((struct ft_table *)ft, entry_idx);                  \
}                                                                          \
static void                                                                 \
testv_make_key_##tag(void *out, unsigned i)                                  \
{                                                                          \
    *(key_t *)out = make_key_fn(i);                                         \
}                                                                          \
static const struct test_variant_ops test_ops_##tag = {                     \
    .name = #prefix,                                                        \
    .table_size = sizeof(struct ft_table),                                  \
    .key_size = sizeof(key_t),                                              \
    .record_size = sizeof(record_t),                                        \
    .entry_offset = offsetof(record_t, entry),                              \
    .meta_offset = offsetof(entry_t, meta),                                 \
    .cookie_offset = offsetof(record_t, cookie),                            \
    .init = testv_init_##tag,                                               \
    .init_with_bucket_entries = testv_init_with_bucket_entries_##tag,       \
    .destroy = testv_destroy_##tag,                                         \
    .flush = testv_flush_##tag,                                             \
    .nb_entries = testv_nb_entries_##tag,                                   \
    .nb_bk = testv_nb_bk_##tag,                                             \
    .stats = testv_stats_##tag,                                             \
    .status = testv_status_##tag,                                           \
    .find = testv_find_##tag,                                               \
    .add_idx = testv_add_idx_##tag,                                         \
    .del_key = testv_del_key_##tag,                                         \
    .del_idx = testv_del_idx_##tag,                             \
    .find_bulk = testv_find_bulk_##tag,                                     \
    .add_idx_bulk = testv_add_idx_bulk_##tag,                               \
    .add_idx_bulk_maint = testv_add_idx_bulk_maint_##tag,                   \
    .set_permanent_idx = testv_set_permanent_idx_##tag,                     \
    .del_idx_bulk = testv_del_idx_bulk_##tag,                   \
    .walk = testv_walk_##tag,                                               \
    .migrate = testv_migrate_##tag,                                         \
    .maintain_idx_bulk = testv_maintain_idx_bulk_##tag,                     \
    .record_ptr = testv_record_ptr_##tag,                                   \
    .entry_ptr = testv_entry_ptr_##tag,                                     \
    .make_key = testv_make_key_##tag                                        \
}

TEST_VARIANT_WRAPPERS(flow4, flow4, FT_TABLE_VARIANT_FLOW4,
                      struct flow4_key, struct test_user_record,
                      struct flow4_entry, ft_table_init,
                      ft_table_destroy, ft_table_flush,
                      ft_table_nb_entries, ft_table_nb_bk,
                      ft_table_stats, ft_table_status,
                      ft_flow4_table_find, ft_table_add_idx,
                      ft_flow4_table_del_key_bulk, ft_table_del_idx,
                      ft_flow4_table_find_bulk, ft_table_add_idx_bulk,
                      ft_table_add_idx_bulk_maint,
                      ft_table_del_idx_bulk, ft_table_walk,
                      ft_table_migrate,
                      ft_flow4_table_maintain_idx_bulk,
                      ft_flow4_table_record_ptr, ft_flow4_table_entry_ptr,
                      test_key);

TEST_VARIANT_WRAPPERS(flow6, flow6, FT_TABLE_VARIANT_FLOW6,
                      struct flow6_key, struct test_user_record6,
                      struct flow6_entry, ft_table_init,
                      ft_table_destroy, ft_table_flush,
                      ft_table_nb_entries, ft_table_nb_bk,
                      ft_table_stats, ft_table_status,
                      ft_flow6_table_find, ft_table_add_idx,
                      ft_flow6_table_del_key_bulk, ft_table_del_idx,
                      ft_flow6_table_find_bulk, ft_table_add_idx_bulk,
                      ft_table_add_idx_bulk_maint,
                      ft_table_del_idx_bulk, ft_table_walk,
                      ft_table_migrate,
                      ft_flow6_table_maintain_idx_bulk,
                      ft_flow6_table_record_ptr, ft_flow6_table_entry_ptr,
                      test_key6);

TEST_VARIANT_WRAPPERS(flowu, flowu, FT_TABLE_VARIANT_FLOWU,
                      struct flowu_key, struct test_user_recordu,
                      struct flowu_entry, ft_table_init,
                      ft_table_destroy, ft_table_flush,
                      ft_table_nb_entries, ft_table_nb_bk,
                      ft_table_stats, ft_table_status,
                      ft_flowu_table_find, ft_table_add_idx,
                      ft_flowu_table_del_key_bulk, ft_table_del_idx,
                      ft_flowu_table_find_bulk, ft_table_add_idx_bulk,
                      ft_table_add_idx_bulk_maint,
                      ft_table_del_idx_bulk, ft_table_walk,
                      ft_table_migrate,
                      ft_flowu_table_maintain_idx_bulk,
                      ft_flowu_table_record_ptr, ft_flowu_table_entry_ptr,
                      test_keyu);

#define TEST_KEY_AT(base, ops, i) \
    ((void *)((unsigned char *)(base) + (size_t)(i) * (ops)->key_size))

#define FT4_FIND(ft, key) \
    ft_flow4_table_find((ft), (key), TEST_NOW_FIND)
#define FT4_FIND_BULK(ft, keys, nb_keys, results) \
    ft_flow4_table_find_bulk((ft), (keys), (nb_keys), TEST_NOW_FIND, (results))
#define FT4_ADD_IDX(ft, entry_idx) \
    ft_flow4_table_add_idx((ft), (entry_idx), TEST_NOW_ADD)
#define FT4_ADD_IDX_BULK(ft, entry_idxv, nb_keys, policy, unused_idxv) \
    ft_flow4_table_add_idx_bulk((ft), (entry_idxv), (nb_keys), (policy), \
                                TEST_NOW_ADD, (unused_idxv))

#define TEST_OPS_FIND(ops, ft, key) \
    ((ops)->find((ft), (key), TEST_NOW_FIND))
#define TEST_OPS_FIND_BULK(ops, ft, keys, nb_keys, results) \
    ((ops)->find_bulk((ft), (keys), (nb_keys), TEST_NOW_FIND, (results)))
#define TEST_OPS_ADD_IDX(ops, ft, entry_idx) \
    ((ops)->add_idx((ft), (entry_idx), TEST_NOW_ADD))
#define TEST_OPS_ADD_IDX_BULK(ops, ft, entry_idxv, nb_keys, policy, unused_idxv) \
    ((ops)->add_idx_bulk((ft), (entry_idxv), (nb_keys), (policy),            \
                         TEST_NOW_ADD, (unused_idxv)))
#define TEST_OPS_ADD_IDX_BULK_MAINT(ops, ft, entry_idxv, nb_keys, policy,     \
                                    now, timeout, unused_idxv, max_unused,    \
                                    min_bk_used)                              \
    ((ops)->add_idx_bulk_maint((ft), (entry_idxv), (nb_keys), (policy),       \
                               (now), (timeout), (unused_idxv),               \
                               (max_unused), (min_bk_used)))

static u32
test_add_idx_key(struct ft_table *ft, u32 entry_idx,
                 const struct flow4_key *key)
{
    struct flow4_entry *entry = ft_flow4_table_entry_ptr(ft, entry_idx);

    if (entry == NULL)
        return 0u;
    entry->key = *key;
    return FT4_ADD_IDX(ft, entry_idx);
}

struct walk_ctx {
    unsigned count;
    u64 sum;
    unsigned stop_after;
};

static int
walk_count_cb(u32 entry_idx, void *arg)
{
    struct walk_ctx *ctx = (struct walk_ctx *)arg;

    ctx->count++;
    ctx->sum += entry_idx;
    if (ctx->stop_after != 0u && ctx->count >= ctx->stop_after)
        return 1;
    return 0;
}

static int
test_basic_add_find_del(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key k1 = test_key(1u);
    struct flow4_key k2 = test_key(2u);
    u32 idx1, idx2;

    printf("[T] flow4 table basic add/find/del\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("ft_flow4_table_init failed");
    idx1 = test_add_idx_key(&ft, 1u, &k1);
    idx2 = test_add_idx_key(&ft, 2u, &k2);
    if (idx1 != 1u || idx2 != 2u || idx1 == idx2)
        FAIL("basic add returned invalid indices");
    if (FT4_FIND(&ft, &k1) != idx1)
        FAIL("find should return idx1");
    if (FT4_FIND(&ft, &k2) != idx2)
        FAIL("find should return idx2");
    if (test_add_idx_key(&ft, 3u, &k1) != idx1)
        FAIL("duplicate key add should return existing idx");
    if (ft_flow4_table_nb_entries(&ft) != 2u)
        FAIL("duplicate key add should not increase count");
    if (ft_flow4_table_del_key_oneshot(&ft, &k1) != idx1)
        FAIL("del should return idx1");
    if (FT4_FIND(&ft, &k1) != 0u)
        FAIL("deleted key should miss");
    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_init_ex_and_mapping(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *users;
    void *bk;
    struct flow4_key key = test_key(10u);
    u32 idx;

    printf("[T] flow4 table init_ex mapping\n");
    users = test_aligned_calloc(64u, sizeof(*users), FT_TABLE_CACHE_LINE_SIZE);
    if (users == NULL)
        FAIL("calloc users");
    for (unsigned i = 0; i < 64u; i++)
        users[i].cookie = UINT32_C(0xabc00000) + i;
    bk = test_alloc_buckets(64u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, users, 64u,
                                  struct test_user_record, entry, bk, ft_table_bucket_size(64u), &cfg) != 0)
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
    free(bk);
    free(users);
    return 0;
}

static int
test_manual_grow_preserves_entries(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    u32 idxs[256];

    printf("[T] flow4 table migrate preserves entries\n");
    pool = test_aligned_calloc(4096u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(4096u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 4096u, struct test_user_record, entry, bk, ft_table_bucket_size(4096u), &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 256u; i++) {
        struct flow4_key key = test_key(i + 100u);

        idxs[i] = test_add_idx_key(&ft, i + 1u, &key);
        if (idxs[i] != i + 1u)
            FAILF("add before grow failed at %u", i);
    }
    {
        size_t new_bsz = (size_t)ft.nb_bk * 2u * FT_TABLE_BUCKET_SIZE;
        void *new_bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, new_bsz);
        void *old_bk = ft.buckets;

        if (ft_flow4_table_migrate(&ft, new_bk, new_bsz) != 0) {
            free(new_bk);
            FAIL("migrate failed");
        }
        free(old_bk);
    }
    for (unsigned i = 0; i < 256u; i++) {
        struct flow4_key key = test_key(i + 100u);

        if (FT4_FIND(&ft, &key) != idxs[i])
            FAILF("find after grow mismatch at %u", i);
    }
    {
        void *final_bk = ft.buckets;
        ft_flow4_table_destroy(&ft);
        free(final_bk);
    }
    free(pool);
    return 0;
}

static int
test_migrate_doubles_buckets(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    unsigned old_nb_bk;

    printf("[T] flow4 table migrate doubles buckets\n");
    pool = test_aligned_calloc(200000u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(200000u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 200000u, struct test_user_record, entry, bk, ft_table_bucket_size(200000u), &cfg) != 0)
        FAIL("init failed");
    old_nb_bk = ft_flow4_table_nb_bk(&ft);
    {
        size_t new_bsz = (size_t)old_nb_bk * 2u * FT_TABLE_BUCKET_SIZE;
        void *new_bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, new_bsz);
        void *old_bk = ft.buckets;

        if (ft_flow4_table_migrate(&ft, new_bk, new_bsz) != 0) {
            free(new_bk);
            FAIL("migrate failed");
        }
        free(old_bk);
    }
    if (ft_flow4_table_nb_bk(&ft) != old_nb_bk * 2u)
        FAILF("migrate should double nb_bk to %u, got %u",
              old_nb_bk * 2u, ft_flow4_table_nb_bk(&ft));

    {
        void *final_bk = ft.buckets;
        ft_flow4_table_destroy(&ft);
        free(final_bk);
    }
    free(pool);
    return 0;
}

static int
test_bulk_ops_and_stats(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key keys[8];
    u32 entry_idxv[8];
    struct ft_table_result find_results[8];
    u32 unused_idxv[8];
    struct ft_table_stats stats;

    printf("[T] flow4 table bulk ops and stats\n");
    pool = test_aligned_calloc(256u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(256u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 256u, struct test_user_record, entry, bk, ft_table_bucket_size(256u), &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 2000u);
        pool[i].entry.key = keys[i];
        entry_idxv[i] = i + 1u;
    }

    if (FT4_ADD_IDX_BULK(&ft, entry_idxv, 8u, FT_ADD_IGNORE,
                         unused_idxv) != 0u)
        FAIL("add_bulk inserted should not return unused idx");
    for (unsigned i = 0; i < 8u; i++) {
        if (entry_idxv[i] != i + 1u)
            FAILF("add_bulk failed at %u", i);
    }

    memset(find_results, 0, sizeof(find_results));
    FT4_FIND_BULK(&ft, keys, 8u, find_results);
    for (unsigned i = 0; i < 8u; i++) {
        if (find_results[i].entry_idx == 0u)
            FAILF("find_bulk miss at %u", i);
    }

    for (unsigned i = 0; i < 4u; i++) {
        if (ft_flow4_table_del_key_oneshot(&ft, &keys[i]) == 0u)
            FAILF("del_key failed at %u", i);
    }

    memset(find_results, 0, sizeof(find_results));
    FT4_FIND_BULK(&ft, keys, 8u, find_results);
    for (unsigned i = 0; i < 4u; i++) {
        if (find_results[i].entry_idx != 0u)
            FAILF("deleted key should miss at %u", i);
    }
    for (unsigned i = 4u; i < 8u; i++) {
        if (find_results[i].entry_idx == 0u)
            FAILF("remaining key should hit at %u", i);
    }

    ft_flow4_table_stats(&ft, &stats);
    if (stats.core.adds != 8u)
        FAILF("adds=%llu", (unsigned long long)stats.core.adds);
    if (stats.core.add_existing != 0u)
        FAILF("add_existing=%llu", (unsigned long long)stats.core.add_existing);
    if (stats.core.lookups != 16u)
        FAILF("lookups=%llu", (unsigned long long)stats.core.lookups);
    if (stats.core.hits != 12u)
        FAILF("hits=%llu", (unsigned long long)stats.core.hits);
    if (stats.core.misses != 4u)
        FAILF("misses=%llu", (unsigned long long)stats.core.misses);
    if (stats.core.dels != 4u)
        FAILF("dels=%llu", (unsigned long long)stats.core.dels);
    if (stats.core.del_miss != 0u)
        FAILF("del_miss=%llu", (unsigned long long)stats.core.del_miss);

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_walk_flush_and_stats_reset(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct walk_ctx ctx;
    struct ft_table_stats stats;

    printf("[T] flow4 table walk/flush/stats\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
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
    if (stats.core.adds != 16u)
        FAILF("adds before flush=%llu", (unsigned long long)stats.core.adds);

    ft_flow4_table_flush(&ft);
    if (ft_flow4_table_nb_entries(&ft) != 0u)
        FAIL("flush should clear entries");
    memset(&ctx, 0, sizeof(ctx));
    if (ft_flow4_table_walk(&ft, walk_count_cb, &ctx) != 0)
        FAIL("walk after flush failed");
    if (ctx.count != 0u)
        FAILF("walk after flush count=%u", ctx.count);

    ft_flow4_table_stats(&ft, &stats);
    if (stats.core.adds != 16u)
        FAILF("stats should be preserved across flush, adds=%llu",
              (unsigned long long)stats.core.adds);

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_migrate_invalid_args(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;

    printf("[T] flow4 table migrate invalid args\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    if (ft_flow4_table_migrate(&ft, NULL, (size_t)FT_TABLE_MIN_NB_BK * 2u * FT_TABLE_BUCKET_SIZE) == 0)
        FAIL("migrate with NULL buckets should fail");
    {
        size_t bad_sz = 123u;
        void *tmp = aligned_alloc(FT_TABLE_BUCKET_ALIGN, FT_TABLE_MIN_NB_BK * FT_TABLE_BUCKET_SIZE);

        if (ft_flow4_table_migrate(&ft, tmp, bad_sz) == 0) {
            free(tmp);
            FAIL("migrate with non-power-of-2 bucket_size should fail");
        }
        free(tmp);
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_duplicate_and_delete_miss_stats(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct ft_table_stats stats;
    struct flow4_key key = test_key(6000u);
    struct flow4_key miss = test_key(6001u);
    u32 idx;

    printf("[T] flow4 table duplicate add/del miss stats\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    idx = test_add_idx_key(&ft, 1u, &key);
    if (idx != 1u)
        FAIL("initial add failed");
    if (test_add_idx_key(&ft, 2u, &key) != idx)
        FAIL("duplicate key add should return existing idx");
    if (ft_flow4_table_del_key_oneshot(&ft, &miss) != 0u)
        FAIL("delete miss should return 0");

    ft_flow4_table_stats(&ft, &stats);
    if (stats.core.adds != 1u)
        FAILF("adds=%llu", (unsigned long long)stats.core.adds);
    if (stats.core.add_existing != 1u)
        FAILF("add_existing=%llu", (unsigned long long)stats.core.add_existing);
    if (stats.core.del_miss != 1u)
        FAILF("del_miss=%llu", (unsigned long long)stats.core.del_miss);
    if (stats.core.add_failed != 0u)
        FAILF("add_failed=%llu", (unsigned long long)stats.core.add_failed);

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_walk_early_stop(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct walk_ctx ctx;

    printf("[T] flow4 table walk early stop\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
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
    free(bk);
    free(pool);
    return 0;
}

static int
test_migrate_failure_preserves_table(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    u32 idxs[64];
    unsigned old_nb_bk;

    printf("[T] flow4 table migrate failure preserves table\n");
    pool = test_aligned_calloc(2048u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(2048u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 2048u, struct test_user_record, entry, bk, ft_table_bucket_size(2048u), &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 64u; i++) {
        struct flow4_key key = test_key(i + 8000u);

        idxs[i] = test_add_idx_key(&ft, i + 1u, &key);
        if (idxs[i] != i + 1u)
            FAILF("add failed at %u", i);
    }

    old_nb_bk = ft_flow4_table_nb_bk(&ft);
    {
        size_t small_bsz = (size_t)(old_nb_bk / 2u) * FT_TABLE_BUCKET_SIZE;
        void *small_bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, small_bsz);

        if (ft_flow4_table_migrate(&ft, small_bk, small_bsz) == 0) {
            free(small_bk);
            FAIL("migrate to smaller buckets should fail");
        }
        free(small_bk);
    }
    if (ft_flow4_table_nb_bk(&ft) != old_nb_bk)
        FAIL("nb_bk changed after failed migrate");

    for (unsigned i = 0; i < 64u; i++) {
        struct flow4_key key = test_key(i + 8000u);

        if (FT4_FIND(&ft, &key) != idxs[i])
            FAILF("find after failed migrate mismatch at %u", i);
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_bucket_size_determines_nb_bk(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;

    printf("[T] flow4 table bucket_size determines nb_bk\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    if (ft_flow4_table_nb_bk(&ft) != FT_TABLE_MIN_NB_BK)
        FAILF("nb_bk=%u expected=%u", ft_flow4_table_nb_bk(&ft), FT_TABLE_MIN_NB_BK);

    ft_flow4_table_destroy(&ft);
    free(bk);
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
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = (size_t)FT_TABLE_MIN_NB_BK * FT_TABLE_BUCKET_SIZE;
    unsigned inserted = 0u;

    printf("[T] flow4 table high fill (94%%)\n");
    pool = test_aligned_calloc(capacity + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, capacity, struct test_user_record, entry, bk, bsz, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < target; i++) {
        struct flow4_key key = test_key(i + 10000u);
        u32 idx = test_add_idx_key(&ft, i + 1u, &key);

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

        if (FT4_FIND(&ft, &key) != i + 1u)
            FAILF("find failed at high fill i=%u", i);
    }

    /* remove every other entry, verify remaining */
    for (unsigned i = 0; i < inserted; i += 2u) {
        struct flow4_key key = test_key(i + 10000u);

        if (ft_flow4_table_del_key_oneshot(&ft, &key) != i + 1u)
            FAILF("del at high fill failed i=%u", i);
    }
    for (unsigned i = 1u; i < inserted; i += 2u) {
        struct flow4_key key = test_key(i + 10000u);

        if (FT4_FIND(&ft, &key) != i + 1u)
            FAILF("find after partial del failed i=%u", i);
    }
    /* deleted keys must miss */
    for (unsigned i = 0; i < inserted; i += 2u) {
        struct flow4_key key = test_key(i + 10000u);

        if (FT4_FIND(&ft, &key) != 0u)
            FAILF("deleted key still found i=%u", i);
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
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
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = (size_t)FT_TABLE_MIN_NB_BK * FT_TABLE_BUCKET_SIZE;
    unsigned inserted = 0u;

    printf("[T] flow4 table max fill\n");
    pool = test_aligned_calloc(capacity + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, capacity, struct test_user_record, entry, bk, bsz, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < capacity; i++) {
        struct flow4_key key = test_key(i + 20000u);
        u32 idx = test_add_idx_key(&ft, i + 1u, &key);

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

        if (FT4_FIND(&ft, &key) != i + 1u)
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
    free(bk);
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
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = (size_t)FT_TABLE_MIN_NB_BK * FT_TABLE_BUCKET_SIZE;
    unsigned phase1_count = 0u;
    unsigned lost = 0u;

    printf("[T] flow4 table kickout safety\n");
    pool = test_aligned_calloc(capacity + overflow + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, capacity + overflow,
                                  struct test_user_record, entry, bk, bsz, &cfg) != 0)
        FAIL("init failed");

    /* Phase 1: fill until first rejection */
    for (unsigned i = 0; i < capacity; i++) {
        struct flow4_key key = test_key(i + 30000u);
        u32 idx = test_add_idx_key(&ft, i + 1u, &key);

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

        if (FT4_FIND(&ft, &key) != i + 1u)
            lost++;
    }
    printf("  phase1=%u lost=%u\n", phase1_count, lost);
    if (lost != 0u)
        FAILF("kickout caused %u victim(s) to become unreachable", lost);

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

/*
 * force_expire basic: fill the table until kickout fails, then use
 * FT_ADD_IGNORE_FORCE_EXPIRE to insert new entries.  Verifies:
 *  - the new entry is registered (not add_failed)
 *  - the new entry is findable
 *  - force_expired stat is incremented
 *  - entry count stays the same (replace, not add)
 *
 * With 4096 minimum buckets (65536 slots), we need ~60000 entries
 * to trigger cuckoo exhaustion.
 */
static int
test_force_expire_basic(void)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 64u;
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = ft_table_bucket_size(bucket_entries);
    unsigned inserted = 0u;
    unsigned add_failed_before = 0u;
    struct ft_table_stats stats;

    printf("[T] flow4 table force_expire basic\n");
    pool = test_aligned_calloc(phase1_entries + overflow + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, phase1_entries + overflow,
                                  struct test_user_record, entry,
                                  bk, bsz, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < phase1_entries; i++) {
        struct flow4_key key = test_key(i + 40000u);
        u32 idx = test_add_idx_key(&ft, i + 1u, &key);

        if (idx != 0u)
            inserted++;
    }
    printf("  phase1: inserted %u / %u\n", inserted, phase1_entries);
    ft_flow4_table_stats(&ft, &stats);
    add_failed_before = (unsigned)stats.core.add_failed;
    if (add_failed_before == 0u)
        FAIL("expected some add failures at high fill");

    unsigned nb_before = ft_flow4_table_nb_entries(&ft);

    {
        for (unsigned i = 0; i < overflow; i++) {
            struct flow4_key key = test_key(phase1_entries + i + 40000u);
            struct flow4_entry *entry;
            u32 entry_idxv[1];
            u32 unused_idxv[1];

            entry_idxv[0] = phase1_entries + i + 1u;
            entry = ft_flow4_table_entry_ptr(&ft, entry_idxv[0]);
            if (entry == NULL)
                FAIL("entry_ptr NULL");
            entry->key = key;

            ft_flow4_table_add_idx_bulk(&ft, entry_idxv, 1u,
                                        FT_ADD_IGNORE_FORCE_EXPIRE,
                                        TEST_NOW_ADD, unused_idxv);
            if (entry_idxv[0] == 0u)
                FAILF("force_expire add failed at i=%u", i);
            if (FT4_FIND(&ft, &key) != entry_idxv[0])
                FAILF("force_expire entry not findable at i=%u", i);
        }
    }

    ft_flow4_table_stats(&ft, &stats);
    printf("  force_expired=%llu add_failed=%llu\n",
           (unsigned long long)stats.core.force_expired,
           (unsigned long long)stats.core.add_failed);
    if (stats.core.force_expired == 0u)
        FAIL("force_expired should be > 0");

    {
        unsigned nb_after = ft_flow4_table_nb_entries(&ft);
        unsigned fe = (unsigned)(stats.core.force_expired);
        unsigned expected = nb_before + overflow - fe;
        if (nb_after != expected)
            FAILF("entry count mismatch: %u (expected %u)", nb_after, expected);
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

/*
 * force_expire with permanent entries: entries with ts=0 are never evicted.
 * Fill the table until kickout failure occurs, using now=0 so all entries
 * get ts=0 (permanent).  Then attempt FT_ADD_IGNORE_FORCE_EXPIRE with
 * multiple entries.  No permanent entries should be evicted: the force_expire
 * path must fall back to add_failed for any entry whose kickout chain fails.
 */
static int
test_force_expire_permanent_skip(void)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 128u;
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = ft_table_bucket_size(bucket_entries);
    unsigned inserted = 0u;
    struct ft_table_stats stats;

    printf("[T] flow4 table force_expire permanent skip\n");
    pool = test_aligned_calloc(phase1_entries + overflow + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, phase1_entries + overflow,
                                  struct test_user_record, entry,
                                  bk, bsz, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < phase1_entries; i++) {
        struct flow4_entry *entry;
        struct flow4_key key = test_key(i + 50000u);
        u32 entry_idxv[1] = { i + 1u };
        u32 unused_idxv[1];

        entry = ft_flow4_table_entry_ptr(&ft, i + 1u);
        if (entry == NULL)
            break;
        entry->key = key;
        ft_flow4_table_add_idx_bulk(&ft, entry_idxv, 1u, FT_ADD_IGNORE,
                                    0u, unused_idxv);
        if (entry_idxv[0] != 0u)
            inserted++;
    }
    printf("  filled %u permanent entries\n", inserted);

    ft_flow4_table_stats(&ft, &stats);
    if (stats.core.add_failed == 0u)
        FAIL("expected some add failures at high fill");

    u64 add_failed_before = stats.core.add_failed;

    for (unsigned i = 0; i < overflow; i++) {
        struct flow4_key key = test_key(phase1_entries + i + 50000u);
        struct flow4_entry *entry;
        u32 entry_idxv[1] = { phase1_entries + i + 1u };
        u32 unused_idxv[1] = { 0u };

        entry = ft_flow4_table_entry_ptr(&ft, phase1_entries + i + 1u);
        if (entry == NULL)
            FAIL("entry_ptr NULL for overflow");
        entry->key = key;
        ft_flow4_table_add_idx_bulk(&ft, entry_idxv, 1u,
                                    FT_ADD_IGNORE_FORCE_EXPIRE,
                                    0u, unused_idxv);
    }

    ft_flow4_table_stats(&ft, &stats);
    printf("  add_failed: %llu -> %llu, force_expired=%llu\n",
           (unsigned long long)add_failed_before,
           (unsigned long long)stats.core.add_failed,
           (unsigned long long)stats.core.force_expired);

    if (stats.core.force_expired != 0u)
        FAILF("force_expired should be 0 with all permanent, got %llu",
               (unsigned long long)stats.core.force_expired);
    if (stats.core.add_failed <= add_failed_before)
        FAIL("expected add_failed to increase (some kickouts should fail)");

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

/*
 * force_expire bulk: exercise the bulk pipeline (nb_keys >= 4) with
 * FT_ADD_UPDATE_FORCE_EXPIRE.
 */
static int
test_force_expire_bulk(void)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 8u;
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = ft_table_bucket_size(bucket_entries);
    unsigned inserted = 0u;
    struct ft_table_stats stats;

    printf("[T] flow4 table force_expire bulk\n");
    pool = test_aligned_calloc(phase1_entries + overflow + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, phase1_entries + overflow,
                                  struct test_user_record, entry,
                                  bk, bsz, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < phase1_entries; i++) {
        struct flow4_key key = test_key(i + 60000u);
        u32 idx = test_add_idx_key(&ft, i + 1u, &key);

        if (idx != 0u)
            inserted++;
    }
    printf("  filled %u entries\n", inserted);
    ft_flow4_table_stats(&ft, &stats);
    if (stats.core.add_failed == 0u)
        FAIL("expected some add failures at high fill");

    unsigned nb_before = ft_flow4_table_nb_entries(&ft);

    {
        u32 entry_idxv[8];
        u32 unused_idxv[8];

        for (unsigned i = 0; i < overflow; i++) {
            struct flow4_key key = test_key(phase1_entries + i + 60000u);
            struct flow4_entry *entry;

            entry_idxv[i] = phase1_entries + i + 1u;
            entry = ft_flow4_table_entry_ptr(&ft, entry_idxv[i]);
            if (entry == NULL)
                FAIL("entry_ptr NULL");
            entry->key = key;
        }
        FT4_ADD_IDX_BULK(&ft, entry_idxv, overflow,
                         FT_ADD_UPDATE_FORCE_EXPIRE,
                         unused_idxv);
        for (unsigned i = 0; i < overflow; i++) {
            struct flow4_key key = test_key(phase1_entries + i + 60000u);

            if (entry_idxv[i] == 0u)
                FAILF("bulk force_expire add failed at i=%u", i);
            if (FT4_FIND(&ft, &key) == 0u)
                FAILF("bulk force_expire entry not findable at i=%u", i);
        }
    }

    ft_flow4_table_stats(&ft, &stats);
    printf("  force_expired=%llu\n",
           (unsigned long long)stats.core.force_expired);
    if (stats.core.force_expired == 0u)
        FAIL("force_expired should be > 0");

    {
        unsigned nb_after = ft_flow4_table_nb_entries(&ft);
        unsigned fe = (unsigned)(stats.core.force_expired);
        unsigned expected = nb_before + overflow - fe;
        if (nb_after != expected)
            FAILF("entry count mismatch: %u (expected %u)", nb_after, expected);
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

/*
 * force_expire evicts the oldest entry: insert entries with increasing
 * timestamps, fill the table, then try multiple FORCE_EXPIRE adds until
 * one triggers eviction.  Verify the evicted entry's timestamp was cleared
 * and the entry is no longer findable.
 */
static int
test_force_expire_evicts_oldest(void)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 128u;
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = ft_table_bucket_size(bucket_entries);
    unsigned inserted = 0u;
    u64 base_now = UINT64_C(0x10000);
    struct ft_table_stats stats;

    printf("[T] flow4 table force_expire evicts oldest\n");
    pool = test_aligned_calloc(phase1_entries + overflow + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, phase1_entries + overflow,
                                  struct test_user_record, entry,
                                  bk, bsz, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < phase1_entries; i++) {
        struct flow4_entry *entry;
        struct flow4_key key = test_key(i + 70000u);
        u32 entry_idxv[1] = { i + 1u };
        u32 unused_idxv[1];
        u64 now = base_now + (u64)i * 256u;

        entry = ft_flow4_table_entry_ptr(&ft, i + 1u);
        if (entry == NULL)
            break;
        entry->key = key;
        ft_flow4_table_add_idx_bulk(&ft, entry_idxv, 1u,
                                    FT_ADD_IGNORE, now, unused_idxv);
        if (entry_idxv[0] != 0u)
            inserted++;
    }
    printf("  filled %u entries with progressive timestamps\n", inserted);
    ft_flow4_table_stats(&ft, &stats);
    if (stats.core.add_failed == 0u)
        FAIL("expected some add failures at high fill");

    {
        int eviction_found = 0;

        for (unsigned i = 0; i < overflow; i++) {
            struct flow4_key key = test_key(phase1_entries + i + 70000u);
            struct flow4_entry *entry;
            u32 entry_idxv[1] = { phase1_entries + i + 1u };
            u32 unused_idxv[1] = { 0u };
            u64 now = base_now + (u64)(phase1_entries + i) * 256u;

            entry = ft_flow4_table_entry_ptr(&ft, phase1_entries + i + 1u);
            if (entry == NULL)
                FAIL("entry_ptr NULL");
            entry->key = key;
            ft_flow4_table_add_idx_bulk(&ft, entry_idxv, 1u,
                                        FT_ADD_IGNORE_FORCE_EXPIRE, now,
                                        unused_idxv);

            if (unused_idxv[0] == 0u)
                continue;

            u32 evicted_idx = unused_idxv[0];
            printf("  evicted idx=%u at attempt %u\n", evicted_idx, i);

            struct flow4_entry *evicted =
                ft_flow4_table_entry_ptr(&ft, evicted_idx);
            if (evicted == NULL)
                FAIL("evicted entry_ptr NULL");
            if (!flow_timestamp_is_zero(&evicted->meta))
                FAIL("evicted entry timestamp not cleared");

            if (FT4_FIND(&ft, &key) == 0u)
                FAIL("new entry not findable after force_expire");

            struct flow4_key evicted_key =
                test_key(evicted_idx - 1u + 70000u);
            if (FT4_FIND(&ft, &evicted_key) != 0u)
                FAIL("evicted entry should not be findable");

            eviction_found = 1;
            break;
        }
        if (!eviction_found)
            FAIL("no force_expire eviction triggered in overflow entries");
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
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
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key keys[8];
    u32 idxs[8];

    printf("[T] flow4 table del_idx\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 40000u);
        idxs[i] = test_add_idx_key(&ft, i + 1u, &keys[i]);
        if (idxs[i] != i + 1u)
            FAILF("add failed at %u", i);
    }

    /* del_idx for even entries */
    for (unsigned i = 0; i < 8u; i += 2u) {
        if (ft_flow4_table_del_idx(&ft, idxs[i]) != idxs[i])
            FAILF("del_idx failed at idx=%u", idxs[i]);
    }
    /* deleted entries miss, remaining entries hit */
    for (unsigned i = 0; i < 8u; i++) {
        u32 found = FT4_FIND(&ft, &keys[i]);

        if ((i & 1u) == 0u) {
            if (found != 0u)
                FAILF("del_idx entry still found i=%u", i);
        } else {
            if (found != idxs[i])
                FAILF("remaining entry not found i=%u", i);
        }
    }
    /* double del_idx should return 0 */
    if (ft_flow4_table_del_idx(&ft, idxs[0]) != 0u)
        FAIL("double del_idx should return 0");

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_del_idx_stale_meta_safe(void)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key key1;
    struct flow4_key key2;
    struct flow4_entry *entry1;
    struct flow4_entry *entry2;

    printf("[T] flow4 table del_idx stale meta safe\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u,
                                  struct test_user_record, entry,
                                  bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    key1 = test_key(41000u);
    key2 = test_key(41001u);
    if (test_add_idx_key(&ft, 1u, &key1) != 1u)
        FAIL("add key1 failed");
    if (test_add_idx_key(&ft, 2u, &key2) != 2u)
        FAIL("add key2 failed");

    entry1 = ft_flow4_table_entry_ptr(&ft, 1u);
    entry2 = ft_flow4_table_entry_ptr(&ft, 2u);
    if (entry1 == NULL || entry2 == NULL)
        FAIL("entry ptr failed");

    if (ft_flow4_table_del_idx(&ft, 1u) != 1u)
        FAIL("initial del_idx failed");
    if (FT4_FIND(&ft, &key1) != 0u)
        FAIL("deleted key1 still found");

    entry1->meta.cur_hash = entry2->meta.cur_hash;
    entry1->meta.slot = entry2->meta.slot;

    if (ft_flow4_table_del_idx(&ft, 1u) != 0u)
        FAIL("stale-metadata del_idx should miss");
    if (FT4_FIND(&ft, &key2) != 2u)
        FAIL("stale-metadata del_idx removed the wrong entry");
    if (ft_flow4_table_nb_entries(&ft) != 1u)
        FAIL("stale-metadata del_idx changed entry count");

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_maintain_basic(void)
{
    struct ft_table_config cfg = test_cfg();
    const u64 expire_tsc = UINT64_C(100000);
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key keys[16];
    struct flow4_key new_key;
    u32 expired[16];
    unsigned n;
    unsigned next_bk;

    printf("[T] flow4 table maintain basic\n");
    /* expire_tsc=100000; ts_shift=4 → timeout_encoded=6250 */
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    /* Add 8 entries (TEST_NOW_ADD = 0x101 = 257) */
    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 50000u);
        if (test_add_idx_key(&ft, i + 1u, &keys[i]) != i + 1u)
            FAILF("add failed at %u", i);
    }
    if (ft_flow4_table_nb_entries(&ft) != 8u)
        FAIL("should have 8 entries");

    /* maintain with expire_tsc=0 should do nothing */
    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(999999), 0u, expired, 16u, 0u, &next_bk);
    if (n != 0u)
        FAIL("maintain with expire_tsc=0 should return 0");
    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(200000), expire_tsc,
                                expired, 16u, 0u, &next_bk);
    if (n != 8u)
        FAIL("maintain expire_tsc override should expire entries");
    if (ft_flow4_table_nb_entries(&ft) != 0u)
        FAIL("override maintain should remove all entries");

    for (unsigned i = 0; i < 8u; i++) {
        if (test_add_idx_key(&ft, i + 1u, &keys[i]) != i + 1u)
            FAILF("re-add failed at %u", i);
    }
    /* Verify: at now=1000, entries (added at TEST_NOW_ADD=0x101) are NOT expired */
    {
        struct flow4_entry *e = ft_flow4_table_entry_ptr(&ft, 1u);
        if (flow_timestamp_is_expired(&e->meta, UINT64_C(1000),
                                      expire_tsc, ft.ts_shift))
            FAIL("entry should NOT be expired at now=1000");
    }
    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(1000), expire_tsc,
                                expired, 16u, 0u, &next_bk);
    if (n != 0u)
        FAIL("no entries should be expired yet");
    if (ft_flow4_table_nb_entries(&ft) != 8u)
        FAIL("should still have 8 entries");

    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(200000), UINT64_C(300000),
                                expired, 16u, 0u, &next_bk);
    if (n != 0u)
        FAIL("longer expire_tsc override should keep entries");

    /* Verify: at now=200000, entries ARE expired */
    {
        struct flow4_entry *e = ft_flow4_table_entry_ptr(&ft, 1u);
        if (!flow_timestamp_is_expired(&e->meta, UINT64_C(200000),
                                       expire_tsc, ft.ts_shift))
            FAIL("entry should be expired at now=200000");
    }
    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(200000), expire_tsc,
                                expired, 16u, 0u, &next_bk);
    if (n != 8u)
        FAILF("should expire 8 entries, got %u", n);
    if (ft_flow4_table_nb_entries(&ft) != 0u)
        FAIL("should have 0 entries after maintain");

    /* expired entries should no longer be findable */
    for (unsigned i = 0; i < 8u; i++) {
        if (FT4_FIND(&ft, &keys[i]) != 0u)
            FAILF("expired entry still found i=%u", i);
    }

    /*
     * Contract: maintain() returns expired idx to the user, and after user
     * reclaim/reset that same idx can be reused.
     */
    new_key = test_key(90000u);
    if (test_add_idx_key(&ft, 1u, &new_key) != 1u)
        FAIL("same-idx reuse add failed");
    if (FT4_FIND(&ft, &keys[0]) != 0u)
        FAIL("old key should remain missing after same-idx reuse");
    if (FT4_FIND(&ft, &new_key) != 1u)
        FAIL("new key should be found after same-idx reuse");

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_maintain_partial_and_limit(void)
{
    struct ft_table_config cfg = test_cfg();
    const u64 expire_tsc = UINT64_C(100000);
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key keys[16];
    struct flow4_entry *entry;
    u32 expired[4];
    unsigned n, total_expired;
    unsigned next_bk;

    printf("[T] flow4 table maintain partial expiry and max_expired limit\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    /* Add 8 entries (TEST_NOW_ADD = 0x101 = 257) */
    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 60000u);
        if (test_add_idx_key(&ft, i + 1u, &keys[i]) != i + 1u)
            FAILF("add failed at %u", i);
    }

    /* Touch entries 5..8 with a recent timestamp so they survive maintain */
    for (unsigned i = 4; i < 8u; i++) {
        entry = ft_flow4_table_entry_ptr(&ft, i + 1u);
        if (entry == NULL)
            FAILF("entry_ptr NULL at %u", i);
        flow_timestamp_store(&entry->meta, UINT64_C(150000),
                             ft.ts_shift);
    }

    /* Verify expiry expectations at now=200000 using helpers:
     * entries 1..4 (ts=TEST_NOW_ADD=257) should be expired,
     * entries 5..8 (ts=150000) should NOT be expired */
    for (unsigned i = 0; i < 8u; i++) {
        entry = ft_flow4_table_entry_ptr(&ft, i + 1u);
        int is_exp = flow_timestamp_is_expired(&entry->meta,
                                               UINT64_C(200000),
                                               expire_tsc, ft.ts_shift);
        if (i < 4u && !is_exp)
            FAILF("entry %u should be expired", i);
        if (i >= 4u && is_exp)
            FAILF("entry %u should NOT be expired", i);
    }

    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(200000), expire_tsc,
                                expired, 16u, 0u, &next_bk);
    if (n != 4u)
        FAILF("should expire 4 entries, got %u", n);
    if (ft_flow4_table_nb_entries(&ft) != 4u)
        FAILF("should have 4 entries, got %u", ft_flow4_table_nb_entries(&ft));

    /* expired entries (1..4) should not be findable */
    for (unsigned i = 0; i < 4u; i++) {
        if (FT4_FIND(&ft, &keys[i]) != 0u)
            FAILF("expired entry still found i=%u", i);
    }
    /* surviving entries (5..8) should still be findable */
    for (unsigned i = 4; i < 8u; i++) {
        if (FT4_FIND(&ft, &keys[i]) != i + 1u)
            FAILF("surviving entry not found i=%u", i);
    }

    /* Test max_expired limit with next_bk continuation.
     * Use a fresh table to avoid flush+re-add cur_hash issues. */
    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);

    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool 2");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init 2 failed");

    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 70000u);
        if (test_add_idx_key(&ft, i + 1u, &keys[i]) != i + 1u)
            FAILF("add2 failed at %u", i);
    }

    /* Sweep with max_expired=4, then continue from next_bk
     * now=500000 → all entries at TEST_NOW_ADD=257 are well expired */
    total_expired = 0u;
    next_bk = 0u;
    for (unsigned pass = 0; pass < 10u; pass++) {
        n = ft_flow4_table_maintain(&ft, next_bk, UINT64_C(500000), expire_tsc,
                                    expired, 4u, 0u, &next_bk);
        total_expired += n;
        if (total_expired >= 8u)
            break;
    }
    if (total_expired != 8u)
        FAILF("loop should expire all 8, got %u", total_expired);
    if (ft_flow4_table_nb_entries(&ft) != 0u)
        FAIL("should have 0 entries after full sweep");

    /* Stats check */
    {
        struct ft_table_stats stats;
        ft_flow4_table_stats(&ft, &stats);
        if (stats.maint_evictions == 0u)
            FAIL("maint_evictions should be > 0");
        if (stats.maint_calls == 0u)
            FAIL("maint_calls should be > 0");
        if (stats.maint_bucket_checks == 0u)
            FAIL("maint_bucket_checks should be > 0");
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_maintain_idx_bulk_stale_meta_safe(void)
{
    struct ft_table_config cfg = test_cfg();
    const u64 expire_tsc = UINT64_C(100000);
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key keys[256];
    u32 expired_idxv[4] = { 0u };
    u32 *first_idx_by_bk = NULL;
    unsigned nb_bk;
    unsigned free_idx = 0u;
    unsigned live_idx = 0u;
    unsigned evicted;

    printf("[T] flow4 table maintain_idx_bulk stale meta safe\n");
    pool = test_aligned_calloc(256u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(256u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 256u,
                                  struct test_user_record, entry,
                                  bk, ft_table_bucket_size(256u), &cfg) != 0)
        FAIL("init failed");

    nb_bk = ft_flow4_table_nb_bk(&ft);
    first_idx_by_bk = calloc(nb_bk, sizeof(*first_idx_by_bk));
    if (first_idx_by_bk == NULL)
        FAIL("calloc bucket tracker failed");

    for (unsigned i = 0u; i < 256u; i++) {
        struct flow4_entry *entry;
        unsigned idx = i + 1u;
        unsigned cur_bk;

        keys[i] = test_key(92000u + i);
        if (test_add_idx_key(&ft, idx, &keys[i]) != idx)
            FAILF("seed add failed at %u", i);
        entry = ft_flow4_table_entry_ptr(&ft, idx);
        if (entry == NULL)
            FAIL("entry ptr failed");
        cur_bk = entry->meta.cur_hash & (nb_bk - 1u);
        if (first_idx_by_bk[cur_bk] == 0u) {
            first_idx_by_bk[cur_bk] = idx;
            continue;
        }
        free_idx = first_idx_by_bk[cur_bk];
        live_idx = idx;
        break;
    }

    if (free_idx == 0u || live_idx == 0u)
        FAIL("failed to find bucket with multiple entries");
    if (ft_flow4_table_del_idx(&ft, free_idx) != free_idx)
        FAIL("del_idx free_idx failed");

    {
        struct flow4_entry *live = ft_flow4_table_entry_ptr(&ft, live_idx);

        if (live == NULL)
            FAIL("live entry ptr failed");
        flow_timestamp_store(&live->meta, UINT64_C(16), ft.ts_shift);
    }

    evicted = ft_flow4_table_maintain_idx_bulk(&ft, &free_idx, 1u,
                                               UINT64_C(200000), expire_tsc,
                                               expired_idxv, 4u, 0u, 0);
    if (evicted != 0u)
        FAIL("stale free idx should not evict from maintain_idx_bulk");
    if (FT4_FIND(&ft, &keys[live_idx - 1u]) != live_idx)
        FAIL("stale free idx expired the wrong live entry");

    free(first_idx_by_bk);
    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    return 0;
}

static int
test_maintain_min_bk_entries(void)
{
    struct ft_table_config cfg = test_cfg();
    const u64 expire_tsc = UINT64_C(100000);
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    struct flow4_key keys[16];
    struct flow4_entry *entry;
    u32 expired[16];
    unsigned n;
    unsigned next_bk;

    printf("[T] flow4 table maintain min_bk_entries skip\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init failed");

    /* Add 8 entries, all expired */
    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 80000u);
        if (test_add_idx_key(&ft, i + 1u, &keys[i]) != i + 1u)
            FAILF("add failed at %u", i);
    }

    /* Verify all are expired at now=200000 */
    for (unsigned i = 0; i < 8u; i++) {
        entry = ft_flow4_table_entry_ptr(&ft, i + 1u);
        if (!flow_timestamp_is_expired(&entry->meta, UINT64_C(200000),
                                       expire_tsc, ft.ts_shift))
            FAILF("entry %u should be expired", i);
    }

    /* min_bk_entries=0: should expire all 8 (no skip) */
    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(200000), expire_tsc,
                                expired, 16u, 0u, &next_bk);
    if (n != 8u)
        FAILF("min_bk=0 should expire 8, got %u", n);

    /* Re-create table for skip test */
    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool 2");
    bk = test_alloc_buckets(128u);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, 128u, struct test_user_record, entry, bk, ft_table_bucket_size(128u), &cfg) != 0)
        FAIL("init 2 failed");

    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 80000u);
        if (test_add_idx_key(&ft, i + 1u, &keys[i]) != i + 1u)
            FAILF("add2 failed at %u", i);
    }

    /* min_bk_entries=16: only buckets with >=16 entries are scanned.
     * 8 entries across many buckets means all buckets have < 16 → all skipped */
    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(200000), expire_tsc,
                                expired, 16u, 16u, &next_bk);
    if (n != 0u)
        FAILF("min_bk=16 should skip all, got %u expired", n);
    if (ft_flow4_table_nb_entries(&ft) != 8u)
        FAILF("should still have 8 entries, got %u", ft_flow4_table_nb_entries(&ft));

    /* min_bk_entries=1: equivalent to 0 (no skip), should expire all */
    n = ft_flow4_table_maintain(&ft, 0u, UINT64_C(200000), expire_tsc,
                                expired, 16u, 1u, &next_bk);
    if (n != 8u)
        FAILF("min_bk=1 should expire 8, got %u", n);

    ft_flow4_table_destroy(&ft);
    free(bk);
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
    struct ft_table_config cfg = test_cfg();
    struct ft_table ft;
    struct test_user_record *pool;
    void *bk;
    size_t bsz = (size_t)nb_bk * FT_TABLE_BUCKET_SIZE;
    u8 *in_table; /* 1 if entry i is in table */
    unsigned in_count = 0u;

    printf("[T] flow4 table fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           seed, n, nb_bk, ops);
    if (bsz < (size_t)FT_TABLE_MIN_NB_BK * FT_TABLE_BUCKET_SIZE)
        bsz = (size_t)FT_TABLE_MIN_NB_BK * FT_TABLE_BUCKET_SIZE;
    pool = test_aligned_calloc(n + 1u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    in_table = calloc(n, 1u);
    if (pool == NULL || in_table == NULL)
        FAIL("alloc failed");
    bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, bsz);
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, pool, n, struct test_user_record, entry, bk, bsz, &cfg) != 0)
        FAIL("init failed");

    for (unsigned op = 0; op < ops; op++) {
        seed = seed * 1103515245u + 12345u;
        unsigned idx0 = (seed >> 16) % n;
        unsigned action = (seed >> 8) & 3u;
        struct flow4_key key = test_key(idx0 + 50000u);
        u32 entry_idx = idx0 + 1u;

        switch (action) {
        case 0: /* insert */
            if (!in_table[idx0]) {
                u32 ret = test_add_idx_key(&ft, entry_idx, &key);

                if (ret == entry_idx) {
                    in_table[idx0] = 1u;
                    in_count++;
                }
                /* ret == 0 means table full, acceptable */
            }
            break;
        case 1: /* find */
        {
            u32 ret = FT4_FIND(&ft, &key);

            if (in_table[idx0] && ret != entry_idx)
                FAILF("fuzz find miss: op=%u idx0=%u", op, idx0);
            if (!in_table[idx0] && ret != 0u)
                FAILF("fuzz find ghost: op=%u idx0=%u", op, idx0);
            break;
        }
        case 2: /* delete */
            if (in_table[idx0]) {
                u32 ret = ft_flow4_table_del_key_oneshot(&ft, &key);

                if (ret != entry_idx)
                    FAILF("fuzz del mismatch: op=%u idx0=%u ret=%u",
                           op, idx0, ret);
                in_table[idx0] = 0u;
                in_count--;
            }
            break;
        default: /* find (bias toward reads) */
        {
            u32 ret = FT4_FIND(&ft, &key);

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

            if (FT4_FIND(&ft, &key) != i + 1u)
                FAILF("fuzz final find miss i=%u", i);
        }
    }

    ft_flow4_table_destroy(&ft);
    free(bk);
    free(pool);
    free(in_table);
    return 0;
}

static void
testv_bind_key(const struct test_variant_ops *ops, void *ft,
               u32 entry_idx, const void *key)
{
    void *entry = ops->entry_ptr(ft, entry_idx);

    if (entry == NULL) {
        fprintf(stderr, "FAIL: %s bind_key failed at idx=%u\n",
                ops->name, entry_idx);
        exit(1);
    }
    memcpy(entry, key, ops->key_size);
}

static struct flow_entry_meta *
testv_meta_ptr(const struct test_variant_ops *ops, void *ft, u32 entry_idx)
{
    unsigned char *entry = (unsigned char *)ops->entry_ptr(ft, entry_idx);

    if (entry == NULL)
        return NULL;
    return (struct flow_entry_meta *)(void *)(entry + ops->meta_offset);
}

static u32
testv_add_idx_key(const struct test_variant_ops *ops, void *ft,
                  u32 entry_idx, const void *key)
{
    testv_bind_key(ops, ft, entry_idx, key);
    return TEST_OPS_ADD_IDX(ops, ft, entry_idx);
}

static int
testv_prepare_bucket_for_add_maint(const struct test_variant_ops *ops,
                                   void *ft,
                                   void *keys,
                                   unsigned seed_cap,
                                   unsigned key_base,
                                   u32 *live_idx_out,
                                   u32 *old_idxv,
                                   unsigned *old_count_out,
                                   unsigned *inserted_out)
{
    unsigned nb_bk = ops->nb_bk(ft);
    unsigned mask = nb_bk - 1u;
    unsigned *bucket_counts = calloc(nb_bk, sizeof(*bucket_counts));
    unsigned inserted = 0u;
    unsigned target_bk = UINT_MAX;
    int rc = -1;

    if (bucket_counts == NULL)
        return -1;

    for (unsigned i = 0u; i < seed_cap; i++) {
        struct flow_entry_meta *meta;
        unsigned bk;

        ops->make_key(TEST_KEY_AT(keys, ops, i), key_base + i);
        testv_bind_key(ops, ft, i + 1u, TEST_KEY_AT(keys, ops, i));
        if (TEST_OPS_ADD_IDX(ops, ft, i + 1u) != i + 1u)
            goto out;
        meta = testv_meta_ptr(ops, ft, i + 1u);
        if (meta == NULL)
            goto out;
        bk = meta->cur_hash & mask;
        bucket_counts[bk]++;
        inserted = i + 1u;
        if (bucket_counts[bk] >= 2u) {
            target_bk = bk;
            break;
        }
    }
    if (target_bk == UINT_MAX)
        goto out;

    *live_idx_out = 0u;
    *old_count_out = 0u;
    for (unsigned idx = 1u; idx <= inserted; idx++) {
        struct flow_entry_meta *meta = testv_meta_ptr(ops, ft, idx);

        if (meta == NULL)
            goto out;
        if ((meta->cur_hash & mask) != target_bk)
            continue;
        if (*live_idx_out == 0u) {
            *live_idx_out = idx;
            flow_timestamp_store(meta, UINT64_C(200000),
                                 FLOW_TIMESTAMP_DEFAULT_SHIFT);
        } else {
            if (*old_count_out >= 16u)
                goto out;
            old_idxv[*old_count_out] = idx;
            (*old_count_out)++;
            flow_timestamp_store(meta, UINT64_C(16),
                                 FLOW_TIMESTAMP_DEFAULT_SHIFT);
        }
    }
    if (*live_idx_out == 0u || *old_count_out == 0u)
        goto out;

    if (inserted_out != NULL)
        *inserted_out = inserted;
    rc = 0;
out:
    free(bucket_counts);
    return rc;
}

static int
testv_basic_add_find_del(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(128u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key k1, k2;
    u32 idx1, idx2;

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
    if (TEST_OPS_FIND(ops, &ft, &k1) != idx1)
        FAIL("find should return idx1");
    if (TEST_OPS_FIND(ops, &ft, &k2) != idx2)
        FAIL("find should return idx2");
    if (testv_add_idx_key(ops, &ft, 3u, &k1) != idx1)
        FAIL("duplicate key add should return existing idx");
    if (ops->nb_entries(&ft) != 2u)
        FAIL("duplicate key add should not increase count");
    if (ops->del_key(&ft, &k1) != idx1)
        FAIL("del should return idx1");
    if (TEST_OPS_FIND(ops, &ft, &k1) != 0u)
        FAIL("deleted key should miss");
    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_init_ex_and_mapping(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *users = test_aligned_calloc(64u, ops->record_size,
                                      FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key;
    u32 idx;

    printf("[T] %s table init_ex mapping\n", ops->name);
    if (users == NULL)
        FAIL("calloc users");
    for (unsigned i = 0; i < 64u; i++) {
        u32 cookie = UINT32_C(0xabc00000) + i;
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
        u32 cookie = 0u;

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
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(256u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(8u, ops->key_size);
    u32 entry_idxv[8];
    struct ft_table_result find_results[8];
    u32 unused_idxv[8];
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

    if (TEST_OPS_ADD_IDX_BULK(ops, &ft, entry_idxv, 8u, FT_ADD_IGNORE,
                              unused_idxv) != 0u)
        FAIL("add_bulk inserted should not return unused idx");
    for (unsigned i = 0; i < 8u; i++) {
        if (entry_idxv[i] != i + 1u)
            FAILF("add_bulk failed at %u", i);
    }

    memset(find_results, 0, sizeof(find_results));
    TEST_OPS_FIND_BULK(ops, &ft, keys, 8u, find_results);
    for (unsigned i = 0; i < 8u; i++) {
        if (find_results[i].entry_idx == 0u)
            FAILF("find_bulk miss at %u", i);
    }

    for (unsigned i = 0; i < 4u; i++) {
        if (ops->del_key(&ft, TEST_KEY_AT(keys, ops, i)) == 0u)
            FAILF("del_key failed at %u", i);
    }

    memset(find_results, 0, sizeof(find_results));
    TEST_OPS_FIND_BULK(ops, &ft, keys, 8u, find_results);
    for (unsigned i = 0; i < 4u; i++) {
        if (find_results[i].entry_idx != 0u)
            FAILF("deleted key should miss at %u", i);
    }
    for (unsigned i = 4u; i < 8u; i++) {
        if (find_results[i].entry_idx == 0u)
            FAILF("remaining key should hit at %u", i);
    }

    ops->stats(&ft, &stats);
    if (stats.core.adds != 8u || stats.core.add_existing != 0u
        || stats.core.lookups != 16u || stats.core.hits != 12u
        || stats.core.misses != 4u || stats.core.dels != 4u
        || stats.core.del_miss != 0u)
        FAIL("bulk stats mismatch");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_add_idx_bulk_duplicate_ignore(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(64u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(4u, ops->key_size);
    u32 base_idxv[2] = { 1u, 2u };
    u32 dup_idxv[2] = { 3u, 4u };
    u32 unused_idxv[2];
    struct ft_table_stats stats;
    unsigned unused_n;

    printf("[T] %s table add_idx_bulk duplicate ignore\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, 64u, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 2u; i++) {
        ops->make_key(TEST_KEY_AT(keys, ops, i), i + 6000u);
        testv_bind_key(ops, &ft, base_idxv[i], TEST_KEY_AT(keys, ops, i));
    }
    unused_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, base_idxv, 2u, FT_ADD_IGNORE,
                                     unused_idxv);
    if (unused_n != 0u)
        FAIL("base insert should not return unused idx");

    for (unsigned i = 0; i < 2u; i++)
        testv_bind_key(ops, &ft, dup_idxv[i], TEST_KEY_AT(keys, ops, i));

    memset(unused_idxv, 0, sizeof(unused_idxv));
    unused_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, dup_idxv, 2u, FT_ADD_IGNORE,
                                     unused_idxv);
    if (unused_n != 2u)
        FAIL("duplicate ignore unused count mismatch");
    if (dup_idxv[0] != base_idxv[0])
        FAIL("duplicate ignore result mismatch at 0");
    if (dup_idxv[1] != base_idxv[1])
        FAIL("duplicate ignore result mismatch at 1");
    if (unused_idxv[0] != 3u || unused_idxv[1] != 4u)
        FAIL("duplicate ignore unused idx mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 0)) != base_idxv[0])
        FAIL("duplicate ignore retained idx mismatch at 0");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 1)) != base_idxv[1])
        FAIL("duplicate ignore retained idx mismatch at 1");
    if (ops->nb_entries(&ft) != 2u)
        FAIL("duplicate ignore should not increase entry count");

    ops->stats(&ft, &stats);
    if (stats.core.adds != 2u || stats.core.add_existing != 2u)
        FAIL("duplicate ignore stats mismatch");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_add_idx_bulk_mixed_batch(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(64u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(4u, ops->key_size);
    u32 base_idxv[2] = { 1u, 2u };
    u32 mix_idxv[5] = { 7u, 3u, 4u, 5u, 6u };
    u32 unused_idxv[5];
    struct ft_table_stats stats;
    unsigned unused_n;

    printf("[T] %s table add_idx_bulk mixed batch\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, 64u, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 2u; i++) {
        ops->make_key(TEST_KEY_AT(keys, ops, i), i + 6100u);
        testv_bind_key(ops, &ft, base_idxv[i], TEST_KEY_AT(keys, ops, i));
    }
    unused_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, base_idxv, 2u, FT_ADD_IGNORE,
                                     unused_idxv);
    if (unused_n != 0u)
        FAIL("base insert should not return unused idx");

    ops->make_key(TEST_KEY_AT(keys, ops, 2u), 6200u);
    ops->make_key(TEST_KEY_AT(keys, ops, 3u), 6201u);
    testv_bind_key(ops, &ft, 7u, TEST_KEY_AT(keys, ops, 0));
    testv_bind_key(ops, &ft, 3u, TEST_KEY_AT(keys, ops, 1));
    testv_bind_key(ops, &ft, 4u, TEST_KEY_AT(keys, ops, 2u));
    testv_bind_key(ops, &ft, 5u, TEST_KEY_AT(keys, ops, 2u));
    testv_bind_key(ops, &ft, 6u, TEST_KEY_AT(keys, ops, 3u));

    memset(unused_idxv, 0, sizeof(unused_idxv));
    unused_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, mix_idxv, 5u, FT_ADD_IGNORE,
                                     unused_idxv);
    if (unused_n != 3u)
        FAIL("mixed batch unused count mismatch");
    if (mix_idxv[0] != 1u)
        FAIL("mixed key-duplicate result mismatch");
    if (mix_idxv[1] != 2u)
        FAIL("mixed existing-duplicate result mismatch");
    if (mix_idxv[2] != 4u)
        FAIL("mixed insert result mismatch at 2");
    if (mix_idxv[3] != 4u)
        FAIL("mixed batch-duplicate result mismatch");
    if (mix_idxv[4] != 6u)
        FAIL("mixed insert result mismatch at 4");
    if (unused_idxv[0] != 7u || unused_idxv[1] != 3u || unused_idxv[2] != 5u)
        FAIL("mixed batch unused idx mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 0)) != 1u)
        FAIL("mixed key-duplicate retained idx mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 1)) != 2u)
        FAIL("mixed existing-duplicate retained idx mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 2u)) != 4u)
        FAIL("mixed inserted/duplicated key mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 3u)) != 6u)
        FAIL("mixed inserted key mismatch");
    if (ops->nb_entries(&ft) != 4u)
        FAIL("mixed batch should add exactly two entries");

    ops->stats(&ft, &stats);
    if (stats.core.adds != 4u || stats.core.add_existing != 3u)
        FAIL("mixed batch stats mismatch");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_add_idx_bulk_policy(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(64u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(16u, ops->key_size);
    u32 base_idxv[4] = { 1u, 2u, 3u, 4u };
    u32 dup_idxv[4] = { 5u, 6u, 7u, 8u };
    u32 ins_idxv[4] = { 9u, 10u, 11u, 12u };
    u32 mix_idxv[4] = { 20u, 13u, 14u, 15u };
    u32 unused_idxv[4];
    unsigned free_n;

    printf("[T] %s table add_idx_bulk policy\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, 64u, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 4u; i++) {
        ops->make_key(TEST_KEY_AT(keys, ops, i), i + 7000u);
        testv_bind_key(ops, &ft, base_idxv[i], TEST_KEY_AT(keys, ops, i));
    }
    free_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, base_idxv, 4u, FT_ADD_IGNORE,
                                   unused_idxv);
    if (free_n != 0u)
        FAIL("base insert should not return unused idx");

    for (unsigned i = 0; i < 4u; i++)
        testv_bind_key(ops, &ft, dup_idxv[i], TEST_KEY_AT(keys, ops, i));

    memset(unused_idxv, 0xff, sizeof(unused_idxv));
    free_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, dup_idxv, 4u, FT_ADD_IGNORE,
                                   unused_idxv);
    if (free_n != 4u)
        FAILF("ignore free_n=%u", free_n);
    for (unsigned i = 0; i < 4u; i++) {
        if (dup_idxv[i] != base_idxv[i])
            FAILF("ignore result mismatch at %u", i);
        if (unused_idxv[i] != i + 5u)
            FAILF("ignore unused mismatch at %u", i);
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, i)) != base_idxv[i])
            FAILF("ignore find mismatch at %u", i);
    }

    dup_idxv[0] = 5u;
    dup_idxv[1] = 6u;
    dup_idxv[2] = 7u;
    dup_idxv[3] = 8u;
    memset(unused_idxv, 0xff, sizeof(unused_idxv));
    free_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, dup_idxv, 4u, FT_ADD_UPDATE,
                                   unused_idxv);
    if (free_n != 4u)
        FAILF("update free_n=%u", free_n);
    for (unsigned i = 0; i < 4u; i++) {
        if (dup_idxv[i] != i + 5u)
            FAILF("update result mismatch at %u", i);
        if (unused_idxv[i] != base_idxv[i])
            FAILF("update unused mismatch at %u", i);
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, i)) != dup_idxv[i])
            FAILF("update find mismatch at %u", i);
    }

    for (unsigned i = 0; i < 4u; i++) {
        ops->make_key(TEST_KEY_AT(keys, ops, i + 4u), i + 8000u);
        testv_bind_key(ops, &ft, ins_idxv[i], TEST_KEY_AT(keys, ops, i + 4u));
    }
    memset(unused_idxv, 0xff, sizeof(unused_idxv));
    free_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, ins_idxv, 4u, FT_ADD_IGNORE,
                                   unused_idxv);
    if (free_n != 0u)
        FAILF("insert free_n=%u", free_n);
    for (unsigned i = 0; i < 4u; i++) {
        if (ins_idxv[i] != i + 9u)
            FAILF("insert result mismatch at %u", i);
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, i + 4u)) != ins_idxv[i])
            FAILF("insert find mismatch at %u", i);
    }

    testv_bind_key(ops, &ft, mix_idxv[0], TEST_KEY_AT(keys, ops, 0));
    testv_bind_key(ops, &ft, mix_idxv[1], TEST_KEY_AT(keys, ops, 1));
    ops->make_key(TEST_KEY_AT(keys, ops, 8u), 9000u);
    testv_bind_key(ops, &ft, mix_idxv[2], TEST_KEY_AT(keys, ops, 8u));
    ops->make_key(TEST_KEY_AT(keys, ops, 9u), 9001u);
    testv_bind_key(ops, &ft, mix_idxv[3], TEST_KEY_AT(keys, ops, 9u));

    memset(unused_idxv, 0xff, sizeof(unused_idxv));
    free_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, mix_idxv, 4u, FT_ADD_IGNORE,
                                   unused_idxv);
    if (free_n != 2u)
        FAILF("mixed ignore free_n=%u", free_n);
    if (mix_idxv[0] != 5u)
        FAIL("mixed ignore key-dup result mismatch");
    if (mix_idxv[1] != dup_idxv[1])
        FAIL("mixed ignore dup result mismatch");
    if (mix_idxv[2] != 14u || mix_idxv[3] != 15u)
        FAIL("mixed ignore insert result mismatch");
    if (unused_idxv[0] != 20u || unused_idxv[1] != 13u)
        FAIL("mixed ignore unused mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 1)) != dup_idxv[1])
        FAIL("mixed ignore retained idx mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 8u)) != mix_idxv[2] ||
        TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 9u)) != mix_idxv[3])
        FAIL("mixed ignore inserted find mismatch");

    ops->make_key(TEST_KEY_AT(keys, ops, 10u), 9002u);
    ops->make_key(TEST_KEY_AT(keys, ops, 11u), 9003u);
    testv_bind_key(ops, &ft, 16u, TEST_KEY_AT(keys, ops, 0));
    testv_bind_key(ops, &ft, 17u, TEST_KEY_AT(keys, ops, 1));
    testv_bind_key(ops, &ft, 18u, TEST_KEY_AT(keys, ops, 10u));
    testv_bind_key(ops, &ft, 19u, TEST_KEY_AT(keys, ops, 11u));
    {
        u32 upd_mix_idxv[4] = { 16u, 17u, 18u, 19u };

        memset(unused_idxv, 0xff, sizeof(unused_idxv));
        free_n = TEST_OPS_ADD_IDX_BULK(ops, &ft, upd_mix_idxv, 4u,
                                       FT_ADD_UPDATE, unused_idxv);
        if (free_n != 2u)
            FAILF("mixed update free_n=%u", free_n);
        if (upd_mix_idxv[0] != 16u)
            FAIL("mixed update key-dup result mismatch");
        if (upd_mix_idxv[1] != 17u)
            FAIL("mixed update replace result mismatch");
        if (upd_mix_idxv[2] != 18u || upd_mix_idxv[3] != 19u)
            FAIL("mixed update insert result mismatch");
        if (unused_idxv[0] != 5u || unused_idxv[1] != dup_idxv[1])
            FAIL("mixed update unused mismatch");
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 0)) != 16u)
            FAIL("mixed update key-dup find mismatch");
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 1)) != 17u)
            FAIL("mixed update replaced idx mismatch");
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 10u)) != 18u ||
            TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 11u)) != 19u)
            FAIL("mixed update inserted find mismatch");
    }

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_add_idx_bulk_maint_duplicate_reclaims(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(2048u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(2048u, ops->key_size);
    u32 old_idxv[16];
    u32 unused_idxv[17];
    u8 seen_old[2049];
    u32 req_idxv[1];
    u32 live_idx;
    unsigned old_count;
    unsigned inserted;
    unsigned unused_n;
    struct ft_table_stats stats;

    printf("[T] %s table add_idx_bulk_maint duplicate reclaims\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, 2048u, &cfg) != 0)
        FAIL("init failed");
    if (testv_prepare_bucket_for_add_maint(ops, &ft, keys, 2048u, 12000u,
                                           &live_idx, old_idxv, &old_count,
                                           &inserted) != 0)
        FAIL("failed to prepare target bucket");

    req_idxv[0] = inserted + 1u;
    testv_bind_key(ops, &ft, req_idxv[0], TEST_KEY_AT(keys, ops, live_idx - 1u));
    memset(unused_idxv, 0, sizeof(unused_idxv));
    memset(seen_old, 0, sizeof(seen_old));

    unused_n = TEST_OPS_ADD_IDX_BULK_MAINT(ops, &ft, req_idxv, 1u,
                                           FT_ADD_IGNORE, UINT64_C(200000),
                                           UINT64_C(100000), unused_idxv,
                                           1u + old_count, 1u);
    if (unused_n != old_count + 1u)
        FAIL("add_idx_bulk_maint unused count mismatch");
    if (req_idxv[0] != live_idx)
        FAIL("add_idx_bulk_maint duplicate result mismatch");
    if (unused_idxv[0] != inserted + 1u)
        FAIL("add_idx_bulk_maint should return request idx first");
    for (unsigned i = 0u; i < old_count; i++) {
        u32 idx = unused_idxv[i + 1u];

        if (idx == 0u || idx > inserted)
            FAIL("add_idx_bulk_maint expired idx invalid");
        seen_old[idx] = 1u;
    }
    for (unsigned i = 0u; i < old_count; i++) {
        if (seen_old[old_idxv[i]] == 0u)
            FAIL("add_idx_bulk_maint missed expired idx");
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, old_idxv[i] - 1u)) != 0u)
            FAIL("expired key should miss after add_idx_bulk_maint");
    }
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, live_idx - 1u)) != live_idx)
        FAIL("kept key should still hit after add_idx_bulk_maint");

    ops->stats(&ft, &stats);
    if (stats.maint_calls != 1u)
        FAIL("add_idx_bulk_maint should increment maint_calls once");
    if (stats.maint_evictions != old_count)
        FAIL("add_idx_bulk_maint maint_evictions mismatch");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_add_idx_bulk_maint_zero_extra_skips(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(2048u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(2048u, ops->key_size);
    u32 old_idxv[16];
    u32 unused_idxv[1];
    u32 req_idxv[1];
    u32 live_idx;
    unsigned old_count;
    unsigned inserted;
    unsigned unused_n;
    struct ft_table_stats stats;

    printf("[T] %s table add_idx_bulk_maint zero-extra skip\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, 2048u, &cfg) != 0)
        FAIL("init failed");
    if (testv_prepare_bucket_for_add_maint(ops, &ft, keys, 2048u, 14000u,
                                           &live_idx, old_idxv, &old_count,
                                           &inserted) != 0)
        FAIL("failed to prepare target bucket");

    req_idxv[0] = inserted + 1u;
    testv_bind_key(ops, &ft, req_idxv[0], TEST_KEY_AT(keys, ops, live_idx - 1u));
    unused_idxv[0] = 0u;

    unused_n = TEST_OPS_ADD_IDX_BULK_MAINT(ops, &ft, req_idxv, 1u,
                                           FT_ADD_IGNORE, UINT64_C(200000),
                                           UINT64_C(100000), unused_idxv,
                                           1u, 1u);
    if (unused_n != 1u)
        FAIL("zero-extra add_idx_bulk_maint should only return add-unused");
    if (req_idxv[0] != live_idx)
        FAIL("zero-extra add_idx_bulk_maint duplicate result mismatch");
    if (unused_idxv[0] != inserted + 1u)
        FAIL("zero-extra add_idx_bulk_maint unused idx mismatch");
    for (unsigned i = 0u; i < old_count; i++) {
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, old_idxv[i] - 1u))
            != old_idxv[i])
            FAIL("zero-extra add_idx_bulk_maint should skip expiry");
    }

    ops->stats(&ft, &stats);
    if (stats.maint_calls != 0u || stats.maint_bucket_checks != 0u
        || stats.maint_evictions != 0u)
        FAIL("zero-extra add_idx_bulk_maint should not touch maint stats");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_timestamp_update(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    struct ft_table_config cfg_shift = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(32u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(4u, ops->key_size);
    u32 idxv[1] = { 2u };
    u32 unused_idxv[1];
    unsigned unused_n;
    struct flow_entry_meta *meta1;
    struct flow_entry_meta *meta2;

    printf("[T] %s table timestamp update\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    cfg_shift.ts_shift = 1u;
    if (ops->init(&ft, pool, 32u, &cfg) != 0)
        FAIL("init failed");

    ops->make_key(TEST_KEY_AT(keys, ops, 0), 9100u);
    testv_bind_key(ops, &ft, 1u, TEST_KEY_AT(keys, ops, 0));
    if (TEST_OPS_ADD_IDX(ops, &ft, 1u) != 1u)
        FAIL("insert failed");
    meta1 = testv_meta_ptr(ops, &ft, 1u);
    if (meta1 == NULL || flow_timestamp_get(meta1) !=
        flow_timestamp_encode(TEST_NOW_ADD, FLOW_TIMESTAMP_DEFAULT_SHIFT))
        FAIL("insert timestamp mismatch");

    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 0)) != 1u)
        FAIL("find hit failed");
    if (flow_timestamp_get(meta1) !=
        flow_timestamp_encode(TEST_NOW_FIND, FLOW_TIMESTAMP_DEFAULT_SHIFT))
        FAIL("find timestamp mismatch");

    testv_bind_key(ops, &ft, 2u, TEST_KEY_AT(keys, ops, 0));
    unused_n = ops->add_idx_bulk(&ft, idxv, 1u, FT_ADD_IGNORE,
                                 TEST_NOW_DUP, unused_idxv);
    if (unused_n != 1u || idxv[0] != 1u || unused_idxv[0] != 2u)
        FAIL("ignore duplicate result mismatch");
    if (flow_timestamp_get(meta1) !=
        flow_timestamp_encode(TEST_NOW_DUP, FLOW_TIMESTAMP_DEFAULT_SHIFT))
        FAIL("ignore duplicate timestamp mismatch");
    meta2 = testv_meta_ptr(ops, &ft, 2u);
    if (meta2 == NULL || !flow_timestamp_is_zero(meta2))
        FAIL("ignore duplicate request timestamp mismatch");

    idxv[0] = 2u;
    unused_idxv[0] = 0u;
    unused_n = ops->add_idx_bulk(&ft, idxv, 1u, FT_ADD_UPDATE,
                                 TEST_NOW_ADD + 1u, unused_idxv);
    if (unused_n != 1u || idxv[0] != 2u || unused_idxv[0] != 1u)
        FAIL("update duplicate result mismatch");
    meta1 = testv_meta_ptr(ops, &ft, 1u);
    meta2 = testv_meta_ptr(ops, &ft, 2u);
    if (meta1 == NULL || meta2 == NULL)
        FAIL("meta ptr failed");
    if (!flow_timestamp_is_zero(meta1))
        FAIL("updated-out entry timestamp should clear");
    if (flow_timestamp_get(meta2) !=
        flow_timestamp_encode(TEST_NOW_ADD + 1u, FLOW_TIMESTAMP_DEFAULT_SHIFT))
        FAIL("updated-in entry timestamp mismatch");

    ops->destroy(&ft);
    memset(pool, 0, ops->record_size * 32u);
    memset(keys, 0, ops->key_size * 4u);
    if (ops->init(&ft, pool, 32u, &cfg_shift) != 0)
        FAIL("shifted init failed");

    ops->make_key(TEST_KEY_AT(keys, ops, 0), 9200u);
    testv_bind_key(ops, &ft, 1u, TEST_KEY_AT(keys, ops, 0));
    if (TEST_OPS_ADD_IDX(ops, &ft, 1u) != 1u)
        FAIL("shifted insert failed");
    meta1 = testv_meta_ptr(ops, &ft, 1u);
    if (meta1 == NULL || flow_timestamp_get(meta1) !=
        flow_timestamp_encode(TEST_NOW_ADD, cfg_shift.ts_shift))
        FAIL("shifted insert timestamp mismatch");
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 0)) != 1u)
        FAIL("shifted find hit failed");
    if (flow_timestamp_get(meta1) !=
        flow_timestamp_encode(TEST_NOW_FIND, cfg_shift.ts_shift))
        FAIL("shifted find timestamp mismatch");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_permanent_timestamp(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(32u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(4u, ops->key_size);
    u32 idxv[1];
    u32 unused_idxv[1];
    struct flow_entry_meta *meta1;
    struct flow_entry_meta *meta2;
    unsigned unused_n;

    printf("[T] %s table permanent timestamp\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, 32u, &cfg) != 0)
        FAIL("init failed");

    ops->make_key(TEST_KEY_AT(keys, ops, 0u), 3000u);
    testv_bind_key(ops, &ft, 1u, TEST_KEY_AT(keys, ops, 0u));
    if (TEST_OPS_ADD_IDX(ops, &ft, 1u) != 1u)
        FAIL("add failed");
    if (ops->set_permanent_idx(&ft, 1u) != 0)
        FAIL("set_permanent_idx failed");

    meta1 = testv_meta_ptr(ops, &ft, 1u);
    if (meta1 == NULL || !flow_timestamp_is_zero(meta1))
        FAIL("permanent entry should have zero timestamp");

    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, 0u)) != 1u)
        FAIL("permanent entry find failed");
    if (!flow_timestamp_is_zero(meta1))
        FAIL("find should not update permanent timestamp");

    testv_bind_key(ops, &ft, 2u, TEST_KEY_AT(keys, ops, 0u));
    idxv[0] = 2u;
    unused_idxv[0] = 0u;
    unused_n = ops->add_idx_bulk(&ft, idxv, 1u, FT_ADD_IGNORE,
                                 TEST_NOW_DUP, unused_idxv);
    if (unused_n != 1u || idxv[0] != 1u || unused_idxv[0] != 2u)
        FAIL("permanent duplicate ignore mismatch");
    if (!flow_timestamp_is_zero(meta1))
        FAIL("ignore duplicate should not update permanent timestamp");

    meta2 = testv_meta_ptr(ops, &ft, 2u);
    if (meta2 == NULL || !flow_timestamp_is_zero(meta2))
        FAIL("ignored request timestamp should stay zero");

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_force_expire_basic(const struct test_variant_ops *ops)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 64u;
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(phase1_entries + overflow + 1u,
                                     ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key;
    struct ft_table_stats stats;
    unsigned inserted = 0u;
    unsigned add_failed_before;

    printf("[T] %s table force_expire basic\n", ops->name);
    if (pool == NULL)
        FAIL("alloc failed");
    if (ops->init_with_bucket_entries(&ft, pool, phase1_entries + overflow,
                                      bucket_entries, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0u; i < phase1_entries; i++) {
        ops->make_key(&key, i + 40000u);
        if (testv_add_idx_key(ops, &ft, i + 1u, &key) != 0u)
            inserted++;
    }
    printf("  phase1: inserted %u / %u\n", inserted, phase1_entries);
    ops->stats(&ft, &stats);
    add_failed_before = (unsigned)stats.core.add_failed;
    if (add_failed_before == 0u)
        FAIL("expected some add failures at high fill");

    {
        unsigned nb_before = ops->nb_entries(&ft);

        for (unsigned i = 0u; i < overflow; i++) {
            u32 entry_idxv[1] = { phase1_entries + i + 1u };
            u32 unused_idxv[1] = { 0u };

            ops->make_key(&key, phase1_entries + i + 40000u);
            testv_bind_key(ops, &ft, entry_idxv[0], &key);
            ops->add_idx_bulk(&ft, entry_idxv, 1u,
                              FT_ADD_IGNORE_FORCE_EXPIRE,
                              TEST_NOW_ADD, unused_idxv);
            if (entry_idxv[0] == 0u)
                FAILF("force_expire add failed at i=%u", i);
            if (TEST_OPS_FIND(ops, &ft, &key) != entry_idxv[0])
                FAILF("force_expire entry not findable at i=%u", i);
        }

        ops->stats(&ft, &stats);
        printf("  force_expired=%llu add_failed=%llu\n",
               (unsigned long long)stats.core.force_expired,
               (unsigned long long)stats.core.add_failed);
        if (stats.core.force_expired == 0u)
            FAIL("force_expired should be > 0");

        {
            unsigned nb_after = ops->nb_entries(&ft);
            unsigned fe = (unsigned)stats.core.force_expired;
            unsigned expected = nb_before + overflow - fe;

            if (nb_after != expected)
                FAILF("entry count mismatch: %u (expected %u)",
                      nb_after, expected);
        }
    }

    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_force_expire_permanent_skip(const struct test_variant_ops *ops)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 128u;
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(phase1_entries + overflow + 1u,
                                     ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key;
    struct ft_table_stats stats;
    unsigned inserted = 0u;
    u64 add_failed_before;

    printf("[T] %s table force_expire permanent skip\n", ops->name);
    if (pool == NULL)
        FAIL("alloc failed");
    if (ops->init_with_bucket_entries(&ft, pool, phase1_entries + overflow,
                                      bucket_entries, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0u; i < phase1_entries; i++) {
        u32 entry_idxv[1] = { i + 1u };
        u32 unused_idxv[1] = { 0u };

        ops->make_key(&key, i + 50000u);
        testv_bind_key(ops, &ft, entry_idxv[0], &key);
        ops->add_idx_bulk(&ft, entry_idxv, 1u, FT_ADD_IGNORE, 0u,
                          unused_idxv);
        if (entry_idxv[0] != 0u)
            inserted++;
    }
    printf("  filled %u permanent entries\n", inserted);

    ops->stats(&ft, &stats);
    if (stats.core.add_failed == 0u)
        FAIL("expected some add failures at high fill");
    add_failed_before = stats.core.add_failed;

    for (unsigned i = 0u; i < overflow; i++) {
        u32 entry_idxv[1] = { phase1_entries + i + 1u };
        u32 unused_idxv[1] = { 0u };

        ops->make_key(&key, phase1_entries + i + 50000u);
        testv_bind_key(ops, &ft, entry_idxv[0], &key);
        ops->add_idx_bulk(&ft, entry_idxv, 1u,
                          FT_ADD_IGNORE_FORCE_EXPIRE,
                          0u, unused_idxv);
    }

    ops->stats(&ft, &stats);
    printf("  add_failed: %llu -> %llu, force_expired=%llu\n",
           (unsigned long long)add_failed_before,
           (unsigned long long)stats.core.add_failed,
           (unsigned long long)stats.core.force_expired);
    if (stats.core.force_expired != 0u)
        FAILF("force_expired should be 0 with all permanent, got %llu",
              (unsigned long long)stats.core.force_expired);
    if (stats.core.add_failed <= add_failed_before)
        FAIL("expected add_failed to increase (some kickouts should fail)");

    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_force_expire_bulk(const struct test_variant_ops *ops)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 8u;
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(phase1_entries + overflow + 1u,
                                     ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *keys = calloc(overflow, ops->key_size);
    struct ft_table_stats stats;
    unsigned inserted = 0u;

    printf("[T] %s table force_expire bulk\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");
    if (ops->init_with_bucket_entries(&ft, pool, phase1_entries + overflow,
                                      bucket_entries, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0u; i < phase1_entries; i++) {
        ops->make_key(TEST_KEY_AT(keys, ops, 0u), i + 60000u);
        if (testv_add_idx_key(ops, &ft, i + 1u, TEST_KEY_AT(keys, ops, 0u))
            != 0u)
            inserted++;
    }
    printf("  filled %u entries\n", inserted);
    ops->stats(&ft, &stats);
    if (stats.core.add_failed == 0u)
        FAIL("expected some add failures at high fill");

    {
        unsigned nb_before = ops->nb_entries(&ft);
        u32 entry_idxv[8];
        u32 unused_idxv[8];

        for (unsigned i = 0u; i < overflow; i++) {
            entry_idxv[i] = phase1_entries + i + 1u;
            ops->make_key(TEST_KEY_AT(keys, ops, i),
                          phase1_entries + i + 60000u);
            testv_bind_key(ops, &ft, entry_idxv[i], TEST_KEY_AT(keys, ops, i));
        }
        TEST_OPS_ADD_IDX_BULK(ops, &ft, entry_idxv, overflow,
                              FT_ADD_UPDATE_FORCE_EXPIRE,
                              unused_idxv);
        for (unsigned i = 0u; i < overflow; i++) {
            if (entry_idxv[i] == 0u)
                FAILF("bulk force_expire add failed at i=%u", i);
            if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, i)) == 0u)
                FAILF("bulk force_expire entry not findable at i=%u", i);
        }

        ops->stats(&ft, &stats);
        printf("  force_expired=%llu\n",
               (unsigned long long)stats.core.force_expired);
        if (stats.core.force_expired == 0u)
            FAIL("force_expired should be > 0");

        {
            unsigned nb_after = ops->nb_entries(&ft);
            unsigned fe = (unsigned)stats.core.force_expired;
            unsigned expected = nb_before + overflow - fe;

            if (nb_after != expected)
                FAILF("entry count mismatch: %u (expected %u)",
                      nb_after, expected);
        }
    }

    ops->destroy(&ft);
    free(keys);
    free(pool);
    return 0;
}

static int
testv_force_expire_evicts_oldest(const struct test_variant_ops *ops)
{
    const unsigned bucket_entries = TEST_FORCE_EXPIRE_BUCKET_ENTRIES;
    const unsigned phase1_entries = TEST_FORCE_EXPIRE_PHASE1_ENTRIES;
    const unsigned overflow = 128u;
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(phase1_entries + overflow + 1u,
                                     ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key, evicted_key;
    struct ft_table_stats stats;
    unsigned inserted = 0u;
    u64 base_now = UINT64_C(0x10000);

    printf("[T] %s table force_expire evicts oldest\n", ops->name);
    if (pool == NULL)
        FAIL("alloc failed");
    if (ops->init_with_bucket_entries(&ft, pool, phase1_entries + overflow,
                                      bucket_entries, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0u; i < phase1_entries; i++) {
        u32 entry_idxv[1] = { i + 1u };
        u32 unused_idxv[1] = { 0u };
        u64 now = base_now + (u64)i * 256u;

        ops->make_key(&key, i + 70000u);
        testv_bind_key(ops, &ft, entry_idxv[0], &key);
        ops->add_idx_bulk(&ft, entry_idxv, 1u, FT_ADD_IGNORE, now,
                          unused_idxv);
        if (entry_idxv[0] != 0u)
            inserted++;
    }
    printf("  filled %u entries with progressive timestamps\n", inserted);
    ops->stats(&ft, &stats);
    if (stats.core.add_failed == 0u)
        FAIL("expected some add failures at high fill");

    {
        int eviction_found = 0;

        for (unsigned i = 0u; i < overflow; i++) {
            u32 entry_idxv[1] = { phase1_entries + i + 1u };
            u32 unused_idxv[1] = { 0u };
            u64 now = base_now + (u64)(phase1_entries + i) * 256u;
            u32 evicted_idx;
            struct flow_entry_meta *evicted_meta;

            ops->make_key(&key, phase1_entries + i + 70000u);
            testv_bind_key(ops, &ft, entry_idxv[0], &key);
            ops->add_idx_bulk(&ft, entry_idxv, 1u,
                              FT_ADD_IGNORE_FORCE_EXPIRE, now, unused_idxv);
            if (unused_idxv[0] == 0u)
                continue;

            evicted_idx = unused_idxv[0];
            printf("  evicted idx=%u at attempt %u\n", evicted_idx, i);
            evicted_meta = testv_meta_ptr(ops, &ft, evicted_idx);
            if (evicted_meta == NULL)
                FAIL("evicted meta ptr NULL");
            if (!flow_timestamp_is_zero(evicted_meta))
                FAIL("evicted entry timestamp not cleared");
            if (TEST_OPS_FIND(ops, &ft, &key) == 0u)
                FAIL("new entry not findable after force_expire");

            ops->make_key(&evicted_key, evicted_idx - 1u + 70000u);
            if (TEST_OPS_FIND(ops, &ft, &evicted_key) != 0u)
                FAIL("evicted entry should not be findable");

            eviction_found = 1;
            break;
        }
        if (!eviction_found)
            FAIL("no force_expire eviction triggered in overflow entries");
    }

    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
test_variant_force_expire_suite(const struct test_variant_ops *ops)
{
    if (testv_force_expire_basic(ops) != 0)
        return 1;
    if (testv_force_expire_permanent_skip(ops) != 0)
        return 1;
    if (testv_force_expire_bulk(ops) != 0)
        return 1;
    if (testv_force_expire_evicts_oldest(ops) != 0)
        return 1;
    return 0;
}

static int
testv_maintain_idx_bulk(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    const u64 expire_tsc = UINT64_C(100000);
    union test_any_table ft;
    void *pool = test_aligned_calloc(2048u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    void *pool2 = NULL;
    void *keys = calloc(2048u, ops->key_size);
    unsigned *bucket_of_idx = NULL;
    unsigned *bucket_counts = NULL;
    u32 expired_idxv[16];
    u32 hit_idxv[1];
    unsigned target_bk = UINT_MAX;
    unsigned nb_bk;
    unsigned inserted = 0u;
    unsigned old_count = 0u;
    u32 old_idxv[16];
    u8 seen_old[129];
    unsigned evicted;

    printf("[T] %s table maintain_idx_bulk\n", ops->name);
    if (pool == NULL || keys == NULL)
        FAIL("alloc failed");

    if (ops->init(&ft, pool, 2048u, &cfg) != 0)
        FAIL("init failed");
    nb_bk = ops->nb_bk(&ft);
    bucket_of_idx = calloc(2049u, sizeof(*bucket_of_idx));
    bucket_counts = calloc(nb_bk, sizeof(*bucket_counts));
    if (bucket_of_idx == NULL || bucket_counts == NULL)
        FAIL("alloc bucket state failed");

    for (unsigned i = 0u; i < 2048u; i++) {
        struct flow_entry_meta *meta;
        unsigned bk;

        ops->make_key(TEST_KEY_AT(keys, ops, i), i + 10000u);
        testv_bind_key(ops, &ft, i + 1u, TEST_KEY_AT(keys, ops, i));
        if (TEST_OPS_ADD_IDX(ops, &ft, i + 1u) != i + 1u)
            FAIL("seed add failed");
        meta = testv_meta_ptr(ops, &ft, i + 1u);
        if (meta == NULL)
            FAIL("meta ptr failed");
        bk = meta->cur_hash & (ops->nb_bk(&ft) - 1u);
        bucket_of_idx[i + 1u] = bk;
        bucket_counts[bk]++;
        inserted = i + 1u;
        if (bucket_counts[bk] >= 2u) {
            target_bk = bk;
            break;
        }
    }
    if (target_bk == UINT_MAX)
        FAIL("failed to find bucket with multiple entries");

    memset(seen_old, 0, sizeof(seen_old));
    hit_idxv[0] = 0u;
    for (unsigned idx = 1u; idx <= inserted; idx++) {
        struct flow_entry_meta *meta;

        if (bucket_of_idx[idx] != target_bk)
            continue;
        meta = testv_meta_ptr(ops, &ft, idx);
        if (meta == NULL)
            FAIL("meta ptr failed");
        if (hit_idxv[0] == 0u) {
            hit_idxv[0] = idx;
            flow_timestamp_store(meta, UINT64_C(200000),
                                 FLOW_TIMESTAMP_DEFAULT_SHIFT);
        } else {
            if (old_count >= 16u)
                FAIL("too many entries in target bucket for test");
            old_idxv[old_count++] = idx;
            flow_timestamp_store(meta, UINT64_C(16), FLOW_TIMESTAMP_DEFAULT_SHIFT);
        }
    }
    if (hit_idxv[0] == 0u || old_count == 0u)
        FAIL("target bucket selection failed");

    /* Test with enable_filter=0 */
    memset(expired_idxv, 0, sizeof(expired_idxv));
    evicted = ops->maintain_idx_bulk(&ft, hit_idxv, 1u, UINT64_C(200000),
                                     expire_tsc, expired_idxv, 16u, 1u, 0);
    if (evicted != old_count)
        FAIL("maintain_idx_bulk evicted count mismatch");
    for (unsigned i = 0u; i < evicted; i++) {
        u32 idx = expired_idxv[i];

        if (idx == 0u || idx > inserted)
            FAIL("maintain_idx_bulk expired idx invalid");
        if (bucket_of_idx[idx] != target_bk)
            FAIL("maintain_idx_bulk expired wrong bucket");
        seen_old[idx] = 1u;
    }
    for (unsigned i = 0u; i < old_count; i++) {
        if (seen_old[old_idxv[i]] == 0u)
            FAIL("maintain_idx_bulk missed expired idx");
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, old_idxv[i] - 1u)) != 0u)
            FAIL("expired key should miss after maintain_idx_bulk");
    }
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, hit_idxv[0] - 1u))
        != hit_idxv[0])
        FAIL("kept key should still hit after maintain_idx_bulk");

    /* Test with enable_filter=1 using a fresh table */
    ops->destroy(&ft);
    memset(bucket_counts, 0, nb_bk * sizeof(*bucket_counts));
    memset(bucket_of_idx, 0, 2049u * sizeof(*bucket_of_idx));
    pool2 = test_aligned_calloc(2048u, ops->record_size,
                                FT_TABLE_CACHE_LINE_SIZE);
    if (pool2 == NULL)
        FAIL("alloc pool2 failed");
    if (ops->init(&ft, pool2, 2048u, &cfg) != 0)
        FAIL("init pool2 failed");

    target_bk = UINT_MAX;
    inserted = 0u;
    for (unsigned i = 0u; i < 2048u; i++) {
        struct flow_entry_meta *meta;
        unsigned bk;

        ops->make_key(TEST_KEY_AT(keys, ops, i), i + 20000u);
        testv_bind_key(ops, &ft, i + 1u, TEST_KEY_AT(keys, ops, i));
        if (TEST_OPS_ADD_IDX(ops, &ft, i + 1u) != i + 1u)
            FAIL("seed add (filtered) failed");
        meta = testv_meta_ptr(ops, &ft, i + 1u);
        if (meta == NULL)
            FAIL("meta ptr (filtered) failed");
        bk = meta->cur_hash & (ops->nb_bk(&ft) - 1u);
        bucket_of_idx[i + 1u] = bk;
        bucket_counts[bk]++;
        inserted = i + 1u;
        if (bucket_counts[bk] >= 2u) {
            target_bk = bk;
            break;
        }
    }
    if (target_bk == UINT_MAX)
        FAIL("failed to find bucket (filtered)");

    memset(seen_old, 0, sizeof(seen_old));
    hit_idxv[0] = 0u;
    old_count = 0u;
    for (unsigned idx = 1u; idx <= inserted; idx++) {
        struct flow_entry_meta *meta;

        if (bucket_of_idx[idx] != target_bk)
            continue;
        meta = testv_meta_ptr(ops, &ft, idx);
        if (meta == NULL)
            FAIL("meta ptr (filtered) failed");
        if (hit_idxv[0] == 0u) {
            hit_idxv[0] = idx;
            flow_timestamp_store(meta, UINT64_C(200000),
                                 FLOW_TIMESTAMP_DEFAULT_SHIFT);
        } else {
            if (old_count >= 16u)
                FAIL("too many entries in target bucket (filtered)");
            old_idxv[old_count++] = idx;
            flow_timestamp_store(meta, UINT64_C(16), FLOW_TIMESTAMP_DEFAULT_SHIFT);
        }
    }
    if (hit_idxv[0] == 0u || old_count == 0u)
        FAIL("target bucket selection (filtered) failed");

    memset(expired_idxv, 0, sizeof(expired_idxv));
    evicted = ops->maintain_idx_bulk(&ft, hit_idxv, 1u, UINT64_C(200000),
                                     expire_tsc, expired_idxv, 16u, 1u, 1);
    if (evicted != old_count)
        FAIL("maintain_idx_bulk (filtered) evicted count mismatch");
    for (unsigned i = 0u; i < evicted; i++) {
        u32 idx = expired_idxv[i];

        if (idx == 0u || idx > inserted)
            FAIL("maintain_idx_bulk (filtered) expired idx invalid");
        if (bucket_of_idx[idx] != target_bk)
            FAIL("maintain_idx_bulk (filtered) expired wrong bucket");
        seen_old[idx] = 1u;
    }
    for (unsigned i = 0u; i < old_count; i++) {
        if (seen_old[old_idxv[i]] == 0u)
            FAIL("maintain_idx_bulk (filtered) missed expired idx");
        if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, old_idxv[i] - 1u)) != 0u)
            FAIL("expired key should miss after maintain_idx_bulk (filtered)");
    }
    if (TEST_OPS_FIND(ops, &ft, TEST_KEY_AT(keys, ops, hit_idxv[0] - 1u))
        != hit_idxv[0])
        FAIL("kept key should still hit after maintain_idx_bulk (filtered)");

    ops->destroy(&ft);
    free(bucket_counts);
    free(bucket_of_idx);
    free(keys);
    free(pool2);
    free(pool);
    return 0;
}

static int
testv_manual_grow_preserves_entries(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(4096u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    union test_any_key key;
    u32 idxs[256];

    printf("[T] %s table migrate preserves entries\n", ops->name);
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
    {
        unsigned cur_nb_bk = ops->nb_bk(&ft);
        size_t new_bsz = (size_t)cur_nb_bk * 2u * FT_TABLE_BUCKET_SIZE;
        void *new_bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, new_bsz);
        void *old_bk = ft.flow4.buckets;

        if (ops->migrate(&ft, new_bk, new_bsz) != 0) {
            free(new_bk);
            FAIL("migrate failed");
        }
        free(old_bk);
    }
    for (unsigned i = 0; i < 256u; i++) {
        ops->make_key(&key, i + 100u);
        if (TEST_OPS_FIND(ops, &ft, &key) != idxs[i])
            FAILF("find after grow mismatch at %u", i);
    }
    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_migrate_doubles_buckets(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(200000u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    unsigned old_nb_bk;

    printf("[T] %s table migrate doubles buckets\n", ops->name);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ops->init(&ft, pool, 200000u, &cfg) != 0)
        FAIL("init failed");
    old_nb_bk = ops->nb_bk(&ft);
    {
        size_t new_bsz = (size_t)old_nb_bk * 2u * FT_TABLE_BUCKET_SIZE;
        void *new_bk = aligned_alloc(FT_TABLE_BUCKET_ALIGN, new_bsz);
        void *old_bk = ft.flow4.buckets;

        if (ops->migrate(&ft, new_bk, new_bsz) != 0) {
            free(new_bk);
            FAIL("migrate failed");
        }
        free(old_bk);
    }
    if (ops->nb_bk(&ft) != old_nb_bk * 2u)
        FAIL("migrate should double nb_bk");

    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_walk_flush_and_del_idx(const struct test_variant_ops *ops)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(128u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    struct walk_ctx ctx;
    union test_any_key key;
    struct flow_status status;

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
    ops->status(&ft, &status);
    if (status.entries != 16u)
        FAIL("status.entries mismatch after add");
    if (status.add_bk0 + status.add_bk1 + status.kickouts != 16u)
        FAIL("status add path counters mismatch after add");
    memset(&ctx, 0, sizeof(ctx));
    if (ops->walk(&ft, walk_count_cb, &ctx) != 0 || ctx.count != 16u)
        FAIL("walk count mismatch");
    if (ops->del_idx(&ft, 2u) != 2u)
        FAIL("del_idx failed");
    ops->status(&ft, &status);
    if (status.entries != 15u)
        FAIL("status.entries mismatch after del");
    ops->make_key(&key, 5001u);
    if (TEST_OPS_FIND(ops, &ft, &key) != 0u)
        FAIL("del_idx entry still found");
    ops->flush(&ft);
    if (ops->nb_entries(&ft) != 0u)
        FAIL("flush should clear entries");
    ops->status(&ft, &status);
    if (status.entries != 0u || status.kickouts != 0u
        || status.add_bk0 != 0u || status.add_bk1 != 0u)
        FAIL("status should reset on flush");
    for (unsigned i = 0; i < 4u; i++) {
        ops->make_key(&key, i + 5000u);
        if (testv_add_idx_key(ops, &ft, i + 1u, &key) != i + 1u)
            FAILF("re-add after flush failed at %u", i);
        if (TEST_OPS_FIND(ops, &ft, &key) != i + 1u)
            FAILF("re-add after flush miss at %u", i);
    }
    ops->destroy(&ft);
    free(pool);
    return 0;
}

static int
testv_fuzz(const struct test_variant_ops *ops,
           unsigned seed, unsigned n, unsigned nb_bk, unsigned ops_n)
{
    struct ft_table_config cfg = test_cfg();
    union test_any_table ft;
    void *pool = test_aligned_calloc(n + 1u, ops->record_size,
                                     FT_TABLE_CACHE_LINE_SIZE);
    u8 *in_table = calloc(n, 1u);
    unsigned in_count = 0u;
    union test_any_key key;

    printf("[T] %s table fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           ops->name, seed, n, nb_bk, ops_n);
    if (pool == NULL || in_table == NULL)
        FAIL("alloc failed");
    if (ops->init(&ft, pool, n, &cfg) != 0)
        FAIL("init failed");

    for (unsigned op = 0; op < ops_n; op++) {
        u32 ret;
        seed = seed * 1103515245u + 12345u;
        unsigned idx0 = (seed >> 16) % n;
        unsigned action = (seed >> 8) & 3u;
        u32 entry_idx = idx0 + 1u;

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
            ret = TEST_OPS_FIND(ops, &ft, &key);
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
            if (TEST_OPS_FIND(ops, &ft, &key) != i + 1u)
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
    if (testv_init_ex_and_mapping(ops) != 0)
        return 1;
    if (testv_basic_add_find_del(ops) != 0)
        return 1;
    if (testv_bulk_ops_and_stats(ops) != 0)
        return 1;
    if (testv_add_idx_bulk_duplicate_ignore(ops) != 0)
        return 1;
    if (testv_add_idx_bulk_mixed_batch(ops) != 0)
        return 1;
    if (testv_add_idx_bulk_policy(ops) != 0)
        return 1;
    if (testv_add_idx_bulk_maint_duplicate_reclaims(ops) != 0)
        return 1;
    if (testv_add_idx_bulk_maint_zero_extra_skips(ops) != 0)
        return 1;
    if (testv_timestamp_update(ops) != 0)
        return 1;
    if (testv_permanent_timestamp(ops) != 0)
        return 1;
    if (testv_maintain_idx_bulk(ops) != 0)
        return 1;
    if (testv_walk_flush_and_del_idx(ops) != 0)
        return 1;
    if (testv_manual_grow_preserves_entries(ops) != 0)
        return 1;
    if (testv_migrate_doubles_buckets(ops) != 0)
        return 1;
    if (testv_fuzz(ops, 3237998097u, 512u, 64u, 200000u) != 0)
        return 1;
    return 0;
}

int
main(void)
{
    if (test_record_allocator_basic() != 0)
        return 1;
    if (test_record_allocator_bulk_and_reset() != 0)
        return 1;

    /* lifecycle / mapping */
    if (test_init_ex_and_mapping() != 0)
        return 1;

    /* find / add / del */
    if (test_basic_add_find_del() != 0)
        return 1;
    if (test_duplicate_and_delete_miss_stats() != 0)
        return 1;
    if (test_bulk_ops_and_stats() != 0)
        return 1;
    if (test_del_idx() != 0)
        return 1;
    if (test_del_idx_stale_meta_safe() != 0)
        return 1;

    /* walk / flush / stats */
    if (test_walk_early_stop() != 0)
        return 1;
    if (test_walk_flush_and_stats_reset() != 0)
        return 1;

    /* migrate / config */
    if (test_manual_grow_preserves_entries() != 0)
        return 1;
    if (test_migrate_failure_preserves_table() != 0)
        return 1;
    if (test_migrate_doubles_buckets() != 0)
        return 1;
    if (test_migrate_invalid_args() != 0)
        return 1;
    if (test_bucket_size_determines_nb_bk() != 0)
        return 1;

    /* maintain */
    if (test_maintain_basic() != 0)
        return 1;
    if (test_maintain_idx_bulk_stale_meta_safe() != 0)
        return 1;
    if (test_maintain_partial_and_limit() != 0)
        return 1;
    if (test_maintain_min_bk_entries() != 0)
        return 1;

    /* fill / kickout / fuzz */
    if (test_high_fill() != 0)
        return 1;
    if (test_max_fill() != 0)
        return 1;
    if (test_kickout_safety() != 0)
        return 1;
    if (test_force_expire_basic() != 0)
        return 1;
    if (test_force_expire_permanent_skip() != 0)
        return 1;
    if (test_force_expire_bulk() != 0)
        return 1;
    if (test_force_expire_evicts_oldest() != 0)
        return 1;
    if (test_fuzz(3237998097u, 512u, 64u, 200000u) != 0)
        return 1;
    if (test_fuzz(3237998097u, 1000u, 64u, 500000u) != 0)
        return 1;
    if (test_variant_force_expire_suite(&test_ops_flow6) != 0)
        return 1;
    if (test_variant_force_expire_suite(&test_ops_flowu) != 0)
        return 1;
    if (test_variant_suite(&test_ops_flow4) != 0)
        return 1;
    if (test_variant_suite(&test_ops_flow6) != 0)
        return 1;
    if (test_variant_suite(&test_ops_flowu) != 0)
        return 1;
    printf("ALL FLOW TABLE TESTS PASSED\n");
    return 0;
}
