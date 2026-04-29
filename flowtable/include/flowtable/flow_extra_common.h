/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_extra_common.h - protocol-independent types for the flowtable
 * slot_extra variant. Mirrors flow_common.h but uses the 192 B bucket
 * (rix_hash_bucket_extra_s) and a slimmed maintenance context.
 */

#ifndef _FLOW_EXTRA_COMMON_H_
#define _FLOW_EXTRA_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>
#include <rix/rix_queue.h>

#include "flow_common.h"   /* reuse FT_ARCH_*, ft_add_policy, flow_stats,
                              flow_status, ft_table_variant */

#ifndef FT_TABLE_EXTRA_BUCKET_SIZE
#define FT_TABLE_EXTRA_BUCKET_SIZE 192u
#endif

struct ft_table_extra_config {
    unsigned ts_shift;
};

struct ft_table_extra_stats {
    struct flow_stats core;
    u64 grow_execs;
    u64 grow_failures;
    u64 maint_calls;
    u64 maint_bucket_checks;
    u64 maint_evictions;
    u64 maint_extras_loaded;   /* EXTRA: bucket extra[] reads in maintain */
};

RIX_HASH_HEAD(ft_table_extra_ht);

struct ft_table_extra {
    struct rix_hash_bucket_extra_s *buckets;
    unsigned char                  *pool_base;
    size_t                          pool_stride;
    size_t                          pool_entry_offset;
    size_t                          meta_offset;        /* record base -> meta */
    struct ft_table_extra_ht        ht_head;
    unsigned                        start_mask;
    unsigned                        nb_bk;
    unsigned                        max_entries;
    u8                              variant;
    u8                              ts_shift;
    u8                              reserved0[6];
    struct ft_table_extra_stats     stats;
    struct flow_status              status;
};

/*
 * Maintenance context.  ft_table_extra_maintain (bucket sweep) only needs
 * buckets/rhh_nb/stats/rhh_mask/ts_shift.  ft_table_extra_maintain_idx_bulk
 * needs to resolve (bucket, slot) from entry->meta, so the pool pointer,
 * stride, entry offset, and max_entries are also provided.
 */
struct ft_maint_extra_ctx {
    struct rix_hash_bucket_extra_s *buckets;
    unsigned                       *rhh_nb;
    struct ft_table_extra_stats    *stats;
    unsigned char                  *pool_base;
    size_t                          pool_stride;
    size_t                          meta_off;
    unsigned                        max_entries;
    unsigned                        rhh_mask;
    u8                              ts_shift;
};

/**
 * @brief Initialize an ft_table_extra over a caller-provided record pool and
 *        bucket-array memory.
 *
 * The pool is described as a contiguous array of @p max_entries records each
 * of @p stride bytes; the variant entry struct begins @p entry_offset bytes
 * into each record.  Bucket storage is carved from @p buckets of size
 * @p bucket_size bytes; the largest power-of-two block of correctly-aligned
 * struct rix_hash_bucket_extra_s entries that fits is used.  @p variant
 * selects the entry layout.
 *
 * @param ft           Table handle to initialize (zeroed on success).
 * @param variant      Variant tag; FT_TABLE_VARIANT_FLOW4,
 *                     FT_TABLE_VARIANT_FLOW6, or FT_TABLE_VARIANT_FLOWU.
 * @param array        Record pool memory.
 * @param max_entries  Number of records in the pool.
 * @param stride       Per-record stride in bytes (>= sizeof(entry struct)).
 * @param entry_offset Byte offset of the entry struct within each record.
 * @param buckets      Bucket-array backing memory.
 * @param bucket_size  Size in bytes of @p buckets.
 * @param cfg          Optional config (NULL = defaults).
 * @return 0 on success, -1 on invalid arguments or insufficient bucket memory.
 */
int ft_table_extra_init(struct ft_table_extra *ft,
                        enum ft_table_variant variant,
                        void *array,
                        unsigned max_entries,
                        size_t stride,
                        size_t entry_offset,
                        void *buckets,
                        size_t bucket_size,
                        const struct ft_table_extra_config *cfg);

/**
 * @brief Tear down a table.  Zeroes @p ft.  Caller-owned memory is not freed.
 */
void ft_table_extra_destroy(struct ft_table_extra *ft);

/**
 * @brief Drop every entry without releasing memory or stats counters of
 *        long-running interest; resets occupancy and per-flush status.
 */
void ft_table_extra_flush(struct ft_table_extra *ft);

/**
 * @brief Number of entries currently registered in the hash table.
 */
unsigned ft_table_extra_nb_entries(const struct ft_table_extra *ft);

/**
 * @brief Number of buckets in the live hash table (power of two).
 */
unsigned ft_table_extra_nb_bk(const struct ft_table_extra *ft);

/**
 * @brief Snapshot performance / maintenance counters into @p out.
 *        @p out is zeroed if @p ft is NULL.
 */
