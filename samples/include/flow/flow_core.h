/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_core.h - Shared compile-time layer for fcache / ftable.
 *
 * Provides:
 *   1. Record pool macros (stride/offset-based indirection)
 *   2. Bucket allocator / record layout types
 *   3. FCORE_GENERATE macro: find_bulk, add_idx_bulk, del_idx_bulk,
 *      free list, counters, and optionally findadd_bulk
 *
 * Usage in variant .c files:
 *
 *   // 1. Include owner header (which includes this via common.h)
 *   #include "flow4_cache.h"   // or "flow4_table.h"
 *
 *   // 2. Define hash/cmp functions
 *   static inline union rix_hash_hash_u
 *   fcore_flow4_hash_fn(const struct flow4_key *key, uint32_t mask) { ... }
 *   static inline int
 *   fcore_flow4_cmp(const struct flow4_key *a, const struct flow4_key *b) { ... }
 *
 *   // 3. Override layout hooks (for stride/offset indirection)
 *   #define FCORE_LAYOUT_HASH_BASE(owner) ...
 *   #define FCORE_LAYOUT_ENTRY_PTR(owner, idx) ...
 *   #define FCORE_LAYOUT_ENTRY_INDEX(owner, entry) ...
 *
 *   // 4. Override RIX_HASH_SLOT_DEFINE_INDEXERS
 *   #undef RIX_HASH_SLOT_DEFINE_INDEXERS
 *   #define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type) ...
 *
 *   // 5. Generate rix_hash_slot for flowX_entry
 *   RIX_HASH_GENERATE_STATIC_SLOT_EX(fcore_flow4_ht, flow4_entry,
 *       key, cur_hash, slot, fcore_flow4_cmp, fcore_flow4_hash_fn)
 *
 *   // 6. Optional hit hook (fcache only)
 *   #define FCORE_ON_HIT(owner, entry, idx) ...
 *   // #define FCORE_ON_FINDADD_MISS(owner, entry, idx) ...  // fcache only
 *
 *   // 7. Expand
 *   FCORE_GENERATE(flow4, fc_flow4_cache, fcore_flow4_ht,
 *                  fcore_flow4_hash_fn, fcore_flow4_cmp)
 *
 * Owner struct requirements (field names are part of the contract):
 *   owner->buckets            : struct rix_hash_bucket_s *
 *   owner->ht_head            : struct { unsigned rhh_mask; unsigned rhh_nb; }
 *   owner->max_entries        : unsigned
 *   owner->pool_base          : unsigned char *
 *   owner->pool_stride        : size_t
 *   owner->pool_entry_offset  : size_t
 *   owner->free_head          : uint32_t (RIX_NIL when empty)
 *   FCORE_STATS(owner).lookups      : uint64_t
 *   FCORE_STATS(owner).hits         : uint64_t
 *   FCORE_STATS(owner).misses       : uint64_t
 *   FCORE_STATS(owner).adds         : uint64_t
 *   FCORE_STATS(owner).add_existing : uint64_t
 *   FCORE_STATS(owner).add_failed   : uint64_t
 *   FCORE_STATS(owner).dels         : uint64_t
 *   FCORE_STATS(owner).del_miss     : uint64_t
 *   FCORE_STATUS(owner).entries     : uint32_t
 *   FCORE_STATUS(owner).kickouts    : uint32_t
 *   FCORE_STATUS(owner).add_bk0     : uint32_t
 *   FCORE_STATUS(owner).add_bk1     : uint32_t
 *   owner->ts_shift                 : uint8_t
 */

#ifndef _FLOW_CORE_H_
#define _FLOW_CORE_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rix/rix_hash.h>

/*===========================================================================
 * Constants
 *===========================================================================*/
#ifndef FCORE_CACHE_LINE_SIZE
#define FCORE_CACHE_LINE_SIZE 64u
#endif

#ifndef FCORE_FLOW_FAMILY_IPV4
#define FCORE_FLOW_FAMILY_IPV4  4u
#endif

#ifndef FCORE_FLOW_FAMILY_IPV6
#define FCORE_FLOW_FAMILY_IPV6  6u
#endif

/*===========================================================================
 * Architecture dispatch flags
 *===========================================================================*/
#ifndef FCORE_ARCH_GEN
#define FCORE_ARCH_GEN     0u
#endif
#ifndef FCORE_ARCH_SSE
#define FCORE_ARCH_SSE     (1u << 0)
#endif
#ifndef FCORE_ARCH_AVX2
#define FCORE_ARCH_AVX2    (1u << 1)
#endif
#ifndef FCORE_ARCH_AVX512
#define FCORE_ARCH_AVX512  (1u << 2)
#endif
#ifndef FCORE_ARCH_AUTO
#define FCORE_ARCH_AUTO    (FCORE_ARCH_SSE | FCORE_ARCH_AVX2 | FCORE_ARCH_AVX512)
#endif

/*===========================================================================
 * Pointer arithmetic helpers
 *===========================================================================*/
#define FCORE_BYTE_PTR(ptr)      ((unsigned char *)(void *)(ptr))
#define FCORE_BYTE_CPTR(ptr)     ((const unsigned char *)(const void *)(ptr))
#define FCORE_PTR_ADDR(ptr)      ((uintptr_t)(const void *)(ptr))
#define FCORE_BYTE_PTR_ADD(base, bytes)  (FCORE_BYTE_PTR(base) + (size_t)(bytes))
#define FCORE_BYTE_CPTR_ADD(base, bytes) (FCORE_BYTE_CPTR(base) + (size_t)(bytes))
#define FCORE_MEMBER_PTR(objp, member)   (&((objp)->member))

#define FCORE_PTR_IS_ALIGNED(ptr, align) \
    ((FCORE_PTR_ADDR(ptr) & (uintptr_t)((align) - 1u)) == 0u)

/*===========================================================================
 * Record pool macros (1-origin indexing, stride-based)
 *===========================================================================*/
#define FCORE_RECORD_PTR(base, stride, idx)                                   \
    ((idx) == RIX_NIL ? NULL : (void *)FCORE_BYTE_PTR_ADD(                   \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))

#define FCORE_RECORD_CPTR(base, stride, idx)                                  \
    ((idx) == RIX_NIL ? NULL : (const void *)FCORE_BYTE_CPTR_ADD(            \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))

#define FCORE_RECORD_MEMBER_PTR(base, stride, idx, member_offset, type)       \
    ((type *)__builtin_assume_aligned(                                        \
        FCORE_BYTE_PTR_ADD(FCORE_RECORD_PTR((base), (stride), (idx)),        \
                           (member_offset)),                                  \
        _Alignof(type)))

