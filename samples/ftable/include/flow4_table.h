/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_TABLE_H_
#define _FLOW4_TABLE_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_defs_private.h>

#include "ft_table_common.h"
#include <flow/flow_key.h>
#include <flow/flow_core.h>

/*===========================================================================
 * Init / destroy / query
 *===========================================================================*/

/**
 * @brief Initialise a flow4 table.
 *
 * @p buckets / @p bucket_size need not be aligned — the library carves
 * out the largest power-of-2 aligned region internally
 * (see ft_table_bucket_carve()).  The resulting bucket count must be
 * >= FT_TABLE_MIN_NB_BK (4096), otherwise init fails.
 *
 * Use ft_table_bucket_size() to query the recommended allocation size.
 *
 * @return 0 on success, -1 on invalid arguments.
 */
int ft_flow4_table_init(struct ft_table *ft,
                        void *array,
                        unsigned max_entries,
                        size_t stride,
                        size_t entry_offset,
                        void *buckets,
                        size_t bucket_size,
                        const struct ft_table_config *cfg);

void ft_flow4_table_destroy(struct ft_table *ft);
void ft_flow4_table_flush(struct ft_table *ft);
unsigned ft_flow4_table_nb_entries(const struct ft_table *ft);
unsigned ft_flow4_table_nb_bk(const struct ft_table *ft);
void ft_flow4_table_stats(const struct ft_table *ft,
                          struct ft_table_stats *out);
void ft_flow4_table_status(const struct ft_table *ft,
                           struct flow_status *out);

/*===========================================================================
 * Single-key operations
 *===========================================================================*/

u32 ft_flow4_table_find(struct ft_table *ft,
                             const struct flow4_key *key,
                             u64 now);

void ft_flow4_table_find_bulk(struct ft_table *ft,
                              const struct flow4_key *keys,
                              unsigned nb_keys,
                              u64 now,
                              struct ft_table_result *results);

u32 ft_flow4_table_add_idx(struct ft_table *ft,
                                u32 entry_idx,
                                u64 now);

static inline int
ft_flow4_table_set_permanent_idx(struct ft_table *ft, u32 entry_idx)
{
    struct flow4_entry *entry;

    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return -1;
    entry = FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, entry_idx,
                                 ft->pool_entry_offset, struct flow4_entry);
    flow_timestamp_set_permanent(&entry->meta);
    return 0;
}

unsigned ft_flow4_table_add_idx_bulk(struct ft_table *ft,
                                     u32 *entry_idxv,
                                     unsigned nb_keys,
                                     enum ft_add_policy policy,
                                     u64 now,
                                     u32 *unused_idxv);


unsigned ft_flow4_table_del_key_bulk(struct ft_table *ft,
                                    const struct flow4_key *keys,
                                    unsigned nb_keys,
                                    u32 *unused_idxv);

u32 ft_flow4_table_del_idx(struct ft_table *ft,
                                      u32 entry_idx);

unsigned ft_flow4_table_del_idx_bulk(struct ft_table *ft,
                                       const u32 *entry_idxv,
                                       unsigned nb_keys,
                                       u32 *unused_idxv);

/*===========================================================================
 * Walk / grow
 *===========================================================================*/

int ft_flow4_table_walk(struct ft_table *ft,
                        int (*cb)(u32 entry_idx, void *arg),
                        void *arg);

/**
 * @brief Migrate entries to a new bucket region (grow, shrink, or rehash).
 *
 * @p new_buckets / @p new_bucket_size need not be aligned — the library
 * carves out the largest power-of-2 aligned region internally.
 *
 * Constraints on the carved bucket count (new_nb_bk):
 *  - new_nb_bk > start_mask  (i.e. >= init-time bucket count)
 *  - new_nb_bk != current nb_bk is NOT required (same-size rehash allowed)
 *
 * On success the table switches to the new buckets; the caller frees the
 * old bucket memory (read ft->buckets / ft->nb_bk before calling).
 * On failure (-1) the table is unchanged.
 */
int ft_flow4_table_migrate(struct ft_table *ft,
                           void *new_buckets,
                           size_t new_bucket_size);

/*===========================================================================
 * Convenience macros / inline helpers
 *===========================================================================*/

#define FT_FLOW4_TABLE_INIT_TYPED(ft, array, max_entries, type, member, \
                                  buckets, bucket_size, cfg)           \
    FT_TABLE_INIT_TYPED((ft), FT_TABLE_VARIANT_FLOW4, (array),          \
                        (max_entries), type, member,                    \
                        (buckets), (bucket_size), (cfg))