void ft_table_extra_stats_get(const struct ft_table_extra *ft,
                              struct ft_table_extra_stats *out);

/**
 * @brief Snapshot fast-changing status into @p out
 *        (per-flush / per-window counters).
 */
void ft_table_extra_status_get(const struct ft_table_extra *ft,
                               struct flow_status *out);

/**
 * @brief Register a pre-populated entry by index.
 *
 * @param ft         Table handle.
 * @param entry_idx  1-origin index into the record pool.
 * @param now        Current TSC; encoded into bucket extra[].
 * @return The actually registered index (existing one on duplicate),
 *         or @c RIX_NIL if the table is full.
 */
u32 ft_table_extra_add_idx(struct ft_table_extra *ft, u32 entry_idx,
                           u64 now);

/**
 * @brief Bulk index-based add.  Each slot of @p entry_idxv is read as a
 *        pre-populated request idx and rewritten with the actually
 *        registered idx (request idx, or existing idx on ignore-duplicate).
 *        Released indices (request rejected, or replaced existing) are
 *        appended to @p unused_idxv.
 *
 * @return The number of entries appended to @p unused_idxv.
 */
unsigned ft_table_extra_add_idx_bulk(struct ft_table_extra *ft,
                                     u32 *entry_idxv, unsigned nb_keys,
                                     enum ft_add_policy policy, u64 now,
                                     u32 *unused_idxv);

/**
 * @brief Like ft_table_extra_add_idx_bulk(), but on bucket-full collision
 *        opportunistically expires entries older than @p timeout in the
 *        target bucket pair before retrying the insert.  @p min_bk_used
 *        is the occupancy floor below which a bucket is left untouched.
 *        Up to @p max_unused freed indices are returned in @p unused_idxv.
 */
unsigned ft_table_extra_add_idx_bulk_maint(struct ft_table_extra *ft,
                                           u32 *entry_idxv,
                                           unsigned nb_keys,
                                           enum ft_add_policy policy,
                                           u64 now, u64 timeout,
                                           u32 *unused_idxv,
                                           unsigned max_unused,
                                           unsigned min_bk_used);

/**
 * @brief Remove an entry by index.
 * @return The freed index (== @p entry_idx) on success, @c RIX_NIL on miss.
 */
u32 ft_table_extra_del_idx(struct ft_table_extra *ft, u32 entry_idx);

/**
 * @brief Bulk index-based delete.  For each slot @p entry_idxv[i],
 *        deletes the entry if registered and appends the freed index to
 *        @p unused_idxv.
 * @return The number of entries appended to @p unused_idxv.
 */
unsigned ft_table_extra_del_idx_bulk(struct ft_table_extra *ft,
                                     const u32 *entry_idxv,
                                     unsigned nb_keys,
                                     u32 *unused_idxv);

/**
 * @brief Iterate every registered entry, invoking @p cb(entry_idx, arg).
 *        Walk stops if @p cb returns non-zero.
 * @return The first non-zero @p cb return value, or 0 if walk completed.
 */
int ft_table_extra_walk(struct ft_table_extra *ft,
                        int (*cb)(u32 entry_idx, void *arg),
                        void *arg);

/**
 * @brief Move all entries to a fresh bucket array of larger size.  After
 *        this call the table uses @p new_buckets and the bucket mask is
 *        updated; @p ft->start_mask is unchanged so hash-pair derivation
 *        remains stable.
 *
 * @param new_buckets        Memory for the new bucket array.
 * @param new_bucket_size    Size in bytes; must yield more buckets than the
 *                           current array.
 * @return 0 on success, -1 on size or alignment issues.
 */
int ft_table_extra_migrate(struct ft_table_extra *ft,
                           void *new_buckets, size_t new_bucket_size);

/**
 * @brief Refresh the bucket-side timestamp for @p entry_idx (best-effort).
 *
 * Convenience void wrapper around ft_table_extra_touch_checked(); silently
 * ignores stale or out-of-range indices.  Kept for ABI compatibility.
 *
 * @param ft         Table handle.
 * @param entry_idx  1-origin entry index (must currently live in the table).
 * @param now        Current TSC value; encoded with @c ft->ts_shift.
 */
void ft_table_extra_touch(struct ft_table_extra *ft, u32 entry_idx,
                          u64 now);

/**
 * @brief Refresh the bucket-side timestamp for @p entry_idx, validating that
 *        the current bucket layout still maps the entry.
 *
 * Validates @p ft state, that @p entry_idx is in range, that the bucket
 * resolved via @c ht_head.rhh_mask still holds @p entry_idx at the slot
 * recorded in the entry meta, and only then writes the encoded timestamp.
 * This is migrate-safe (uses the live mask, not @c start_mask) and rejects
 * deleted or relocated entries instead of corrupting unrelated slots.
 *
 * @param ft         Table handle.
 * @param entry_idx  1-origin entry index.
 * @param now        Current TSC value; encoded with @c ft->ts_shift.
 * @return 0 on success, -1 if @p ft / @p entry_idx is invalid or the entry
 *         no longer maps to the recorded slot.
 */
