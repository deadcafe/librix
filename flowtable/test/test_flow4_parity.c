/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * test_flow4_parity.c - drive pure flow4 and the slot_extra flow4 with
 * the same op stream and assert externally observable equivalence.
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

#include "flowtable/flow4_table.h"
#include "flowtable/flow4_extra_table.h"
#include "flowtable/flow_key.h"

#define FUZZ_POOL 512u
#define FUZZ_OPS  20000u
#define VAL_EVERY 128u

static u32 rng_state = 0xC0FFEE1Du;

static inline u32
xs32(void)
{
    u32 x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
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

static struct flow4_key
pure_key(unsigned slot)
{
    struct flow4_key k;

    memset(&k, 0, sizeof(k));
    k.family   = 4u;
    k.proto    = 6u;
    k.src_ip   = 0x0A000000u | slot;
    k.dst_ip   = 0x0B000000u | (slot + 1u);
    k.src_port = (u16)(slot * 7u);
    k.dst_port = (u16)(slot * 11u);
    return k;
}

static struct flow4_extra_key
extra_key(unsigned slot)
{
    struct flow4_extra_key k;

    memset(&k, 0, sizeof(k));
    k.family   = 4u;
    k.proto    = 6u;
    k.src_addr = 0x0A000000u | slot;
    k.dst_addr = 0x0B000000u | (slot + 1u);
    k.src_port = (u16)(slot * 7u);
    k.dst_port = (u16)(slot * 11u);
    return k;
}

int
main(void)
{
    size_t bk_c_sz = ft_table_bucket_size(FUZZ_POOL);
    size_t bk_e_sz = flow4_extra_table_bucket_size(FUZZ_POOL);
    struct flow4_entry       *pool_c = hugealloc(FUZZ_POOL * sizeof(*pool_c));
    struct flow4_extra_entry *pool_e = hugealloc(FUZZ_POOL * sizeof(*pool_e));
    void *bk_c = hugealloc(bk_c_sz);
    void *bk_e = hugealloc(bk_e_sz);
    struct ft_table        ft_c;
    struct ft_table_extra  ft_e;
    struct ft_table_config       cfg_c = { .ts_shift = 4u };
    struct ft_table_extra_config cfg_e = { .ts_shift = 4u };
    unsigned op;
    unsigned ops_performed = 0u;
    u64 now = 1000u;
    int rc;

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    ft_arch_init(FT_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);

    rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, FUZZ_POOL,
                                   struct flow4_entry, key,
                                   bk_c, bk_c_sz, &cfg_c);
    assert(rc == 0);
    rc = flow4_extra_table_init(&ft_e, pool_e, FUZZ_POOL, bk_e, bk_e_sz,
                                &cfg_e);
    assert(rc == 0);

    for (unsigned i = 0u; i < FUZZ_POOL; i++) {
        pool_c[i].key = pure_key(i);
        pool_e[i].key = extra_key(i);
    }

    printf("[T] flow4 parity fuzz seed=%u N=%u ops=%u\n",
           rng_state, FUZZ_POOL, FUZZ_OPS);

    for (op = 0u; op < FUZZ_OPS; op++) {
        unsigned which = xs32() & 0x7u;
        unsigned i     = xs32() % FUZZ_POOL;
        u32 idx_c;
        u32 idx_e;

        switch (which) {
        case 0: case 1: case 2:  /* ADD by idx (key already in pool) */
            idx_c = ft_flow4_table_add_idx(&ft_c, i + 1u, now);
            idx_e = flow4_extra_table_add(&ft_e, &pool_e[i], now);
            assert((idx_c == 0u) == (idx_e == 0u));
            break;

        case 3: case 4: {  /* FIND (pass now=0 so pure does not touch) */
            struct flow4_key       kc = pure_key(i);
            struct flow4_extra_key ke = extra_key(i);

            idx_c = ft_flow4_table_find(&ft_c, &kc, 0u);
            idx_e = flow4_extra_table_find(&ft_e, &ke);
            assert((idx_c == 0u) == (idx_e == 0u));
            if (idx_c != 0u)
                assert(idx_c == idx_e);
            break;
        }

        case 5: {  /* DEL by key */
            struct flow4_key       kc = pure_key(i);
            struct flow4_extra_key ke = extra_key(i);

            idx_c = ft_flow4_table_del_key_oneshot(&ft_c, &kc);
            idx_e = flow4_extra_table_del(&ft_e, &ke);
            assert((idx_c == 0u) == (idx_e == 0u));
            if (idx_c != 0u)
                assert(idx_c == idx_e);
            break;
        }

        case 6: {  /* TOUCH (directly, no maintain divergence) */
            u32 id = (xs32() % FUZZ_POOL) + 1u;
            struct flow4_entry *ec =
                ft_flow4_table_entry_ptr(&ft_c, id);

            if (ec != NULL)
                flow_timestamp_touch(&ec->meta, now, cfg_c.ts_shift);
            ft_table_extra_touch(&ft_e, id, now);
            break;
        }

        case 7:  /* ADVANCE TIME */
            now += (u64)(1u + (xs32() & 0xFFu));
            break;
        }

        ops_performed++;
        if (ops_performed % VAL_EVERY == 0u)
            assert(ft_flow4_table_nb_entries(&ft_c) ==
                   ft_table_extra_nb_entries(&ft_e));
    }

    assert(ft_flow4_table_nb_entries(&ft_c) ==
           ft_table_extra_nb_entries(&ft_e));

    ft_flow4_table_destroy(&ft_c);
    ft_table_extra_destroy(&ft_e);
    munmap(pool_c, FUZZ_POOL * sizeof(*pool_c));
    munmap(pool_e, FUZZ_POOL * sizeof(*pool_e));
    munmap(bk_c, bk_c_sz);
    munmap(bk_e, bk_e_sz);

    printf("PASS parity fuzz %u ops\n", ops_performed);
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
