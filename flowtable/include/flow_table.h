/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_TABLE_H_
#define _FLOW_TABLE_H_

#include "flowtable/flow4_table.h"
#include "flowtable/flow6_table.h"
#include "flowtable/flowu_table.h"
#include "flowtable/flow_extra_table.h"

/**
 * @file flow_table.h
 *
 * Public umbrella header for the flowtable API.
 *
 * Usage:
 *
 *   #include "flow_table.h"
 *
 * Normal users should include only this header.  It exposes the pure
 * flow4/flow6/flowu tables, the slot-extra variants, and the FT_TABLE_*
 * facade macros that dispatch between pure and slot-extra table types.
 * "Pure" means the base table model: 128 B buckets with timestamps stored
 * in the caller-owned flow entry metadata, without bucket-side extra slots.
 *
 * Lower-level family headers live under the flowtable include namespace and
 * may be included directly by code that intentionally wants a narrow API
 * surface.
 */

/**
 * @brief Type-generic initializer for pure ft_table and slot-extra tables.
 *
 * Dispatches on @p ft pointer type.  For pure tables @p cfg must be a
 * struct ft_table_config pointer (or NULL).  For slot-extra tables @p cfg
 * must be a struct ft_table_extra_config pointer (or NULL).
 *
 * Usage:
 *
 *   struct my_record { struct flow4_entry flow; ... };
 *   struct ft_table ft;
 *   FT_TABLE_INIT_TYPED(&ft, FT_TABLE_VARIANT_FLOW4, records, n,
 *                       struct my_record, flow, buckets, bucket_size, NULL);
 */
#undef FT_TABLE_INIT_TYPED
#define FT_TABLE_INIT_TYPED(ft, variant, array, max_entries, type, member,    \
                            buckets, bucket_size, cfg)                        \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_init,                                     \
        struct ft_table_extra *: ft_table_extra_init                          \
    )((ft), (variant), (array), (max_entries), sizeof(type),                  \
      offsetof(type, member), (buckets), (bucket_size), (cfg))

/**
 * @brief Type-generic dispatch helpers for APIs shared by ft_table and
 *        ft_table_extra.
 *
 * These macros hide pure vs slot-extra for protocol-independent work:
 * init/destroy, add/del by index, stats/status, migrate, walk, and
 * maintenance.  Key-specific operations are intentionally not hidden because
 * flow4/flow6/flowu and extra keys have different C types.
 *
 * Usage:
 *
 *   FT_TABLE_ADD_IDX(&ft, entry_idx, now);
 *   FT_TABLE_STATS(&ft, &stats);
 *
 * Exception:
 *
 *   Use ft_flow4_table_find(), ft_flow6_table_find(), ft_flowu_table_find(),
 *   flow4_extra_table_find(), flow6_extra_table_find(), or
 *   flowu_extra_table_find() for key lookup.
 */
/** @brief Destroy a pure or slot-extra table handle. */
#define FT_TABLE_DESTROY(ft)                                                  \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_destroy,                                  \
        struct ft_table_extra *: ft_table_extra_destroy                       \
    )((ft))

/** @brief Remove all entries from a pure or slot-extra table. */
#define FT_TABLE_FLUSH(ft)                                                    \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_flush,                                    \
        struct ft_table_extra *: ft_table_extra_flush                         \
    )((ft))

/** @brief Return the number of registered entries. */
#define FT_TABLE_NB_ENTRIES(ft)                                               \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_nb_entries,                               \
        const struct ft_table *: ft_table_nb_entries,                         \
        struct ft_table_extra *: ft_table_extra_nb_entries,                   \
        const struct ft_table_extra *: ft_table_extra_nb_entries              \
    )((ft))

/** @brief Return the number of active buckets. */
#define FT_TABLE_NB_BK(ft)                                                    \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_nb_bk,                                    \
        const struct ft_table *: ft_table_nb_bk,                              \
        struct ft_table_extra *: ft_table_extra_nb_bk,                        \
        const struct ft_table_extra *: ft_table_extra_nb_bk                   \
    )((ft))

/** @brief Copy cumulative table counters into @p out. */
#define FT_TABLE_STATS(ft, out)                                               \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_stats,                                    \
        const struct ft_table *: ft_table_stats,                              \
        struct ft_table_extra *: ft_table_extra_stats_get,                    \
        const struct ft_table_extra *: ft_table_extra_stats_get               \
    )((ft), (out))

/** @brief Copy current status counters into @p out. */
#define FT_TABLE_STATUS(ft, out)                                              \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_status,                                   \
        const struct ft_table *: ft_table_status,                             \
        struct ft_table_extra *: ft_table_extra_status_get,                   \
        const struct ft_table_extra *: ft_table_extra_status_get              \
    )((ft), (out))

/** @brief Register one already-populated record by 1-origin index. */
#define FT_TABLE_ADD_IDX(ft, entry_idx, now)                                  \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_add_idx,                                  \
        struct ft_table_extra *: ft_table_extra_add_idx                       \
    )((ft), (entry_idx), (now))

/** @brief Bulk register already-populated records by index. */
#define FT_TABLE_ADD_IDX_BULK(ft, entry_idxv, nb_keys, policy, now,           \
                              unused_idxv)                                    \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_add_idx_bulk,                             \
        struct ft_table_extra *: ft_table_extra_add_idx_bulk                  \
    )((ft), (entry_idxv), (nb_keys), (policy), (now), (unused_idxv))

