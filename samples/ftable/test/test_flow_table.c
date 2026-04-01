/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flow4_table.h"

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

static struct ft_flow4_config
test_cfg(unsigned start_nb_bk, unsigned max_nb_bk, unsigned grow_fill_pct)
{
    struct ft_flow4_config cfg;

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
    k.proto    = 6u;
    k.src_port = (uint16_t)(1000u + i);
    k.dst_port = (uint16_t)(2000u + i);
    k.vrfid    = 1u;
    k.src_ip   = UINT32_C(0x0a000001) + i;
    k.dst_ip   = UINT32_C(0x0a000101) + i;
    return k;
}

static uint32_t
test_add_idx_key(struct ft_flow4_table *ft, uint32_t entry_idx,
                 const struct flow4_key *key)
{
    struct ft_flow4_entry *entry = ft_flow4_table_entry_ptr(ft, entry_idx);

    if (entry == NULL)
        return 0u;
    entry->key = *key;
    return ft_flow4_table_add_entry(ft, entry_idx);
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

static unsigned
test_mask(unsigned shift)
{
    return (1u << shift) - 1u;
}

static int
test_basic_add_find_del(void)
{
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    struct flow4_key k1 = test_key(1u);
    struct flow4_key k2 = test_key(2u);
    uint32_t idx1, idx2;

    printf("[T] flow4 table basic add/find/del\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 128u, &cfg) != 0)
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
    if (ft_flow4_table_del(&ft, &k1) != idx1)
        FAIL("del should return idx1");
    if (ft_flow4_table_find(&ft, &k1) != 0u)
        FAIL("deleted key should miss");
    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

struct test_user_rec {
    unsigned char pad[64];
    struct ft_flow4_entry entry;
    uint32_t cookie;
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

static int
test_init_ex_and_mapping(void)
{
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct test_user_rec *users;
    struct flow4_key key = test_key(10u);
    uint32_t idx;

    printf("[T] flow4 table init_ex mapping\n");
    users = test_aligned_calloc(64u, sizeof(*users), FT_TABLE_CACHE_LINE_SIZE);
    if (users == NULL)
        FAIL("calloc users");
    for (unsigned i = 0; i < 64u; i++)
        users[i].cookie = UINT32_C(0xabc00000) + i;
    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, users, 64u,
                                  struct test_user_rec, entry, &cfg) != 0)
        FAIL("FT_FLOW4_TABLE_INIT_TYPED failed");
    idx = test_add_idx_key(&ft, 1u, &key);
    if (idx != 1u)
        FAIL("init_ex add failed");
    if (FT_FLOW4_TABLE_RECORD_FROM_ENTRY(struct test_user_rec, entry,
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
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    uint32_t idxs[256];

    printf("[T] flow4 table grow_2x preserves entries\n");
    pool = test_aligned_calloc(4096u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 4096u, &cfg) != 0)
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
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;

    printf("[T] flow4 table need_grow/reserve\n");
    pool = test_aligned_calloc(200000u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 200000u, &cfg) != 0)
        FAIL("init failed");
    if (ft_flow4_table_nb_bk(&ft) != FT_FLOW4_DEFAULT_MIN_NB_BK)
        FAILF("unexpected initial nb_bk=%u", ft_flow4_table_nb_bk(&ft));
    if (ft_flow4_table_reserve(&ft, 160000u) != 0)
        FAIL("reserve failed");
    if (ft_flow4_table_nb_bk(&ft) != (FT_FLOW4_DEFAULT_MIN_NB_BK << 1))
        FAILF("reserve should grow to %u, got %u",
              FT_FLOW4_DEFAULT_MIN_NB_BK << 1, ft_flow4_table_nb_bk(&ft));

    ft_flow4_table_destroy(&ft);
    if (ft_flow4_table_init(&ft, pool, 200000u, &cfg) != 0)
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
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    struct flow4_key keys[8];
    uint32_t entry_idxv[8];
    struct ft_flow4_result results[8];
    struct ft_flow4_stats stats;

    printf("[T] flow4 table bulk ops and stats\n");
    pool = test_aligned_calloc(256u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 256u, &cfg) != 0)
        FAIL("init failed");
    for (unsigned i = 0; i < 8u; i++) {
        keys[i] = test_key(i + 2000u);
        pool[i].key = keys[i];
        entry_idxv[i] = i + 1u;
    }

    ft_flow4_table_add_entry_bulk(&ft, entry_idxv, 8u, results);
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

    ft_flow4_table_add_entry_bulk(&ft, entry_idxv, 8u, results);
    for (unsigned i = 0; i < 8u; i++) {
        if (results[i].entry_idx != entry_idxv[i])
            FAILF("duplicate add_bulk failed at %u", i);
    }

    ft_flow4_table_del_bulk(&ft, keys, 4u, results);
    for (unsigned i = 0; i < 4u; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("del_bulk failed at %u", i);
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
test_entry_bulk_registration(void)
{
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    struct flow4_key keys[4];
    uint32_t entry_idxv[4];
    struct ft_flow4_result results[4];

    printf("[T] flow4 table entry bulk registration\n");
    pool = test_aligned_calloc(64u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 64u, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 4u; i++) {
        keys[i] = test_key(i + 2500u);
        pool[8u + i].key = keys[i];
        entry_idxv[i] = 9u + i;
    }

    ft_flow4_table_add_entry_bulk(&ft, entry_idxv, 4u, results);
    for (unsigned i = 0; i < 4u; i++) {
        if (results[i].entry_idx != 9u + i)
            FAILF("add_entry_bulk failed at %u", i);
        if (ft_flow4_table_find(&ft, &keys[i]) != 9u + i)
            FAILF("find after add_entry_bulk failed at %u", i);
    }

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_hash_pair_invariants(void)
{
    struct ft_flow4_config cfg = test_cfg(FT_FLOW4_DEFAULT_MIN_NB_BK,
                                          1048576u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;

    printf("[T] flow4 table hash pair invariants\n");
    pool = test_aligned_calloc(512u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 512u, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 128u; i++) {
        struct flow4_key key = test_key(i + 3000u);
        uint32_t idx = test_add_idx_key(&ft, i + 1u, &key);
        const struct ft_flow4_entry *entry;

        if (idx != i + 1u)
            FAILF("add failed at %u", i);
        entry = ft_flow4_table_entry_cptr(&ft, idx);
        if (entry == NULL)
            FAIL("entry_cptr returned NULL");
        if (entry->hash0 == 0u || entry->hash1 == 0u)
            FAILF("zero hash at idx=%u", idx);
        for (unsigned shift = 14u; shift <= 20u; shift++) {
            unsigned mask = test_mask(shift);

            if ((entry->hash0 & mask) == (entry->hash1 & mask))
                FAILF("hash pair collided for shift=%u idx=%u", shift, idx);
        }
    }

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_grow_preserves_hash_pair(void)
{
    struct ft_flow4_config cfg = test_cfg(FT_FLOW4_DEFAULT_MIN_NB_BK,
                                          65536u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    uint32_t idxs[64];
    uint32_t hash0[64];
    uint32_t hash1[64];

    printf("[T] flow4 table grow preserves hash pair\n");
    pool = test_aligned_calloc(2048u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 2048u, &cfg) != 0)
        FAIL("init failed");

    for (unsigned i = 0; i < 64u; i++) {
        struct flow4_key key = test_key(i + 4000u);
        const struct ft_flow4_entry *entry;

        idxs[i] = test_add_idx_key(&ft, i + 1u, &key);
        if (idxs[i] != i + 1u)
            FAILF("add failed at %u", i);
        entry = ft_flow4_table_entry_cptr(&ft, idxs[i]);
        if (entry == NULL)
            FAIL("entry_cptr returned NULL");
        hash0[i] = entry->hash0;
        hash1[i] = entry->hash1;
    }

    if (ft_flow4_table_grow_2x(&ft) != 0)
        FAIL("first grow failed");
    if (ft_flow4_table_grow_2x(&ft) != 0)
        FAIL("second grow failed");

    for (unsigned i = 0; i < 64u; i++) {
        struct flow4_key key = test_key(i + 4000u);
        const struct ft_flow4_entry *entry = ft_flow4_table_entry_cptr(&ft, idxs[i]);

        if (ft_flow4_table_find(&ft, &key) != idxs[i])
            FAILF("find after repeated grow mismatch at %u", i);
        if (entry == NULL)
            FAIL("entry_cptr returned NULL");
        if (entry->hash0 != hash0[i] || entry->hash1 != hash1[i])
            FAILF("hash pair changed at %u", i);
    }

    ft_flow4_table_destroy(&ft);
    free(pool);
    return 0;
}

static int
test_walk_flush_and_stats_reset(void)
{
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    struct walk_ctx ctx;
    struct ft_flow4_stats stats;

    printf("[T] flow4 table walk/flush/stats\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 128u, &cfg) != 0)
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
    struct ft_flow4_config cfg;
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;

    printf("[T] flow4 table allocator failure/max bucket limit\n");
    cfg = test_cfg(FT_FLOW4_DEFAULT_MIN_NB_BK,
                   FT_FLOW4_DEFAULT_MIN_NB_BK, 60u);
    cfg.bucket_alloc.alloc = fail_bucket_alloc;
    cfg.bucket_alloc.free = test_bucket_free;
    cfg.bucket_alloc.arg = &alloc_ctx;

    pool = test_aligned_calloc(200000u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 200000u, &cfg) != 0)
        FAIL("init failed");
    if (ft_flow4_table_reserve(&ft, 160000u) == 0)
        FAIL("reserve should fail when max_nb_bk is reached");
    if (ft_flow4_table_grow_2x(&ft) == 0)
        FAIL("grow_2x should fail at max_nb_bk");
    ft_flow4_table_destroy(&ft);

    alloc_ctx.call_count = 0u;
    alloc_ctx.fail_after = 0u;
    if (ft_flow4_table_init(&ft, pool, 200000u, &cfg) == 0) {
        ft_flow4_table_destroy(&ft);
        FAIL("init should fail when allocator returns NULL");
    }

    free(pool);
    return 0;
}

static int
test_duplicate_and_delete_miss_stats(void)
{
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    struct ft_flow4_stats stats;
    struct flow4_key key = test_key(6000u);
    struct flow4_key miss = test_key(6001u);
    uint32_t idx;

    printf("[T] flow4 table duplicate add/del miss stats\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 128u, &cfg) != 0)
        FAIL("init failed");

    idx = test_add_idx_key(&ft, 1u, &key);
    if (idx != 1u)
        FAIL("initial add failed");
    if (test_add_idx_key(&ft, 1u, &key) != idx)
        FAIL("duplicate add should return existing idx");
    if (ft_flow4_table_del(&ft, &miss) != 0u)
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
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    struct walk_ctx ctx;

    printf("[T] flow4 table walk early stop\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 128u, &cfg) != 0)
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
    struct ft_flow4_config cfg;
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
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
    if (ft_flow4_table_init(&ft, pool, 2048u, &cfg) != 0)
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
    struct ft_flow4_config cfg = test_cfg(9000u, 9000u, 0u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;

    printf("[T] flow4 table config rounding/clamp\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 128u, &cfg) != 0)
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
    struct ft_flow4_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    unsigned inserted = 0u;

    printf("[T] flow4 table high fill (94%%)\n");
    pool = test_aligned_calloc(capacity + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, capacity, &cfg) != 0)
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

        if (ft_flow4_table_del(&ft, &key) != i + 1u)
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
    struct ft_flow4_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    unsigned inserted = 0u;

    printf("[T] flow4 table max fill\n");
    pool = test_aligned_calloc(capacity + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, capacity, &cfg) != 0)
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
    struct ft_flow4_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    unsigned phase1_count = 0u;
    unsigned lost = 0u;

    printf("[T] flow4 table kickout safety\n");
    pool = test_aligned_calloc(capacity + overflow + 1u, sizeof(*pool),
                               FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, capacity + overflow, &cfg) != 0)
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
    struct ft_flow4_config cfg = test_cfg(0u, 0u, 60u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    struct flow4_key keys[8];
    uint32_t idxs[8];

    printf("[T] flow4 table del_idx\n");
    pool = test_aligned_calloc(128u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    if (pool == NULL)
        FAIL("calloc pool");
    if (ft_flow4_table_init(&ft, pool, 128u, &cfg) != 0)
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
    if (ft_flow4_table_del_idx(&ft, idxs[0]) != 0u)
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
    struct ft_flow4_config cfg = test_cfg(nb_bk, nb_bk, 99u);
    struct ft_flow4_table ft;
    struct ft_flow4_entry *pool;
    uint8_t *in_table; /* 1 if entry i is in table */
    unsigned in_count = 0u;

    printf("[T] flow4 table fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           seed, n, nb_bk, ops);
    pool = test_aligned_calloc(n + 1u, sizeof(*pool), FT_TABLE_CACHE_LINE_SIZE);
    in_table = calloc(n, 1u);
    if (pool == NULL || in_table == NULL)
        FAIL("alloc failed");
    if (ft_flow4_table_init(&ft, pool, n, &cfg) != 0)
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
                uint32_t ret = ft_flow4_table_del(&ft, &key);

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

int
main(void)
{
    if (test_basic_add_find_del() != 0)
        return 1;
    if (test_duplicate_and_delete_miss_stats() != 0)
        return 1;
    if (test_bulk_ops_and_stats() != 0)
        return 1;
    if (test_entry_bulk_registration() != 0)
        return 1;
    if (test_init_ex_and_mapping() != 0)
        return 1;
    if (test_walk_early_stop() != 0)
        return 1;
    if (test_hash_pair_invariants() != 0)
        return 1;
    if (test_manual_grow_preserves_entries() != 0)
        return 1;
    if (test_grow_preserves_hash_pair() != 0)
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
    printf("ALL FLOW TABLE TESTS PASSED\n");
    return 0;
}