#define FCORE_RECORD_MEMBER_CPTR(base, stride, idx, member_offset, type)      \
    ((const type *)__builtin_assume_aligned(                                   \
        FCORE_BYTE_CPTR_ADD(FCORE_RECORD_CPTR((base), (stride), (idx)),      \
                            (member_offset)),                                  \
        _Alignof(type)))

/*===========================================================================
 * Record index from member pointer (reverse lookup)
 *===========================================================================*/
static inline unsigned
fcore_record_index_from_member_ptr(const void *base,
                                   size_t stride,
                                   size_t member_offset,
                                   const void *member_ptr)
{
    uintptr_t member_addr, base_addr;
    ptrdiff_t delta;

    if (member_ptr == NULL)
        return RIX_NIL;
    member_addr = (uintptr_t)member_ptr;
    base_addr = (uintptr_t)FCORE_BYTE_CPTR_ADD(base, member_offset);
    delta = (ptrdiff_t)(member_addr - base_addr);
    RIX_ASSERT(delta >= 0);
    RIX_ASSERT(stride != 0u);
    RIX_ASSERT(((size_t)delta % stride) == 0u);
    return (unsigned)((size_t)delta / stride) + 1u;
}

/*===========================================================================
 * Utility functions
 *===========================================================================*/
static inline unsigned
fcore_roundup_pow2_u32(unsigned v)
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

static inline size_t
fcore_align_up(size_t value, size_t align)
{
    return (align <= 1u) ? value : ((value + align - 1u) / align) * align;
}

/*===========================================================================
 * Bucket allocator interface (non-hot-path: init / destroy / grow)
 *===========================================================================*/
typedef void *(*fcore_bucket_alloc_fn)(size_t size, size_t align, void *arg);
typedef void  (*fcore_bucket_free_fn)(void *ptr, size_t size, size_t align,
                                      void *arg);

struct fcore_bucket_allocator {
    fcore_bucket_alloc_fn alloc;
    fcore_bucket_free_fn  free;
    void                 *arg;
};

/*===========================================================================
 * Shared datapath statistics
 *===========================================================================*/
struct fcore_stats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t adds;
    uint64_t add_existing;
    uint64_t add_failed;
    uint64_t dels;
    uint64_t del_miss;
};

struct fcore_status {
    uint32_t entries;
    uint32_t kickouts;
    uint32_t add_bk0;
    uint32_t add_bk1;
};

static inline void
fcore_status_reset(struct fcore_status *status, uint32_t entries)
{
    if (status == NULL)
        return;
    status->entries = entries;
    status->kickouts = 0u;
    status->add_bk0 = 0u;
    status->add_bk1 = 0u;
}

/*===========================================================================
 * Record layout descriptor
 *===========================================================================*/
struct fcore_record_layout {
    size_t record_stride;
    size_t entry_offset;
};

/*===========================================================================
 * Free-list note
 *
 * When an entry is NOT linked in any bucket, htbl_elm.cur_hash is
 * unused.  The free list reuses this field to store the next-free
 * index (1-origin, RIX_NIL = end of list).
 *
 *   #define FCORE_FREE_NEXT(entry) ((entry)->htbl_elm.cur_hash)
 *===========================================================================*/
#define FCORE_FREE_NEXT(entry) ((entry)->htbl_elm.cur_hash)

/*===========================================================================
 * Pipeline tuning defaults
 *===========================================================================*/
#ifndef FCORE_BULK_CTX_COUNT
#define FCORE_BULK_CTX_COUNT  128u
#endif

#ifndef FCORE_BULK_AHEAD_KEYS
#define FCORE_BULK_AHEAD_KEYS 16u
#endif

#ifndef FCORE_BULK_STEP_KEYS
#define FCORE_BULK_STEP_KEYS  4u
#endif

/*===========================================================================
 * Token-paste helpers for FCORE_GENERATE
 *===========================================================================*/
#define _FCORE_CAT2(a, b)  a##b
#define _FCORE_CAT(a, b)   _FCORE_CAT2(a, b)

