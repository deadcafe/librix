/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_core_extra.h - Shared compile-time layer for ftable slot_extra variant.
 *
 * Provides:
 *   1. Record pool macros (stride/offset-based indirection)
 *   2. Bucket allocator / record layout types (192 B rix_hash_bucket_extra_s)
 *   3. FCORE_EXTRA_GENERATE macro: find_bulk, add_idx_bulk, del_idx,
 *      del_idx_bulk, del_key_bulk, counters, walk, flush
 *
 * Usage in variant .c files:
 *
 *   // 1. Include owner header and this internal core header
 *   #include "flow4_extra_table.h"
 *   #include "flow_core_extra.h"
 *
 *   // 2. Define hash/cmp functions
 *   static inline union rix_hash_hash_u
 *   fcore_flow4x_hash_fn(const struct flow4_extra_key *key, u32 mask) { ... }
 *   static inline int
 *   fcore_flow4x_cmp(const struct flow4_extra_key *a,
 *                    const struct flow4_extra_key *b) { ... }
 *
 *   // 3. Override layout hooks (for stride/offset indirection)
 *   #define FCORE_EXTRA_LAYOUT_HASH_BASE(owner) ...
 *   #define FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, idx) ...
 *   #define FCORE_EXTRA_LAYOUT_ENTRY_INDEX(owner, entry) ...
 *
 *   // 4. Override RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS if needed
 *
 *   // 5. Generate rix_hash_slot_extra for flowX_extra_entry
 *   RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(fcore_flow4x_ht, flow4_extra_entry,
 *       key, meta.cur_hash, meta.slot, fcore_flow4x_cmp, fcore_flow4x_hash_fn)
 *
 *   // 6. Optional hit hook
 *   #define FCORE_EXTRA_ON_HIT(owner, entry, idx) ...
 *
 *   // 7. Expand
 *   FCORE_EXTRA_GENERATE(flow4_extra, ft_table_extra, fcore_flow4x_ht,
 *                        fcore_flow4x_hash_fn, fcore_flow4x_cmp)
 *
 * Owner struct requirements (field names are part of the contract):
 *   owner->buckets            : struct rix_hash_bucket_extra_s *
 *   owner->ht_head            : struct { unsigned rhh_mask; unsigned rhh_nb; }
 *   owner->max_entries        : unsigned
 *   owner->pool_base          : unsigned char *
 *   owner->pool_stride        : size_t
 *   owner->pool_entry_offset  : size_t
 *   owner->free_head          : u32 (unused by FCORE_EXTRA, managed externally)
 *   FLOW_STATS(owner).lookups      : u64
 *   FLOW_STATS(owner).hits         : u64
 *   FLOW_STATS(owner).misses       : u64
 *   FLOW_STATS(owner).adds         : u64
 *   FLOW_STATS(owner).add_existing : u64
 *   FLOW_STATS(owner).add_failed   : u64
 *   FLOW_STATS(owner).dels         : u64
 *   FLOW_STATS(owner).del_miss     : u64
 *   FLOW_STATUS(owner).entries     : u32
 *   FLOW_STATUS(owner).kickouts    : u32
 *   FLOW_STATUS(owner).add_bk0     : u32
 *   FLOW_STATUS(owner).add_bk1     : u32
 *   owner->ts_shift                : u8
 */

/*
 * TIMESTAMP SEMANTICS (differs from classic flow_core.h):
 *
 * Classic FCORE_INIT/TOUCH_TIMESTAMP wrote to entry->meta.timestamp.
 * In the extra variant the timestamp lives at bk->extra[slot], and
 * writing it requires (bk, slot) context that is local to the insert /
 * touch call site. The generated hot path therefore performs the
 * timestamp store itself:
 *   - insert: the 5-arg rix_hash_slot_extra insert takes the encoded
 *     timestamp as its 5th argument; the kickout/flipflop preserves
 *     extra[] automatically.
 *   - find-hit: a local (bk, slot) derivation updates bk->extra[slot]
 *     inline before the hook is invoked.
 * The FCORE_EXTRA_*_TIMESTAMP hooks remain as no-ops so variant .c
 * files can plug extra bookkeeping in if needed.
 */

#ifndef _FLOW_CORE_EXTRA_H_
#define _FLOW_CORE_EXTRA_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "flow_extra_key.h"

/*===========================================================================
 * Private layout helpers
 *===========================================================================*/
static inline void *
fcore_extra_record_member_ptr_nonnull(void *base,
                                      size_t stride,
                                      unsigned idx,
                                      size_t member_offset)
{
    RIX_ASSERT(idx != RIX_NIL);
    return FT_BYTE_PTR_ADD(FT_BYTE_PTR_ADD(base,
                                           RIX_IDX_TO_OFF0(idx) * stride),
                           member_offset);
}

#define FCORE_EXTRA_RECORD_MEMBER_PTR_NONNULL_ALIGNED(type, base, stride,    \
                                                      idx, member_offset)    \
    ((type *)__builtin_assume_aligned(                                        \
        fcore_extra_record_member_ptr_nonnull((base), (stride), (idx),       \
                                              (member_offset)),               \
        _Alignof(type)))

/*===========================================================================
 * Pipeline tuning defaults
 *===========================================================================*/
/*===========================================================================
 * Token-paste helpers for FCORE_EXTRA_GENERATE
 *===========================================================================*/
#define _FCORE_EXTRA_CAT2(a, b)  a##b
#define _FCORE_EXTRA_CAT(a, b)   _FCORE_EXTRA_CAT2(a, b)

