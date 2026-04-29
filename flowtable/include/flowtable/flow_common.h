/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_COMMON_H_
#define _FLOW_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <rix/rix_queue.h>

#ifndef FT_TABLE_CACHE_LINE_SIZE
#define FT_TABLE_CACHE_LINE_SIZE 64u
#endif

/*===========================================================================
 * Architecture dispatch flags
 *===========================================================================*/
#ifndef FT_ARCH_GEN
#define FT_ARCH_GEN     0u
#endif
#ifndef FT_ARCH_SSE
#define FT_ARCH_SSE     (1u << 0)
#endif
#ifndef FT_ARCH_AVX2
#define FT_ARCH_AVX2    (1u << 1)
#endif
#ifndef FT_ARCH_AVX512
#define FT_ARCH_AVX512  (1u << 2)
#endif
#ifndef FT_ARCH_AUTO
#define FT_ARCH_AUTO    (FT_ARCH_SSE | FT_ARCH_AVX2 | FT_ARCH_AVX512)
#endif

#ifndef FT_MEMBER_PTR
#define FT_MEMBER_PTR(objp, member) (&((objp)->member))
#endif

#ifndef FT_BYTE_PTR
#define FT_BYTE_PTR(ptr) ((unsigned char *)(void *)(ptr))
#endif

#ifndef FT_BYTE_CPTR
#define FT_BYTE_CPTR(ptr) ((const unsigned char *)(const void *)(ptr))
#endif

#ifndef FT_PTR_ADDR
#define FT_PTR_ADDR(ptr) ((uintptr_t)(const void *)(ptr))
#endif

#ifndef FT_PTR_IS_ALIGNED
#define FT_PTR_IS_ALIGNED(ptr, align)                                         \
    ((FT_PTR_ADDR(ptr) & (uintptr_t)((align) - 1u)) == 0u)
#endif

#ifndef FT_BYTE_PTR_ADD
#define FT_BYTE_PTR_ADD(base, bytes)                                          \
    (FT_BYTE_PTR(base) + (size_t)(bytes))
#endif

#ifndef FT_BYTE_CPTR_ADD
#define FT_BYTE_CPTR_ADD(base, bytes)                                         \
    (FT_BYTE_CPTR(base) + (size_t)(bytes))
#endif

