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
 *   3. FCORE_GENERATE macro: find_bulk, add_idx_bulk, del_idx, del_idx_bulk,
 *      del_key_bulk, counters, walk, flush
 *
 * Usage in variant .c files:
 *
 *   // 1. Include owner header (which includes this via common.h)
 *   #include "flow4_cache.h"   // or "flow4_table.h"
 *
 *   // 2. Define hash/cmp functions
 *   static inline union rix_hash_hash_u
 *   fcore_flow4_hash_fn(const struct flow4_key *key, u32 mask) { ... }
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
 *   owner->free_head          : u32 (unused by FCORE, managed externally)
 *   FCORE_STATS(owner).lookups      : u64
 *   FCORE_STATS(owner).hits         : u64
 *   FCORE_STATS(owner).misses       : u64
 *   FCORE_STATS(owner).adds         : u64
 *   FCORE_STATS(owner).add_existing : u64
 *   FCORE_STATS(owner).add_failed   : u64
 *   FCORE_STATS(owner).dels         : u64
 *   FCORE_STATS(owner).del_miss     : u64
 *   FCORE_STATUS(owner).entries     : u32
 *   FCORE_STATUS(owner).kickouts    : u32
 *   FCORE_STATUS(owner).add_bk0     : u32
 *   FCORE_STATUS(owner).add_bk1     : u32
 *   owner->ts_shift                 : u8
 */

#ifndef _FLOW_CORE_H_
#define _FLOW_CORE_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rix/rix_hash.h>

/*===========================================================================
 * Shared datapath statistics
 *===========================================================================*/
struct fcore_stats {
    u64 lookups;
    u64 hits;
    u64 misses;
    u64 adds;
    u64 add_existing;
    u64 add_failed;
    u64 dels;
    u64 del_miss;
};

struct fcore_status {
    u32 entries;
    u32 kickouts;
    u32 add_bk0;
    u32 add_bk1;
};

static inline void
fcore_status_reset(struct fcore_status *status, u32 entries)
{
    if (status == NULL)
        return;
    status->entries = entries;
    status->kickouts = 0u;
    status->add_bk0 = 0u;
    status->add_bk1 = 0u;
}

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

#ifndef FCORE_INIT_TIMESTAMP
#define FCORE_INIT_TIMESTAMP(owner, entry, now)                             \
    do {                                                                    \
        if ((now) != 0u)                                                    \
            flow_timestamp_store(&(entry)->meta, (now),                     \
                                 FCORE_TIMESTAMP_SHIFT(owner));             \
    } while (0)
#endif

#ifndef FCORE_TOUCH_TIMESTAMP
#define FCORE_TOUCH_TIMESTAMP(owner, entry, now)                            \
    do {                                                                    \
        if ((now) != 0u)                                                    \
            flow_timestamp_touch(&(entry)->meta, (now),                     \
                                 FCORE_TIMESTAMP_SHIFT(owner));             \
    } while (0)
#endif

