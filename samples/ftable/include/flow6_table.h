/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW6_TABLE_H_
#define _FLOW6_TABLE_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <rix/rix_defs_private.h>

#include "ft_table_common.h"
#include <flow/flow_key.h>
#include <flow/flow_core.h>

#define FT_FLOW6_DEFAULT_GROW_FILL_PCT 60u
#define FT_FLOW6_DEFAULT_MIN_NB_BK    16384u
#define FT_FLOW6_DEFAULT_MAX_NB_BK    1048576u

RIX_HASH_HEAD(ft_flow6_ht);


struct ft_flow6_table {
    struct rix_hash_bucket_s   *buckets;
    struct flow6_entry         *pool;
    unsigned char              *pool_base;
    size_t                      pool_stride;
    size_t                      pool_entry_offset;
    struct ft_flow6_ht          ht_head;
    struct ft_bucket_allocator  bucket_alloc;
    unsigned                    start_mask;
    unsigned                    nb_bk;
    unsigned                    max_nb_bk;
    unsigned                    max_entries;
    uint32_t                    free_head;
    unsigned                    grow_fill_pct;
    unsigned                    need_grow;
    struct ft_table_stats       stats;
};

/*===========================================================================
 * Init / destroy / query
 *===========================================================================*/

int ft_flow6_table_init_ex(struct ft_flow6_table *ft,
                           void *array,
                           unsigned max_entries,
                           size_t stride,
                           size_t entry_offset,
                           const struct ft_table_config *cfg);

int ft_flow6_table_init(struct ft_flow6_table *ft,
                        struct flow6_entry *pool,
                        unsigned max_entries,
                        const struct ft_table_config *cfg);

void ft_flow6_table_destroy(struct ft_flow6_table *ft);
void ft_flow6_table_flush(struct ft_flow6_table *ft);
unsigned ft_flow6_table_nb_entries(const struct ft_flow6_table *ft);
unsigned ft_flow6_table_nb_bk(const struct ft_flow6_table *ft);
unsigned ft_flow6_table_need_grow(const struct ft_flow6_table *ft);
void ft_flow6_table_stats(const struct ft_flow6_table *ft,
                          struct ft_table_stats *out);

/*===========================================================================
 * Single-key operations
 *===========================================================================*/

uint32_t ft_flow6_table_find(struct ft_flow6_table *ft,
                             const struct flow6_key *key);

void ft_flow6_table_find_bulk(struct ft_flow6_table *ft,
                              const struct flow6_key *keys,
                              unsigned nb_keys,
                              struct ft_table_result *results);

uint32_t ft_flow6_table_add_idx(struct ft_flow6_table *ft,
                                uint32_t entry_idx);

void ft_flow6_table_add_idx_bulk(struct ft_flow6_table *ft,
                                 const uint32_t *entry_idxv,
                                 unsigned nb_keys,
                                 struct ft_table_result *results);


uint32_t ft_flow6_table_del_key(struct ft_flow6_table *ft,
                                const struct flow6_key *key);

uint32_t ft_flow6_table_del_entry_idx(struct ft_flow6_table *ft,
                                      uint32_t entry_idx);

void ft_flow6_table_del_entry_idx_bulk(struct ft_flow6_table *ft,
                                       const uint32_t *entry_idxv,
                                       unsigned nb_keys);

/*===========================================================================
 * Walk / grow
 *===========================================================================*/

int ft_flow6_table_walk(struct ft_flow6_table *ft,
                        int (*cb)(uint32_t entry_idx, void *arg),
                        void *arg);

int ft_flow6_table_grow_2x(struct ft_flow6_table *ft);
int ft_flow6_table_reserve(struct ft_flow6_table *ft,
                           unsigned min_entries);

/*===========================================================================
 * Convenience macros / inline helpers
 *===========================================================================*/

#define FT_FLOW6_TABLE_INIT_TYPED(ft, array, max_entries, type, member, cfg) \
    ft_flow6_table_init_ex((ft), (array), (max_entries), sizeof(type),        \
                           offsetof(type, member), (cfg))

static inline void *
ft_flow6_table_record_ptr(struct ft_flow6_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_PTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline const void *
ft_flow6_table_record_cptr(const struct ft_flow6_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_CPTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline struct flow6_entry *
ft_flow6_table_entry_ptr(struct ft_flow6_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, entry_idx,
                                ft->pool_entry_offset, struct flow6_entry);
}

static inline const struct flow6_entry *
ft_flow6_table_entry_cptr(const struct ft_flow6_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_CPTR(ft->pool_base, ft->pool_stride, entry_idx,
                                 ft->pool_entry_offset,
                                 struct flow6_entry);
}

static inline uint32_t
ft_flow6_table_entry_idx(const struct ft_flow6_table *ft,
                         const struct flow6_entry *entry)
{
    if (ft == NULL || entry == NULL)
        return 0u;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

static inline size_t
ft_flow6_table_record_stride(const struct ft_flow6_table *ft)
{
    return ft == NULL ? 0u : ft->pool_stride;
}

static inline size_t
ft_flow6_table_entry_offset(const struct ft_flow6_table *ft)
{
    return ft == NULL ? 0u : ft->pool_entry_offset;
}

#define FT_FLOW6_TABLE_RECORD_PTR_AS(ft, type, entry_idx) \
    ((type *)ft_flow6_table_record_ptr((ft), (entry_idx)))

#define FT_FLOW6_TABLE_RECORD_CPTR_AS(ft, type, entry_idx) \
    ((const type *)ft_flow6_table_record_cptr((ft), (entry_idx)))

#define FT_FLOW6_TABLE_RECORD_FROM_ENTRY(type, member, entry_ptr) \
    RIX_CONTAINER_OF((entry_ptr), type, member)

#define FT_FLOW6_TABLE_ENTRY_FROM_RECORD(record_ptr, member) \
    FT_MEMBER_PTR((record_ptr), member)


static inline uint32_t
ft_flow6_table_add_entry(struct ft_flow6_table *ft, uint32_t entry_idx)
{
    return ft_flow6_table_add_idx(ft, entry_idx);
}

static inline uint32_t
ft_flow6_table_add_entry_idx(struct ft_flow6_table *ft, uint32_t entry_idx)
{
    return ft_flow6_table_add_idx(ft, entry_idx);
}

static inline uint32_t
ft_flow6_table_del(struct ft_flow6_table *ft, const struct flow6_key *key)
{
    return ft_flow6_table_del_key(ft, key);
}

static inline uint32_t
ft_flow6_table_del_idx(struct ft_flow6_table *ft, uint32_t entry_idx)
{
    return ft_flow6_table_del_entry_idx(ft, entry_idx);
}

#endif /* _FLOW6_TABLE_H_ */