static inline void *
ft_flow4_table_record_ptr(struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_PTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline const void *
ft_flow4_table_record_cptr(const struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_CPTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline struct flow4_entry *
ft_flow4_table_entry_ptr(struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, entry_idx,
                                ft->pool_entry_offset, struct flow4_entry);
}

static inline const struct flow4_entry *
ft_flow4_table_entry_cptr(const struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_CPTR(ft->pool_base, ft->pool_stride, entry_idx,
                                 ft->pool_entry_offset,
                                 struct flow4_entry);
}

static inline u32
ft_flow4_table_entry_idx(const struct ft_table *ft,
                         const struct flow4_entry *entry)
{
    if (ft == NULL || entry == NULL)
        return 0u;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

static inline size_t
ft_flow4_table_record_stride(const struct ft_table *ft)
{
    return ft == NULL ? 0u : ft->pool_stride;
}

static inline size_t
ft_flow4_table_entry_offset(const struct ft_table *ft)
{
    return ft == NULL ? 0u : ft->pool_entry_offset;
}

#define FT_FLOW4_TABLE_RECORD_PTR_AS(ft, type, entry_idx) \
    ((type *)ft_flow4_table_record_ptr((ft), (entry_idx)))

#define FT_FLOW4_TABLE_RECORD_CPTR_AS(ft, type, entry_idx) \
    ((const type *)ft_flow4_table_record_cptr((ft), (entry_idx)))

#define FT_FLOW4_TABLE_RECORD_FROM_ENTRY(type, member, entry_ptr) \
    RIX_CONTAINER_OF((entry_ptr), type, member)

#define FT_FLOW4_TABLE_ENTRY_FROM_RECORD(record_ptr, member) \
    FT_MEMBER_PTR((record_ptr), member)


/* Compatibility wrappers */
static inline u32
ft_flow4_table_add_entry(struct ft_table *ft, u32 entry_idx)
{
    return ft_flow4_table_add_idx(ft, entry_idx, 0u);
}

static inline u32
ft_flow4_table_add_entry_idx(struct ft_table *ft, u32 entry_idx)
{
    return ft_flow4_table_add_idx(ft, entry_idx, 0u);
}

static inline u32
ft_flow4_table_del_key_oneshot(struct ft_table *ft,
                               const struct flow4_key *key)
{
    u32 idx;
    return ft_flow4_table_del_key_bulk(ft, key, 1u, &idx) > 0u ? idx : 0u;
}

/*===========================================================================
 * Maintenance (protocol-free, delegates to ft_table_maintain)
 *===========================================================================*/

static inline unsigned
ft_flow4_table_maintain(struct ft_table *ft,
                        unsigned start_bk,
                        u64 now,
                        u64 expire_tsc,
                        u32 *expired_idxv,
                        unsigned max_expired,
                        unsigned min_bk_entries,
                        unsigned *next_bk)
{
    struct ft_maint_ctx ctx = {
        .buckets      = ft->buckets,
        .rhh_mask     = ft->ht_head.rhh_mask,
        .rhh_nb       = &ft->ht_head.rhh_nb,
        .pool_base    = ft->pool_base,
        .pool_stride  = ft->pool_stride,
        .meta_off     = ft->pool_entry_offset +
                        offsetof(struct flow4_entry, meta),
        .ts_shift     = ft->ts_shift,
        .stats        = &ft->stats,
    };
    return ft_table_maintain(&ctx, start_bk, now, expire_tsc, expired_idxv,
                             max_expired, min_bk_entries, next_bk);
}

static inline unsigned
ft_flow4_table_maintain_idx_bulk(struct ft_table *ft,
                                 const u32 *entry_idxv,
                                 unsigned nb_idx,
                                 u64 now,
                                 u64 expire_tsc,
                                 u32 *expired_idxv,
                                 unsigned max_expired,
                                 unsigned min_bk_entries,
                                 int enable_filter)
{
    struct ft_maint_ctx ctx = {
        .buckets      = ft->buckets,
        .rhh_mask     = ft->ht_head.rhh_mask,
        .rhh_nb       = &ft->ht_head.rhh_nb,
        .pool_base    = ft->pool_base,
        .pool_stride  = ft->pool_stride,
        .meta_off     = ft->pool_entry_offset +
                        offsetof(struct flow4_entry, meta),
        .ts_shift     = ft->ts_shift,
        .stats        = &ft->stats,
    };
    return ft_table_maintain_idx_bulk(&ctx, entry_idxv, nb_idx, now,
                                      expire_tsc,
                                      expired_idxv, max_expired,
                                      min_bk_entries, enable_filter);
}

#endif /* _FLOW4_TABLE_H_ */