int ft_table_extra_touch_checked(struct ft_table_extra *ft, u32 entry_idx,
                                 u64 now);

/**
 * @brief Populate @p ctx from @p ft so it can be passed to
 *        ft_table_extra_maintain() / ft_table_extra_maintain_idx_bulk().
 *
 * Fills @c buckets, @c rhh_nb, @c stats, @c pool_base, @c pool_stride,
 * @c meta_off (resolved per variant at @c ft_table_extra_init() time),
 * @c max_entries, @c rhh_mask, @c ts_shift.  Other fields are zeroed.
 *
 * @param ft   Initialized table handle.
 * @param ctx  Maintenance context to fill.
 * @return 0 on success, -1 if @p ft / @p ctx is NULL or @p ft is uninitialized.
 *
 * Call again after ft_table_extra_migrate() so @c rhh_mask and @c buckets
 * reflect the new bucket array.
 */
int ft_table_extra_maint_ctx_init(struct ft_table_extra *ft,
                                  struct ft_maint_extra_ctx *ctx);

/**
 * @brief Bucket-sweep maintenance: scan up to one full revolution starting at
 *        @p start_bk and evict entries whose bucket extra[] timestamp is
 *        older than @p expire_tsc relative to @p now.
 *
 * @param ctx              Maintenance context (see ft_table_extra_maint_ctx_init()).
 * @param start_bk         First bucket to scan; modulo bucket count.
 * @param now              Current TSC value.
 * @param expire_tsc       TSC duration after which an entry is considered stale.
 * @param expired_idxv     Out: idx of evicted entries, capped at @p max_expired.
 * @param max_expired      Capacity of @p expired_idxv.
 * @param min_bk_entries   Skip buckets with fewer occupied slots than this.
 *                         Pass 0 (or 1) to disable the sparse fast-skip.
 * @param next_bk          Out: bucket to resume the next sweep from.
 * @return Number of entries written to @p expired_idxv.
 */
unsigned ft_table_extra_maintain(const struct ft_maint_extra_ctx *ctx,
                                 unsigned start_bk,
                                 u64 now, u64 expire_tsc,
                                 u32 *expired_idxv, unsigned max_expired,
                                 unsigned min_bk_entries,
                                 unsigned *next_bk);

/**
 * @brief Maintenance over a known idx list (e.g. expiry candidates produced
 *        by a sampler).  Resolves each idx -> (bucket, slot) via the entry
 *        meta and evicts when the timestamp is stale.
 *
 * @param entry_idxv     Input list of 1-origin idx values to inspect.
 * @param nb_idx         Length of @p entry_idxv.
 * @param expire_tsc     Same semantics as ft_table_extra_maintain().
 * @param expired_idxv   Out: evicted idx values (up to @p max_expired).
 * @param min_bk_entries Same semantics as ft_table_extra_maintain().
 * @param enable_filter  If non-zero, also skips entries whose bucket falls
 *                       below @p min_bk_entries occupancy.
 * @return Number of entries written to @p expired_idxv.
 */
unsigned ft_table_extra_maintain_idx_bulk(const struct ft_maint_extra_ctx *ctx,
                                          const u32 *entry_idxv,
                                          unsigned nb_idx,
                                          u64 now, u64 expire_tsc,
                                          u32 *expired_idxv,
                                          unsigned max_expired,
                                          unsigned min_bk_entries,
                                          int enable_filter);

/**
 * @brief Recommended raw-bucket-memory size for a table holding
 *        @p max_entries.  Includes alignment slack so the carve helper
 *        always yields enough power-of-two buckets.
 */
size_t ft_table_extra_bucket_size(unsigned max_entries);

/**
 * @brief Exact bucket-array byte count for @p nb_bk buckets (no slack).
 *        Useful when the caller already knows the desired bucket count.
 */
static inline size_t
ft_table_extra_bucket_mem_size(unsigned nb_bk)
{
    return (size_t)nb_bk * sizeof(struct rix_hash_bucket_extra_s);
}

/**
 * @brief Select which compiled SIMD variants are runtime-allowed for the
 *        flowtable extra-variant ops.  Bitmask of FT_ARCH_* values; pass
 *        FT_ARCH_AUTO to use the best available.
 */
void ft_arch_extra_init(unsigned arch_enable);

/**
 * @brief Convenience macro that derives @c stride and @c entry_offset from
 *        the entry struct type and member name, then calls
 *        ft_table_extra_init().
 */
#define FT_TABLE_EXTRA_INIT_TYPED(ft, variant, array, max_entries, type,      \
                                  member, buckets, bucket_size, cfg)          \
    ft_table_extra_init((ft), (variant), (array), (max_entries),              \
                        sizeof(type), offsetof(type, member),                 \
                        (buckets), (bucket_size), (cfg))

#endif /* _FLOW_EXTRA_COMMON_H_ */
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
