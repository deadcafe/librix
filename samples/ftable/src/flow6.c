/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot.h>

#include "flow6_table.h"

#define ft_flow6_hash_fn flow6_key_hash
#define ft_flow6_cmp     flow6_key_cmp

static inline struct flow6_entry *
ft_flow6_layout_entry_ptr_(const struct ft_flow6_table *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset, struct flow6_entry);
}

static inline unsigned
ft_flow6_layout_entry_idx_(const struct ft_flow6_table *ft,
                           const struct flow6_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

/*===========================================================================
 * FCORE layer: flow6_entry slot + FCORE_GENERATE
 *===========================================================================*/

static inline struct flow6_entry *
fcore_flow6_layout_entry_ptr_(const struct ft_flow6_table *ft, unsigned idx)
{
    return (struct flow6_entry *)(void *)
        ft_flow6_layout_entry_ptr_(ft, idx);
}

static inline unsigned
fcore_flow6_layout_entry_idx_(const struct ft_flow6_table *ft,
                              const struct flow6_entry *hdr)
{
    return ft_flow6_layout_entry_idx_(
        ft, hdr);
}

RIX_HASH_HEAD(fcore_flow6_ht);

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_flow6_table *ft =                                         \
        (const struct ft_flow6_table *)(const void *)base;                    \
    return fcore_flow6_layout_entry_idx_(ft, p);                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_flow6_table *ft =                                         \
        (const struct ft_flow6_table *)(const void *)base;                    \
    return fcore_flow6_layout_entry_ptr_(ft, i);                              \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(fcore_flow6_ht, flow6_entry,
    key, meta.cur_hash, meta.slot, ft_flow6_cmp, ft_flow6_hash_fn)

#define FCORE_LAYOUT_ENTRY_PTR(owner, idx) \
    fcore_flow6_layout_entry_ptr_((owner), (idx))

#define FCORE_LAYOUT_ENTRY_INDEX(owner, entry) \
    fcore_flow6_layout_entry_idx_((owner), (entry))

#undef FCORE_LAYOUT_HASH_BASE
#define FCORE_LAYOUT_HASH_BASE(owner) \
    ((struct flow6_entry *)(void *)(owner))

#undef FCORE_STATS
#define FCORE_STATS(owner) ((owner)->stats.core)

#undef FCORE_HASH_MASK
#define FCORE_HASH_MASK(owner, ht) ((owner)->start_mask)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
FCORE_GENERATE(flow6, ft_flow6_table, fcore_flow6_ht,
               ft_flow6_hash_fn, ft_flow6_cmp)
#pragma GCC diagnostic pop

#undef FCORE_HASH_MASK

/* Clean up FCORE hooks before FT_TABLE_GENERATE */
#undef FCORE_LAYOUT_ENTRY_PTR
#undef FCORE_LAYOUT_ENTRY_INDEX
#undef FCORE_LAYOUT_HASH_BASE
#undef FCORE_STATS
#undef RIX_HASH_SLOT_DEFINE_INDEXERS

/*===========================================================================
 * FT_TABLE layer: flow6_entry slot + FT_TABLE_GENERATE
 *===========================================================================*/

#define FTG_LAYOUT_INIT_STORAGE(ft, array, stride, entry_offset)               \
    do {                                                                       \
        (ft)->pool_base = (unsigned char *)(array);                            \
        (ft)->pool_stride = (stride);                                          \
        (ft)->pool_entry_offset = (entry_offset);                              \
        (ft)->pool = FT_RECORD_MEMBER_PTR((ft)->pool_base, (ft)->pool_stride, \
                                          1u, (ft)->pool_entry_offset,         \
                                          struct flow6_entry);                 \
    } while (0)

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct flow6_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx) ft_flow6_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flow6_layout_entry_idx_((ft), (entry))

#define FTG_LAYOUT_ENTRY_AT(ft, off0)                                          \
    ft_flow6_layout_entry_ptr_((ft), (unsigned)(off0) + 1u)