/** @brief Bulk add with inline maintenance on buckets touched by add results. */
#define FT_TABLE_ADD_IDX_BULK_MAINT(ft, entry_idxv, nb_keys, policy, now,     \
                                    timeout, unused_idxv, max_unused,         \
                                    min_bk_used)                              \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_add_idx_bulk_maint,                       \
        struct ft_table_extra *: ft_table_extra_add_idx_bulk_maint            \
    )((ft), (entry_idxv), (nb_keys), (policy), (now), (timeout),              \
      (unused_idxv), (max_unused), (min_bk_used))

/** @brief Remove one registered record by 1-origin index. */
#define FT_TABLE_DEL_IDX(ft, entry_idx)                                       \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_del_idx,                                  \
        struct ft_table_extra *: ft_table_extra_del_idx                       \
    )((ft), (entry_idx))

/** @brief Bulk remove registered records by 1-origin index. */
#define FT_TABLE_DEL_IDX_BULK(ft, entry_idxv, nb_keys, unused_idxv)           \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_del_idx_bulk,                             \
        struct ft_table_extra *: ft_table_extra_del_idx_bulk                  \
    )((ft), (entry_idxv), (nb_keys), (unused_idxv))

/** @brief Iterate registered entries in bucket order. */
#define FT_TABLE_WALK(ft, cb, arg)                                            \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_walk,                                     \
        struct ft_table_extra *: ft_table_extra_walk                          \
    )((ft), (cb), (arg))

/** @brief Move entries to a new caller-owned bucket array. */
#define FT_TABLE_MIGRATE(ft, new_buckets, new_bucket_size)                    \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_migrate,                                  \
        struct ft_table_extra *: ft_table_extra_migrate                       \
    )((ft), (new_buckets), (new_bucket_size))

/**
 * @brief Populate a maintenance context for a pure or slot-extra table.
 *
 * The context object type remains variant-specific because pure and
 * slot-extra store timestamps in different places.  Dispatch is on @p ft:
 * use struct ft_maint_ctx for struct ft_table, and struct ft_maint_extra_ctx
 * for struct ft_table_extra.
 */
static inline int
ft_table_maint_ctx_init(struct ft_table *ft, struct ft_maint_ctx *ctx)
{
    size_t meta_off;

    if (ft == NULL || ctx == NULL || ft->buckets == NULL)
        return -1;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        meta_off = ft->pool_entry_offset + offsetof(struct flow4_entry, meta);
        break;
    case FT_TABLE_VARIANT_FLOW6:
        meta_off = ft->pool_entry_offset + offsetof(struct flow6_entry, meta);
        break;
    case FT_TABLE_VARIANT_FLOWU:
        meta_off = ft->pool_entry_offset + offsetof(struct flowu_entry, meta);
        break;
    default:
        return -1;
    }
    *ctx = (struct ft_maint_ctx) {
        .buckets = ft->buckets,
        .rhh_nb = &ft->ht_head.rhh_nb,
        .pool_base = ft->pool_base,
        .stats = &ft->stats,
        .pool_stride = ft->pool_stride,
        .meta_off = meta_off,
        .max_entries = ft->max_entries,
        .rhh_mask = ft->ht_head.rhh_mask,
        .ts_shift = ft->ts_shift,
    };
    return 0;
}

/** @brief Populate a maintenance context for later FT_TABLE_MAINTAIN calls. */
#define FT_TABLE_MAINT_CTX_INIT(ft, ctx)                                      \
    _Generic((ft),                                                            \
        struct ft_table *: ft_table_maint_ctx_init,                           \
        struct ft_table_extra *: ft_table_extra_maint_ctx_init                \
    )((ft), (ctx))

/** @brief Sweep buckets and evict expired entries. */
#define FT_TABLE_MAINTAIN(ctx, start_bk, now, expire_tsc, expired_idxv,       \
                          max_expired, min_bk_entries, next_bk)               \
    _Generic((ctx),                                                           \
        const struct ft_maint_ctx *: ft_table_maintain,                       \
        struct ft_maint_ctx *: ft_table_maintain,                             \
        const struct ft_maint_extra_ctx *: ft_table_extra_maintain,           \
        struct ft_maint_extra_ctx *: ft_table_extra_maintain                  \
    )((ctx), (start_bk), (now), (expire_tsc), (expired_idxv),                 \
      (max_expired), (min_bk_entries), (next_bk))

/** @brief Evict expired entries from buckets derived from an index list. */
#define FT_TABLE_MAINTAIN_IDX_BULK(ctx, entry_idxv, nb_idx, now, expire_tsc,  \
                                   expired_idxv, max_expired,                 \
                                   min_bk_entries, enable_filter)             \
    _Generic((ctx),                                                           \
        const struct ft_maint_ctx *: ft_table_maintain_idx_bulk,              \
        struct ft_maint_ctx *: ft_table_maintain_idx_bulk,                    \
        const struct ft_maint_extra_ctx *: ft_table_extra_maintain_idx_bulk,  \
        struct ft_maint_extra_ctx *: ft_table_extra_maintain_idx_bulk         \
    )((ctx), (entry_idxv), (nb_idx), (now), (expire_tsc), (expired_idxv),     \
      (max_expired), (min_bk_entries), (enable_filter))

#endif /* _FLOW_TABLE_H_ */
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
