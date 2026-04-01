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

static inline struct ft_flow6_entry *
ft_flow6_layout_entry_ptr_(const struct ft_flow6_table *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset, struct ft_flow6_entry);
}

static inline unsigned
ft_flow6_layout_entry_idx_(const struct ft_flow6_table *ft,
                           const struct ft_flow6_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

#define FTG_LAYOUT_INIT_STORAGE(ft, array, stride, entry_offset)               \
    do {                                                                       \
        (ft)->pool_base = (unsigned char *)(array);                            \
        (ft)->pool_stride = (stride);                                          \
        (ft)->pool_entry_offset = (entry_offset);                              \
        (ft)->pool = FT_RECORD_MEMBER_PTR((ft)->pool_base, (ft)->pool_stride, \
                                          1u, (ft)->pool_entry_offset,         \
                                          struct ft_flow6_entry);              \
    } while (0)

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct ft_flow6_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx) ft_flow6_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flow6_layout_entry_idx_((ft), (entry))

#define FTG_LAYOUT_ENTRY_AT(ft, off0)                                          \
    ft_flow6_layout_entry_ptr_((ft), (unsigned)(off0) + 1u)

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

RIX_HASH_GENERATE_STATIC_SLOT_EX(ft_flow6_ht, ft_flow6_entry, key, cur_hash,
                                 slot, ft_flow6_cmp, ft_flow6_hash_fn)

#include "ft_table_generate.h"

#ifdef FT_ARCH_SUFFIX
int _FTG_API(flow6, init_ex)(struct ft_flow6_table *ft,
                             void *array, unsigned max_entries,
                             size_t stride, size_t entry_offset,
                             const struct ft_flow6_config *cfg);
int _FTG_API(flow6, init)(struct ft_flow6_table *ft,
                          struct ft_flow6_entry *pool,
                          unsigned max_entries,
                          const struct ft_flow6_config *cfg);
void _FTG_API(flow6, destroy)(struct ft_flow6_table *ft);
void _FTG_API(flow6, flush)(struct ft_flow6_table *ft);
unsigned _FTG_API(flow6, nb_entries)(const struct ft_flow6_table *ft);
unsigned _FTG_API(flow6, nb_bk)(const struct ft_flow6_table *ft);
unsigned _FTG_API(flow6, need_grow)(const struct ft_flow6_table *ft);
void _FTG_API(flow6, stats)(const struct ft_flow6_table *ft,
                            struct ft_flow6_stats *out);
uint32_t _FTG_API(flow6, find)(struct ft_flow6_table *ft,
                               const struct flow6_key *key);
uint32_t _FTG_API(flow6, add_entry)(struct ft_flow6_table *ft,
                                    uint32_t entry_idx);
uint32_t _FTG_API(flow6, del)(struct ft_flow6_table *ft,
                              const struct flow6_key *key);
uint32_t _FTG_API(flow6, del_idx)(struct ft_flow6_table *ft,
                                  uint32_t entry_idx);
void _FTG_API(flow6, find_bulk)(struct ft_flow6_table *ft,
                                const struct flow6_key *keys,
                                unsigned nb_keys,
                                struct ft_flow6_result *results);
void _FTG_API(flow6, add_entry_bulk)(struct ft_flow6_table *ft,
                                     const uint32_t *entry_idxv,
                                     unsigned nb_keys,
                                     struct ft_flow6_result *results);
void _FTG_API(flow6, del_bulk)(struct ft_flow6_table *ft,
                               const struct flow6_key *keys,
                               unsigned nb_keys,
                               struct ft_flow6_result *results);
int _FTG_API(flow6, walk)(struct ft_flow6_table *ft,
                          int (*cb)(uint32_t entry_idx, void *arg),
                          void *arg);
int _FTG_API(flow6, grow_2x)(struct ft_flow6_table *ft);
int _FTG_API(flow6, reserve)(struct ft_flow6_table *ft,
                             unsigned min_entries);
#endif

FT_TABLE_GENERATE(flow6,
                  FT_FLOW6_DEFAULT_MIN_NB_BK,
                  FT_FLOW6_DEFAULT_MAX_NB_BK,
                  FT_FLOW6_DEFAULT_GROW_FILL_PCT,
                  FT_FLOW6_ENTRY_FLAG_ACTIVE,
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
