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

#include <rix/rix_defs_private.h>

#include "flow_common.h"
#include "flow_key.h"

/*===========================================================================
 * Init / destroy / query
 *===========================================================================*/

/**
 * @brief Initialise a flow6 table.
 *
 * Same ownership model as flow4: caller provides records and raw bucket
 * memory, and the library only stores indices into the record pool.
 *
 * @return 0 on success, -1 on invalid arguments.
 */
int ft_flow6_table_init(struct ft_table *ft,
                        void *array,
                        unsigned max_entries,
                        size_t stride,
                        size_t entry_offset,
                        void *buckets,
                        size_t bucket_size,
                        const struct ft_table_config *cfg);

/** @brief Destroy the table handle; caller-owned records/buckets are not freed. */
void ft_flow6_table_destroy(struct ft_table *ft);
/** @brief Remove all registered flow6 entries while keeping memory attached. */
void ft_flow6_table_flush(struct ft_table *ft);
/** @brief Return the current number of registered flow6 entries. */
unsigned ft_flow6_table_nb_entries(const struct ft_table *ft);
/** @brief Return the number of active hash buckets. */
unsigned ft_flow6_table_nb_bk(const struct ft_table *ft);
/** @brief Copy cumulative counters into @p out. */
void ft_flow6_table_stats(const struct ft_table *ft,
                          struct ft_table_stats *out);
/** @brief Copy current status counters into @p out. */
void ft_flow6_table_status(const struct ft_table *ft,
                           struct flow_status *out);

/*===========================================================================
 * Single-key operations
 *===========================================================================*/

/**
 * @brief Look up a flow6 key and optionally refresh its timestamp.
 *
 * @param now Current time. Pass 0 for non-touching lookup.
 * @return Registered entry index on hit, RIX_NIL on miss.
 */
u32 ft_flow6_table_find(struct ft_table *ft,
                        const struct flow6_key *key,
                        u64 now);

/**
 * @brief Bulk lookup for @p nb_keys flow6 keys.
 */
void ft_flow6_table_find_bulk(struct ft_table *ft,
                              const struct flow6_key *keys,
                              unsigned nb_keys,
                              u64 now,
                              struct ft_table_result *results);

/**
 * @brief Register an already-populated flow6 entry by 1-origin index.
 *
 * @return Registered index on success or duplicate, RIX_NIL on failure.
 */
u32 ft_flow6_table_add_idx(struct ft_table *ft,
                           u32 entry_idx,
                           u64 now);

/**
 * @brief Mark @p entry_idx as permanent so timeout maintenance will skip it.
 */
static inline int
ft_flow6_table_set_permanent_idx(struct ft_table *ft, u32 entry_idx)
{
    struct flow6_entry *entry;

    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return -1;
    entry = FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, entry_idx,
                                 ft->pool_entry_offset, struct flow6_entry);
    flow_timestamp_set_permanent(&entry->meta);
    return 0;
}

/**
 * @brief Bulk register already-populated flow6 entries by index.
 *
 * @p entry_idxv is input/output: candidates in, registered indices out.
 * Unused or replaced indices are appended to @p unused_idxv.
 *
 * @return Number of indices written to @p unused_idxv.
 */
unsigned ft_flow6_table_add_idx_bulk(struct ft_table *ft,
                                     u32 *entry_idxv,
                                     unsigned nb_keys,
                                     enum ft_add_policy policy,
                                     u64 now,
                                     u32 *unused_idxv);


/**
 * @brief Add flow6 entries with inline registered-bucket maintenance.
 *
 * Same semantics as ft_flow4_table_add_idx_bulk_maint(), using flow6 entry
 * metadata for expiry.
 */
unsigned ft_flow6_table_add_idx_bulk_maint(struct ft_table *ft,
                                           u32 *entry_idxv,
                                           unsigned nb_keys,
                                           enum ft_add_policy policy,
                                           u64 now,
                                           u64 timeout,
                                           u32 *unused_idxv,
                                           unsigned max_unused,
                                           unsigned min_bk_used);

/**
 * @brief Bulk delete by flow6 key.
 *
 * Removed indices are appended to @p unused_idxv.
 *
 * @return Number of removed entries.
 */
unsigned ft_flow6_table_del_key_bulk(struct ft_table *ft,
                                     const struct flow6_key *keys,
                                     unsigned nb_keys,
                                     u32 *unused_idxv);

/**
 * @brief Delete one registered flow6 entry by index.
 *
 * @return Removed index, or RIX_NIL when not registered.
 */
u32 ft_flow6_table_del_idx(struct ft_table *ft,
                           u32 entry_idx);

/**
 * @brief Bulk delete by 1-origin entry indices.
 *
 * Removed indices are appended to @p unused_idxv.
 *
 * @return Number of removed entries.
 */
unsigned ft_flow6_table_del_idx_bulk(struct ft_table *ft,
                                     const u32 *entry_idxv,
                                     unsigned nb_keys,
                                     u32 *unused_idxv);

