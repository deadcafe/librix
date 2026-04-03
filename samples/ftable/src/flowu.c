/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot.h>

#include "flowu_table.h"

#define ft_flowu_hash_fn flowu_key_hash
#define ft_flowu_cmp     flowu_key_cmp

static inline struct flowu_entry *
ft_flowu_layout_entry_ptr_(const struct ft_flowu_table *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset, struct flowu_entry);
}

static inline unsigned
ft_flowu_layout_entry_idx_(const struct ft_flowu_table *ft,
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
fcore_flowu_layout_entry_ptr_(const struct ft_flowu_table *ft, unsigned idx)
{
    return (struct flowu_entry *)(void *)
        ft_flowu_layout_entry_ptr_(ft, idx);
}

static inline unsigned
fcore_flowu_layout_entry_idx_(const struct ft_flowu_table *ft,
                              const struct flowu_entry *hdr)
{
    return ft_flowu_layout_entry_idx_(
        ft, hdr);
}

RIX_HASH_HEAD(fcore_flowu_ht);

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_flowu_table *ft =                                         \
        (const struct ft_flowu_table *)(const void *)base;                    \
    return fcore_flowu_layout_entry_idx_(ft, p);                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_flowu_table *ft =                                         \
        (const struct ft_flowu_table *)(const void *)base;                    \
    return fcore_flowu_layout_entry_ptr_(ft, i);                              \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(fcore_flowu_ht, flowu_entry,
    key, htbl_elm.cur_hash, htbl_elm.slot, ft_flowu_cmp, ft_flowu_hash_fn)

#define FCORE_LAYOUT_ENTRY_PTR(owner, idx) \
    fcore_flowu_layout_entry_ptr_((owner), (idx))

#define FCORE_LAYOUT_ENTRY_INDEX(owner, entry) \
    fcore_flowu_layout_entry_idx_((owner), (entry))

#undef FCORE_LAYOUT_HASH_BASE
#define FCORE_LAYOUT_HASH_BASE(owner) \
    ((struct flowu_entry *)(void *)(owner))

#undef FCORE_ON_REMOVE
#define FCORE_ON_REMOVE(owner, entry, idx) \
    do { (entry)->htbl_elm.cur_hash = 0u; } while (0)

#undef FCORE_ON_INSERT
#define FCORE_ON_INSERT(owner, entry, idx)                                    \
    do {                                                                       \
        (void)(entry);                                                         \
        (void)(idx);                                                           \
        if ((owner)->need_grow == 0u &&                                        \
            (owner)->nb_bk < (owner)->max_nb_bk &&                             \
            _FCORE_INT(flowu, fill_pct_)(owner) >= (owner)->grow_fill_pct) {   \
            (owner)->need_grow = 1u;                                           \
            (owner)->stats.grow_marks++;                                       \
        }                                                                      \
    } while (0)

#undef FCORE_ON_ADD_FAIL
#define FCORE_ON_ADD_FAIL(owner, entry, idx)                                  \
    do {                                                                       \
        (void)(entry);                                                         \
        (void)(idx);                                                           \
        if ((owner)->nb_bk < (owner)->max_nb_bk && (owner)->need_grow == 0u) {\
            (owner)->need_grow = 1u;                                           \
            (owner)->stats.grow_marks++;                                       \
        }                                                                      \
    } while (0)

#undef FCORE_HASH_MASK
#define FCORE_HASH_MASK(owner, ht) ((owner)->start_mask)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
FCORE_GENERATE(flowu, ft_flowu_table, fcore_flowu_ht,
               ft_flowu_hash_fn, ft_flowu_cmp)
#pragma GCC diagnostic pop

#undef FCORE_HASH_MASK

/* Clean up FCORE hooks before FT_TABLE_GENERATE */
#undef FCORE_LAYOUT_ENTRY_PTR
#undef FCORE_LAYOUT_ENTRY_INDEX
#undef FCORE_LAYOUT_HASH_BASE
#undef FCORE_ON_INSERT
#undef FCORE_ON_ADD_FAIL
#undef RIX_HASH_SLOT_DEFINE_INDEXERS

/*===========================================================================
 * FT_TABLE layer: flowu_entry slot + FT_TABLE_GENERATE
 *===========================================================================*/

#define FTG_LAYOUT_INIT_STORAGE(ft, array, stride, entry_offset)               \
    do {                                                                       \
        (ft)->pool_base = (unsigned char *)(array);                            \
        (ft)->pool_stride = (stride);                                          \
        (ft)->pool_entry_offset = (entry_offset);                              \
        (ft)->pool = FT_RECORD_MEMBER_PTR((ft)->pool_base, (ft)->pool_stride, \
                                          1u, (ft)->pool_entry_offset,         \
                                          struct flowu_entry);                 \
    } while (0)

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct flowu_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx) ft_flowu_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flowu_layout_entry_idx_((ft), (entry))

