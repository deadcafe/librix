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
 *   3. FCORE_GENERATE macro: find_bulk, add_idx_bulk, del_bulk,
 *      del_idx_bulk, free list, counters, and optionally findadd_bulk
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
 *   // 5. Generate rix_hash_slot for entry_hdr
 *   RIX_HASH_GENERATE_STATIC_SLOT_EX(fcore_flow4_ht, flow4_entry_hdr,
 *       key, cur_hash, slot, fcore_flow4_cmp, fcore_flow4_hash_fn)
 *
 *   // 6. Define trait hooks
 *   #define FCORE_ON_HIT(owner, entry_hdr, idx) ...
 *   #define FCORE_ON_INSERT(owner, entry_hdr, idx) ...
 *   #define FCORE_ON_REMOVE(owner, entry_hdr, idx) ...
 *   // #define FCORE_ON_FINDADD_MISS(owner, entry_hdr, idx) ...  // fcache only
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
 *   owner->stats.lookups      : uint64_t
 *   owner->stats.hits         : uint64_t
 *   owner->stats.misses       : uint64_t
 *   owner->stats.adds         : uint64_t
 *   owner->stats.add_existing : uint64_t
 *   owner->stats.add_failed   : uint64_t
 *   owner->stats.dels         : uint64_t
 *   owner->stats.del_miss     : uint64_t
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
 *   #define FCORE_FREE_NEXT(hdr) ((hdr)->htbl_elm.cur_hash)
 *===========================================================================*/
#define FCORE_FREE_NEXT(hdr) ((hdr)->htbl_elm.cur_hash)

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
#define _FCORE_KEY_T(p)     struct _FCORE_CAT(p, _key)
#define _FCORE_HDR_T(p)     struct _FCORE_CAT(p, _entry_hdr)
#define _FCORE_OT(ot)       struct ot

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
    ((_FCORE_HDR_T(_fcore_cur_p) *)(void *)(owner))
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

#ifndef FCORE_ON_INSERT
#define FCORE_ON_INSERT(owner, entry, idx)   (void)0
#endif

#ifndef FCORE_ON_REMOVE
#define FCORE_ON_REMOVE(owner, entry, idx)   (void)0
#endif

/* FCORE_ON_FINDADD_MISS: if defined, findadd_bulk is generated. */

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
 *               with name = <ht>, type = <p>_entry_hdr.
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
        _FCORE_HDR_T(p) *hdr = FCORE_LAYOUT_ENTRY_PTR(owner, i);             \
        RIX_ASSUME_NONNULL(hdr);                                              \
        FCORE_FREE_NEXT(hdr) = (i < owner->max_entries) ? i + 1u             \
                                                        : (uint32_t)RIX_NIL; \
        hdr->htbl_elm.slot = 0u;                                              \
    }                                                                         \
    owner->free_head = 1u;                                                    \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE uint32_t                                   \
