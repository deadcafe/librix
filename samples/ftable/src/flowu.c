/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot.h>

#include "flowu_table.h"

static inline union rix_hash_hash_u
ft_flowu_hash_fn(const struct flowu_key *key, uint32_t mask)
{
    union rix_hash_hash_u r = rix_hash_hash_bytes_fast(key, sizeof(*key),
                                                       mask);
    if (r.val32[0] == 0u)
        r.val32[0] = 1u;
    if (r.val32[1] == 0u)
        r.val32[1] = 1u;
    return r;
}

static inline int
ft_flowu_cmp(const struct flowu_key *a, const struct flowu_key *b)
{
    return memcmp(a, b, sizeof(*a));
}

static inline struct ft_flowu_entry *
ft_flowu_layout_entry_ptr_(const struct ft_flowu_table *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset, struct ft_flowu_entry);
}

static inline unsigned
ft_flowu_layout_entry_idx_(const struct ft_flowu_table *ft,
                           const struct ft_flowu_entry *entry)
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
                                          struct ft_flowu_entry);              \
    } while (0)

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct ft_flowu_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx) ft_flowu_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flowu_layout_entry_idx_((ft), (entry))

#define FTG_LAYOUT_ENTRY_AT(ft, off0)                                          \
    ft_flowu_layout_entry_ptr_((ft), (unsigned)(off0) + 1u)

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

RIX_HASH_GENERATE_STATIC_SLOT_EX(ft_flowu_ht, ft_flowu_entry, key, cur_hash,
                                 slot, ft_flowu_cmp, ft_flowu_hash_fn)

#include "ft_table_generate.h"

#ifdef FT_ARCH_SUFFIX
int _FTG_API(flowu, init_ex)(struct ft_flowu_table *ft,
                             void *array, unsigned max_entries,
                             size_t stride, size_t entry_offset,
                             const struct ft_flowu_config *cfg);
int _FTG_API(flowu, init)(struct ft_flowu_table *ft,
                          struct ft_flowu_entry *pool,
                          unsigned max_entries,
                          const struct ft_flowu_config *cfg);
void _FTG_API(flowu, destroy)(struct ft_flowu_table *ft);
void _FTG_API(flowu, flush)(struct ft_flowu_table *ft);
unsigned _FTG_API(flowu, nb_entries)(const struct ft_flowu_table *ft);
unsigned _FTG_API(flowu, nb_bk)(const struct ft_flowu_table *ft);
unsigned _FTG_API(flowu, need_grow)(const struct ft_flowu_table *ft);
void _FTG_API(flowu, stats)(const struct ft_flowu_table *ft,
                            struct ft_flowu_stats *out);
uint32_t _FTG_API(flowu, find)(struct ft_flowu_table *ft,
                               const struct flowu_key *key);
uint32_t _FTG_API(flowu, add_entry)(struct ft_flowu_table *ft,
                                    uint32_t entry_idx);
uint32_t _FTG_API(flowu, del)(struct ft_flowu_table *ft,
                              const struct flowu_key *key);
uint32_t _FTG_API(flowu, del_idx)(struct ft_flowu_table *ft,
                                  uint32_t entry_idx);
void _FTG_API(flowu, find_bulk)(struct ft_flowu_table *ft,
                                const struct flowu_key *keys,
                                unsigned nb_keys,
                                struct ft_flowu_result *results);
void _FTG_API(flowu, add_entry_bulk)(struct ft_flowu_table *ft,
                                     const uint32_t *entry_idxv,
                                     unsigned nb_keys,
                                     struct ft_flowu_result *results);
void _FTG_API(flowu, del_bulk)(struct ft_flowu_table *ft,
                               const struct flowu_key *keys,
                               unsigned nb_keys,
                               struct ft_flowu_result *results);
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
                  FT_FLOWU_ENTRY_FLAG_ACTIVE,
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