/* rix_hash_slot function names: ht##_xxx (ht param of FCORE_GENERATE) */
#define _FCORE_HT(ht, name) \
    _FCORE_CAT(ht, _##name)

/* Internal helper names: _fcore_flow4_xxx */
#define _FCORE_INT(p, name) \
    _FCORE_CAT(_fcore_, _FCORE_CAT(p, _##name))

/* Type helpers */
#define _FCORE_KEY_T(p)      struct _FCORE_CAT(p, _key)
#define _FCORE_ENTRY_T(p)    struct _FCORE_CAT(p, _entry)
#define _FCORE_OT(ot)        struct ot

/* ht_head type: struct <ht> (same layout as any RIX_HASH_HEAD) */
#define _FCORE_HT_T(ht)     struct ht

/* Cast owner->ht_head to the ht type (layout-compatible) */
#define _FCORE_HT_HEAD(ht, owner) \
    ((_FCORE_HT_T(ht) *)(void *)&(owner)->ht_head)

/*===========================================================================
 * Layout hook defaults (override before FCORE_GENERATE expansion)
 *===========================================================================*/
#ifndef FCORE_LAYOUT_HASH_BASE
#define FCORE_LAYOUT_HASH_BASE(owner) \
    ((_FCORE_ENTRY_T(_fcore_cur_p) *)(void *)(owner))
#endif

/* These must be overridden per-variant for stride/offset indirection. */

/*===========================================================================
 * Trait hook defaults (override before FCORE_GENERATE expansion)
 *
 * Trait hooks are expanded inside bulk function bodies.
 * Available local variables: owner, now (for find/findadd), entry (hdr ptr),
 * idx (1-origin pool index).
 *===========================================================================*/
#ifndef FCORE_ON_HIT
#define FCORE_ON_HIT(owner, entry, idx)     (void)0
#endif

#ifndef FCORE_TIMESTAMP_SHIFT
#define FCORE_TIMESTAMP_SHIFT(owner) ((owner)->ts_shift)
#endif

#ifndef FCORE_STORE_TIMESTAMP
#define FCORE_STORE_TIMESTAMP(owner, entry, now)                            \
    do {                                                                    \
        if ((now) != 0u)                                                    \
            flow_timestamp_store(&(entry)->htbl_elm, (now),                 \
                                 FCORE_TIMESTAMP_SHIFT(owner));             \
    } while (0)
#endif

#ifndef FCORE_CLEAR_TIMESTAMP
#define FCORE_CLEAR_TIMESTAMP(entry) flow_timestamp_clear(&(entry)->htbl_elm)
#endif

/* FCORE_ON_FINDADD_MISS: if defined, findadd_bulk is generated. */

/*---------------------------------------------------------------------------
 * FCORE_HASH_MASK(owner, ht)
 *
 * Returns the mask to pass to hash_fn() for hash-pair computation.
 *
 * Default: ht_head.rhh_mask (the current bucket mask).
 * ftable overrides this to start_mask so that the hash pair remains
 * stable across grow_2x.
 *---------------------------------------------------------------------------*/
#ifndef FCORE_HASH_MASK
#define FCORE_HASH_MASK(owner, ht) \
    _FCORE_HT_HEAD(ht, owner)->rhh_mask
#endif

#ifndef FCORE_STATS
#define FCORE_STATS(owner) ((owner)->stats)
#endif

#ifndef FCORE_STATUS
#define FCORE_STATUS(owner) ((owner)->status)
#endif

/*===========================================================================
 * FCORE_GENERATE(p, ot, ht, hash_fn, cmp_fn)
 *
 * p       : variant prefix (flow4, flow6, flowu)
 * ot      : owner type name without "struct" (e.g. fc_flow4_cache)
 * ht      : rix_hash_slot name prefix (e.g. ft_flow4_ht)
 * hash_fn : hash function for this variant
 * cmp_fn  : compare function for this variant
 *
 * Prerequisite: RIX_HASH_GENERATE_STATIC_SLOT_EX must be expanded
 *               with name = <ht>, type = <p>_entry.
 *===========================================================================*/

#define FCORE_GENERATE(p, ot, ht, hash_fn, cmp_fn)                           \
                                                                              \
/*=== Free list ============================================================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, free_list_init_)(_FCORE_OT(ot) *owner)                         \
{                                                                             \
    unsigned i;                                                               \
    for (i = 1u; i <= owner->max_entries; i++) {                              \
        _FCORE_ENTRY_T(p) *entry = FCORE_LAYOUT_ENTRY_PTR(owner, i);         \
        RIX_ASSUME_NONNULL(entry);                                            \
        FCORE_FREE_NEXT(entry) = (i < owner->max_entries) ? i + 1u           \
                                                        : (uint32_t)RIX_NIL; \
        entry->htbl_elm.slot = 0u;                                            \
        FCORE_CLEAR_TIMESTAMP(entry);                                         \
    }                                                                         \
    owner->free_head = 1u;                                                    \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE uint32_t                                   \
_FCORE_INT(p, alloc_idx_)(_FCORE_OT(ot) *owner)                              \
{                                                                             \
    uint32_t idx = owner->free_head;                                          \
    if (idx != (uint32_t)RIX_NIL) {                                           \
        _FCORE_ENTRY_T(p) *entry = FCORE_LAYOUT_ENTRY_PTR(owner, idx);       \
        RIX_ASSUME_NONNULL(entry);                                            \
        owner->free_head = FCORE_FREE_NEXT(entry);                            \
        FCORE_FREE_NEXT(entry) = 0u;                                          \
        entry->htbl_elm.slot = 0u;                                            \
        FCORE_CLEAR_TIMESTAMP(entry);                                         \
    }                                                                         \
    return idx;                                                               \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
_FCORE_INT(p, free_idx_)(_FCORE_OT(ot) *owner, uint32_t idx)                 \
{                                                                             \
    _FCORE_ENTRY_T(p) *entry = FCORE_LAYOUT_ENTRY_PTR(owner, idx);           \
    RIX_ASSUME_NONNULL(entry);                                                \
    memset(entry, 0, sizeof(*entry));                                         \
    FCORE_FREE_NEXT(entry) = owner->free_head;                                \
    owner->free_head = idx;                                                   \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
_FCORE_INT(p, prefetch_free_head_)(_FCORE_OT(ot) *owner)                     \
{                                                                             \
    if (owner->free_head != (uint32_t)RIX_NIL) {                              \
        _FCORE_ENTRY_T(p) *entry = FCORE_LAYOUT_ENTRY_PTR(owner,             \
                                                       owner->free_head);     \
        if (entry != NULL)                                                    \
            rix_hash_prefetch_entry_of(entry);                                \
    }                                                                         \
}                                                                             \
                                                                              \
/*=== Counters =============================================================*/\
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
_FCORE_INT(p, nb_entries_)(const _FCORE_OT(ot) *owner)                        \
{                                                                             \
    return owner->ht_head.rhh_nb;                                             \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
_FCORE_INT(p, nb_bk_)(const _FCORE_OT(ot) *owner)                            \
{                                                                             \
    return owner->ht_head.rhh_mask + 1u;                                      \
}                                                                             \
                                                                              \
/*=== find_key_oneshot (single key via bulk path) ==========================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, find_key_bulk_)(_FCORE_OT(ot) *owner,                          \
                              const _FCORE_KEY_T(p) *keys,                    \
                              unsigned nb_keys,                               \
                              uint64_t now,                                   \
                              uint32_t *results);                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE uint32_t                                   \
_FCORE_INT(p, find_key_oneshot_)(_FCORE_OT(ot) *owner,                       \
                                 const _FCORE_KEY_T(p) *key,                  \
                                 uint64_t now)                                \
{                                                                             \
    uint32_t result = 0u;                                                     \
    _FCORE_INT(p, find_key_bulk_)(owner, key, 1u, now, &result);             \
    return result;                                                            \
}                                                                             \
                                                                              \
/*=== find_key_bulk_org (4-stage ctx pipeline) =============================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, find_key_bulk_org_)(_FCORE_OT(ot) *owner,                      \
                                  const _FCORE_KEY_T(p) *keys,               \
                                  unsigned nb_keys,                          \
                                  uint64_t now,                              \
                                  uint32_t *results)                         \
{                                                                             \
    enum { _FCORE_FIND_CTX_COUNT = 64u };                                     \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    const uint32_t hash_mask = FCORE_HASH_MASK(owner, ht);                    \
    struct rix_hash_find_ctx_s ctx[_FCORE_FIND_CTX_COUNT];                    \
    uint64_t hit_count = 0u;                                                  \
    uint64_t miss_count = 0u;                                                 \
    const unsigned ctx_mask = _FCORE_FIND_CTX_COUNT - 1u;                     \
    const unsigned ahead = 12u;                                               \
    const unsigned step  = 4u;                                                \
    const unsigned total = nb_keys + 3u * ahead;                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        if (_i < nb_keys) {                                                   \
            unsigned _n = (_i + step <= nb_keys) ? step : (nb_keys - _i);     \
            for (unsigned _j = 0; _j < _n; _j++)                              \
                _FCORE_HT(ht, hash_key_2bk_masked)(                           \
                    &ctx[(_i + _j) & ctx_mask], buckets,                     \
                    &keys[_i + _j], hash_mask, head->rhh_mask);              \
        }                                                                     \
        if (_i >= ahead && _i - ahead < nb_keys) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++)                              \
                _FCORE_HT(ht, scan_bk)(                                       \
                    &ctx[(_base + _j) & ctx_mask], head, buckets);           \
        }                                                                     \
        if (_i >= 2u * ahead && _i - 2u * ahead < nb_keys) {                  \
            unsigned _base = _i - 2u * ahead;                                 \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++)                              \
                _FCORE_HT(ht, prefetch_node)(                                 \
                    &ctx[(_base + _j) & ctx_mask],                            \
                    hash_base);                                               \
        }                                                                     \
        if (_i >= 3u * ahead && _i - 3u * ahead < nb_keys) {                  \
            unsigned _base = _i - 3u * ahead;                                 \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _base + _j;                                   \
                _FCORE_ENTRY_T(p) *entry = _FCORE_HT(ht, cmp_key)(            \
                    &ctx[_idx & ctx_mask], hash_base);                        \
                if (RIX_LIKELY(entry != NULL)) {                              \
                    uint32_t _eidx = FCORE_LAYOUT_ENTRY_INDEX(owner, entry);  \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    FCORE_ON_HIT(owner, entry, _eidx);                        \
                    results[_idx] = _eidx;                                    \
                    hit_count++;                                              \
                } else {                                                      \
                    results[_idx] = 0u;                                       \
                    miss_count++;                                             \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    FCORE_STATS(owner).lookups += nb_keys;                                          \
    FCORE_STATS(owner).hits += hit_count;                                           \
    FCORE_STATS(owner).misses += miss_count;                                        \
}                                                                             \
                                                                              \
/*=== find_key_bulk (scan -> immediate prefetch) ===========================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, find_key_bulk_)(_FCORE_OT(ot) *owner,                          \
                              const _FCORE_KEY_T(p) *keys,                    \
                              unsigned nb_keys,                               \
                              uint64_t now,                                   \
                              uint32_t *results)                              \
{                                                                             \
    enum { _FCORE_FIND_CTX_COUNT = 32u };                                     \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    const uint32_t hash_mask = FCORE_HASH_MASK(owner, ht);                    \
    struct rix_hash_find_ctx_s ctx[_FCORE_FIND_CTX_COUNT];                    \
    uint64_t hit_count = 0u;                                                  \
    uint64_t miss_count = 0u;                                                 \
    const unsigned ctx_mask = _FCORE_FIND_CTX_COUNT - 1u;                     \
    const unsigned ahead = 8u;                                                \
    const unsigned step  = 8u;                                                \
    const unsigned total = nb_keys + 2u * ahead;                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        if (_i < nb_keys) {                                                   \
            unsigned _n = (_i + step <= nb_keys) ? step : (nb_keys - _i);     \
            for (unsigned _j = 0; _j < _n; _j++)                              \
                _FCORE_HT(ht, hash_key_2bk_masked)(                           \
                    &ctx[(_i + _j) & ctx_mask], buckets,                     \
                    &keys[_i + _j], hash_mask, head->rhh_mask);              \
        }                                                                     \
        if (_i >= ahead && _i - ahead < nb_keys) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                struct rix_hash_find_ctx_s *_ctxp =                           \
                    &ctx[(_base + _j) & ctx_mask];                            \
                _FCORE_HT(ht, scan_bk)(_ctxp, head, buckets);                \
                _FCORE_HT(ht, prefetch_node)(_ctxp, hash_base);              \
            }                                                                 \
        }                                                                     \
        if (_i >= 2u * ahead && _i - 2u * ahead < nb_keys) {                  \
            unsigned _base = _i - 2u * ahead;                                 \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _base + _j;                                   \
                _FCORE_ENTRY_T(p) *entry = _FCORE_HT(ht, cmp_key)(            \
                    &ctx[_idx & ctx_mask], hash_base);                        \
                if (RIX_LIKELY(entry != NULL)) {                              \
                    uint32_t _eidx = FCORE_LAYOUT_ENTRY_INDEX(owner, entry);  \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    FCORE_ON_HIT(owner, entry, _eidx);                        \
                    results[_idx] = _eidx;                                    \
                    hit_count++;                                              \
                } else {                                                      \
                    results[_idx] = 0u;                                       \
                    miss_count++;                                             \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    FCORE_STATS(owner).lookups += nb_keys;                                    \
    FCORE_STATS(owner).hits += hit_count;                                     \
    FCORE_STATS(owner).misses += miss_count;                                  \
}                                                                             \
                                                                              \
/*=== add_idx_bulk (2-stage pipeline) ======================================*/\
/*   Caller provides entry indices; keys must already be written in entry.   */\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, add_idx_bulk_legacy_)(_FCORE_OT(ot) *owner,                    \
                             const uint32_t *entry_idxv,                      \
                             unsigned nb_keys,                                 \
                             uint64_t now,                                     \
                             uint32_t *results)                                \
{                                                                             \
    enum { _FCORE_ADD_CTX_COUNT = 8u };                                       \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    const uint32_t hash_mask = FCORE_HASH_MASK(owner, ht);                    \
    const unsigned ctx_mask = _FCORE_ADD_CTX_COUNT - 1u;                      \
    const unsigned step  = 4u;                                                \
    const unsigned ahead = 8u;                                                \
    const unsigned total = nb_keys + ahead;                                   \
    struct rix_hash_find_ctx_s ctx[_FCORE_ADD_CTX_COUNT];                     \
    RIX_ASSERT(results != NULL);                                              \
                                                                              \
    /* Pre-prefetch entry keys */                                             \
    {                                                                         \
        unsigned _prefetch_n = (nb_keys < ahead) ? nb_keys : ahead;           \
        for (unsigned _i = 0; _i < _prefetch_n; _i++) {                       \
            uint32_t _eidx = entry_idxv[_i];                                  \
            _FCORE_ENTRY_T(p) *entry =                                        \
                FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                         \
            RIX_ASSERT(_eidx != 0u);                                          \
            RIX_ASSERT(_eidx <= owner->max_entries);                          \
            RIX_ASSUME_NONNULL(entry);                                        \
            rix_hash_prefetch_key(&entry->key);                               \
        }                                                                     \
    }                                                                         \
                                                                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        /* Stage 1: hash + prefetch buckets */                                \
        if (_i < nb_keys) {                                                   \
            unsigned _n = (_i + step <= nb_keys) ? step : (nb_keys - _i);     \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                unsigned _idx = _i + _j;                                      \
                uint32_t _eidx = entry_idxv[_idx];                            \
                _FCORE_ENTRY_T(p) *entry;                                     \
                struct rix_hash_find_ctx_s *_ctxp =                           \
                    &ctx[_idx & ctx_mask];                                    \
                unsigned _bk0, _bk1;                                          \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                entry = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                 \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (_idx + ahead < nb_keys) {                                 \
                    uint32_t _peidx = entry_idxv[_idx + ahead];               \
                    _FCORE_ENTRY_T(p) *_pentry =                              \
                        FCORE_LAYOUT_ENTRY_PTR(owner, _peidx);                \
                    RIX_ASSERT(_peidx != 0u);                                 \
                    RIX_ASSERT(_peidx <= owner->max_entries);                 \
                    RIX_ASSUME_NONNULL(_pentry);                              \
                    rix_hash_prefetch_key(&_pentry->key);                     \
                }                                                             \
                _ctxp->hash = hash_fn(&entry->key, hash_mask);                \
                rix_hash_buckets(_ctxp->hash, head->rhh_mask,                 \
                                 &_bk0, &_bk1, &_ctxp->fp);                   \
                _ctxp->key = (const void *)entry;                             \
                _ctxp->bk[0] = buckets + _bk0;                                \
                _ctxp->bk[1] = buckets + _bk1;                                \
                rix_hash_prefetch_bucket_of(_ctxp->bk[0]);                    \
                rix_hash_prefetch_bucket_of(_ctxp->bk[1]);                    \
            }                                                                 \
        }                                                                     \
        /* Stage 2: duplicate check + inline insert */                        \
        if (_i >= ahead && _i - ahead < nb_keys) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                unsigned _idx = _base + _j;                                   \
                uint32_t _eidx = entry_idxv[_idx];                            \
                struct rix_hash_find_ctx_s *_ctxp;                            \
                _FCORE_ENTRY_T(p) *entry;                                     \
                uint32_t _ret_idx = (uint32_t)RIX_NIL;                        \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                _ctxp = &ctx[_idx & ctx_mask];                                \
                entry = (_FCORE_ENTRY_T(p) *)(uintptr_t)_ctxp->key;           \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (RIX_UNLIKELY(entry->htbl_elm.cur_hash != 0u)) {           \
                    /* entry already in table (self-duplicate) */              \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    FCORE_STATS(owner).add_existing++;                               \
                    results[_idx] = _eidx;                                    \
                    continue;                                                  \
                }                                                             \
                _FCORE_HT(ht, scan_bk_empties)(_ctxp, 0u);                    \
                if (RIX_UNLIKELY(_ctxp->fp_hits[0] != 0u)) {                  \
                    u32 _hits = _ctxp->fp_hits[0];                            \
                    while (RIX_UNLIKELY(_hits != 0u)) {                       \
                        unsigned _bit = (unsigned)__builtin_ctz(_hits);       \
                        uint32_t _nidx = _ctxp->bk[0]->idx[_bit];             \
                        _FCORE_ENTRY_T(p) *_node;                             \
                        _hits &= _hits - 1u;                                  \
                        if (RIX_UNLIKELY(_nidx == (uint32_t)RIX_NIL))         \
                            continue;                                         \
                        _node = FCORE_LAYOUT_ENTRY_PTR(owner, _nidx);         \
                        RIX_ASSUME_NONNULL(_node);                            \
                        if (RIX_UNLIKELY(                                     \
                                cmp_fn(&entry->key, &_node->key) == 0)) {     \
                            FCORE_STORE_TIMESTAMP(owner, _node, now);         \
                            FCORE_STATS(owner).add_existing++;                      \
                            results[_idx] = _nidx;                            \
                            goto _fcore_add_idx_next_;                        \
                        }                                                     \
                    }                                                         \
                }                                                             \
                _FCORE_HT(ht, scan_bk_empties)(_ctxp, 1u);                    \
                if (RIX_UNLIKELY(_ctxp->fp_hits[1] != 0u)) {                  \
                    u32 _hits = _ctxp->fp_hits[1];                            \
                    while (RIX_UNLIKELY(_hits != 0u)) {                       \
                        unsigned _bit = (unsigned)__builtin_ctz(_hits);       \
                        uint32_t _nidx = _ctxp->bk[1]->idx[_bit];             \
                        _FCORE_ENTRY_T(p) *_node;                             \
                        _hits &= _hits - 1u;                                  \
                        if (RIX_UNLIKELY(_nidx == (uint32_t)RIX_NIL))         \
                            continue;                                         \
                        _node = FCORE_LAYOUT_ENTRY_PTR(owner, _nidx);         \
                        RIX_ASSUME_NONNULL(_node);                            \
                        if (RIX_UNLIKELY(                                     \
                                cmp_fn(&entry->key, &_node->key) == 0)) {     \
                            FCORE_STORE_TIMESTAMP(owner, _node, now);         \
                            FCORE_STATS(owner).add_existing++;                      \
                            results[_idx] = _nidx;                            \
                            goto _fcore_add_idx_next_;                        \
                        }                                                     \
                    }                                                         \
                }                                                             \
                if (RIX_LIKELY(_ctxp->empties[0] != 0u)) {                    \
                    unsigned _slot =                                           \
                        (unsigned)__builtin_ctz(_ctxp->empties[0]);           \
                    _ctxp->bk[0]->hash[_slot] = _ctxp->fp;                    \
                    _ctxp->bk[0]->idx[_slot] = _eidx;                         \
                    entry->htbl_elm.cur_hash = _ctxp->hash.val32[0];          \
                    entry->htbl_elm.slot = (__typeof__(entry->htbl_elm.slot)) \
                        _slot;                                                \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    head->rhh_nb++;                                           \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).add_bk0++;                            \
                    results[_idx] = _eidx;                                    \
                    continue;                                                  \
                }                                                             \
                if (RIX_LIKELY(_ctxp->empties[1] != 0u)) {                    \
                    unsigned _slot =                                           \
                        (unsigned)__builtin_ctz(_ctxp->empties[1]);           \
                    _ctxp->bk[1]->hash[_slot] = _ctxp->fp;                    \
                    _ctxp->bk[1]->idx[_slot] = _eidx;                         \
                    entry->htbl_elm.cur_hash = _ctxp->hash.val32[1];          \
                    entry->htbl_elm.slot = (__typeof__(entry->htbl_elm.slot)) \
                        _slot;                                                \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    head->rhh_nb++;                                           \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).add_bk1++;                            \
                    results[_idx] = _eidx;                                    \
                    continue;                                                  \
                }                                                             \
                _ret_idx = _FCORE_HT(ht, insert_hashed_idx)(                  \
                    head, buckets, hash_base, entry, _ctxp->hash);            \
                if (RIX_LIKELY(_ret_idx == 0u)) {                             \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).kickouts++;                           \
                    if (entry->htbl_elm.cur_hash == _ctxp->hash.val32[1])     \
                        FCORE_STATUS(owner).add_bk1++;                        \
                    else                                                      \
                        FCORE_STATUS(owner).add_bk0++;                        \
                    results[_idx] = _eidx;                                    \
                } else if (RIX_UNLIKELY(_ret_idx != _eidx)) {                 \
                    _FCORE_ENTRY_T(p) *_node =                                \
                        FCORE_LAYOUT_ENTRY_PTR(owner, _ret_idx);              \
                    RIX_ASSUME_NONNULL(_node);                                \
                    FCORE_STORE_TIMESTAMP(owner, _node, now);                 \
                    FCORE_STATS(owner).add_existing++;                              \
                    results[_idx] = _ret_idx;                                 \
                } else {                                                      \
                    FCORE_CLEAR_TIMESTAMP(entry);                             \
                    FCORE_STATS(owner).add_failed++;                                \
                    results[_idx] = 0u;                                       \
                }                                                             \
