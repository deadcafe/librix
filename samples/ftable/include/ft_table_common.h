/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FT_TABLE_COMMON_H_
#define _FT_TABLE_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <flow/flow_core.h>

#ifndef FT_TABLE_CACHE_LINE_SIZE
#define FT_TABLE_CACHE_LINE_SIZE 64u
#endif

#ifndef FT_FLOW_FAMILY_IPV4
#define FT_FLOW_FAMILY_IPV4 4u
#endif

#ifndef FT_FLOW_FAMILY_IPV6
#define FT_FLOW_FAMILY_IPV6 6u
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
#define FT_PTR_IS_ALIGNED(ptr, align) \
    ((FT_PTR_ADDR(ptr) & (uintptr_t)((align) - 1u)) == 0u)
#endif

#ifndef FT_BYTE_PTR_ADD
#define FT_BYTE_PTR_ADD(base, bytes) \
    (FT_BYTE_PTR(base) + (size_t)(bytes))
#endif

#ifndef FT_BYTE_CPTR_ADD
#define FT_BYTE_CPTR_ADD(base, bytes) \
    (FT_BYTE_CPTR(base) + (size_t)(bytes))
#endif

#ifndef FT_RECORD_PTR
#define FT_RECORD_PTR(base, stride, idx)                                       \
    ((idx) == RIX_NIL ? NULL : (void *)FT_BYTE_PTR_ADD(                       \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FT_RECORD_CPTR
#define FT_RECORD_CPTR(base, stride, idx)                                      \
    ((idx) == RIX_NIL ? NULL : (const void *)FT_BYTE_CPTR_ADD(                \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FT_RECORD_MEMBER_PTR
#define FT_RECORD_MEMBER_PTR(base, stride, idx, member_offset, type)           \
    ((type *)__builtin_assume_aligned(                                         \
        FT_BYTE_PTR_ADD(FT_RECORD_PTR((base), (stride), (idx)),               \
                        (member_offset)),                                      \
        _Alignof(type)))
#endif

#ifndef FT_RECORD_MEMBER_CPTR
#define FT_RECORD_MEMBER_CPTR(base, stride, idx, member_offset, type)          \
    ((const type *)__builtin_assume_aligned(                                   \
        FT_BYTE_CPTR_ADD(FT_RECORD_CPTR((base), (stride), (idx)),             \
                         (member_offset)),                                     \
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
    FT_ADD_IGNORE = 0u,
    FT_ADD_UPDATE = 1u,
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

struct ft_table_stats {
    struct fcore_stats core;
    u64 grow_execs;
    u64 grow_failures;
    u64 maint_calls;
    u64 maint_bucket_checks;
    u64 maint_evictions;
};

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
    unsigned                  rhh_mask;
    u8                   ts_shift;
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
 * For each entry in @p entry_idxv, reads its flow_entry_meta to determine
 * the owning bucket, then scans that bucket for expired entries.  Uses a
 * three-stage software pipeline (meta prefetch -> bucket identification
 * + bucket prefetch -> bucket scan) to hide memory latency.
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

#endif /* _FT_TABLE_COMMON_H_ */