#ifndef FCORE_CLEAR_TIMESTAMP
#define FCORE_CLEAR_TIMESTAMP(entry) flow_timestamp_clear(&(entry)->meta)
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
/*=== find_key_bulk (scan -> immediate prefetch) ===========================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, find_key_bulk_)(_FCORE_OT(ot) *owner,                          \
                              const _FCORE_KEY_T(p) *keys,                    \
                              unsigned nb_keys,                               \
                              u64 now,                                   \
                              u32 *results)                              \
{                                                                             \
    enum { _FCORE_FIND_CTX_COUNT = 32u };                                     \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    const u32 hash_mask = FCORE_HASH_MASK(owner, ht);                    \
    struct rix_hash_find_ctx_s ctx[_FCORE_FIND_CTX_COUNT];                    \
    u64 hit_count = 0u;                                                  \
    u64 miss_count = 0u;                                                 \
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
                    u32 _eidx = FCORE_LAYOUT_ENTRY_INDEX(owner, entry);  \
                    FCORE_TOUCH_TIMESTAMP(owner, entry, now);                 \
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
                              u32 *entry_idxv,                           \
                              unsigned nb_keys,                               \
                              enum ft_add_policy policy,                      \
                              u64 now,                                   \
                              u32 *unused_idxv)                          \
{                                                                             \
    enum { _FCORE_ADD_CTX_COUNT = 8u };                                       \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    const u32 hash_mask = FCORE_HASH_MASK(owner, ht);                    \
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
            u32 _eidx = entry_idxv[_i];                                  \
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
                u32 _eidx = entry_idxv[_idx];                            \
                _FCORE_ENTRY_T(p) *entry;                                     \
                struct rix_hash_find_ctx_s *_ctxp =                           \
                    &ctx[_idx & ctx_mask];                                    \
                unsigned _bk0, _bk1;                                          \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                entry = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                 \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (_idx + ahead < nb_keys) {                                 \
                    u32 _peidx = entry_idxv[_idx + ahead];               \
                    _FCORE_ENTRY_T(p) *_pentry =                              \
                        FCORE_LAYOUT_ENTRY_PTR(owner, _peidx);                \
                    RIX_ASSERT(_peidx != 0u);                                 \
                    RIX_ASSERT(_peidx <= owner->max_entries);                 \
                    RIX_ASSUME_NONNULL(_pentry);                              \
                    rix_hash_prefetch_key(&_pentry->key);                     \
                }                                                             \
                _ctxp->hash = hash_fn(&entry->key, hash_mask);                \
                _ctxp->fp = rix_hash_fp(_ctxp->hash, head->rhh_mask,        \
                                        &_bk0, &_bk1);                        \
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
                u32 _eidx = entry_idxv[_idx];                            \
                struct rix_hash_find_ctx_s *_ctxp = &ctx[_idx & ctx_mask];    \
                _FCORE_ENTRY_T(p) *entry =                                    \
                    (_FCORE_ENTRY_T(p) *)(uintptr_t)_ctxp->key;               \
                u32 _ret_idx = (u32)RIX_NIL;                        \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (RIX_UNLIKELY(entry->meta.cur_hash != 0u)) {               \
                    FCORE_TOUCH_TIMESTAMP(owner, entry, now);                 \
                    FCORE_STATS(owner).add_existing++;                        \
                    continue;                                                  \
                }                                                             \
                _FCORE_HT(ht, scan_bk_empties)(_ctxp, 0u);                    \
                if (RIX_UNLIKELY(_ctxp->fp_hits[0] != 0u)) {                  \
                    u32 _hits = _ctxp->fp_hits[0];                            \
                    while (RIX_UNLIKELY(_hits != 0u)) {                       \
                        unsigned _bit = (unsigned)__builtin_ctz(_hits);       \
                        u32 _nidx = _ctxp->bk[0]->idx[_bit];             \
                        _FCORE_ENTRY_T(p) *_node;                             \
                        _hits &= _hits - 1u;                                  \
                        if (RIX_UNLIKELY(_nidx == (u32)RIX_NIL))         \
                            continue;                                         \
                        _node = FCORE_LAYOUT_ENTRY_PTR(owner, _nidx);         \
                        RIX_ASSUME_NONNULL(_node);                            \
                        if (RIX_UNLIKELY(cmp_fn(&entry->key, &_node->key)     \
                                         == 0)) {                             \
                            FCORE_STATS(owner).add_existing++;                      \
                            if (policy == FT_ADD_UPDATE) {                    \
                                _ctxp->bk[0]->idx[_bit] = _eidx;              \
                                entry->meta.cur_hash =                        \
                                    _ctxp->hash.val32[0];                     \
                                entry->meta.slot =                            \
                                    (__typeof__(entry->meta.slot))_bit;       \
                                FCORE_INIT_TIMESTAMP(owner, entry, now);      \
                                _node->meta.cur_hash = 0u;                    \
                                FCORE_CLEAR_TIMESTAMP(_node);                 \
                                unused_idxv[free_count++] = _nidx;            \
                            } else {                                          \
                                FCORE_TOUCH_TIMESTAMP(owner, _node, now);     \
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
                        u32 _nidx = _ctxp->bk[1]->idx[_bit];             \
                        _FCORE_ENTRY_T(p) *_node;                             \
                        _hits &= _hits - 1u;                                  \
                        if (RIX_UNLIKELY(_nidx == (u32)RIX_NIL))         \
                            continue;                                         \
                        _node = FCORE_LAYOUT_ENTRY_PTR(owner, _nidx);         \
                        RIX_ASSUME_NONNULL(_node);                            \
                        if (RIX_UNLIKELY(cmp_fn(&entry->key, &_node->key)     \
                                         == 0)) {                             \
                            FCORE_STATS(owner).add_existing++;                      \
                            if (policy == FT_ADD_UPDATE) {                    \
                                _ctxp->bk[1]->idx[_bit] = _eidx;              \
                                entry->meta.cur_hash =                        \
                                    _ctxp->hash.val32[1];                     \
                                entry->meta.slot =                            \
                                    (__typeof__(entry->meta.slot))_bit;       \
                                FCORE_INIT_TIMESTAMP(owner, entry, now);      \
                                _node->meta.cur_hash = 0u;                    \
                                FCORE_CLEAR_TIMESTAMP(_node);                 \
                                unused_idxv[free_count++] = _nidx;            \
                            } else {                                          \
                                FCORE_TOUCH_TIMESTAMP(owner, _node, now);     \
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
                    entry->meta.cur_hash = _ctxp->hash.val32[0];              \
                    entry->meta.slot = (__typeof__(entry->meta.slot))         \
                        _slot;                                                \
                    FCORE_INIT_TIMESTAMP(owner, entry, now);                  \
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
                    entry->meta.cur_hash = _ctxp->hash.val32[1];              \
                    entry->meta.slot = (__typeof__(entry->meta.slot))         \
                        _slot;                                                \
                    FCORE_INIT_TIMESTAMP(owner, entry, now);                  \
                    head->rhh_nb++;                                           \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).add_bk1++;                            \
                    continue;                                                  \
                }                                                             \
                _ret_idx = _FCORE_HT(ht, insert_hashed_idx)(                  \
                    head, buckets, hash_base, entry, _ctxp->hash);            \
                if (RIX_LIKELY(_ret_idx == 0u)) {                             \
                    FCORE_INIT_TIMESTAMP(owner, entry, now);                  \
                    FCORE_STATS(owner).adds++;                                \
                    FCORE_STATUS(owner).entries++;                            \
                    FCORE_STATUS(owner).kickouts++;                           \
                    if (entry->meta.cur_hash == _ctxp->hash.val32[1])         \
                        FCORE_STATUS(owner).add_bk1++;                        \
                    else                                                      \
                        FCORE_STATUS(owner).add_bk0++;                        \
                } else if (RIX_UNLIKELY(_ret_idx != _eidx)) {                 \
                    FCORE_STATS(owner).add_existing++;                        \
                    if (policy == FT_ADD_UPDATE) {                            \
                        _FCORE_ENTRY_T(p) *_node =                            \
                            FCORE_LAYOUT_ENTRY_PTR(owner, _ret_idx);          \
                        u32 _ins_idx;                                    \
                        RIX_ASSUME_NONNULL(_node);                            \
                        _ins_idx = _ret_idx;                                  \
                        RIX_ASSERT(_FCORE_HT(ht, remove)(                      \
                                       head, buckets, hash_base, _node)       \
                                   != NULL);                                  \
                        _node->meta.cur_hash = 0u;                            \
                        FCORE_CLEAR_TIMESTAMP(_node);                         \
                        _ins_idx = _FCORE_HT(ht, insert_hashed_idx)(          \
                            head, buckets, hash_base, entry, _ctxp->hash);    \
                        RIX_ASSERT(_ins_idx == 0u);                           \
                        FCORE_INIT_TIMESTAMP(owner, entry, now);              \
                        unused_idxv[free_count++] = _ret_idx;                 \
                    } else {                                                  \
                        _FCORE_ENTRY_T(p) *_node =                            \
                            FCORE_LAYOUT_ENTRY_PTR(owner, _ret_idx);          \
                        RIX_ASSUME_NONNULL(_node);                            \
                        FCORE_TOUCH_TIMESTAMP(owner, _node, now);             \
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
/*=== del_idx_bulk (2-stage pipeline) ======================================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_INT(p, del_idx_bulk_)(_FCORE_OT(ot) *owner,                           \
                             const u32 *idxs,                            \
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
                u32 _eidx = idxs[_i + _j];                              \
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
                u32 _eidx = idxs[_base + _j];                           \
                _FCORE_ENTRY_T(p) *entry;                                     \
                RIX_ASSERT(_eidx != 0u);                                      \
                RIX_ASSERT(_eidx <= owner->max_entries);                      \
                entry = FCORE_LAYOUT_ENTRY_PTR(owner, _eidx);                \
                RIX_ASSUME_NONNULL(entry);                                    \
                if (entry->meta.cur_hash == 0u)                               \
                    continue;                                                 \
                if (_FCORE_HT(ht, remove)(                                     \
                        head, buckets, hash_base, entry) != NULL) {           \
                    entry->meta.cur_hash = 0u;                                \
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
/*=== del_key_bulk (find by key + remove) ==================================*/\
                                                                              \