_fcore_add_idx_next_: ;                                                      \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/*=== add_idx_bulk (2-stage pipeline with duplicate policy) ===============*/\
/* idxv[] is input/output and returns the actually registered idx.           */\
/* unused_idxv[] packs free-list candidates in order and return value is     */\
/* the number of packed entries.                                             */\
/*   inserted           -> idxv[i] = request idx, unused none                */\
/*   duplicate+ignore   -> idxv[i] = existing idx, unused += request idx     */\
/*   duplicate+update   -> idxv[i] = request idx, unused += old idx          */\
/*   self-duplicate     -> idxv[i] = request idx, unused none                */\
/*   add failed         -> idxv[i] = 0, unused += request idx                */\
/* UPDATE requires that a batch does not contain duplicate keys.             */\
                                                                              \
static RIX_UNUSED unsigned                                                    \
_FCORE_INT(p, add_idx_bulk_)(_FCORE_OT(ot) *owner,                           \
                              uint32_t *entry_idxv,                           \
                              unsigned nb_keys,                               \
                              enum ft_add_policy policy,                      \
                              uint64_t now,                                   \
                              uint32_t *unused_idxv)                          \
{                                                                             \
    enum { _FCORE_ADD_CTX_COUNT = 8u };                                       \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    const uint32_t hash_mask = FCORE_HASH_MASK(owner, ht);                    \
    const unsigned ctx_mask = _FCORE_ADD_CTX_COUNT - 1u;                      \
    const unsigned step  = 4u;                                                \
    const unsigned ahead = 8u;                                                \
    const unsigned total = nb_keys + ahead;                                   \
    struct rix_hash_find_ctx_s ctx[_FCORE_ADD_CTX_COUNT];                     \
    unsigned free_count = 0u;                                                 \
    RIX_ASSERT(unused_idxv != NULL);                                          \
                                                                              \
    {                                                                         \
        unsigned _prefetch_n = (nb_keys < ahead) ? nb_keys : ahead;           \
        for (unsigned _i = 0; _i < _prefetch_n; _i++) {                       \
            uint32_t _eidx = entry_idxv[_i];                                  \
            _FCORE_ENTRY_T(p) *entry =                                        \
                FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                         \
            RIX_ASSERT(_eidx != 0u);                                          \
            RIX_ASSERT(_eidx <= owner->max_entries);                          \
            RIX_ASSUME_NONNULL(entry);                                        \
            rix_hash_prefetch_key(&entry->key);                               \
        }                                                                     \
    }                                                                         \
                                                                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        if (_i < nb_keys) {                                                   \
            unsigned _n = (_i + step <= nb_keys) ? step : (nb_keys - _i);     \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _i + _j;                                      \
                uint32_t _eidx = entry_idxv[_idx];                            \
                _FCORE_ENTRY_T(p) *entry;                                     \
                struct rix_hash_find_ctx_s *_ctxp =                           \
                    &ctx[_idx & ctx_mask];                                    \
                unsigned _bk0, _bk1;                                          \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                entry = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                 \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (_idx + ahead < nb_keys) {                                 \
                    uint32_t _peidx = entry_idxv[_idx + ahead];               \
                    _FCORE_ENTRY_T(p) *_pentry =                              \
                        FCORE_LAYOUT_ENTRY_PTR(owner, _peidx);                \
                    RIX_ASSERT(_peidx != 0u);                                 \
                    RIX_ASSERT(_peidx <= owner->max_entries);                 \
                    RIX_ASSUME_NONNULL(_pentry);                              \
                    rix_hash_prefetch_key(&_pentry->key);                     \
                }                                                             \
                _ctxp->hash = hash_fn(&entry->key, hash_mask);                \
                rix_hash_buckets(_ctxp->hash, head->rhh_mask,                 \
                                 &_bk0, &_bk1, &_ctxp->fp);                   \
                _ctxp->key = (const void *)entry;                             \
                _ctxp->bk[0] = buckets + _bk0;                                \
                _ctxp->bk[1] = buckets + _bk1;                                \
                rix_hash_prefetch_bucket_of(_ctxp->bk[0]);                    \
                rix_hash_prefetch_bucket_of(_ctxp->bk[1]);                    \
            }                                                                 \
        }                                                                     \
        if (_i >= ahead && _i - ahead < nb_keys) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _base + _j;                                   \
                uint32_t _eidx = entry_idxv[_idx];                            \
                struct rix_hash_find_ctx_s *_ctxp = &ctx[_idx & ctx_mask];    \
                _FCORE_ENTRY_T(p) *entry =                                    \
                    (_FCORE_ENTRY_T(p) *)(uintptr_t)_ctxp->key;               \
                uint32_t _ret_idx = (uint32_t)RIX_NIL;                        \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (RIX_UNLIKELY(entry->htbl_elm.cur_hash != 0u)) {           \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    FCORE_STATS(owner).add_existing++;                        \
                    continue;                                                  \
                }                                                             \
                _FCORE_HT(ht, scan_bk_empties)(_ctxp, 0u);                    \
                if (RIX_UNLIKELY(_ctxp->fp_hits[0] != 0u)) {                  \
                    u32 _hits = _ctxp->fp_hits[0];                            \
                    while (RIX_UNLIKELY(_hits != 0u)) {                       \
                        unsigned _bit = (unsigned)__builtin_ctz(_hits);       \
                        uint32_t _nidx = _ctxp->bk[0]->idx[_bit];             \
                        _FCORE_ENTRY_T(p) *_node;                             \
                        _hits &= _hits - 1u;                                  \
                        if (RIX_UNLIKELY(_nidx == (uint32_t)RIX_NIL))         \
                            continue;                                         \
                        _node = FCORE_LAYOUT_ENTRY_PTR(owner, _nidx);         \
                        RIX_ASSUME_NONNULL(_node);                            \
                        if (RIX_UNLIKELY(cmp_fn(&entry->key, &_node->key)     \
                                         == 0)) {                             \
                            FCORE_STATS(owner).add_existing++;                      \
                            if (policy == FT_ADD_UPDATE) {                    \
                                _ctxp->bk[0]->idx[_bit] = _eidx;              \
                                entry->htbl_elm.cur_hash =                    \
                                    _ctxp->hash.val32[0];                     \
                                entry->htbl_elm.slot =                        \
                                    (__typeof__(entry->htbl_elm.slot))_bit;   \
                                FCORE_STORE_TIMESTAMP(owner, entry, now);     \
                                _node->htbl_elm.cur_hash = 0u;                \
                                FCORE_CLEAR_TIMESTAMP(_node);                 \
                                unused_idxv[free_count++] = _nidx;            \
                            } else {                                          \
                                FCORE_STORE_TIMESTAMP(owner, _node, now);     \
                                FCORE_CLEAR_TIMESTAMP(entry);                 \
                                entry_idxv[_idx] = _nidx;                     \
                                unused_idxv[free_count++] = _eidx;            \
                            }                                                 \
                            goto _fcore_add_idx_next_;                        \
                        }                                                     \
                    }                                                         \
                }                                                             \
                _FCORE_HT(ht, scan_bk_empties)(_ctxp, 1u);                    \
                if (RIX_UNLIKELY(_ctxp->fp_hits[1] != 0u)) {                  \
                    u32 _hits = _ctxp->fp_hits[1];                            \
                    while (RIX_UNLIKELY(_hits != 0u)) {                       \
                        unsigned _bit = (unsigned)__builtin_ctz(_hits);       \
                        uint32_t _nidx = _ctxp->bk[1]->idx[_bit];             \
                        _FCORE_ENTRY_T(p) *_node;                             \
                        _hits &= _hits - 1u;                                  \
                        if (RIX_UNLIKELY(_nidx == (uint32_t)RIX_NIL))         \
                            continue;                                         \
                        _node = FCORE_LAYOUT_ENTRY_PTR(owner, _nidx);         \
                        RIX_ASSUME_NONNULL(_node);                            \
                        if (RIX_UNLIKELY(cmp_fn(&entry->key, &_node->key)     \
                                         == 0)) {                             \
                            FCORE_STATS(owner).add_existing++;                      \
                            if (policy == FT_ADD_UPDATE) {                    \
                                _ctxp->bk[1]->idx[_bit] = _eidx;              \
                                entry->htbl_elm.cur_hash =                    \
                                    _ctxp->hash.val32[1];                     \
                                entry->htbl_elm.slot =                        \
                                    (__typeof__(entry->htbl_elm.slot))_bit;   \
                                FCORE_STORE_TIMESTAMP(owner, entry, now);     \
                                _node->htbl_elm.cur_hash = 0u;                \
                                FCORE_CLEAR_TIMESTAMP(_node);                 \
                                unused_idxv[free_count++] = _nidx;            \
                            } else {                                          \
                                FCORE_STORE_TIMESTAMP(owner, _node, now);     \
                                FCORE_CLEAR_TIMESTAMP(entry);                 \
                                entry_idxv[_idx] = _nidx;                     \
                                unused_idxv[free_count++] = _eidx;            \
                            }                                                 \
                            goto _fcore_add_idx_next_;                        \
                        }                                                     \
                    }                                                         \
                }                                                             \
                if (RIX_LIKELY(_ctxp->empties[0] != 0u)) {                    \
                    unsigned _slot =                                           \
                        (unsigned)__builtin_ctz(_ctxp->empties[0]);           \
                    _ctxp->bk[0]->hash[_slot] = _ctxp->fp;                    \
                    _ctxp->bk[0]->idx[_slot] = _eidx;                         \
                    entry->htbl_elm.cur_hash = _ctxp->hash.val32[0];          \
                    entry->htbl_elm.slot = (__typeof__(entry->htbl_elm.slot)) \
                        _slot;                                                \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    head->rhh_nb++;                                           \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).add_bk0++;                            \
                    continue;                                                  \
                }                                                             \
                if (RIX_LIKELY(_ctxp->empties[1] != 0u)) {                    \
                    unsigned _slot =                                           \
                        (unsigned)__builtin_ctz(_ctxp->empties[1]);           \
                    _ctxp->bk[1]->hash[_slot] = _ctxp->fp;                    \
                    _ctxp->bk[1]->idx[_slot] = _eidx;                         \
                    entry->htbl_elm.cur_hash = _ctxp->hash.val32[1];          \
                    entry->htbl_elm.slot = (__typeof__(entry->htbl_elm.slot)) \
                        _slot;                                                \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    head->rhh_nb++;                                           \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).add_bk1++;                            \
                    continue;                                                  \
                }                                                             \
                _ret_idx = _FCORE_HT(ht, insert_hashed_idx)(                  \
                    head, buckets, hash_base, entry, _ctxp->hash);            \
                if (RIX_LIKELY(_ret_idx == 0u)) {                             \
                    FCORE_STORE_TIMESTAMP(owner, entry, now);                 \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).kickouts++;                           \
                    if (entry->htbl_elm.cur_hash == _ctxp->hash.val32[1])     \
                        FCORE_STATUS(owner).add_bk1++;                        \
                    else                                                      \
                        FCORE_STATUS(owner).add_bk0++;                        \
                } else if (RIX_UNLIKELY(_ret_idx != _eidx)) {                 \
                    FCORE_STATS(owner).add_existing++;                        \
                    if (policy == FT_ADD_UPDATE) {                            \
                        _FCORE_ENTRY_T(p) *_node =                            \
                            FCORE_LAYOUT_ENTRY_PTR(owner, _ret_idx);          \
                        uint32_t _ins_idx;                                    \
                        RIX_ASSUME_NONNULL(_node);                            \
                        _ins_idx = _ret_idx;                                  \
                        RIX_ASSERT(_FCORE_HT(ht, remove)(                      \
                                       head, buckets, hash_base, _node)       \
                                   != NULL);                                  \
                        _node->htbl_elm.cur_hash = 0u;                        \
                        FCORE_CLEAR_TIMESTAMP(_node);                         \
                        _ins_idx = _FCORE_HT(ht, insert_hashed_idx)(          \
                            head, buckets, hash_base, entry, _ctxp->hash);    \
                        RIX_ASSERT(_ins_idx == 0u);                           \
                        FCORE_STORE_TIMESTAMP(owner, entry, now);             \
                        unused_idxv[free_count++] = _ret_idx;                 \
                    } else {                                                  \
                        _FCORE_ENTRY_T(p) *_node =                            \
                            FCORE_LAYOUT_ENTRY_PTR(owner, _ret_idx);          \
                        RIX_ASSUME_NONNULL(_node);                            \
                        FCORE_STORE_TIMESTAMP(owner, _node, now);             \
                        FCORE_CLEAR_TIMESTAMP(entry);                         \
                        entry_idxv[_idx] = _ret_idx;                          \
                        unused_idxv[free_count++] = _eidx;                    \
                    }                                                         \
                } else {                                                      \
                    FCORE_CLEAR_TIMESTAMP(entry);                             \
                    FCORE_STATS(owner).add_failed++;                          \
                    entry_idxv[_idx] = 0u;                                    \
                    unused_idxv[free_count++] = _eidx;                        \
                }                                                             \