/* rix_hash_slot_extra function names: ht##_xxx (ht param of FCORE_EXTRA_GENERATE) */
#define _FCORE_EXTRA_HT(ht, name) \
    _FCORE_EXTRA_CAT(ht, _##name)

/* Internal helper names: _fcore_extra_flow4_xxx */
#define _FCORE_EXTRA_INT(p, name) \
    _FCORE_EXTRA_CAT(_fcore_extra_, _FCORE_EXTRA_CAT(p, _##name))

/* Type helpers */
#define _FCORE_EXTRA_KEY_T(p)      struct _FCORE_EXTRA_CAT(p, _key)
#define _FCORE_EXTRA_ENTRY_T(p)    struct _FCORE_EXTRA_CAT(p, _entry)
#define _FCORE_EXTRA_OT(ot)        struct ot

/* ht_head type: struct <ht> (same layout as any RIX_HASH_HEAD) */
#define _FCORE_EXTRA_HT_T(ht)     struct ht

/* Cast owner->ht_head to the ht type (layout-compatible) */
#define _FCORE_EXTRA_HT_HEAD(ht, owner) \
    ((_FCORE_EXTRA_HT_T(ht) *)(void *)&(owner)->ht_head)

/*===========================================================================
 * Layout hook defaults (override before FCORE_EXTRA_GENERATE expansion)
 *===========================================================================*/
#ifndef FCORE_EXTRA_LAYOUT_HASH_BASE
#define FCORE_EXTRA_LAYOUT_HASH_BASE(owner) \
    ((_FCORE_EXTRA_ENTRY_T(_fcore_extra_cur_p) *)(void *)(owner))
#endif

/* These must be overridden per-variant for stride/offset indirection. */

/*===========================================================================
 * Trait hook defaults (override before FCORE_EXTRA_GENERATE expansion)
 *
 * Trait hooks are expanded inside bulk function bodies.
 * Available local variables: owner, now (for find/findadd), entry (hdr ptr),
 * idx (1-origin pool index).
 *
 * NOTE: In the extra variant, the actual timestamp write to bk->extra[slot]
 * is performed inline at each call site before these hooks are invoked.
 * The hooks default to no-ops; override them for stats/debug if needed.
 *===========================================================================*/
#ifndef FCORE_EXTRA_ON_HIT
#define FCORE_EXTRA_ON_HIT(owner, entry, idx)     (void)0
#endif

#ifndef FCORE_EXTRA_TIMESTAMP_SHIFT
#define FCORE_EXTRA_TIMESTAMP_SHIFT(owner) ((owner)->ts_shift)
#endif

#ifndef FCORE_EXTRA_INIT_TIMESTAMP
#define FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now)                        \
    ((void)(owner), (void)(entry), (void)(now))
#endif

#ifndef FCORE_EXTRA_TOUCH_TIMESTAMP
#define FCORE_EXTRA_TOUCH_TIMESTAMP(owner, entry, now)                       \
    ((void)(owner), (void)(entry), (void)(now))
#endif

#ifndef FCORE_EXTRA_CLEAR_TIMESTAMP
#define FCORE_EXTRA_CLEAR_TIMESTAMP(entry) ((void)(entry))
#endif

/* FCORE_EXTRA_ON_FINDADD_MISS: if defined, findadd_bulk is generated. */

/*---------------------------------------------------------------------------
 * FCORE_EXTRA_HASH_MASK(owner, ht)
 *
 * Returns the mask to pass to hash_fn() for hash-pair computation.
 *
 * Default: ht_head.rhh_mask (the current bucket mask).
 * ftable overrides this to start_mask so that the hash pair remains
 * stable across grow_2x.
 *---------------------------------------------------------------------------*/
#ifndef FCORE_EXTRA_HASH_MASK
#define FCORE_EXTRA_HASH_MASK(owner, ht) \
    _FCORE_EXTRA_HT_HEAD(ht, owner)->rhh_mask
#endif

#ifndef FLOW_STATS
#define FLOW_STATS(owner) ((owner)->stats)
#endif

#ifndef FLOW_STATUS
#define FLOW_STATUS(owner) ((owner)->status)
#endif

/*===========================================================================
 * FCORE_EXTRA_GENERATE(p, ot, ht, hash_fn, cmp_fn)
 *
 * p       : variant prefix (flow4_extra, flow6_extra, flowu_extra)
 * ot      : owner type name without "struct" (e.g. ft_table_extra)
 * ht      : rix_hash_slot_extra name prefix (e.g. fc_flow4x_ht)
 * hash_fn : hash function for this variant
 * cmp_fn  : compare function for this variant
 *
 * Prerequisite: RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX must be expanded
 *               with name = <ht>, type = <p>_entry.
 *===========================================================================*/

#define FCORE_EXTRA_GENERATE(p, ot, ht, hash_fn, cmp_fn)                     \
                                                                              \
/*=== Counters =============================================================*/\
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
_FCORE_EXTRA_INT(p, nb_entries_)(const _FCORE_EXTRA_OT(ot) *owner)           \
{                                                                             \
    return owner->ht_head.rhh_nb;                                             \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
_FCORE_EXTRA_INT(p, nb_bk_)(const _FCORE_EXTRA_OT(ot) *owner)                \
{                                                                             \
    return owner->ht_head.rhh_mask + 1u;                                      \
}                                                                             \
                                                                              \
/*=== find_key_bulk (scan -> immediate prefetch) ===========================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_EXTRA_INT(p, find_key_bulk_)(_FCORE_EXTRA_OT(ot) *owner,              \
                                    const _FCORE_EXTRA_KEY_T(p) *keys,       \
                                    unsigned nb_keys,                         \
                                    u64 now,                                  \
                                    u32 *results)                             \
{                                                                             \
    enum { _FCORE_EXTRA_FIND_CTX_COUNT = 64u };                               \
    _FCORE_EXTRA_HT_T(ht) *head = _FCORE_EXTRA_HT_HEAD(ht, owner);           \
    struct rix_hash_bucket_extra_s *buckets = owner->buckets;                 \
    _FCORE_EXTRA_ENTRY_T(p) *hash_base =                                      \
        FCORE_EXTRA_LAYOUT_HASH_BASE(owner);                                  \
    const u32 hash_mask = FCORE_EXTRA_HASH_MASK(owner, ht);                  \
    struct rix_hash_find_ctx_extra_s ctx[_FCORE_EXTRA_FIND_CTX_COUNT];        \
    u64 hit_count = 0u;                                                       \
    u64 miss_count = 0u;                                                      \
    const unsigned ctx_mask = _FCORE_EXTRA_FIND_CTX_COUNT - 1u;              \
    unsigned ahead;                                                           \
    unsigned step;                                                            \
    if (nb_keys >= 128u) {                                                    \
        step = 16u;                                                           \
        ahead = 16u;                                                          \
    } else if (nb_keys >= 64u) {                                              \
        step = 8u;                                                            \
        ahead = 16u;                                                          \
    } else if (nb_keys >= 32u) {                                              \
        step = 8u;                                                            \
        ahead = 8u;                                                           \
    } else {                                                                  \
        step = 8u;                                                            \
        ahead = 8u;                                                           \
    }                                                                         \
    const unsigned total = nb_keys + 2u * ahead;                              \
    for (unsigned i = 0; i < total; i += step) {                              \
        if (i < nb_keys) {                                                    \
            unsigned n = (i + step <= nb_keys) ? step : (nb_keys - i);        \
            for (unsigned j = 0; j < n; j++)                                  \
                _FCORE_EXTRA_HT(ht, hash_key_2bk_masked)(                    \
                    &ctx[(i + j) & ctx_mask], buckets,                        \
                    &keys[i + j], hash_mask, head->rhh_mask);                 \
        }                                                                     \
        if (i >= ahead && i - ahead < nb_keys) {                              \
            unsigned base = i - ahead;                                        \
            unsigned n = (base + step <= nb_keys) ? step                      \
                                                  : (nb_keys - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                struct rix_hash_find_ctx_extra_s *ctxp =                      \
                    &ctx[(base + j) & ctx_mask];                              \
                _FCORE_EXTRA_HT(ht, scan_bk)(ctxp, head, buckets);           \
                _FCORE_EXTRA_HT(ht, prefetch_node)(ctxp, hash_base);         \
            }                                                                 \
        }                                                                     \
        if (i >= 2u * ahead && i - 2u * ahead < nb_keys) {                   \
            unsigned base = i - 2u * ahead;                                   \
            unsigned n = (base + step <= nb_keys) ? step                      \
                                                  : (nb_keys - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                unsigned idx = base + j;                                      \
                _FCORE_EXTRA_ENTRY_T(p) *entry =                              \
                    _FCORE_EXTRA_HT(ht, cmp_key)(                             \
                        &ctx[idx & ctx_mask], hash_base);                     \
                if (RIX_LIKELY(entry != NULL)) {                              \
                    u32 eidx =                                                 \
                        FCORE_EXTRA_LAYOUT_ENTRY_INDEX(owner, entry);         \
                    /* Write timestamp to bk->extra[slot] inline */           \
                    {                                                         \
                        unsigned _slot = entry->meta.slot;                    \
                        struct rix_hash_bucket_extra_s *_bk =                 \
                            ctx[idx & ctx_mask].bk[0];                        \
                        if (_bk->idx[_slot] != eidx)                          \
                            _bk = ctx[idx & ctx_mask].bk[1];                 \
                        if ((now) != 0u)                                      \
                            _bk->extra[_slot] =                               \
                                flow_timestamp_encode(                        \
                                    (now),                                    \
                                    FCORE_EXTRA_TIMESTAMP_SHIFT(owner));      \
                    }                                                         \
                    FCORE_EXTRA_TOUCH_TIMESTAMP(owner, entry, (now));         \
                    FCORE_EXTRA_ON_HIT(owner, entry, eidx);                   \
                    results[idx] = eidx;                                      \
                    hit_count++;                                              \
                } else {                                                      \
                    results[idx] = 0u;                                        \
                    miss_count++;                                             \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    FLOW_STATS(owner).lookups += nb_keys;                                     \
    FLOW_STATS(owner).hits += hit_count;                                      \
    FLOW_STATS(owner).misses += miss_count;                                   \
}                                                                             \
                                                                              \
/*=== add_idx_bulk (scalar fast path for q<4 + staged bulk otherwise) =====*/\
/* Stage 1: prefetch entry node                                             */\
/* Stage 2: read entry key, hash, prefetch buckets                          */\
/* Stage 3: scan both buckets, prefetch matching nodes                      */\
/* Stage 4: duplicate check + insert                                        */\
/* idxv[] is input/output and returns the actually registered idx.           */\
/* unused_idxv[] packs free-list candidates in order and return value is     */\
/* the number of packed entries.                                             */\
/*   inserted           -> idxv[i] = request idx, unused none                */\
/*   duplicate+ignore   -> idxv[i] = existing idx, unused += request idx     */\
/*   duplicate+update   -> idxv[i] = request idx, unused += old idx          */\
/*   add failed         -> idxv[i] = 0, unused += request idx                */\
/* UPDATE requires that a batch does not contain duplicate keys.             */\
                                                                              \
static RIX_UNUSED unsigned                                                    \
_FCORE_EXTRA_INT(p, add_idx_small_)(_FCORE_EXTRA_OT(ot) *owner,              \
                                    u32 *entry_idxv,                          \
                                    unsigned nb_keys,                         \
                                    enum ft_add_policy policy,                \
                                    u64 now,                                  \
                                    u32 *unused_idxv)                         \
{                                                                             \
    _FCORE_EXTRA_HT_T(ht) *head = _FCORE_EXTRA_HT_HEAD(ht, owner);           \
    struct rix_hash_bucket_extra_s *buckets = owner->buckets;                 \
    _FCORE_EXTRA_ENTRY_T(p) *hash_base =                                      \
        FCORE_EXTRA_LAYOUT_HASH_BASE(owner);                                  \
    const u32 hash_mask = FCORE_EXTRA_HASH_MASK(owner, ht);                  \
    unsigned free_count = 0u;                                                 \
                                                                              \
    for (unsigned idx = 0; idx < nb_keys; idx++) {                            \
        u32 eidx = entry_idxv[idx];                                           \
        struct rix_hash_find_ctx_extra_s ctx;                                 \
        _FCORE_EXTRA_ENTRY_T(p) *entry;                                       \
        u32 ret_idx = (u32)RIX_NIL;                                           \
                                                                              \
        RIX_ASSERT(eidx <= owner->max_entries);                               \
        entry = FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, eidx);                    \
        RIX_ASSUME_NONNULL(entry);                                            \
        rix_hash_prefetch_key(&entry->key);                                   \
        _FCORE_EXTRA_HT(ht, hash_key_2bk_masked)(&ctx, buckets, &entry->key, \
                                                  hash_mask, head->rhh_mask); \
        _FCORE_EXTRA_HT(ht, scan_bk_empties)(&ctx, 0u);                      \
        _FCORE_EXTRA_HT(ht, scan_bk_empties)(&ctx, 1u);                      \
        if (RIX_UNLIKELY(ctx.fp_hits[0] != 0u)) {                             \
            unsigned bit = (unsigned)__builtin_ctz(ctx.fp_hits[0]);           \
            u32 nidx = ctx.bk[0]->idx[bit];                                   \
            if (RIX_LIKELY(nidx != (u32)RIX_NIL))                             \
                rix_hash_prefetch_entry_of(                                   \
                    FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, nidx));               \
        }                                                                     \
        if (RIX_UNLIKELY(ctx.fp_hits[1] != 0u)) {                             \
            unsigned bit = (unsigned)__builtin_ctz(ctx.fp_hits[1]);           \
            u32 nidx = ctx.bk[1]->idx[bit];                                   \
            if (RIX_LIKELY(nidx != (u32)RIX_NIL))                             \
                rix_hash_prefetch_entry_of(                                   \
                    FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, nidx));               \
        }                                                                     \
        if (RIX_UNLIKELY(ctx.fp_hits[0] != 0u)) {                             \
            u32 hits = ctx.fp_hits[0];                                        \
            while (RIX_UNLIKELY(hits != 0u)) {                                \
                unsigned bit = (unsigned)__builtin_ctz(hits);                 \
                u32 nidx = ctx.bk[0]->idx[bit];                               \
                _FCORE_EXTRA_ENTRY_T(p) *node;                                \
                hits &= hits - 1u;                                            \
                if (RIX_UNLIKELY(nidx == (u32)RIX_NIL))                       \
                    continue;                                                 \
                node = FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, nidx);             \
                RIX_ASSUME_NONNULL(node);                                     \
                if (RIX_UNLIKELY(cmp_fn(&entry->key, &node->key) == 0)) {     \
                    FLOW_STATS(owner).add_existing++;                         \
                    if ((unsigned)policy & 1u) {                              \
                        ctx.bk[0]->idx[bit] = eidx;                           \
                        ctx.bk[0]->extra[bit] =                               \
                            flow_timestamp_encode(                            \
                                (now),                                        \
                                FCORE_EXTRA_TIMESTAMP_SHIFT(owner));          \
                        entry->meta.cur_hash = ctx.hash.val32[0];             \
                        entry->meta.slot =                                    \
                            (__typeof__(entry->meta.slot))bit;                \
                        FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);        \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(node);                    \
                        unused_idxv[free_count++] = nidx;                     \
                    } else {                                                  \
                        /* touch: update bk->extra[slot] for existing node */ \
                        if ((now) != 0u)                                      \
                            ctx.bk[0]->extra[bit] =                           \
                                flow_timestamp_encode(                        \
                                    (now),                                    \
                                    FCORE_EXTRA_TIMESTAMP_SHIFT(owner));      \
                        FCORE_EXTRA_TOUCH_TIMESTAMP(owner, node, now);        \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                   \
                        entry_idxv[idx] = nidx;                               \
                        unused_idxv[free_count++] = eidx;                     \
                    }                                                         \
                    goto _fcore_extra_add_small_next_;                        \
                }                                                             \
            }                                                                 \
        }                                                                     \
        if (RIX_UNLIKELY(ctx.fp_hits[1] != 0u)) {                             \
            u32 hits = ctx.fp_hits[1];                                        \
            while (RIX_UNLIKELY(hits != 0u)) {                                \
                unsigned bit = (unsigned)__builtin_ctz(hits);                 \
                u32 nidx = ctx.bk[1]->idx[bit];                               \
                _FCORE_EXTRA_ENTRY_T(p) *node;                                \
                hits &= hits - 1u;                                            \
                if (RIX_UNLIKELY(nidx == (u32)RIX_NIL))                       \
                    continue;                                                 \
                node = FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, nidx);             \
                RIX_ASSUME_NONNULL(node);                                     \
                if (RIX_UNLIKELY(cmp_fn(&entry->key, &node->key) == 0)) {     \
                    FLOW_STATS(owner).add_existing++;                         \
                    if ((unsigned)policy & 1u) {                              \
                        ctx.bk[1]->idx[bit] = eidx;                           \
                        ctx.bk[1]->extra[bit] =                               \
                            flow_timestamp_encode(                            \
                                (now),                                        \
                                FCORE_EXTRA_TIMESTAMP_SHIFT(owner));          \
                        entry->meta.cur_hash = ctx.hash.val32[1];             \
                        entry->meta.slot =                                    \
                            (__typeof__(entry->meta.slot))bit;                \
                        FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);        \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(node);                    \
                        unused_idxv[free_count++] = nidx;                     \
                    } else {                                                  \
                        /* touch: update bk->extra[slot] for existing node */ \
                        if ((now) != 0u)                                      \
                            ctx.bk[1]->extra[bit] =                           \
                                flow_timestamp_encode(                        \
                                    (now),                                    \
                                    FCORE_EXTRA_TIMESTAMP_SHIFT(owner));      \
                        FCORE_EXTRA_TOUCH_TIMESTAMP(owner, node, now);        \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                   \
                        entry_idxv[idx] = nidx;                               \
                        unused_idxv[free_count++] = eidx;                     \
                    }                                                         \
                    goto _fcore_extra_add_small_next_;                        \
                }                                                             \
            }                                                                 \
        }                                                                     \
        if (RIX_LIKELY(ctx.empties[0] != 0u)) {                               \
            unsigned slot = (unsigned)__builtin_ctz(ctx.empties[0]);          \
            ctx.bk[0]->hash[slot] = ctx.fp;                                   \
            ctx.bk[0]->idx[slot] = eidx;                                      \
            ctx.bk[0]->extra[slot] =                                          \
                flow_timestamp_encode((now),                                  \
                                      FCORE_EXTRA_TIMESTAMP_SHIFT(owner));    \
            entry->meta.cur_hash = ctx.hash.val32[0];                         \
            entry->meta.slot = (__typeof__(entry->meta.slot))slot;            \
            FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);                    \
            head->rhh_nb++;                                                   \
            FLOW_STATS(owner).adds++;                                         \
            FLOW_STATUS(owner).entries++;                                     \
            FLOW_STATUS(owner).add_bk0++;                                     \
            continue;                                                         \
        }                                                                     \
        if (RIX_LIKELY(ctx.empties[1] != 0u)) {                               \
            unsigned slot = (unsigned)__builtin_ctz(ctx.empties[1]);          \
            ctx.bk[1]->hash[slot] = ctx.fp;                                   \
            ctx.bk[1]->idx[slot] = eidx;                                      \
            ctx.bk[1]->extra[slot] =                                          \
                flow_timestamp_encode((now),                                  \
                                      FCORE_EXTRA_TIMESTAMP_SHIFT(owner));    \
            entry->meta.cur_hash = ctx.hash.val32[1];                         \
            entry->meta.slot = (__typeof__(entry->meta.slot))slot;            \
            FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);                    \
            head->rhh_nb++;                                                   \
            FLOW_STATS(owner).adds++;                                         \
            FLOW_STATUS(owner).entries++;                                     \
            FLOW_STATUS(owner).add_bk1++;                                     \
            continue;                                                         \
        }                                                                     \
        ret_idx = _FCORE_EXTRA_HT(ht, insert_hashed_idx)(                    \
            head, buckets, hash_base, entry, ctx.hash,                        \
            flow_timestamp_encode((now), FCORE_EXTRA_TIMESTAMP_SHIFT(owner)));\
        if (RIX_LIKELY(ret_idx == 0u)) {                                      \
            FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);                    \
            FLOW_STATS(owner).adds++;                                         \
            FLOW_STATUS(owner).entries++;                                     \
            FLOW_STATUS(owner).kickouts++;                                    \
            if (entry->meta.cur_hash == ctx.hash.val32[1])                    \
                FLOW_STATUS(owner).add_bk1++;                                 \
            else                                                              \
                FLOW_STATUS(owner).add_bk0++;                                 \
        } else if (RIX_UNLIKELY(ret_idx != eidx)) {                           \
            FLOW_STATS(owner).add_existing++;                                 \
            if ((unsigned)policy & 1u) {                                      \
                _FCORE_EXTRA_ENTRY_T(p) *node =                               \
                    FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, ret_idx);             \
                u32 ins_idx;                                                  \
                RIX_ASSUME_NONNULL(node);                                     \
                ins_idx = ret_idx;                                            \
                RIX_ASSERT(_FCORE_EXTRA_HT(ht, remove)(                      \
                               head, buckets, hash_base, node)                \
                           != NULL);                                          \
                FCORE_EXTRA_CLEAR_TIMESTAMP(node);                            \
                ins_idx = _FCORE_EXTRA_HT(ht, insert_hashed_idx)(            \
                    head, buckets, hash_base, entry, ctx.hash,                \
                    flow_timestamp_encode((now),                               \
                                         FCORE_EXTRA_TIMESTAMP_SHIFT(owner)));\
                RIX_ASSERT(ins_idx == 0u);                                    \
                FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);                \
                unused_idxv[free_count++] = ret_idx;                          \
            } else {                                                          \
                _FCORE_EXTRA_ENTRY_T(p) *node =                               \
                    FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, ret_idx);             \
                RIX_ASSUME_NONNULL(node);                                     \
                /* touch existing node: find its bk and update extra[slot] */ \
                {                                                             \
                    unsigned _slot = node->meta.slot;                         \
                    unsigned _bki = node->meta.cur_hash & head->rhh_mask;     \
                    struct rix_hash_bucket_extra_s *_bk =                     \
                        buckets + _bki;                                       \
                    if ((now) != 0u)                                          \
                        _bk->extra[_slot] =                                   \
                            flow_timestamp_encode(                            \
                                (now),                                        \
                                FCORE_EXTRA_TIMESTAMP_SHIFT(owner));          \
                }                                                             \
                FCORE_EXTRA_TOUCH_TIMESTAMP(owner, node, now);                \
                FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                           \
                entry_idxv[idx] = ret_idx;                                    \
                unused_idxv[free_count++] = eidx;                             \
            }                                                                 \
        } else if (RIX_LIKELY(!((unsigned)policy & 2u))) {                    \
            FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                               \
            FLOW_STATS(owner).add_failed++;                                   \
            entry_idxv[idx] = 0u;                                             \
            unused_idxv[free_count++] = eidx;                                 \
        } else {                                                              \
            u64 now_enc = flow_timestamp_encode(                              \
                now, FCORE_EXTRA_TIMESTAMP_SHIFT(owner));                     \
            u64 max_elapsed = 0u;                                             \
            unsigned victim_bki = 0u;                                         \
            unsigned victim_slot = 0u;                                        \
            u32 victim_idx = 0u;                                              \
            for (unsigned bki = 0u; bki < 2u; bki++) {                        \
                struct rix_hash_bucket_extra_s *vbk = ctx.bk[bki];            \
                for (unsigned s = 0u; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {   \
                    u32 nidx = vbk->idx[s];                                   \
                    u64 ts, elapsed;                                          \
                    if (nidx == (u32)RIX_NIL)                                 \
                        continue;                                             \
                    ts = (u64)vbk->extra[s];                                  \
                    if (ts == 0u)                                             \
                        continue;                                             \
                    elapsed = flow_timestamp_elapsed(now_enc, ts);            \
                    if (elapsed >= max_elapsed) {                             \
                        max_elapsed = elapsed;                                \
                        victim_bki = bki;                                     \
                        victim_slot = s;                                      \
                        victim_idx = nidx;                                    \
                    }                                                         \
                }                                                             \
            }                                                                 \
            if (RIX_LIKELY(victim_idx != 0u)) {                               \
                _FCORE_EXTRA_ENTRY_T(p) *victim =                             \
                    FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, victim_idx);          \
                RIX_ASSUME_NONNULL(victim);                                   \
                ctx.bk[victim_bki]->hash[victim_slot] = ctx.fp;               \
                ctx.bk[victim_bki]->idx[victim_slot] = eidx;                  \
                ctx.bk[victim_bki]->extra[victim_slot] =                      \
                    flow_timestamp_encode((now),                               \
                                         FCORE_EXTRA_TIMESTAMP_SHIFT(owner)); \
                entry->meta.cur_hash = ctx.hash.val32[victim_bki];            \
                entry->meta.slot =                                            \
                    (__typeof__(entry->meta.slot))victim_slot;                \
                FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);                \
                FCORE_EXTRA_CLEAR_TIMESTAMP(victim);                          \
                unused_idxv[free_count++] = victim_idx;                       \
                FLOW_STATS(owner).force_expired++;                             \
            } else {                                                          \
                FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                           \
                FLOW_STATS(owner).add_failed++;                               \
                entry_idxv[idx] = 0u;                                         \
                unused_idxv[free_count++] = eidx;                             \
            }                                                                 \
        }                                                                     \
