/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot.h>

#include "flow4_table.h"

#define ft_flow4_hash_fn flow4_key_hash
#define ft_flow4_cmp     flow4_key_cmp

static inline struct flow4_entry *
ft_flow4_layout_entry_ptr_(const struct ft_flow4_table *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset, struct flow4_entry);
}

static inline unsigned
ft_flow4_layout_entry_idx_(const struct ft_flow4_table *ft,
                           const struct flow4_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

/*===========================================================================
 * FCORE layer: flow4_entry slot + FCORE_GENERATE
 *===========================================================================*/

static inline struct flow4_entry *
fcore_flow4_layout_entry_ptr_(const struct ft_flow4_table *ft, unsigned idx)
{
    return (struct flow4_entry *)(void *)
        ft_flow4_layout_entry_ptr_(ft, idx);
}

static inline unsigned
fcore_flow4_layout_entry_idx_(const struct ft_flow4_table *ft,
                              const struct flow4_entry *hdr)
{
    return ft_flow4_layout_entry_idx_(
        ft, hdr);
}

RIX_HASH_HEAD(fcore_flow4_ht);

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_flow4_table *ft =                                         \
        (const struct ft_flow4_table *)(const void *)base;                    \
    return fcore_flow4_layout_entry_idx_(ft, p);                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_flow4_table *ft =                                         \
        (const struct ft_flow4_table *)(const void *)base;                    \
    return fcore_flow4_layout_entry_ptr_(ft, i);                              \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(fcore_flow4_ht, flow4_entry,
    key, meta.cur_hash, meta.slot, ft_flow4_cmp, ft_flow4_hash_fn)

#define FCORE_LAYOUT_ENTRY_PTR(owner, idx) \
    fcore_flow4_layout_entry_ptr_((owner), (idx))

#define FCORE_LAYOUT_ENTRY_INDEX(owner, entry) \
    fcore_flow4_layout_entry_idx_((owner), (entry))

#undef FCORE_LAYOUT_HASH_BASE
#define FCORE_LAYOUT_HASH_BASE(owner) \
    ((struct flow4_entry *)(void *)(owner))

#undef FCORE_STATS
#define FCORE_STATS(owner) ((owner)->stats.core)

/*
 * ftable uses start_mask (not ht_head.rhh_mask) so that entries hashed
 * before grow_2x keep their original bucket mapping.
 */
#undef FCORE_HASH_MASK
#define FCORE_HASH_MASK(owner, ht) ((owner)->start_mask)

/*
 * fcore_flow4_ht and ft_flow4_ht are layout-compatible RIX_HASH_HEAD types
 * (both are { unsigned rhh_mask; unsigned rhh_nb; }).  The _FCORE_HT_HEAD
 * cast is safe but triggers GCC strict-aliasing level 2.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
FCORE_GENERATE(flow4, ft_flow4_table, fcore_flow4_ht,
               ft_flow4_hash_fn, ft_flow4_cmp)
#pragma GCC diagnostic pop

#undef FCORE_HASH_MASK

/* Clean up FCORE hooks before FT_TABLE_GENERATE */
#undef FCORE_LAYOUT_ENTRY_PTR
#undef FCORE_LAYOUT_ENTRY_INDEX
#undef FCORE_LAYOUT_HASH_BASE
#undef FCORE_STATS
#undef RIX_HASH_SLOT_DEFINE_INDEXERS

/*===========================================================================
 * FT_TABLE layer: flow4_entry slot + FT_TABLE_GENERATE
 *===========================================================================*/

#define FTG_LAYOUT_INIT_STORAGE(ft, array, stride, entry_offset)               \
    do {                                                                       \
        (ft)->pool_base = (unsigned char *)(array);                            \
        (ft)->pool_stride = (stride);                                          \
        (ft)->pool_entry_offset = (entry_offset);                              \
        (ft)->pool = FT_RECORD_MEMBER_PTR((ft)->pool_base, (ft)->pool_stride, \
                                          1u, (ft)->pool_entry_offset,         \
                                          struct flow4_entry);                 \
    } while (0)

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct flow4_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx) ft_flow4_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flow4_layout_entry_idx_((ft), (entry))

#define FTG_LAYOUT_ENTRY_AT(ft, off0)                                          \
    ft_flow4_layout_entry_ptr_((ft), (unsigned)(off0) + 1u)

#define FTG_ENTRY_TYPE(p) struct flow4_entry
#define FTG_ENTRY_META_CLEAR_TAIL(entry) ((void)(entry))

#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_flow4_table *ft =                                         \
        (const struct ft_flow4_table *)(const void *)base;                    \
    return ft_flow4_layout_entry_idx_(ft, p);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_flow4_table *ft =                                         \
        (const struct ft_flow4_table *)(const void *)base;                    \
    return ft_flow4_layout_entry_ptr_(ft, i);                                 \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(ft_flow4_ht, flow4_entry, key,
                                 meta.cur_hash, meta.slot,
                                 ft_flow4_cmp, ft_flow4_hash_fn)

/* Enable FCORE-delegating bulk ops in FT_TABLE_GENERATE */
#define FTG_USE_FCORE 1

#include "ft_table_generate.h"

FT_TABLE_GENERATE(flow4,
                  FT_FLOW4_DEFAULT_MIN_NB_BK,
                  FT_FLOW4_DEFAULT_MAX_NB_BK,
                  FT_FLOW4_DEFAULT_GROW_FILL_PCT,
                  0u,
                  ft_flow4_hash_fn, ft_flow4_cmp)

#ifdef FT_ARCH_SUFFIX
#include "ft_ops.h"
FT_OPS_TABLE(flow4, FT_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
