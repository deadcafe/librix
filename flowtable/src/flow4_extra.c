/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot_extra.h>

#include "flowtable/flow4_extra_table.h"
#include "flow_hash_extra.h"
#include "flow_core_extra.h"

/*===========================================================================
 * flow4_extra key hash / cmp
 *
 * 16 B key: 2x CRC32C unrolled on x86+SSE4.2, fallback to
 * rix_hash_bytes_fast.
 *===========================================================================*/
static inline union rix_hash_hash_u
flow4_extra_key_hash(const struct flow4_extra_key *key, u32 mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    union rix_hash_hash_u r;
    u64 w0, w1;
    u32 h0, bk0, h1, inc;

    memcpy(&w0, (const char *)key,      8u);
    memcpy(&w1, (const char *)key + 8u, 8u);
    h0  = (u32)__builtin_ia32_crc32di(0ULL, w0);
    h0  = (u32)__builtin_ia32_crc32di((u64)h0, w1);
    h1  = (u32)__builtin_ia32_crc32di(UINT64_C(0x9e3779b97f4a7c15), w0);
    h1  = (u32)__builtin_ia32_crc32di((u64)h1, w1);
    h1  = rix_hash_retry_mix32(h1, UINT32_C(0x85ebca6b));
    bk0 = h0 & mask;
    inc = 1u;
    while ((h1 & mask) == bk0) {
        h1 = rix_hash_retry_mix32(h1, inc);
        inc++;
    }

    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
#else
    return rix_hash_bytes_fast(key, sizeof(*key), mask);
#endif
}

static inline int
flow4_extra_key_cmp(const struct flow4_extra_key *a,
                    const struct flow4_extra_key *b)
{
    u64 a0, a1, b0, b1;

    memcpy(&a0, a,                     8u);
    memcpy(&a1, (const char *)a + 8u,  8u);
    memcpy(&b0, b,                     8u);
    memcpy(&b1, (const char *)b + 8u,  8u);
    return ((a0 ^ b0) | (a1 ^ b1)) ? 1 : 0;
}

#define ft_flow4_extra_hash_fn flow4_extra_key_hash
#define ft_flow4_extra_cmp     flow4_extra_key_cmp

static inline struct flow4_extra_entry *
ft_flow4_extra_layout_entry_ptr_(const struct ft_table_extra *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset,
                                struct flow4_extra_entry);
}