_fcore_extra_add_small_next_: ;                                               \
    }                                                                         \
    return free_count;                                                        \
}                                                                             \
                                                                              \
static RIX_UNUSED unsigned                                                    \
_FCORE_EXTRA_INT(p, add_idx_bulk_)(_FCORE_EXTRA_OT(ot) *owner,               \
                                   u32 *entry_idxv,                           \
                                   unsigned nb_keys,                          \
                                   enum ft_add_policy policy,                 \
                                   u64 now,                                   \
                                   u32 *unused_idxv)                          \
{                                                                             \
    enum { _FCORE_EXTRA_ADD_CTX_COUNT = 64u };                                \
    _FCORE_EXTRA_HT_T(ht) *head = _FCORE_EXTRA_HT_HEAD(ht, owner);           \
    struct rix_hash_bucket_extra_s *buckets = owner->buckets;                 \
    _FCORE_EXTRA_ENTRY_T(p) *hash_base =                                      \
        FCORE_EXTRA_LAYOUT_HASH_BASE(owner);                                  \
    const u32 hash_mask = FCORE_EXTRA_HASH_MASK(owner, ht);                  \
    const unsigned ctx_mask = _FCORE_EXTRA_ADD_CTX_COUNT - 1u;               \
    unsigned step;                                                            \
    unsigned ahead;                                                           \
    unsigned total;                                                           \
    struct rix_hash_find_ctx_extra_s ctx[_FCORE_EXTRA_ADD_CTX_COUNT];         \
    unsigned free_count = 0u;                                                 \
    RIX_ASSERT(unused_idxv != NULL);                                          \
    if (nb_keys < 4u)                                                         \
        return _FCORE_EXTRA_INT(p, add_idx_small_)(owner, entry_idxv,        \
                                                   nb_keys, policy, now,     \
                                                   unused_idxv);             \
    if (nb_keys < 8u) {                                                       \
        step = 2u;                                                            \
        ahead = 2u;                                                           \
    } else if (nb_keys < 32u) {                                               \
        step = 4u;                                                            \
        ahead = 4u;                                                           \
    } else if (nb_keys < 64u) {                                               \
        step = 8u;                                                            \
        ahead = 8u;                                                           \
    } else if (nb_keys < 128u) {                                              \
        step = 8u;                                                            \
        ahead = 16u;                                                          \
    } else {                                                                  \
        step = 8u;                                                            \
        ahead = 16u;                                                          \
    }                                                                         \
    total = nb_keys + 3u * ahead;                                             \
                                                                              \
    for (unsigned i = 0; i < total; i += step) {                              \
        /* Stage 1: prefetch entry node */                                    \
        if (i < nb_keys) {                                                    \
            unsigned n = (i + step <= nb_keys) ? step : (nb_keys - i);        \
            for (unsigned j = 0; j < n; j++) {                                \
                u32 eidx = entry_idxv[i + j];                                 \
                _FCORE_EXTRA_ENTRY_T(p) *pentry =                             \
                    FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, eidx);                \
                RIX_ASSUME_NONNULL(pentry);                                   \
                rix_hash_prefetch_key(&pentry->key);                          \
            }                                                                 \
        }                                                                     \
        /* Stage 2: read entry key, hash, prefetch buckets */                 \
        if (i >= ahead && i - ahead < nb_keys) {                              \
            unsigned base = i - ahead;                                        \
            unsigned n = (base + step <= nb_keys) ? step                      \
                                                  : (nb_keys - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                unsigned idx = base + j;                                      \
                u32 eidx = entry_idxv[idx];                                   \
                struct rix_hash_find_ctx_extra_s *ctxp =                      \
                    &ctx[idx & ctx_mask];                                     \
                _FCORE_EXTRA_ENTRY_T(p) *entry;                               \
                unsigned bk0, bk1;                                            \
                RIX_ASSERT(eidx <= owner->max_entries);                       \
                entry = FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, eidx);            \
                RIX_ASSUME_NONNULL(entry);                                    \
                ctxp->hash = hash_fn(&entry->key, hash_mask);                 \
                ctxp->fp = rix_hash_fp(ctxp->hash, head->rhh_mask,            \
                                       &bk0, &bk1);                           \
                ctxp->key = (const void *)entry;                              \
                ctxp->bk[0] = buckets + bk0;                                  \
                ctxp->bk[1] = buckets + bk1;                                  \
                rix_hash_prefetch_extra_bucket_of(ctxp->bk[0]);               \
                rix_hash_prefetch_extra_bucket_of(ctxp->bk[1]);               \
            }                                                                 \
        }                                                                     \
        /* Stage 3: speculative fp scan of bk[0], prefetch first match */    \
        if (i >= 2u * ahead && i - 2u * ahead < nb_keys) {                   \
            unsigned base = i - 2u * ahead;                                   \
            unsigned n = (base + step <= nb_keys) ? step                      \
                                                  : (nb_keys - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                unsigned idx = base + j;                                      \
                struct rix_hash_find_ctx_extra_s *ctxp =                      \
                    &ctx[idx & ctx_mask];                                     \
                u32 h0 = RIX_HASH_FIND_U32X16(ctxp->bk[0]->hash, ctxp->fp);  \
                if (h0) {                                                     \
                    unsigned bit = (unsigned)__builtin_ctz(h0);               \
                    u32 nidx = ctxp->bk[0]->idx[bit];                         \
                    if (nidx != (u32)RIX_NIL)                                 \
                        rix_hash_prefetch_entry_of(                           \
                            FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, nidx));       \
                }                                                             \
            }                                                                 \
        }                                                                     \
        /* Stage 4: duplicate check + insert */                               \
        if (i >= 3u * ahead && i - 3u * ahead < nb_keys) {                   \
            unsigned base = i - 3u * ahead;                                   \
            unsigned n = (base + step <= nb_keys) ? step                      \
                                                  : (nb_keys - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                unsigned idx = base + j;                                      \
                u32 eidx = entry_idxv[idx];                                   \
                struct rix_hash_find_ctx_extra_s *ctxp =                      \
                    &ctx[idx & ctx_mask];                                     \
                _FCORE_EXTRA_ENTRY_T(p) *entry =                              \
                    (_FCORE_EXTRA_ENTRY_T(p) *)(uintptr_t)ctxp->key;         \
                u32 ret_idx = (u32)RIX_NIL;                                   \
                RIX_ASSERT(eidx <= owner->max_entries);                       \
                RIX_ASSUME_NONNULL(entry);                                    \
                _FCORE_EXTRA_HT(ht, scan_bk_empties)(ctxp, 0u);              \
                _FCORE_EXTRA_HT(ht, scan_bk_empties)(ctxp, 1u);              \
                if (RIX_UNLIKELY(ctxp->fp_hits[0] != 0u)) {                   \
                    u32 hits = ctxp->fp_hits[0];                              \
                    while (RIX_UNLIKELY(hits != 0u)) {                        \
                        unsigned bit = (unsigned)__builtin_ctz(hits);         \
                        u32 nidx = ctxp->bk[0]->idx[bit];                     \
                        _FCORE_EXTRA_ENTRY_T(p) *node;                        \
                        hits &= hits - 1u;                                    \
                        if (RIX_UNLIKELY(nidx == (u32)RIX_NIL))               \
                            continue;                                         \
                        node = FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, nidx);     \
                        RIX_ASSUME_NONNULL(node);                             \
                        if (RIX_UNLIKELY(cmp_fn(&entry->key, &node->key)      \
                                         == 0)) {                             \
                            FLOW_STATS(owner).add_existing++;                 \
                            if ((unsigned)policy & 1u) {                      \
                                ctxp->bk[0]->idx[bit] = eidx;                 \
                                ctxp->bk[0]->extra[bit] =                     \
                                    flow_timestamp_encode(                    \
                                        (now),                                \
                                        FCORE_EXTRA_TIMESTAMP_SHIFT(owner));  \
                                entry->meta.cur_hash =                        \
                                    ctxp->hash.val32[0];                      \
                                entry->meta.slot =                            \
                                    (__typeof__(entry->meta.slot))bit;        \
                                FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);\
                                FCORE_EXTRA_CLEAR_TIMESTAMP(node);            \
                                unused_idxv[free_count++] = nidx;             \
                            } else {                                          \
                                /* touch existing via bk[0] */                \
                                if ((now) != 0u)                              \
                                    ctxp->bk[0]->extra[bit] =                 \
                                        flow_timestamp_encode(                \
                                            (now),                            \
                                            FCORE_EXTRA_TIMESTAMP_SHIFT(      \
                                                owner));                      \
                                FCORE_EXTRA_TOUCH_TIMESTAMP(owner, node, now);\
                                FCORE_EXTRA_CLEAR_TIMESTAMP(entry);           \
                                entry_idxv[idx] = nidx;                       \
                                unused_idxv[free_count++] = eidx;             \
                            }                                                 \
                            goto _fcore_extra_add_idx_next_;                  \
                        }                                                     \
                    }                                                         \
                }                                                             \
                if (RIX_UNLIKELY(ctxp->fp_hits[1] != 0u)) {                   \
                    u32 hits = ctxp->fp_hits[1];                              \
                    while (RIX_UNLIKELY(hits != 0u)) {                        \
                        unsigned bit = (unsigned)__builtin_ctz(hits);         \
                        u32 nidx = ctxp->bk[1]->idx[bit];                     \
                        _FCORE_EXTRA_ENTRY_T(p) *node;                        \
                        hits &= hits - 1u;                                    \
                        if (RIX_UNLIKELY(nidx == (u32)RIX_NIL))               \
                            continue;                                         \
                        node = FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, nidx);     \
                        RIX_ASSUME_NONNULL(node);                             \
                        if (RIX_UNLIKELY(cmp_fn(&entry->key, &node->key)      \
                                         == 0)) {                             \
                            FLOW_STATS(owner).add_existing++;                 \
                            if ((unsigned)policy & 1u) {                      \
                                ctxp->bk[1]->idx[bit] = eidx;                 \
                                ctxp->bk[1]->extra[bit] =                     \
                                    flow_timestamp_encode(                    \
                                        (now),                                \
                                        FCORE_EXTRA_TIMESTAMP_SHIFT(owner));  \
                                entry->meta.cur_hash =                        \
                                    ctxp->hash.val32[1];                      \
                                entry->meta.slot =                            \
                                    (__typeof__(entry->meta.slot))bit;        \
                                FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);\
                                FCORE_EXTRA_CLEAR_TIMESTAMP(node);            \
                                unused_idxv[free_count++] = nidx;             \
                            } else {                                          \
                                /* touch existing via bk[1] */                \
                                if ((now) != 0u)                              \
                                    ctxp->bk[1]->extra[bit] =                 \
                                        flow_timestamp_encode(                \
                                            (now),                            \
                                            FCORE_EXTRA_TIMESTAMP_SHIFT(      \
                                                owner));                      \
                                FCORE_EXTRA_TOUCH_TIMESTAMP(owner, node, now);\
                                FCORE_EXTRA_CLEAR_TIMESTAMP(entry);           \
                                entry_idxv[idx] = nidx;                       \
                                unused_idxv[free_count++] = eidx;             \
                            }                                                 \
                            goto _fcore_extra_add_idx_next_;                  \
                        }                                                     \
                    }                                                         \
                }                                                             \
                if (RIX_LIKELY(ctxp->empties[0] != 0u)) {                     \
                    unsigned slot =                                           \
                        (unsigned)__builtin_ctz(ctxp->empties[0]);            \
                    ctxp->bk[0]->hash[slot] = ctxp->fp;                       \
                    ctxp->bk[0]->idx[slot] = eidx;                            \
                    ctxp->bk[0]->extra[slot] =                                \
                        flow_timestamp_encode((now),                           \
                                              FCORE_EXTRA_TIMESTAMP_SHIFT(    \
                                                  owner));                    \
                    entry->meta.cur_hash = ctxp->hash.val32[0];               \
                    entry->meta.slot = (__typeof__(entry->meta.slot))         \
                        slot;                                                 \
                    FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);            \
                    head->rhh_nb++;                                           \
                    FLOW_STATS(owner).adds++;                                 \
                    FLOW_STATUS(owner).entries++;                             \
                    FLOW_STATUS(owner).add_bk0++;                             \
                    continue;                                                  \
                }                                                             \
                if (RIX_LIKELY(ctxp->empties[1] != 0u)) {                     \
                    unsigned slot =                                           \
                        (unsigned)__builtin_ctz(ctxp->empties[1]);            \
                    ctxp->bk[1]->hash[slot] = ctxp->fp;                       \
                    ctxp->bk[1]->idx[slot] = eidx;                            \
                    ctxp->bk[1]->extra[slot] =                                \
                        flow_timestamp_encode((now),                           \
                                              FCORE_EXTRA_TIMESTAMP_SHIFT(    \
                                                  owner));                    \
                    entry->meta.cur_hash = ctxp->hash.val32[1];               \
                    entry->meta.slot = (__typeof__(entry->meta.slot))         \
                        slot;                                                 \
                    FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);            \
                    head->rhh_nb++;                                           \
                    FLOW_STATS(owner).adds++;                                 \
                    FLOW_STATUS(owner).entries++;                             \
                    FLOW_STATUS(owner).add_bk1++;                             \
                    continue;                                                  \
                }                                                             \
                ret_idx = _FCORE_EXTRA_HT(ht, insert_hashed_idx)(            \
                    head, buckets, hash_base, entry, ctxp->hash,              \
                    flow_timestamp_encode((now),                               \
                                         FCORE_EXTRA_TIMESTAMP_SHIFT(owner)));\
                if (RIX_LIKELY(ret_idx == 0u)) {                              \
                    FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);            \
                    FLOW_STATS(owner).adds++;                                 \
                    FLOW_STATUS(owner).entries++;                             \
                    FLOW_STATUS(owner).kickouts++;                            \
                    if (entry->meta.cur_hash == ctxp->hash.val32[1])          \
                        FLOW_STATUS(owner).add_bk1++;                         \
                    else                                                      \
                        FLOW_STATUS(owner).add_bk0++;                         \
                } else if (RIX_UNLIKELY(ret_idx != eidx)) {                   \
                    FLOW_STATS(owner).add_existing++;                         \
                    if ((unsigned)policy & 1u) {                              \
                        _FCORE_EXTRA_ENTRY_T(p) *node =                       \
                            FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, ret_idx);     \
                        u32 ins_idx;                                          \
                        RIX_ASSUME_NONNULL(node);                             \
                        ins_idx = ret_idx;                                    \
                        RIX_ASSERT(_FCORE_EXTRA_HT(ht, remove)(              \
                                       head, buckets, hash_base, node)        \
                                   != NULL);                                  \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(node);                    \
                        ins_idx = _FCORE_EXTRA_HT(ht, insert_hashed_idx)(    \
                            head, buckets, hash_base, entry, ctxp->hash,      \
                            flow_timestamp_encode(                            \
                                (now),                                        \
                                FCORE_EXTRA_TIMESTAMP_SHIFT(owner)));         \
                        RIX_ASSERT(ins_idx == 0u);                            \
                        FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);        \
                        unused_idxv[free_count++] = ret_idx;                  \
                    } else {                                                  \
                        _FCORE_EXTRA_ENTRY_T(p) *node =                       \
                            FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, ret_idx);     \
                        RIX_ASSUME_NONNULL(node);                             \
                        /* touch existing node via its registered bk */       \
                        {                                                     \
                            unsigned _slot = node->meta.slot;                 \
                            unsigned _bki =                                   \
                                node->meta.cur_hash & head->rhh_mask;         \
                            struct rix_hash_bucket_extra_s *_bk =             \
                                buckets + _bki;                               \
                            if ((now) != 0u)                                  \
                                _bk->extra[_slot] =                           \
                                    flow_timestamp_encode(                    \
                                        (now),                                \
                                        FCORE_EXTRA_TIMESTAMP_SHIFT(owner));  \
                        }                                                     \
                        FCORE_EXTRA_TOUCH_TIMESTAMP(owner, node, now);        \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                   \
                        entry_idxv[idx] = ret_idx;                            \
                        unused_idxv[free_count++] = eidx;                     \
                    }                                                         \
                } else if (RIX_LIKELY(!((unsigned)policy & 2u))) {            \
                    FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                       \
                    FLOW_STATS(owner).add_failed++;                           \
                    entry_idxv[idx] = 0u;                                     \
                    unused_idxv[free_count++] = eidx;                         \
                } else {                                                      \
                    u64 now_enc = flow_timestamp_encode(                      \
                        now, FCORE_EXTRA_TIMESTAMP_SHIFT(owner));             \
                    u64 max_elapsed = 0u;                                     \
                    unsigned victim_bki = 0u;                                 \
                    unsigned victim_slot = 0u;                                \
                    u32 victim_idx = 0u;                                      \
                    for (unsigned bki = 0u; bki < 2u; bki++) {                \
                        struct rix_hash_bucket_extra_s *vbk = ctxp->bk[bki]; \
                        for (unsigned s = 0u; s < RIX_HASH_BUCKET_ENTRY_SZ;  \
                             s++) {                                           \
                            u32 nidx = vbk->idx[s];                           \
                            u64 ts, elapsed;                                  \
                            if (nidx == (u32)RIX_NIL)                         \
                                continue;                                     \
                            ts = (u64)vbk->extra[s];                          \
                            if (ts == 0u)                                     \
                                continue;                                     \
                            elapsed = flow_timestamp_elapsed(now_enc, ts);    \
                            if (elapsed >= max_elapsed) {                     \
                                max_elapsed = elapsed;                        \
                                victim_bki = bki;                             \
                                victim_slot = s;                              \
                                victim_idx = nidx;                            \
                            }                                                 \
                        }                                                     \
                    }                                                         \
                    if (RIX_LIKELY(victim_idx != 0u)) {                       \
                        _FCORE_EXTRA_ENTRY_T(p) *victim =                     \
                            FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, victim_idx);  \
                        RIX_ASSUME_NONNULL(victim);                           \
                        ctxp->bk[victim_bki]->hash[victim_slot] =             \
                            ctxp->fp;                                         \
                        ctxp->bk[victim_bki]->idx[victim_slot] =              \
                            eidx;                                             \
                        ctxp->bk[victim_bki]->extra[victim_slot] =            \
                            flow_timestamp_encode(                            \
                                (now),                                        \
                                FCORE_EXTRA_TIMESTAMP_SHIFT(owner));          \
                        entry->meta.cur_hash =                                \
                            ctxp->hash.val32[victim_bki];                     \
                        entry->meta.slot =                                    \
                            (__typeof__(entry->meta.slot))victim_slot;        \
                        FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now);        \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(victim);                  \
                        unused_idxv[free_count++] = victim_idx;               \
                        FLOW_STATS(owner).force_expired++;                    \
                    } else {                                                  \
                        FCORE_EXTRA_CLEAR_TIMESTAMP(entry);                   \
                        FLOW_STATS(owner).add_failed++;                       \
                        entry_idxv[idx] = 0u;                                 \
                        unused_idxv[free_count++] = eidx;                     \
                    }                                                         \
                }                                                             \