/*===========================================================================
 * Walk / grow
 *===========================================================================*/

/** @brief Iterate registered flow6 entries in bucket order. */
int ft_flow6_table_walk(struct ft_table *ft,
                        int (*cb)(u32 entry_idx, void *arg),
                        void *arg);

/**
 * @brief Migrate entries to a new bucket region (grow, shrink, or rehash).
 */
int ft_flow6_table_migrate(struct ft_table *ft,
                           void *new_buckets,
                           size_t new_bucket_size);

/*===========================================================================
 * Convenience macros / inline helpers
 *===========================================================================*/

#define FT_FLOW6_TABLE_INIT_TYPED(ft, array, max_entries, type, member,       \
                                  buckets, bucket_size, cfg)                  \
    FT_TABLE_INIT_TYPED((ft), FT_TABLE_VARIANT_FLOW6, (array),                \
                        (max_entries), type, member,                          \
                        (buckets), (bucket_size), (cfg))

/** @brief Return the containing record pointer for a 1-origin entry index. */
static inline void *
ft_flow6_table_record_ptr(struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_PTR(ft->pool_base, ft->pool_stride, entry_idx);
}

/** @brief Const variant of ft_flow6_table_record_ptr(). */
static inline const void *
ft_flow6_table_record_cptr(const struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_CPTR(ft->pool_base, ft->pool_stride, entry_idx);
}

/** @brief Return the embedded flow6_entry pointer for @p entry_idx. */
static inline struct flow6_entry *
ft_flow6_table_entry_ptr(struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, entry_idx,
                                ft->pool_entry_offset, struct flow6_entry);
}

/** @brief Const variant of ft_flow6_table_entry_ptr(). */
static inline const struct flow6_entry *
ft_flow6_table_entry_cptr(const struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_CPTR(ft->pool_base, ft->pool_stride, entry_idx,
                                 ft->pool_entry_offset,
                                 struct flow6_entry);
}

/** @brief Convert an embedded flow6_entry pointer back to a 1-origin index. */
static inline u32
ft_flow6_table_entry_idx(const struct ft_table *ft,
                         const struct flow6_entry *entry)
{
    if (ft == NULL || entry == NULL)
        return 0u;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

static inline size_t
ft_flow6_table_record_stride(const struct ft_table *ft)
{
    return ft == NULL ? 0u : ft->pool_stride;
}

static inline size_t
ft_flow6_table_entry_offset(const struct ft_table *ft)
{
    return ft == NULL ? 0u : ft->pool_entry_offset;
}

#define FT_FLOW6_TABLE_RECORD_PTR_AS(ft, type, entry_idx)                     \
    ((type *)ft_flow6_table_record_ptr((ft), (entry_idx)))

#define FT_FLOW6_TABLE_RECORD_CPTR_AS(ft, type, entry_idx)                    \
    ((const type *)ft_flow6_table_record_cptr((ft), (entry_idx)))

#define FT_FLOW6_TABLE_RECORD_FROM_ENTRY(type, member, entry_ptr)             \
    RIX_CONTAINER_OF((entry_ptr), type, member)

#define FT_FLOW6_TABLE_ENTRY_FROM_RECORD(record_ptr, member)                  \
    FT_MEMBER_PTR((record_ptr), member)


static inline u32
ft_flow6_table_add_entry(struct ft_table *ft, u32 entry_idx)
{
    return ft_flow6_table_add_idx(ft, entry_idx, 0u);
}

static inline u32
ft_flow6_table_add_entry_idx(struct ft_table *ft, u32 entry_idx)
{
    return ft_flow6_table_add_idx(ft, entry_idx, 0u);
}

static inline u32
ft_flow6_table_del_key_oneshot(struct ft_table *ft,
                               const struct flow6_key *key)
{
    u32 idx;
    return ft_flow6_table_del_key_bulk(ft, key, 1u, &idx) > 0u ? idx : 0u;
}

/*===========================================================================
 * Maintenance (protocol-free, delegates to ft_table_maintain)
 *===========================================================================*/

static inline unsigned
ft_flow6_table_maintain(struct ft_table *ft,
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
                        offsetof(struct flow6_entry, meta),
        .max_entries  = ft->max_entries,
        .ts_shift     = ft->ts_shift,
        .stats        = &ft->stats,
    };
    return ft_table_maintain(&ctx, start_bk, now, expire_tsc, expired_idxv,
                             max_expired, min_bk_entries, next_bk);
}

static inline unsigned
ft_flow6_table_maintain_idx_bulk(struct ft_table *ft,
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
                        offsetof(struct flow6_entry, meta),
        .max_entries  = ft->max_entries,
        .ts_shift     = ft->ts_shift,
        .stats        = &ft->stats,
    };
    return ft_table_maintain_idx_bulk(&ctx, entry_idxv, nb_idx, now,
                                      expire_tsc,
                                      expired_idxv, max_expired,
                                      min_bk_entries, enable_filter);
}

#endif /* _FLOW6_TABLE_H_ */
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
