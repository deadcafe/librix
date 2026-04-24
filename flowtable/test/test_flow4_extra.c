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

#include "flow_extra_table.h"

#define N_MAX 1024u

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

    memset(&ctx, 0, sizeof(ctx));
    ctx.buckets     = ft.buckets;
    ctx.rhh_nb      = &ft.ht_head.rhh_nb;
    ctx.stats       = &ft.stats;
    ctx.pool_base   = (unsigned char *)pool;
    ctx.pool_stride = sizeof(*pool);
    ctx.meta_off    = offsetof(struct flow4_extra_entry, meta);
    ctx.max_entries = N_MAX;
    ctx.rhh_mask    = ft.ht_head.rhh_mask;
    ctx.ts_shift    = (u8)cfg.ts_shift;

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

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);
    test_init_basic();
    test_add_find_del();
    test_ts_in_bucket();
    test_maintain_expires();
    test_touch_updates_bucket_extra();
    printf("PASS\n");
    return 0;
}