_fcore_extra_add_idx_next_: ;                                                 \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    return free_count;                                                        \
}                                                                             \
                                                                              \
/*=== del_idx_bulk (3-stage query-aware pipeline) ==========================*/\
                                                                              \
static RIX_UNUSED unsigned                                                    \
_FCORE_EXTRA_INT(p, del_idx_bulk_)(_FCORE_EXTRA_OT(ot) *owner,               \
                                   const u32 *idxs,                           \
                                   unsigned nb_idxs,                          \
                                   u32 *unused_idxv)                          \
{                                                                             \
    _FCORE_EXTRA_HT_T(ht) *head = _FCORE_EXTRA_HT_HEAD(ht, owner);           \
    struct rix_hash_bucket_extra_s *buckets = owner->buckets;                 \
    enum { _FCORE_EXTRA_DEL_IDX_CTX_COUNT = 32u };                            \
    unsigned count = 0u;                                                      \
    const unsigned ctx_mask = _FCORE_EXTRA_DEL_IDX_CTX_COUNT - 1u;           \
    unsigned ahead;                                                           \
    unsigned step;                                                            \
    unsigned total;                                                           \
    struct { u32 eidx; unsigned bk; unsigned slot; }                          \
        dctx[_FCORE_EXTRA_DEL_IDX_CTX_COUNT];                                \
    if (nb_idxs < 4u) {                                                       \
        step = 1u;                                                            \
        ahead = 1u;                                                           \
    } else if (nb_idxs < 8u) {                                                \
        step = 2u;                                                            \
        ahead = 2u;                                                           \
    } else if (nb_idxs < 32u) {                                               \
        step = 4u;                                                            \
        ahead = 4u;                                                           \
    } else if (nb_idxs < 128u) {                                              \
        step = 8u;                                                            \
        ahead = 8u;                                                           \
    } else {                                                                  \
        step = 8u;                                                            \
        ahead = 16u;                                                          \
    }                                                                         \
    total = nb_idxs + 2u * ahead;                                             \
                                                                              \
    for (unsigned i = 0; i < total; i += step) {                              \
        /* Stage 1: prefetch entry node */                                    \
        if (i < nb_idxs) {                                                    \
            unsigned n = (i + step <= nb_idxs) ? step : (nb_idxs - i);       \
            for (unsigned j = 0; j < n; j++) {                                \
                u32 eidx = idxs[i + j];                                       \
                if (RIX_LIKELY(eidx != 0u && eidx <= owner->max_entries))     \
                    rix_hash_prefetch_entry_of(                               \
                        FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, eidx));           \
            }                                                                 \
        }                                                                     \
        /* Stage 2: read entry meta, stash bk/slot, prefetch bucket */        \
        if (i >= ahead && i - ahead < nb_idxs) {                              \
            unsigned base = i - ahead;                                        \
            unsigned n = (base + step <= nb_idxs) ? step                      \
                                                  : (nb_idxs - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                unsigned idx = base + j;                                      \
                u32 eidx = idxs[idx];                                         \
                if (RIX_LIKELY(eidx != 0u && eidx <= owner->max_entries)) {   \
                    _FCORE_EXTRA_ENTRY_T(p) *entry =                          \
                        FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, eidx);            \
                    unsigned slot;                                            \
                    RIX_ASSUME_NONNULL(entry);                                \
                    slot = (unsigned)entry->meta.slot;                        \
                    if (RIX_LIKELY(slot < RIX_HASH_BUCKET_ENTRY_SZ)) {        \
                        unsigned bk =                                         \
                            (unsigned)(entry->meta.cur_hash & head->rhh_mask);\
                        dctx[idx & ctx_mask].eidx = eidx;                     \
                        dctx[idx & ctx_mask].bk   = bk;                       \
                        dctx[idx & ctx_mask].slot = slot;                     \
                        rix_hash_prefetch_extra_bucket_of(buckets + bk);      \
                    } else {                                                  \
                        dctx[idx & ctx_mask].eidx = 0u;                       \
                    }                                                         \
                } else {                                                      \
                    dctx[idx & ctx_mask].eidx = 0u;                           \
                }                                                             \
            }                                                                 \
        }                                                                     \
        /* Stage 3: remove_at using stashed bk/slot (no entry re-read) */    \
        if (i >= 2u * ahead && i - 2u * ahead < nb_idxs) {                   \
            unsigned base = i - 2u * ahead;                                   \
            unsigned n = (base + step <= nb_idxs) ? step                      \
                                                  : (nb_idxs - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                unsigned idx = base + j;                                      \
                u32 eidx = dctx[idx & ctx_mask].eidx;                         \
                if (RIX_LIKELY(eidx != 0u)) {                                 \
                    unsigned bk = dctx[idx & ctx_mask].bk;                    \
                    unsigned slot = dctx[idx & ctx_mask].slot;                \
                    struct rix_hash_bucket_extra_s *dbk = buckets + bk;       \
                    if (RIX_LIKELY(slot < RIX_HASH_BUCKET_ENTRY_SZ)           \
                        && dbk->idx[slot] == eidx                             \
                        && _FCORE_EXTRA_HT(ht, remove_at)(                    \
                               head, buckets, bk, slot) == eidx) {            \
                        FLOW_STATS(owner).dels++;                             \
                        FLOW_STATUS(owner).entries--;                         \
                        unused_idxv[count++] = eidx;                          \
                    }                                                         \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    return count;                                                             \
}                                                                             \
                                                                              \
/*=== del_key_bulk (3-stage query-aware pipeline: hash -> scan -> remove) ==*/\
                                                                              \
static RIX_UNUSED unsigned                                                    \
_FCORE_EXTRA_INT(p, del_key_bulk_)(_FCORE_EXTRA_OT(ot) *owner,               \
                                   const _FCORE_EXTRA_KEY_T(p) *keys,         \
                                   unsigned nb_keys,                          \
                                   u32 *unused_idxv)                          \
{                                                                             \
    enum { _FCORE_EXTRA_DEL_CTX_COUNT = 64u };                                \
    _FCORE_EXTRA_HT_T(ht) *head = _FCORE_EXTRA_HT_HEAD(ht, owner);           \
    struct rix_hash_bucket_extra_s *buckets = owner->buckets;                 \
    _FCORE_EXTRA_ENTRY_T(p) *hash_base =                                      \
        FCORE_EXTRA_LAYOUT_HASH_BASE(owner);                                  \
    const u32 hash_mask = FCORE_EXTRA_HASH_MASK(owner, ht);                  \
    struct rix_hash_find_ctx_extra_s ctx[_FCORE_EXTRA_DEL_CTX_COUNT];         \
    unsigned count = 0u;                                                      \
    u64 del_miss = 0u;                                                        \
    u64 dels = 0u;                                                            \
    const unsigned ctx_mask = _FCORE_EXTRA_DEL_CTX_COUNT - 1u;               \
    unsigned ahead;                                                           \
    unsigned step;                                                            \
    unsigned total;                                                           \
    if (nb_keys < 4u) {                                                       \
        step = 1u;                                                            \
        ahead = 1u;                                                           \
    } else if (nb_keys < 8u) {                                                \
        step = 2u;                                                            \
        ahead = 2u;                                                           \
    } else if (nb_keys < 32u) {                                               \
        step = 4u;                                                            \
        ahead = 4u;                                                           \
    } else if (nb_keys < 64u) {                                               \
        step = 8u;                                                            \
        ahead = 8u;                                                           \
    } else if (nb_keys < 128u) {                                              \
        step = 8u;                                                            \
        ahead = 16u;                                                          \
    } else {                                                                  \
        step = 8u;                                                            \
        ahead = 16u;                                                          \
    }                                                                         \
    total = nb_keys + 2u * ahead;                                             \
    for (unsigned i = 0; i < total; i += step) {                              \
        /* Stage 1: hash + prefetch buckets */                                \
        if (i < nb_keys) {                                                    \
            unsigned n = (i + step <= nb_keys) ? step : (nb_keys - i);        \
            for (unsigned j = 0; j < n; j++)                                  \
                _FCORE_EXTRA_HT(ht, hash_key_2bk_masked)(                    \
                    &ctx[(i + j) & ctx_mask], buckets,                        \
                    &keys[i + j], hash_mask, head->rhh_mask);                 \
        }                                                                     \
        /* Stage 2: scan bucket + prefetch entry node */                      \
        if (i >= ahead && i - ahead < nb_keys) {                              \
            unsigned base = i - ahead;                                        \
            unsigned n = (base + step <= nb_keys) ? step                      \
                                                  : (nb_keys - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                struct rix_hash_find_ctx_extra_s *ctxp =                      \
                    &ctx[(base + j) & ctx_mask];                              \
                _FCORE_EXTRA_HT(ht, scan_bk)(ctxp, head, buckets);           \
                _FCORE_EXTRA_HT(ht, prefetch_node)(ctxp, hash_base);         \
            }                                                                 \
        }                                                                     \
        /* Stage 3: compare key + remove on hit */                            \
        if (i >= 2u * ahead && i - 2u * ahead < nb_keys) {                   \
            unsigned base = i - 2u * ahead;                                   \
            unsigned n = (base + step <= nb_keys) ? step                      \
                                                  : (nb_keys - base);         \
            for (unsigned j = 0; j < n; j++) {                                \
                unsigned idx = base + j;                                      \
                _FCORE_EXTRA_ENTRY_T(p) *entry =                              \
                    _FCORE_EXTRA_HT(ht, cmp_key)(                             \
                        &ctx[idx & ctx_mask], hash_base);                     \
                if (RIX_LIKELY(entry != NULL)) {                              \
                    u32 eidx =                                                 \
                        FCORE_EXTRA_LAYOUT_ENTRY_INDEX(owner, entry);         \
                    if (_FCORE_EXTRA_HT(ht, remove)(                          \
                            head, buckets, hash_base, entry) != NULL) {       \
                        dels++;                                               \
                        RIX_ASSERT(FLOW_STATUS(owner).entries != 0u);         \
                        FLOW_STATUS(owner).entries--;                         \
                        unused_idxv[count++] = eidx;                          \
                    }                                                         \
                } else {                                                      \
                    del_miss++;                                               \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    FLOW_STATS(owner).dels += dels;                                           \
    FLOW_STATS(owner).del_miss += del_miss;                                   \
    return count;                                                             \
}                                                                             \
                                                                              \
/*=== walk =================================================================*/\
                                                                              \
static RIX_UNUSED int                                                         \
_FCORE_EXTRA_INT(p, walk_)(_FCORE_EXTRA_OT(ot) *owner,                       \
                            int (*cb)(u32 entry_idx, void *arg),              \
                            void *arg)                                        \
{                                                                             \
    unsigned nb_bk = owner->ht_head.rhh_mask + 1u;                            \
    for (unsigned b = 0; b < nb_bk; b++) {                                    \
        struct rix_hash_bucket_extra_s *bk = &owner->buckets[b];              \
        for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {             \
            unsigned nidx = bk->idx[s];                                       \
            if (nidx == (unsigned)RIX_NIL)                                    \
                continue;                                                     \
            int rc = cb((u32)nidx, arg);                                      \
            if (rc != 0)                                                      \
                return rc;                                                    \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
                                                                              \
/*=== flush (remove all entries, rebuild free list) ========================*/\
                                                                              \
static RIX_UNUSED void                                                        \
_FCORE_EXTRA_INT(p, flush_)(_FCORE_EXTRA_OT(ot) *owner)                      \
{                                                                             \
    unsigned nb_bk = owner->ht_head.rhh_mask + 1u;                            \
    /* Clear all buckets */                                                   \
    memset(owner->buckets, 0,                                                 \
           (size_t)nb_bk * sizeof(struct rix_hash_bucket_extra_s));           \
    /* Fill idx with RIX_NIL */                                               \
    for (unsigned b = 0; b < nb_bk; b++)                                      \
        for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++)               \
            owner->buckets[b].idx[s] = (u32)RIX_NIL;                          \
    owner->ht_head.rhh_nb = 0u;                                               \
    flow_status_reset(&FLOW_STATUS(owner), 0u);                               \
}                                                                             \
                                                                              \
/*=== add_idx_bulk_maint (add + inline registered-bucket expiry) ============*/\
/*                                                                           */\
/* Calls add_idx_bulk_, then scans the registered bucket of each result      */\
/* entry for expired entries.  Expired indices are appended to unused_idxv   */\
/* after the add-phase unused entries.                                       */\
/*                                                                           */\
/* Precondition: max_unused >= nb_keys.                                      */\
/* Returns: total entries written to unused_idxv (add unused + maint expired)*/\
                                                                              \
static RIX_UNUSED unsigned                                                    \
_FCORE_EXTRA_INT(p, add_idx_bulk_maint_)(_FCORE_EXTRA_OT(ot) *owner,         \
                                         u32 *entry_idxv,                    \
                                         unsigned nb_keys,                   \
                                         enum ft_add_policy policy,          \
                                         u64 now,                            \
                                         u64 timeout,                        \
                                         u32 *unused_idxv,                   \
                                         unsigned max_unused,                \
                                         unsigned min_bk_used)               \
{                                                                             \
    _FCORE_EXTRA_HT_T(ht) *head = _FCORE_EXTRA_HT_HEAD(ht, owner);           \
    struct rix_hash_bucket_extra_s *buckets = owner->buckets;                 \
    const u32 rhh_mask = head->rhh_mask;                                      \
    unsigned free_count;                                                      \
    u64 now_enc, timeout_enc;                                                 \
    enum { _MAINT_FILTER_SZ = 64u, _MAINT_FILTER_MASK = 63u };               \
    unsigned scanned[_MAINT_FILTER_SZ];                                       \
                                                                              \
    RIX_ASSERT(max_unused >= nb_keys);                                        \
                                                                              \
    /* Phase 1: normal add */                                                 \
    free_count = _FCORE_EXTRA_INT(p, add_idx_bulk_)(owner, entry_idxv,       \
                                                    nb_keys, policy, now,    \
                                                    unused_idxv);            \
                                                                              \
    /* Phase 2: inline maint on registered bucket of each result entry.     \
     * Scan budget = max_unused - nb_keys (= alpha).  The first nb_keys     \
     * slots are reserved for add-unused; maint only uses the extra slots.  \
     * When alpha == 0 (or timeout == 0), skip maint entirely.              \
     */                                                                       \
    {                                                                         \
        unsigned maint_budget = max_unused > nb_keys                          \
                              ? max_unused - nb_keys : 0u;                    \
        if (timeout == 0u || maint_budget == 0u || free_count >= max_unused)  \
            return free_count;                                                \
                                                                              \
        now_enc = flow_timestamp_encode(                                      \
            now, FCORE_EXTRA_TIMESTAMP_SHIFT(owner));                         \
        timeout_enc = flow_timestamp_timeout_encode(                           \
            timeout, FCORE_EXTRA_TIMESTAMP_SHIFT(owner));                     \
        memset(scanned, 0xff, sizeof(scanned));                               \
                                                                              \
        owner->stats.maint_calls++;                                           \
                                                                              \
        for (unsigned i = 0u;                                                 \
             i < nb_keys && maint_budget > 0u && free_count < max_unused;    \
             i++) {                                                           \
            u32 eidx = entry_idxv[i];                                         \
            _FCORE_EXTRA_ENTRY_T(p) *entry;                                   \
            unsigned bk_idx;                                                  \
            unsigned filter_slot;                                             \
            struct rix_hash_bucket_extra_s *bk;                               \
                                                                              \
            if (eidx == 0u)                                                   \
                continue;                                                     \
                                                                              \
            entry = FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, eidx);               \
            RIX_ASSUME_NONNULL(entry);                                        \
            bk_idx = entry->meta.cur_hash & rhh_mask;                         \
                                                                              \
            /* Duplicate bucket filter */                                     \
            filter_slot = bk_idx & _MAINT_FILTER_MASK;                        \
            if (scanned[filter_slot] == bk_idx)                               \
                continue;                                                     \
            scanned[filter_slot] = bk_idx;                                    \
                                                                              \
            maint_budget--;                                                   \
            owner->stats.maint_bucket_checks++;                               \
            bk = buckets + bk_idx;                                            \
                                                                              \
            /* Occupancy check: skip sparse buckets (cheap, bk already cached)*/\
            {                                                                 \
                unsigned occ = 0u;                                            \
                for (unsigned c = 0u; c < RIX_HASH_BUCKET_ENTRY_SZ; c++)     \
                    occ += (bk->idx[c] != (u32)RIX_NIL);                      \
                if (occ < min_bk_used)                                        \
                    continue;                                                 \
            }                                                                 \
                                                                              \
            for (unsigned s = 0u;                                             \
                 s < RIX_HASH_BUCKET_ENTRY_SZ && free_count < max_unused;    \
                 s++) {                                                       \
                u32 nidx = bk->idx[s];                                        \
                u64 ts;                                                       \
                                                                              \
                if (nidx == (u32)RIX_NIL)                                     \
                    continue;                                                 \
                /* In extra variant, timestamp is in bk->extra[s] */         \
                ts = (u64)bk->extra[s];                                       \
                if (!flow_timestamp_is_expired_raw(ts, now_enc, timeout_enc)) \
                    continue;                                                 \
                                                                              \
                /* Expire: remove from bucket */                              \
                bk->hash[s] = 0u;                                             \
                bk->idx[s] = (u32)RIX_NIL;                                    \
                bk->extra[s] = 0u;                                            \
                head->rhh_nb--;                                               \
                FLOW_STATUS(owner).entries--;                                 \
                unused_idxv[free_count++] = nidx;                             \
                owner->stats.maint_evictions++;                               \
            }                                                                 \
        }                                                                     \
    }  /* end Phase 2 scope */                                                \
                                                                              \
    return free_count;                                                        \
}                                                                             \
                                                                              \
/* end FCORE_EXTRA_GENERATE */

#endif /* _FLOW_CORE_EXTRA_H_ */