static RIX_UNUSED unsigned                                                    \
_FCORE_INT(p, del_key_bulk_)(_FCORE_OT(ot) *owner,                           \
                             const _FCORE_KEY_T(p) *keys,                     \
                             unsigned nb_keys,                                \
                             u32 *unused_idxv)                           \
{                                                                             \
    _FCORE_HT_T(ht) *head = _FCORE_HT_HEAD(ht, owner);                        \
    struct rix_hash_bucket_s *buckets = owner->buckets;                       \
    _FCORE_ENTRY_T(p) *hash_base = FCORE_LAYOUT_HASH_BASE(owner);             \
    unsigned _count = 0u;                                                     \
    for (unsigned _i = 0u; _i < nb_keys; _i++) {                              \
        _FCORE_ENTRY_T(p) *entry;                                             \
        entry = _FCORE_HT(ht, find)(head, buckets, hash_base, &keys[_i]);     \
        if (entry == NULL) {                                                  \
            FCORE_STATS(owner).del_miss++;                                    \
            continue;                                                         \
        }                                                                     \
        u32 _eidx = FCORE_LAYOUT_ENTRY_INDEX(owner, entry);             \
        if (_FCORE_HT(ht, remove)(head, buckets, hash_base, entry) == NULL)   \
            continue;                                                         \
        entry->meta.cur_hash = 0u;                                            \
        FCORE_CLEAR_TIMESTAMP(entry);                                         \
        FCORE_STATS(owner).dels++;                                            \
        RIX_ASSERT(FCORE_STATUS(owner).entries != 0u);                        \
        FCORE_STATUS(owner).entries--;                                        \
        unused_idxv[_count++] = _eidx;                                        \
    }                                                                         \
    return _count;                                                            \
}                                                                             \
                                                                              \
/*=== walk =================================================================*/\
                                                                              \
static RIX_UNUSED int                                                         \
_FCORE_INT(p, walk_)(_FCORE_OT(ot) *owner,                                   \
                     int (*cb)(u32 entry_idx, void *arg),                \
                     void *arg)                                               \
{                                                                             \
    unsigned _nb_bk = owner->ht_head.rhh_mask + 1u;                           \
    for (unsigned _b = 0; _b < _nb_bk; _b++) {                                \
        struct rix_hash_bucket_s *_bk = &owner->buckets[_b];                  \
        for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {          \
            unsigned _nidx = _bk->idx[_s];                                    \
            if (_nidx == (unsigned)RIX_NIL)                                   \
                continue;                                                     \
            int _rc = cb((u32)_nidx, arg);                               \
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
            owner->buckets[_b].idx[_s] = (u32)RIX_NIL;                  \
    owner->ht_head.rhh_nb = 0u;                                               \
    fcore_status_reset(&FCORE_STATUS(owner), 0u);                             \
}                                                                             \
                                                                              \
/* end FCORE_GENERATE */

#endif /* _FLOW_CORE_H_ */