#define FTG_LAYOUT_ENTRY_AT(ft, off0)                                          \
    ft_flowu_layout_entry_ptr_((ft), (unsigned)(off0) + 1u)

#define FTG_ENTRY_TYPE(p) struct flowu_entry
#define FTG_ENTRY_IS_ACTIVE(entry, flag_active) ((entry)->htbl_elm.cur_hash != 0u)
#define FTG_ON_INSERT_SUCCESS(entry, flag_active) ((void)(entry))
#define FTG_ENTRY_META_CLEAR_TAIL(entry) ((void)(entry))

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_flowu_table *ft =                                         \
        (const struct ft_flowu_table *)(const void *)base;                    \
    return ft_flowu_layout_entry_idx_(ft, p);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_flowu_table *ft =                                         \
        (const struct ft_flowu_table *)(const void *)base;                    \
    return ft_flowu_layout_entry_ptr_(ft, i);                                 \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(ft_flowu_ht, flowu_entry, key,
                                 htbl_elm.cur_hash, htbl_elm.slot,
                                 ft_flowu_cmp, ft_flowu_hash_fn)

/* Enable FCORE-delegating bulk ops in FT_TABLE_GENERATE */
#define FTG_USE_FCORE 1

#include "ft_table_generate.h"

#ifdef FT_ARCH_SUFFIX
int _FTG_API(flowu, init_ex)(struct ft_flowu_table *ft,
                             void *array, unsigned max_entries,
                             size_t stride, size_t entry_offset,
                             const struct ft_table_config *cfg);
int _FTG_API(flowu, init)(struct ft_flowu_table *ft,
                          struct flowu_entry *pool,
                          unsigned max_entries,
                          const struct ft_table_config *cfg);
void _FTG_API(flowu, destroy)(struct ft_flowu_table *ft);
void _FTG_API(flowu, flush)(struct ft_flowu_table *ft);
unsigned _FTG_API(flowu, nb_entries)(const struct ft_flowu_table *ft);
unsigned _FTG_API(flowu, nb_bk)(const struct ft_flowu_table *ft);
unsigned _FTG_API(flowu, need_grow)(const struct ft_flowu_table *ft);
void _FTG_API(flowu, stats)(const struct ft_flowu_table *ft,
                            struct ft_table_stats *out);
uint32_t _FTG_API(flowu, find)(struct ft_flowu_table *ft,
                               const struct flowu_key *key);
void _FTG_API(flowu, find_bulk)(struct ft_flowu_table *ft,
                                const struct flowu_key *keys,
                                unsigned nb_keys,
                                struct ft_table_result *results);
uint32_t _FTG_API(flowu, add_idx)(struct ft_flowu_table *ft,
                                  uint32_t entry_idx);
void _FTG_API(flowu, add_idx_bulk)(struct ft_flowu_table *ft,
                                   const uint32_t *entry_idxv,
                                   unsigned nb_keys,
                                   struct ft_table_result *results);
uint32_t _FTG_API(flowu, del_key)(struct ft_flowu_table *ft,
                                  const struct flowu_key *key);
uint32_t _FTG_API(flowu, del_entry_idx)(struct ft_flowu_table *ft,
                                        uint32_t entry_idx);
void _FTG_API(flowu, del_entry_idx_bulk)(struct ft_flowu_table *ft,
                                         const uint32_t *entry_idxv,
                                         unsigned nb_keys);
int _FTG_API(flowu, walk)(struct ft_flowu_table *ft,
                          int (*cb)(uint32_t entry_idx, void *arg),
                          void *arg);
int _FTG_API(flowu, grow_2x)(struct ft_flowu_table *ft);
int _FTG_API(flowu, reserve)(struct ft_flowu_table *ft,
                             unsigned min_entries);
#endif

FT_TABLE_GENERATE(flowu,
                  FT_FLOWU_DEFAULT_MIN_NB_BK,
                  FT_FLOWU_DEFAULT_MAX_NB_BK,
                  FT_FLOWU_DEFAULT_GROW_FILL_PCT,
                  0u,
                  ft_flowu_hash_fn, ft_flowu_cmp)

#ifdef FT_ARCH_SUFFIX
#include "ft_ops.h"
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