static inline unsigned
ft_flow4_extra_layout_entry_idx_(const struct ft_table_extra *ft,
                                 const struct flow4_extra_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

/*===========================================================================
 * FCORE_EXTRA layer: flow4_extra_entry slot + FCORE_EXTRA_GENERATE
 *===========================================================================*/

static RIX_UNUSED inline struct flow4_extra_entry *
fcore_flow4_extra_layout_entry_ptr_(const struct ft_table_extra *ft,
                                    unsigned idx)
{
    return (struct flow4_extra_entry *)(void *)
        ft_flow4_extra_layout_entry_ptr_(ft, idx);
}

static RIX_UNUSED inline unsigned
fcore_flow4_extra_layout_entry_idx_(const struct ft_table_extra *ft,
                                    const struct flow4_extra_entry *hdr)
{
    return ft_flow4_extra_layout_entry_idx_(ft, hdr);
}

RIX_HASH_HEAD(fcore_flow4_extra_ht);
RIX_HASH_HEAD(ft_flow4_extra_ht);

#undef RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS
#define RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS(name, type)                        \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                    \
name##_hidx(struct type *base, const struct type *p)                           \
{                                                                              \
    const struct ft_table_extra *ft =                                          \
        (const struct ft_table_extra *)(const void *)base;                     \
    return fcore_flow4_extra_layout_entry_idx_(ft, p);                         \
}                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                               \
name##_hptr(struct type *base, unsigned i)                                     \
{                                                                              \
    const struct ft_table_extra *ft =                                          \
        (const struct ft_table_extra *)(const void *)base;                     \
    return fcore_flow4_extra_layout_entry_ptr_(ft, i);                         \
}

RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(fcore_flow4_extra_ht, flow4_extra_entry,
    key, meta.cur_hash, meta.slot, ft_flow4_extra_cmp, ft_flow4_extra_hash_fn)

#define FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, idx)                               \
    FCORE_EXTRA_RECORD_MEMBER_PTR_NONNULL_ALIGNED(                             \
        struct flow4_extra_entry,                                              \
        (owner)->pool_base, (owner)->pool_stride, (idx),                       \
        (owner)->pool_entry_offset)

#define FCORE_EXTRA_LAYOUT_ENTRY_INDEX(owner, entry)                           \
    fcore_flow4_extra_layout_entry_idx_((owner), (entry))

#undef FCORE_EXTRA_LAYOUT_HASH_BASE
#define FCORE_EXTRA_LAYOUT_HASH_BASE(owner)                                    \
    ((struct flow4_extra_entry *)(void *)(owner))

#undef FLOW_STATS
#define FLOW_STATS(owner) ((owner)->stats.core)

/*
 * ftable uses start_mask (not ht_head.rhh_mask) so that entries hashed
 * before grow_2x keep their original bucket mapping.
 */
#undef FCORE_EXTRA_HASH_MASK
#define FCORE_EXTRA_HASH_MASK(owner, ht) ((owner)->start_mask)

#undef FCORE_EXTRA_TIMESTAMP_SHIFT
#define FCORE_EXTRA_TIMESTAMP_SHIFT(owner) ((owner)->ts_shift)

/*
 * fcore_flow4_extra_ht and ft_flow4_extra_ht are layout-compatible
 * RIX_HASH_HEAD types.  The _FCORE_EXTRA_HT_HEAD cast is safe but
 * triggers GCC strict-aliasing level 2.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
FCORE_EXTRA_GENERATE(flow4, ft_table_extra, fcore_flow4_extra_ht,
                     ft_flow4_extra_hash_fn, ft_flow4_extra_cmp)
#pragma GCC diagnostic pop

#undef FCORE_EXTRA_HASH_MASK
#undef FCORE_EXTRA_TIMESTAMP_SHIFT

/* Clean up FCORE_EXTRA hooks before FT_TABLE_EXTRA_GENERATE */
#undef FCORE_EXTRA_LAYOUT_ENTRY_PTR
#undef FCORE_EXTRA_LAYOUT_ENTRY_INDEX
#undef FCORE_EXTRA_LAYOUT_HASH_BASE
#undef FLOW_STATS
#undef RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS

/*===========================================================================
 * FT_TABLE_EXTRA layer: flow4_extra_entry slot + FT_TABLE_EXTRA_GENERATE
 *===========================================================================*/

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct flow4_extra_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx)                                          \
    ft_flow4_extra_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flow4_extra_layout_entry_idx_((ft), (entry))

#define FTG_ENTRY_TYPE(p) struct flow4_extra_entry

#define RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS(name, type)                        \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                    \
name##_hidx(struct type *base, const struct type *p)                           \
{                                                                              \
    const struct ft_table_extra *ft =                                          \
        (const struct ft_table_extra *)(const void *)base;                     \
    return ft_flow4_extra_layout_entry_idx_(ft, p);                            \
}                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                               \
name##_hptr(struct type *base, unsigned i)                                     \
{                                                                              \
    const struct ft_table_extra *ft =                                          \
        (const struct ft_table_extra *)(const void *)base;                     \
    return ft_flow4_extra_layout_entry_ptr_(ft, i);                            \
}

RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(ft_flow4_extra_ht, flow4_extra_entry,
                                       key, meta.cur_hash, meta.slot,
                                       ft_flow4_extra_cmp,
                                       ft_flow4_extra_hash_fn)

#include "flow_table_generate_extra.h"

FT_TABLE_EXTRA_GENERATE(flow4, ft_table_extra, ft_flow4_extra_ht,
                        ft_flow4_extra_hash_fn, ft_flow4_extra_cmp)

#ifdef FT_ARCH_SUFFIX
#include "flow_dispatch_extra.h"
FT_OPS_TABLE_EXTRA(flow4, FT_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