_fcore_add_idx_next_: ;                                                      \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    return free_count;                                                        \
}                                                                             \
                                                                              \
/*=== add_idx_oneshot (single, by index via bulk path) =====================*/\
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE uint32_t                                   \
_FCORE_INT(p, add_idx_oneshot_)(_FCORE_OT(ot) *owner,                        \
                                uint32_t entry_idx,                           \
                                uint64_t now)                                 \
{                                                                             \
    uint32_t result = 0u;                                                     \
    _FCORE_INT(p, add_idx_bulk_legacy_)(owner, &entry_idx, 1u, now, &result);\
    return result;                                                            \
}                                                                             \
                                                                              \
/*=== del_idx_bulk (2-stage pipeline) ======================================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, del_idx_bulk_)(_FCORE_OT(ot) *owner,                           \
                             const uint32_t *idxs,                            \
                             unsigned nb_idxs)                                 \
{                                                                             \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    const unsigned ahead = FCORE_BULK_AHEAD_KEYS;                             \
    const unsigned step  = FCORE_BULK_STEP_KEYS;                              \
    const unsigned total = nb_idxs + ahead;                                   \
                                                                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        if (_i < nb_idxs) {                                                   \
            unsigned _n = (_i + step <= nb_idxs) ? step : (nb_idxs - _i);    \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                uint32_t _eidx = idxs[_i + _j];                              \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                rix_hash_prefetch_entry_of(                                   \
                    FCORE_LAYOUT_ENTRY_PTR(owner, _eidx));                    \
            }                                                                 \
        }                                                                     \
        if (_i >= ahead && _i - ahead < nb_idxs) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_idxs) ? step                    \
                                                    : (nb_idxs - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                uint32_t _eidx = idxs[_base + _j];                           \
                _FCORE_ENTRY_T(p) *entry;                                     \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                entry = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (entry->htbl_elm.cur_hash == 0u)                           \
                    continue;                                                 \
                if (_FCORE_HT(ht, remove)(                                     \
                        head, buckets, hash_base, entry) != NULL) {           \
                    entry->htbl_elm.cur_hash = 0u;                            \
                    FCORE_CLEAR_TIMESTAMP(entry);                             \
                    FCORE_STATS(owner).dels++;                                \
                    RIX_ASSERT(FCORE_STATUS(owner).entries != 0u);            \
                    FCORE_STATUS(owner).entries--;                            \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/*=== del_idx_oneshot (single, by index via bulk path) =====================*/\
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE uint32_t                                   \
_FCORE_INT(p, del_idx_oneshot_)(_FCORE_OT(ot) *owner,                        \
                                uint32_t entry_idx)                           \
{                                                                             \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    _FCORE_ENTRY_T(p) *entry;                                                 \
    if (entry_idx == 0u || entry_idx > owner->max_entries)                    \
        return 0u;                                                            \
    entry = FCORE_LAYOUT_ENTRY_PTR(owner, entry_idx);                         \
    RIX_ASSUME_NONNULL(entry);                                                \
    if (entry->htbl_elm.cur_hash == 0u)                                       \
        return 0u;                                                            \
    if (_FCORE_HT(ht, remove)(head, owner->buckets,                           \
                              FCORE_LAYOUT_HASH_BASE(owner), entry) == NULL)   \
        return 0u;                                                            \
    entry->htbl_elm.cur_hash = 0u;                                            \
    FCORE_CLEAR_TIMESTAMP(entry);                                             \
    FCORE_STATS(owner).dels++;                                                \
    RIX_ASSERT(FCORE_STATUS(owner).entries != 0u);                            \
    FCORE_STATUS(owner).entries--;                                            \
    return entry_idx;                                                         \
}                                                                             \
                                                                              \
