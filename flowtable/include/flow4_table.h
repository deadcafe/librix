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

#include "flow_common.h"
#include "flow_key.h"

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

/**
 * @brief Add entries with inline registered-bucket maintenance.
 *
 * Superset of add_idx_bulk.  After the normal add phase, scans the
 * registered bucket of each result entry for expired entries and removes
 * them.  Both add-unused and maint-expired indices are packed into
 * @p unused_idxv.
 *
 * Two knobs control the maint cost:
 *
 *  - **Scan budget** = @p max_unused - @p nb_keys  (called "α").
 *    The first @p nb_keys slots of @p unused_idxv are reserved for
 *    add-phase results; the remaining α slots are the budget for
 *    maint-expired entries.  When α == 0 (or @p timeout == 0) the
 *    maint phase is skipped entirely and the function behaves
 *    identically to add_idx_bulk.
 *
 *  - **Bucket occupancy threshold** (@p min_bk_used).
 *    Before scanning a bucket for expired entries the function counts
 *    occupied slots.  If the count is less than @p min_bk_used the
 *    bucket is skipped — avoiding expensive per-entry meta reads on
 *    sparse buckets.  Pass 0 to disable the filter.
 *
 * Tuning guide (with auto-timeout fill-target controller):
 *
 *    α and min_bk_used together determine expire capacity per batch.
 *    Increasing α or decreasing min_bk_used raises the expire rate
 *    (lowers steady-state fill) but increases per-key cost.
 *    Recommended starting point for batch=256, target fill ≤ 75%:
 *
 *      max_unused  = nb_keys + 64          (α = 64)
 *      min_bk_used = 10                    (skip buckets < 10/16)
 *      fill-target = 65%
 *
 *    At 2 Mpps / 3% miss this yields ~115 cy/key total (add+maint),
 *    compared to ~200 cy/key with separate add_idx_bulk + maintain.
 *
 * @param ft          Flow table.
 * @param entry_idxv  [in/out] Entry indices.  On return each element
 *                    is either the inserted/found index or 0 on failure.
 * @param nb_keys     Number of entries in @p entry_idxv.
 * @param policy      Add policy (FT_ADD_IGNORE / FT_ADD_FORCE_EXPIRE).
 * @param now         Current timestamp (TSC or nanoseconds).
 * @param timeout     Expiry threshold.  0 disables maint entirely.
 * @param unused_idxv [out] Buffer for unused + expired entry indices.
 *                    Must hold at least @p max_unused elements.
 * @param max_unused  Capacity of @p unused_idxv.  Must be >= @p nb_keys.
 *                    max_unused - nb_keys is the maint scan budget (α).
 * @param min_bk_used Bucket occupancy threshold (0–16).  Buckets with
 *                    fewer occupied slots are skipped.
 *
 * @return Total entries written to @p unused_idxv
 *         (add-unused + maint-expired).
 */
unsigned ft_flow4_table_add_idx_bulk_maint(struct ft_table *ft,
                                           u32 *entry_idxv,
                                           unsigned nb_keys,
                                           enum ft_add_policy policy,
                                           u64 now,
                                           u64 timeout,
                                           u32 *unused_idxv,
                                           unsigned max_unused,
                                           unsigned min_bk_used);


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
        .max_entries  = ft->max_entries,
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
        .max_entries  = ft->max_entries,
        .ts_shift     = ft->ts_shift,
        .stats        = &ft->stats,
    };
    return ft_table_maintain_idx_bulk(&ctx, entry_idxv, nb_idx, now,
                                      expire_tsc,
                                      expired_idxv, max_expired,
                                      min_bk_entries, enable_filter);
}

#endif /* _FLOW4_TABLE_H_ */