_FCORE_INT(p, alloc_idx_)(_FCORE_OT(ot) *owner)                              \
{                                                                             \
    uint32_t idx = owner->free_head;                                          \
    if (idx != (uint32_t)RIX_NIL) {                                           \
        _FCORE_HDR_T(p) *hdr = FCORE_LAYOUT_ENTRY_PTR(owner, idx);           \
        RIX_ASSUME_NONNULL(hdr);                                              \
        owner->free_head = FCORE_FREE_NEXT(hdr);                              \
        FCORE_FREE_NEXT(hdr) = 0u;                                            \
        hdr->htbl_elm.slot = 0u;                                              \
    }                                                                         \
    return idx;                                                               \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
_FCORE_INT(p, free_idx_)(_FCORE_OT(ot) *owner, uint32_t idx)                 \
{                                                                             \
    _FCORE_HDR_T(p) *hdr = FCORE_LAYOUT_ENTRY_PTR(owner, idx);               \
    RIX_ASSUME_NONNULL(hdr);                                                  \
    memset(hdr, 0, sizeof(*hdr));                                             \
    FCORE_FREE_NEXT(hdr) = owner->free_head;                                  \
    owner->free_head = idx;                                                   \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
_FCORE_INT(p, prefetch_free_head_)(_FCORE_OT(ot) *owner)                     \
{                                                                             \
    if (owner->free_head != (uint32_t)RIX_NIL) {                              \
        _FCORE_HDR_T(p) *hdr = FCORE_LAYOUT_ENTRY_PTR(owner,                 \
                                                       owner->free_head);     \
        if (hdr != NULL)                                                      \
            rix_hash_prefetch_entry_of(hdr);                                  \
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
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
_FCORE_INT(p, fill_pct_)(const _FCORE_OT(ot) *owner)                         \
{                                                                             \
    unsigned total_slots = (owner->ht_head.rhh_mask + 1u)                     \
                           * RIX_HASH_BUCKET_ENTRY_SZ;                        \
    if (total_slots == 0u)                                                    \
        return 0u;                                                            \
    return (unsigned)(((uint64_t)owner->ht_head.rhh_nb * 100u)                \
                      / total_slots);                                         \
}                                                                             \
                                                                              \
/*=== find_bulk (4-stage ctx pipeline) =====================================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, find_bulk_)(_FCORE_OT(ot) *owner,                              \
                          const _FCORE_KEY_T(p) *keys,                        \
                          unsigned nb_keys,                                    \
                          uint64_t now,                                        \
                          uint32_t *results)                                   \
{                                                                             \
    struct rix_hash_find_ctx_s ctx[FCORE_BULK_CTX_COUNT];                     \
    const unsigned ahead = FCORE_BULK_AHEAD_KEYS;                             \
    const unsigned step  = FCORE_BULK_STEP_KEYS;                              \
    const unsigned total = nb_keys + 3u * ahead;                              \
    (void)now;                                                                \
                                                                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        /* Stage 1: hash + prefetch both buckets */                           \
        if (_i < nb_keys) {                                                   \
            unsigned _n = (_i + step <= nb_keys) ? step : (nb_keys - _i);     \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCORE_HT(ht, hash_key_2bk)(                                  \
                    &ctx[(_i + _j) & (FCORE_BULK_CTX_COUNT - 1u)],           \
                    _FCORE_HT_HEAD(ht, owner),                                \
                    owner->buckets, &keys[_i + _j]);                          \
        }                                                                     \
        /* Stage 2: scan bucket fingerprints */                               \
        if (_i >= ahead && _i - ahead < nb_keys) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCORE_HT(ht, scan_bk)(                                       \
                    &ctx[(_base + _j) & (FCORE_BULK_CTX_COUNT - 1u)],        \
                    _FCORE_HT_HEAD(ht, owner), owner->buckets);              \
        }                                                                     \
        /* Stage 3: prefetch matched entry */                                 \
        if (_i >= 2u * ahead && _i - 2u * ahead < nb_keys) {                 \
            unsigned _base = _i - 2u * ahead;                                 \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCORE_HT(ht, prefetch_node)(                                  \
                    &ctx[(_base + _j) & (FCORE_BULK_CTX_COUNT - 1u)],        \
                    FCORE_LAYOUT_HASH_BASE(owner));                           \
        }                                                                     \
        /* Stage 4: compare key, resolve hit/miss */                          \
        if (_i >= 3u * ahead && _i - 3u * ahead < nb_keys) {                 \
            unsigned _base = _i - 3u * ahead;                                 \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                unsigned _idx = _base + _j;                                   \
                _FCORE_HDR_T(p) *_entry = _FCORE_HT(ht, cmp_key)(             \
                    &ctx[_idx & (FCORE_BULK_CTX_COUNT - 1u)],                \
                    FCORE_LAYOUT_HASH_BASE(owner));                           \
                if (RIX_LIKELY(_entry != NULL)) {                             \
                    uint32_t _eidx =                                          \
                        FCORE_LAYOUT_ENTRY_INDEX(owner, _entry);              \
                    FCORE_ON_HIT(owner, _entry, _eidx);                       \
                    results[_idx] = _eidx;                                    \
                    owner->stats.hits++;                                       \
                } else {                                                      \
                    results[_idx] = 0u;                                       \
                    owner->stats.misses++;                                     \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    owner->stats.lookups += nb_keys;                                          \
}                                                                             \
                                                                              \
/*=== del_bulk (4-stage ctx pipeline) ======================================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, del_bulk_)(_FCORE_OT(ot) *owner,                               \
                         const _FCORE_KEY_T(p) *keys,                         \
                         unsigned nb_keys,                                     \
                         uint32_t *results)                                    \
{                                                                             \
    struct rix_hash_find_ctx_s ctx[FCORE_BULK_CTX_COUNT];                     \
    const unsigned ahead = FCORE_BULK_AHEAD_KEYS;                             \
    const unsigned step  = FCORE_BULK_STEP_KEYS;                              \
    const unsigned total = nb_keys + 3u * ahead;                              \
                                                                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        if (_i < nb_keys) {                                                   \
            unsigned _n = (_i + step <= nb_keys) ? step : (nb_keys - _i);     \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCORE_HT(ht, hash_key_2bk)(                                   \
                    &ctx[(_i + _j) & (FCORE_BULK_CTX_COUNT - 1u)],           \
                    _FCORE_HT_HEAD(ht, owner),                                 \
                    owner->buckets, &keys[_i + _j]);                          \
        }                                                                     \
        if (_i >= ahead && _i - ahead < nb_keys) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCORE_HT(ht, scan_bk)(                                        \
                    &ctx[(_base + _j) & (FCORE_BULK_CTX_COUNT - 1u)],        \
                    _FCORE_HT_HEAD(ht, owner), owner->buckets);               \
        }                                                                     \
        if (_i >= 2u * ahead && _i - 2u * ahead < nb_keys) {                 \
            unsigned _base = _i - 2u * ahead;                                 \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCORE_HT(ht, prefetch_node)(                                  \
                    &ctx[(_base + _j) & (FCORE_BULK_CTX_COUNT - 1u)],        \
                    FCORE_LAYOUT_HASH_BASE(owner));                           \
        }                                                                     \
        if (_i >= 3u * ahead && _i - 3u * ahead < nb_keys) {                 \
            unsigned _base = _i - 3u * ahead;                                 \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                unsigned _idx = _base + _j;                                   \
                _FCORE_HDR_T(p) *_entry = _FCORE_HT(ht, cmp_key)(             \
                    &ctx[_idx & (FCORE_BULK_CTX_COUNT - 1u)],                \
                    FCORE_LAYOUT_HASH_BASE(owner));                           \
                if (_entry != NULL) {                                         \
                    uint32_t _eidx =                                          \
                        FCORE_LAYOUT_ENTRY_INDEX(owner, _entry);              \
                    _FCORE_HT(ht, remove)(                                     \
                        _FCORE_HT_HEAD(ht, owner), owner->buckets,            \
                        FCORE_LAYOUT_HASH_BASE(owner), _entry);              \
                    FCORE_ON_REMOVE(owner, _entry, _eidx);                    \
                    if (results != NULL)                                       \
                        results[_idx] = _eidx;                                \
                    owner->stats.dels++;                                       \
                } else {                                                      \
                    if (results != NULL)                                       \
                        results[_idx] = 0u;                                   \
                    owner->stats.del_miss++;                                   \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/*=== del_idx_bulk (2-stage pipeline) ======================================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, del_idx_bulk_)(_FCORE_OT(ot) *owner,                           \
                             const uint32_t *idxs,                            \
                             unsigned nb_idxs)                                 \
{                                                                             \
    const unsigned ahead = FCORE_BULK_AHEAD_KEYS;                             \
    const unsigned step  = FCORE_BULK_STEP_KEYS;                              \
    const unsigned total = nb_idxs + ahead;                                   \
                                                                              \
    for (unsigned _i = 0; _i < total; _i += step) {                           \
        /* Stage 1: prefetch entry */                                         \
        if (_i < nb_idxs) {                                                   \
            unsigned _n = (_i + step <= nb_idxs) ? step : (nb_idxs - _i);    \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                uint32_t _eidx = idxs[_i + _j];                              \
                if (_eidx != 0u && _eidx <= owner->max_entries) {             \
                    _FCORE_HDR_T(p) *_hdr =                                   \
                        FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                \
                    if (_hdr != NULL)                                          \
                        rix_hash_prefetch_entry_of(_hdr);                     \
                }                                                             \
            }                                                                 \
        }                                                                     \
        /* Stage 2: remove */                                                 \
        if (_i >= ahead && _i - ahead < nb_idxs) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_idxs) ? step                    \
                                                    : (nb_idxs - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                uint32_t _eidx = idxs[_base + _j];                           \
                _FCORE_HDR_T(p) *_entry;                                      \
                if (_eidx == 0u || _eidx > owner->max_entries)                \
                    continue;                                                 \
                _entry = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);               \
                if (_entry == NULL)                                           \
                    continue;                                                 \
                if (_FCORE_HT(ht, remove)(                                     \
                        _FCORE_HT_HEAD(ht, owner), owner->buckets,            \
                        FCORE_LAYOUT_HASH_BASE(owner), _entry) != NULL) {    \
                    FCORE_ON_REMOVE(owner, _entry, _eidx);                    \
                    owner->stats.dels++;                                       \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/*=== add_idx_bulk (2-stage pipeline) ======================================*/\
/*    Caller provides entry indices; keys must already be written in hdr.    */\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, add_idx_bulk_)(_FCORE_OT(ot) *owner,                           \
                             const uint32_t *entry_idxv,                      \
                             unsigned nb_keys,                                 \
                             uint64_t now,                                     \
                             uint32_t *results)                                \
{                                                                             \
    const unsigned step  = FCORE_BULK_STEP_KEYS;                              \
    const unsigned ahead = FCORE_BULK_AHEAD_KEYS;                             \
    const unsigned total = nb_keys + ahead;                                   \
    union rix_hash_hash_u hashes[FCORE_BULK_CTX_COUNT];                       \
    (void)now;                                                                \
                                                                              \
    /* Pre-prefetch entry keys */                                             \
    for (unsigned _i = 0; _i < nb_keys && _i < ahead; _i++) {                \
        if (entry_idxv[_i] != 0u &&                                           \
            entry_idxv[_i] <= owner->max_entries) {                           \
            _FCORE_HDR_T(p) *_hdr =                                           \
                FCORE_LAYOUT_ENTRY_PTR(owner, entry_idxv[_i]);               \
            if (_hdr != NULL)                                                 \
                rix_hash_prefetch_key(&_hdr->key);                            \
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
                _FCORE_HDR_T(p) *_hdr;                                        \
                unsigned _bk0, _bk1;                                          \
                uint32_t _fp_unused;                                          \
                if (_eidx == 0u || _eidx > owner->max_entries) {              \
                    hashes[_idx & (FCORE_BULK_CTX_COUNT - 1u)].val64 = 0u;   \
                    continue;                                                 \
                }                                                             \
                _hdr = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                  \
                RIX_ASSUME_NONNULL(_hdr);                                     \
                hashes[_idx & (FCORE_BULK_CTX_COUNT - 1u)] =                  \
                    hash_fn(&_hdr->key,                                       \
                            _FCORE_HT_HEAD(ht, owner)->rhh_mask);              \
                _rix_hash_buckets(                                            \
                    hashes[_idx & (FCORE_BULK_CTX_COUNT - 1u)],              \
                    _FCORE_HT_HEAD(ht, owner)->rhh_mask,                       \
                    &_bk0, &_bk1, &_fp_unused);                              \
                rix_hash_prefetch_bucket_of(&owner->buckets[_bk0]);           \
                if (_bk1 != _bk0)                                             \
                    rix_hash_prefetch_bucket_of(&owner->buckets[_bk1]);       \
            }                                                                 \
        }                                                                     \
        /* Stage 2: insert */                                                 \
        if (_i >= ahead && _i - ahead < nb_keys) {                            \
            unsigned _base = _i - ahead;                                      \
            unsigned _n = (_base + step <= nb_keys) ? step                    \
                                                    : (nb_keys - _base);      \
            for (unsigned _j = 0; _j < _n; _j++) {                           \
                unsigned _idx = _base + _j;                                   \
                uint32_t _eidx = entry_idxv[_idx];                            \
                _FCORE_HDR_T(p) *_hdr;                                        \
                _FCORE_HDR_T(p) *_ret;                                        \
                if (_eidx == 0u || _eidx > owner->max_entries) {              \
                    if (results != NULL) results[_idx] = 0u;                  \
                    continue;                                                 \
                }                                                             \
                _hdr = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                  \
                RIX_ASSUME_NONNULL(_hdr);                                     \
                if (_hdr->htbl_elm.cur_hash != 0u) {                          \
                    /* entry already in table (self-duplicate) */              \
                    owner->stats.add_existing++;                               \
                    if (results != NULL) results[_idx] = _eidx;               \
                    continue;                                                  \
                }                                                             \
                _ret = _FCORE_HT(ht, insert_hashed)(                           \
                    _FCORE_HT_HEAD(ht, owner), owner->buckets,                \
                    FCORE_LAYOUT_HASH_BASE(owner), _hdr,                     \
                    hashes[_idx & (FCORE_BULK_CTX_COUNT - 1u)]);             \
                if (RIX_LIKELY(_ret == NULL)) {                               \
                    /* success: new entry inserted */                          \
                    FCORE_ON_INSERT(owner, _hdr, _eidx);                      \
                    owner->stats.adds++;                                       \
                    if (results != NULL) results[_idx] = _eidx;               \
                } else if (_ret != _hdr) {                                    \
                    /* duplicate: different entry with same key */             \
                    owner->stats.add_existing++;                               \
                    if (results != NULL)                                       \
                        results[_idx] =                                       \
                            FCORE_LAYOUT_ENTRY_INDEX(owner, _ret);            \
                } else {                                                      \
                    /* table full */                                           \
                    owner->stats.add_failed++;                                 \
                    if (results != NULL) results[_idx] = 0u;                  \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
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
}                                                                             \
                                                                              \
/* end FCORE_GENERATE */

#endif /* _FLOW_CORE_H_ */