/*=== walk =================================================================*/\
                                                                              \
static RIX_UNUSED int                                                         \
_FCORE_INT(p, walk_)(_FCORE_OT(ot) *owner,                                   \
                     int (*cb)(uint32_t entry_idx, void *arg),                \
                     void *arg)                                               \
{                                                                             \
    unsigned _nb_bk = owner->ht_head.rhh_mask + 1u;                           \
    for (unsigned _b = 0; _b < _nb_bk; _b++) {                                \
        struct rix_hash_bucket_s *_bk = &owner->buckets[_b];                  \
        for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {          \
            unsigned _nidx = _bk->idx[_s];                                    \
            if (_nidx == (unsigned)RIX_NIL)                                   \
                continue;                                                     \
            int _rc = cb((uint32_t)_nidx, arg);                               \
            if (_rc != 0)                                                     \
                return _rc;                                                   \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
                                                                              \
/*=== flush (remove all entries, rebuild free list) ========================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, flush_)(_FCORE_OT(ot) *owner)                                  \
{                                                                             \
    unsigned _nb_bk = owner->ht_head.rhh_mask + 1u;                           \
    /* Clear all buckets */                                                   \
    memset(owner->buckets, 0,                                                 \
           (size_t)_nb_bk * sizeof(struct rix_hash_bucket_s));                \
    /* Fill idx with RIX_NIL */                                               \
    for (unsigned _b = 0; _b < _nb_bk; _b++)                                  \
        for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++)            \
            owner->buckets[_b].idx[_s] = (uint32_t)RIX_NIL;                  \
    owner->ht_head.rhh_nb = 0u;                                               \
    /* Rebuild free list */                                                   \
    _FCORE_INT(p, free_list_init_)(owner);                                    \
    fcore_status_reset(&FCORE_STATUS(owner), 0u);                             \
}                                                                             \
                                                                              \
/* end FCORE_GENERATE */

#endif /* _FLOW_CORE_H_ */
