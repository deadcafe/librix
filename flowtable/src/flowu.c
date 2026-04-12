/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot.h>

#include "flowu_table.h"
#include "flow_hash.h"
#include "flow_core.h"

#define ft_flowu_hash_fn flowu_key_hash
#define ft_flowu_cmp     flowu_key_cmp

static inline struct flowu_entry *
ft_flowu_layout_entry_ptr_(const struct ft_table *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset, struct flowu_entry);
}

static inline unsigned
ft_flowu_layout_entry_idx_(const struct ft_table *ft,
                           const struct flowu_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

/*===========================================================================
 * FCORE layer: flowu_entry slot + FCORE_GENERATE
 *===========================================================================*/

static inline struct flowu_entry *
fcore_flowu_layout_entry_ptr_(const struct ft_table *ft, unsigned idx)
{
    return (struct flowu_entry *)(void *)
        ft_flowu_layout_entry_ptr_(ft, idx);
}

static inline unsigned
fcore_flowu_layout_entry_idx_(const struct ft_table *ft,
                              const struct flowu_entry *hdr)
{
    return ft_flowu_layout_entry_idx_(
        ft, hdr);
}

RIX_HASH_HEAD(fcore_flowu_ht);
RIX_HASH_HEAD(ft_flowu_ht);

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_table *ft =                                               \
        (const struct ft_table *)(const void *)base;                          \
    return fcore_flowu_layout_entry_idx_(ft, p);                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_table *ft =                                               \
        (const struct ft_table *)(const void *)base;                          \
    return fcore_flowu_layout_entry_ptr_(ft, i);                              \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(fcore_flowu_ht, flowu_entry,
    key, meta.cur_hash, meta.slot, ft_flowu_cmp, ft_flowu_hash_fn)

#define FCORE_LAYOUT_ENTRY_PTR(owner, idx) \
    FCORE_RECORD_MEMBER_PTR_NONNULL_ALIGNED(struct flowu_entry,               \
        (owner)->pool_base, (owner)->pool_stride, (idx),                      \
        (owner)->pool_entry_offset)

#define FCORE_LAYOUT_ENTRY_INDEX(owner, entry) \
    fcore_flowu_layout_entry_idx_((owner), (entry))

#undef FCORE_LAYOUT_HASH_BASE
#define FCORE_LAYOUT_HASH_BASE(owner) \
    ((struct flowu_entry *)(void *)(owner))

#undef FLOW_STATS
#define FLOW_STATS(owner) ((owner)->stats.core)

#undef FCORE_HASH_MASK
#define FCORE_HASH_MASK(owner, ht) ((owner)->start_mask)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
FCORE_GENERATE(flowu, ft_table, fcore_flowu_ht,
               ft_flowu_hash_fn, ft_flowu_cmp)
#pragma GCC diagnostic pop

#undef FCORE_HASH_MASK

/* Clean up FCORE hooks before FT_TABLE_GENERATE */
#undef FCORE_LAYOUT_ENTRY_PTR
#undef FCORE_LAYOUT_ENTRY_INDEX
#undef FCORE_LAYOUT_HASH_BASE
#undef FLOW_STATS
#undef RIX_HASH_SLOT_DEFINE_INDEXERS

/*===========================================================================
 * FT_TABLE layer: flowu_entry slot + FT_TABLE_GENERATE
 *===========================================================================*/

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct flowu_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx) ft_flowu_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flowu_layout_entry_idx_((ft), (entry))

#define FTG_ENTRY_TYPE(p) struct flowu_entry

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_table *ft =                                               \
        (const struct ft_table *)(const void *)base;                          \
    return ft_flowu_layout_entry_idx_(ft, p);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_table *ft =                                               \
        (const struct ft_table *)(const void *)base;                          \
    return ft_flowu_layout_entry_ptr_(ft, i);                                 \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(ft_flowu_ht, flowu_entry, key,
                                 meta.cur_hash, meta.slot,
                                 ft_flowu_cmp, ft_flowu_hash_fn)

#include "flow_table_generate.h"

FT_TABLE_GENERATE(flowu, ft_flowu_hash_fn, ft_flowu_cmp)

#ifdef FT_ARCH_SUFFIX
#include "flow_dispatch.h"
FT_OPS_TABLE(flowu, FT_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