#define FTG_ENTRY_TYPE(p) struct flow6_entry
#define FTG_ENTRY_IS_ACTIVE(entry, flag_active) ((entry)->meta.cur_hash != 0u)
#define FTG_ON_INSERT_SUCCESS(entry, flag_active) ((void)(entry))
#define FTG_ENTRY_META_CLEAR_TAIL(entry) ((void)(entry))

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_flow6_table *ft =                                         \
        (const struct ft_flow6_table *)(const void *)base;                    \
    return ft_flow6_layout_entry_idx_(ft, p);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_flow6_table *ft =                                         \
        (const struct ft_flow6_table *)(const void *)base;                    \
    return ft_flow6_layout_entry_ptr_(ft, i);                                 \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(ft_flow6_ht, flow6_entry, key,
                                 meta.cur_hash, meta.slot,
                                 ft_flow6_cmp, ft_flow6_hash_fn)

/* Enable FCORE-delegating bulk ops in FT_TABLE_GENERATE */
#define FTG_USE_FCORE 1

#include "ft_table_generate.h"

#ifdef FT_ARCH_SUFFIX
int _FTG_API(flow6, init_ex)(struct ft_flow6_table *ft,
                             void *array, unsigned max_entries,
                             size_t stride, size_t entry_offset,
                             const struct ft_table_config *cfg);
int _FTG_API(flow6, init)(struct ft_flow6_table *ft,
                          struct flow6_entry *pool,
                          unsigned max_entries,
                          const struct ft_table_config *cfg);
void _FTG_API(flow6, destroy)(struct ft_flow6_table *ft);
void _FTG_API(flow6, flush)(struct ft_flow6_table *ft);
unsigned _FTG_API(flow6, nb_entries)(const struct ft_flow6_table *ft);
unsigned _FTG_API(flow6, nb_bk)(const struct ft_flow6_table *ft);
void _FTG_API(flow6, stats)(const struct ft_flow6_table *ft,
                            struct ft_table_stats *out);
void _FTG_API(flow6, status)(const struct ft_flow6_table *ft,
                             struct fcore_status *out);
u32 _FTG_API(flow6, find)(struct ft_flow6_table *ft,
                               const struct flow6_key *key,
                               u64 now);
void _FTG_API(flow6, find_bulk)(struct ft_flow6_table *ft,
                                const struct flow6_key *keys,
                                unsigned nb_keys,
                                u64 now,
                                struct ft_table_result *results);
u32 _FTG_API(flow6, add_idx)(struct ft_flow6_table *ft,
                                  u32 entry_idx,
                                  u64 now);
unsigned _FTG_API(flow6, add_idx_bulk)(struct ft_flow6_table *ft,
                                       u32 *entry_idxv,
                                       unsigned nb_keys,
                                       enum ft_add_policy policy,
                                       u64 now,
                                       u32 *unused_idxv);
u32 _FTG_API(flow6, del_key)(struct ft_flow6_table *ft,
                                  const struct flow6_key *key);
u32 _FTG_API(flow6, del_entry_idx)(struct ft_flow6_table *ft,
                                        u32 entry_idx);
void _FTG_API(flow6, del_entry_idx_bulk)(struct ft_flow6_table *ft,
                                         const u32 *entry_idxv,
                                         unsigned nb_keys);
int _FTG_API(flow6, walk)(struct ft_flow6_table *ft,
                          int (*cb)(u32 entry_idx, void *arg),
                          void *arg);
int _FTG_API(flow6, grow_2x)(struct ft_flow6_table *ft);
int _FTG_API(flow6, reserve)(struct ft_flow6_table *ft,
                             unsigned min_entries);
#endif

FT_TABLE_GENERATE(flow6,
                  FT_FLOW6_DEFAULT_MIN_NB_BK,
                  FT_FLOW6_DEFAULT_MAX_NB_BK,
                  FT_FLOW6_DEFAULT_GROW_FILL_PCT,
                  0u,
                  ft_flow6_hash_fn, ft_flow6_cmp)

#ifdef FT_ARCH_SUFFIX
#include "ft_ops.h"
FT_OPS_TABLE(flow6, FT_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