#ifndef FT_RECORD_PTR
#define FT_RECORD_PTR(base, stride, idx)                                      \
    ((idx) == RIX_NIL ? NULL : (void *)FT_BYTE_PTR_ADD(                       \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FT_RECORD_CPTR
#define FT_RECORD_CPTR(base, stride, idx)                                     \
    ((idx) == RIX_NIL ? NULL : (const void *)FT_BYTE_CPTR_ADD(                \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FT_RECORD_MEMBER_PTR
#define FT_RECORD_MEMBER_PTR(base, stride, idx, member_offset, type)          \
    ((type *)__builtin_assume_aligned(                                        \
        FT_BYTE_PTR_ADD(FT_RECORD_PTR((base), (stride), (idx)),               \
                        (member_offset)),                                     \
        _Alignof(type)))
#endif

#ifndef FT_RECORD_MEMBER_CPTR
#define FT_RECORD_MEMBER_CPTR(base, stride, idx, member_offset, type)         \
    ((const type *)__builtin_assume_aligned(                                  \
        FT_BYTE_CPTR_ADD(FT_RECORD_CPTR((base), (stride), (idx)),             \
                         (member_offset)),                                    \
        _Alignof(type)))
#endif

static inline unsigned
ft_record_index_from_member_ptr(const void *base,
                                size_t stride,
                                size_t member_offset,
                                const void *member_ptr)
{
    uintptr_t member_addr;
    uintptr_t base_addr;
    ptrdiff_t delta;

    if (member_ptr == NULL)
        return RIX_NIL;
    member_addr = (uintptr_t)member_ptr;
    base_addr = (uintptr_t)FT_BYTE_CPTR_ADD(base, member_offset);
    delta = (ptrdiff_t)(member_addr - base_addr);
    RIX_ASSERT(delta >= 0);
    RIX_ASSERT(stride != 0u);
    RIX_ASSERT(((size_t)delta % stride) == 0u);
    return (unsigned)((size_t)delta / stride) + 1u;
}

static inline unsigned
ft_roundup_pow2_u32(unsigned v)
{
    if (v <= 1u)
        return 1u;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1u;
}

static inline unsigned
ft_rounddown_pow2_u32(unsigned v)
{
    if (v == 0u)
        return 0u;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v - (v >> 1);
}

/*===========================================================================
 * Protocol-independent types (shared by all flow variants)
 *===========================================================================*/

struct ft_table_result {
    u32 entry_idx;
};

enum ft_add_policy {
    FT_ADD_IGNORE              = 0u,
    FT_ADD_UPDATE              = 1u,
    FT_ADD_IGNORE_FORCE_EXPIRE = 2u,
    FT_ADD_UPDATE_FORCE_EXPIRE = 3u,
};

struct ft_table_config {
    unsigned ts_shift;
};

enum {
    FT_TABLE_BUCKET_SIZE  = 128u,
    FT_TABLE_MIN_NB_BK   = 4096u,
    FT_TABLE_BUCKET_ALIGN = 64u,
};

/**
 * @brief Carve out an aligned bucket region from a raw buffer.
 *
 * Given (raw, raw_size), aligns the pointer up to bucket alignment,
 * computes the largest power-of-2 bucket count that fits, and returns
 * the aligned pointer.  *nb_bk_out is set to the bucket count (0 on
 * failure).
 */
static inline struct rix_hash_bucket_s *
ft_table_bucket_carve(void *raw, size_t raw_size, unsigned *nb_bk_out)
{
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + (_Alignof(struct rix_hash_bucket_s) - 1u))
                      & ~(uintptr_t)(_Alignof(struct rix_hash_bucket_s) - 1u);
    size_t lost = (size_t)(aligned - addr);
    size_t usable = raw_size > lost ? raw_size - lost : 0u;
    unsigned nb = (unsigned)(usable / sizeof(struct rix_hash_bucket_s));

    nb = ft_rounddown_pow2_u32(nb);
    *nb_bk_out = nb;
    return (struct rix_hash_bucket_s *)aligned;
}

/**
 * @brief Compute the required bucket memory size for a given max_entries.
 *
 * Returns 128 * 2^n bytes (n >= 12, i.e. minimum 4096 buckets = 512 KiB).
 * The caller allocates this many bytes and passes the pointer and size
 * to ft_*_table_init().  The buffer need not be aligned; the library
 * aligns internally.
 */
static inline size_t
ft_table_bucket_size(unsigned max_entries)
{
    unsigned nb_bk;
    nb_bk = (max_entries + (RIX_HASH_BUCKET_ENTRY_SZ - 1u))
          / RIX_HASH_BUCKET_ENTRY_SZ;
    if (nb_bk < FT_TABLE_MIN_NB_BK)
        nb_bk = FT_TABLE_MIN_NB_BK;
    return (size_t)ft_roundup_pow2_u32(nb_bk) * FT_TABLE_BUCKET_SIZE;
}

/**
 * @brief Return the bucket memory size for a given bucket count.
 *
 * Useful for computing grow / shrink allocation sizes from the current
 * bucket count (e.g. @c ft_table_bucket_mem_size(ft->nb_bk) * 2 for
 * a 2x grow).  Protocol-independent.
 */
static inline size_t
ft_table_bucket_mem_size(unsigned nb_bk)
{
    return (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
}

/*===========================================================================
 * Shared datapath statistics
 *===========================================================================*/
struct flow_stats {
    u64 lookups;
    u64 hits;
    u64 misses;
    u64 adds;
    u64 add_existing;
    u64 add_failed;
    u64 force_expired;
    u64 dels;
    u64 del_miss;
};

struct flow_status {
    u32 entries;
    u32 kickouts;
    u32 add_bk0;
    u32 add_bk1;
};

static inline void
flow_status_reset(struct flow_status *status, u32 entries)
{
    if (status == NULL)
        return;
    status->entries = entries;
    status->kickouts = 0u;
    status->add_bk0 = 0u;
    status->add_bk1 = 0u;
}

struct ft_table_stats {
    struct flow_stats core;
    u64 grow_execs;
    u64 grow_failures;
    u64 maint_calls;
    u64 maint_bucket_checks;
    u64 maint_evictions;
};

enum ft_table_variant {
    FT_TABLE_VARIANT_FLOW4 = 1u,
    FT_TABLE_VARIANT_FLOW6 = 2u,
    FT_TABLE_VARIANT_FLOWU = 3u,
};

RIX_HASH_HEAD(ft_table_ht);

struct ft_table {
    struct rix_hash_bucket_s *buckets;
    unsigned char            *pool_base;
    size_t                    pool_stride;
    size_t                    pool_entry_offset;
    struct ft_table_ht        ht_head;
    unsigned                  start_mask;
    unsigned                  nb_bk;
    unsigned                  max_entries;
    u8                        variant;
    u8                        ts_shift;
    u8                        reserved0[6];
    struct ft_table_stats     stats;
    struct flow_status        status;
};

/**
 * @brief Initialize a pure table over caller-owned records and buckets.
 *
 * Use this directly when the variant is selected at runtime.  If the record
 * type is known at compile time, prefer FT_TABLE_INIT_TYPED() or the
 * FT_FLOW*_TABLE_INIT_TYPED() wrappers so stride and entry_offset are derived
 * from the C type.
 *
 * @param ft           Table handle to initialize.
 * @param variant      FT_TABLE_VARIANT_FLOW4, FLOW6, or FLOWU.
 * @param array        Caller-owned record pool.
 * @param max_entries  Number of records in @p array; indices are 1-origin.
 * @param stride       Byte distance between adjacent records.
 * @param entry_offset Offset of the flow entry member inside each record.
 * @param buckets      Raw bucket memory. Alignment is not required.
 * @param bucket_size  Size of @p buckets in bytes.
 * @param cfg          Optional config; NULL selects defaults.
 * @return 0 on success, -1 on invalid arguments or insufficient buckets.
 */
int ft_table_init(struct ft_table *ft,
                  enum ft_table_variant variant,
                  void *array,
                  unsigned max_entries,
                  size_t stride,
                  size_t entry_offset,
                  void *buckets,
                  size_t bucket_size,
                  const struct ft_table_config *cfg);
/**
 * @brief Reset a pure table handle; caller-owned memory is not freed.
 */
void ft_table_destroy(struct ft_table *ft);
/**
 * @brief Remove all registered entries while keeping allocated buckets.
 */
void ft_table_flush(struct ft_table *ft);
/**
 * @brief Return the current number of registered entries.
 */
unsigned ft_table_nb_entries(const struct ft_table *ft);
/**
 * @brief Return the number of active buckets in the hash table.
 */
unsigned ft_table_nb_bk(const struct ft_table *ft);
/**
 * @brief Copy cumulative table counters into @p out.
 */
void ft_table_stats(const struct ft_table *ft, struct ft_table_stats *out);
/**
 * @brief Copy current status counters into @p out.
 */
void ft_table_status(const struct ft_table *ft, struct flow_status *out);
/**
 * @brief Register an already-populated record by 1-origin index.
 *
 * The key is read from the record's embedded flow entry.  On duplicate, the
 * existing index is returned according to the active add policy.
 *
 * @return Registered index on success or duplicate, RIX_NIL on failure.
 */
u32 ft_table_add_idx(struct ft_table *ft, u32 entry_idx, u64 now);
/**
 * @brief Bulk variant of ft_table_add_idx().
 *
 * Each @p entry_idxv[i] is read as the candidate index and overwritten with
 * the registered index, or 0/RIX_NIL on failure depending on the handler.
 * Indices that were not inserted are appended to @p unused_idxv.
 *
 * @return Number of indices written to @p unused_idxv.
 */
unsigned ft_table_add_idx_bulk(struct ft_table *ft,
                               u32 *entry_idxv,
                               unsigned nb_keys,
                               enum ft_add_policy policy,
                               u64 now,
                               u32 *unused_idxv);
/**
 * @brief Bulk add with inline maintenance on buckets touched by add results.
 *
 * @p unused_idxv stores both add-unused and maintenance-expired indices.
 * @p max_unused must be >= @p nb_keys; the extra capacity
 * (@p max_unused - @p nb_keys) is the maintenance expiration budget.
 *
 * @return Total number of indices written to @p unused_idxv.
 */
unsigned ft_table_add_idx_bulk_maint(struct ft_table *ft,
                                     u32 *entry_idxv,
                                     unsigned nb_keys,
                                     enum ft_add_policy policy,
                                     u64 now,
                                     u64 timeout,
                                     u32 *unused_idxv,
                                     unsigned max_unused,
                                     unsigned min_bk_used);
/**
 * @brief Remove the entry currently registered at @p entry_idx.
 *
 * @return The removed index, or RIX_NIL when it was not registered.
 */
u32 ft_table_del_idx(struct ft_table *ft, u32 entry_idx);
/**
 * @brief Bulk remove by 1-origin indices.
 *
 * Removed indices are appended to @p unused_idxv.
 *
 * @return Number of removed indices written to @p unused_idxv.
 */
unsigned ft_table_del_idx_bulk(struct ft_table *ft,
                               const u32 *entry_idxv,
                               unsigned nb_keys,
                               u32 *unused_idxv);
/**
 * @brief Iterate registered entries in bucket order.
 *
 * The callback receives a 1-origin entry index.  Returning non-zero stops the
 * walk and that value is returned to the caller.
 */
int ft_table_walk(struct ft_table *ft,
                  int (*cb)(u32 entry_idx, void *arg),
                  void *arg);
/**
 * @brief Move all entries to a new caller-owned bucket array.
 *
 * The record pool does not move.  On success the table switches to
 * @p new_buckets; on failure the old table remains active.
 *
 * @return 0 on success, -1 on invalid size or migration failure.
 */
int ft_table_migrate(struct ft_table *ft,
                     void *new_buckets,
                     size_t new_bucket_size);

#define FT_TABLE_INIT_TYPED(ft, variant, array, max_entries, type, member,    \
                            buckets, bucket_size, cfg)                        \
    ft_table_init((ft), (variant), (array), (max_entries), sizeof(type),      \
                  offsetof(type, member), (buckets), (bucket_size), (cfg))

/*===========================================================================
 * Protocol-free maintenance context
 *===========================================================================*/
struct ft_maint_ctx {
    struct rix_hash_bucket_s *buckets;
    unsigned                 *rhh_nb;
    const unsigned char      *pool_base;
    struct ft_table_stats    *stats;
    size_t                    pool_stride;
    size_t                    meta_off;
    unsigned                  max_entries;
    unsigned                  rhh_mask;
    u8                        ts_shift;
};

/**
 * @brief Sweep buckets linearly and evict expired entries.
 *
 * Scans buckets starting from @p start_bk, wrapping around the full
 * table if needed, until @p max_expired entries have been collected or
 * all buckets have been visited.  Buckets whose occupancy is below
 * @p min_bk_entries are skipped.
 *
 * @param ctx            Maintenance context (pool layout, buckets, stats).
 * @param start_bk       Bucket index to begin scanning (masked internally).
 * @param now            Current timestamp (raw TSC / nanoseconds).
 * @param expire_tsc     Expiry threshold in the same unit as @p now.
 *                       Entries older than @p now - @p expire_tsc are evicted.
 *                       Pass 0 to disable expiry (returns 0 immediately).
 * @param expired_idxv   Output array receiving evicted entry indices.
 * @param max_expired    Capacity of @p expired_idxv.
 * @param min_bk_entries Skip buckets with fewer than this many occupied slots.
 *                       Use 0 or 1 to scan every bucket unconditionally.
 * @param next_bk        If non-NULL, receives the next bucket index to resume
 *                       scanning in a subsequent call.
 * @return Number of evicted entries written to @p expired_idxv.
 */
unsigned ft_table_maintain(const struct ft_maint_ctx *ctx,
                           unsigned start_bk,
                           u64 now,
                           u64 expire_tsc,
                           u32 *expired_idxv,
                           unsigned max_expired,
                           unsigned min_bk_entries,
                           unsigned *next_bk);

/**
 * @brief Evict expired entries from buckets derived from a list of entry indices.
 *
 * For each entry in @p entry_idxv, reads its flow_entry_meta and validates
 * that the referenced bucket/slot still owns the index before scanning that
 * bucket for expired entries. Uses a three-stage software pipeline
 * (meta prefetch -> bucket identification + bucket prefetch -> bucket scan)
 * to hide memory latency. Stale or already-freed indices are skipped.
 *
 * @param ctx            Maintenance context (pool layout, buckets, stats).
 * @param entry_idxv     Array of entry indices whose buckets are to be scanned.
 * @param nb_idx         Number of entries in @p entry_idxv.
 * @param now            Current timestamp (raw TSC / nanoseconds).
 * @param expire_tsc     Expiry threshold in the same unit as @p now.
 *                       Pass 0 to disable expiry (returns 0 immediately).
 * @param expired_idxv   Output array receiving evicted entry indices.
 * @param max_expired    Capacity of @p expired_idxv.
 * @param min_bk_entries Skip buckets with fewer than this many occupied slots.
 *                       Use 0 or 1 to scan every bucket unconditionally.
 * @param enable_filter  If non-zero, enable a direct-mapped duplicate bucket
 *                       filter (64-entry cache) to skip buckets already scanned
 *                       in this call.  Effective when @p nb_idx is large and
 *                       many entries map to the same bucket.
 * @return Number of evicted entries written to @p expired_idxv.
 */
unsigned ft_table_maintain_idx_bulk(const struct ft_maint_ctx *ctx,
                                    const u32 *entry_idxv,
                                    unsigned nb_idx,
                                    u64 now,
                                    u64 expire_tsc,
                                    u32 *expired_idxv,
                                    unsigned max_expired,
                                    unsigned min_bk_entries,
                                    int enable_filter);

/**
 * @brief One-time CPU detection and SIMD dispatch selection.
 *
 * Call once at startup before any table operations.
 *
 * @param arch_enable  Bitmask of FT_ARCH_* flags, or FT_ARCH_AUTO.
 */
void ft_arch_init(unsigned arch_enable);

#endif /* _FLOW_COMMON_H_ */
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
