/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * fc_cache_generate.h - GENERATE macro for per-variant cache implementations.
 *
 * Usage in .c files:
 *
 *   #include "flow4_cache.h"
 *   #include "fc_cache_generate.h"
 *
 *   static inline int
 *   fc_flow4_cmp(const struct flow4_key *a,
 *                 const struct flow4_key *b) { ... }
 *   static inline union rix_hash_hash_u
 *   fc_flow4_hash_fn(const struct flow4_key *key,
 *                     u32 mask) { ... }
 *
 *   FC_CACHE_GENERATE(flow4, FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
 *                      fc_flow4_hash_fn, fc_flow4_cmp)
 */

#ifndef _FC_CACHE_GENERATE_H_
#define _FC_CACHE_GENERATE_H_

/*===========================================================================
 * Parameterized token-paste helpers
 *===========================================================================*/
#define _FCG_CAT2(a, b)  a##b
#define _FCG_CAT(a, b)   _FCG_CAT2(a, b)

#define _FCG_HT(p, name)   _FCG_CAT(fc_, _FCG_CAT(p, _ht_##name))
#define _FCG_INT(p, name)  _FCG_CAT(fc_, _FCG_CAT(p, _##name))

/* Public API names: when FC_ARCH_SUFFIX is defined (e.g. -DFC_ARCH_SUFFIX=_avx2),
 * generated function names become fc_flow4_cache_findadd_bulk_avx2 etc.
 * Without FC_ARCH_SUFFIX, the original names are generated
 * (fc_flow4_cache_findadd_bulk, fc_flow4_cache_find_bulk, ...). */
#ifdef FC_ARCH_SUFFIX
#define _FCG_API(p, name)  _FCG_CAT(_FCG_CAT(fc_, _FCG_CAT(p, _cache_##name)), FC_ARCH_SUFFIX)
#else
#define _FCG_API(p, name)  _FCG_CAT(fc_, _FCG_CAT(p, _cache_##name))
#endif

#define _FCG_KEY_T(p)       struct _FCG_CAT(p, _key)
#define _FCG_RESULT_T(p)    struct _FCG_CAT(fc_, _FCG_CAT(p, _result))
#define _FCG_ENTRY_T(p)     struct _FCG_CAT(fc_, _FCG_CAT(p, _entry))
#define _FCG_CACHE_T(p)     struct _FCG_CAT(fc_, _FCG_CAT(p, _cache))
#define _FCG_CONFIG_T(p)    struct _FCG_CAT(fc_, _FCG_CAT(p, _config))
#define _FCG_STATS_T(p)     struct _FCG_CAT(fc_, _FCG_CAT(p, _stats))

#ifndef FCG_LAYOUT_INIT_STORAGE
#define FCG_LAYOUT_INIT_STORAGE(fc, array, stride, entry_offset)               \
    do {                                                                       \
        (void)(stride);                                                        \
        (void)(entry_offset);                                                  \
        (fc)->pool = (array);                                                  \
    } while (0)
#endif

#ifndef FCG_LAYOUT_HASH_BASE
#define FCG_LAYOUT_HASH_BASE(fc) ((fc)->pool)
#endif

#ifndef FCG_LAYOUT_ENTRY_PTR
#define FCG_LAYOUT_ENTRY_PTR(fc, idx)                                          \
    (((unsigned)(idx) != RIX_NIL) ? &((fc)->pool[(unsigned)(idx) - 1u]) : NULL)
#endif

#ifndef FCG_LAYOUT_ENTRY_INDEX
#define FCG_LAYOUT_ENTRY_INDEX(fc, entry) RIX_IDX_FROM_PTR((fc)->pool, (entry))
#endif

#ifndef FCG_LAYOUT_ENTRY_AT
#define FCG_LAYOUT_ENTRY_AT(fc, off0) (&((fc)->pool[(off0)]))
#endif

#ifndef FCG_LAYOUT_ENTRY_CLEAR
#define FCG_LAYOUT_ENTRY_CLEAR(fc, entry)                                      \
    memset((entry), 0, sizeof(*(entry)))
#endif

#ifndef FCG_FREE_LIST_INIT
#define FCG_FREE_LIST_INIT(fc)                                                 \
    RIX_SLIST_INIT(&(fc)->free_head)
#endif

#ifndef FCG_FREE_LIST_FIRST_IDX
#define FCG_FREE_LIST_FIRST_IDX(fc)                                            \
    ((fc)->free_head.rslh_first)
#endif

#ifndef FCG_FREE_LIST_FIRST_PTR
#define FCG_FREE_LIST_FIRST_PTR(fc)                                            \
    FCG_LAYOUT_ENTRY_PTR((fc), FCG_FREE_LIST_FIRST_IDX(fc))
#endif

#ifndef FCG_FREE_LIST_PUSH_ENTRY
#define FCG_FREE_LIST_PUSH_ENTRY(fc, entry, idx)                               \
    do {                                                                       \
        (entry)->free_link.rsle_next = FCG_FREE_LIST_FIRST_IDX(fc);            \
        (fc)->free_head.rslh_first = (idx);                                    \
    } while (0)
#endif

#ifndef FCG_FREE_LIST_POP_ENTRY
#define FCG_FREE_LIST_POP_ENTRY(fc, entry, idx)                                \
    do {                                                                       \
        (idx) = FCG_FREE_LIST_FIRST_IDX(fc);                                   \
        if ((idx) != RIX_NIL) {                                                \
            (entry) = FCG_LAYOUT_ENTRY_PTR((fc), (idx));                       \
            RIX_ASSUME_NONNULL(entry);                                         \
            (fc)->free_head.rslh_first = (entry)->free_link.rsle_next;         \
            (entry)->free_link.rsle_next = RIX_NIL;                            \
        } else {                                                               \
            (entry) = NULL;                                                    \
        }                                                                      \
    } while (0)
#endif

#ifndef FCG_EVENT_EMIT_ALLOC
#define FCG_EVENT_EMIT_ALLOC(fc, idx)                                          \
    do {                                                                       \
        (void)(fc);                                                            \
        (void)(idx);                                                           \
    } while (0)
#endif

#ifndef FCG_EVENT_EMIT_FREE
#define FCG_EVENT_EMIT_FREE(fc, idx, reason)                                   \
    do {                                                                       \
        (void)(fc);                                                            \
        (void)(idx);                                                           \
        (void)(reason);                                                        \
    } while (0)
#endif

#ifndef FCG_EVENT_REASON_DELETE
#define FCG_EVENT_REASON_DELETE 0u
#endif

#ifndef FCG_EVENT_REASON_TIMEOUT
#define FCG_EVENT_REASON_TIMEOUT 0u
#endif

#ifndef FCG_EVENT_REASON_PRESSURE
#define FCG_EVENT_REASON_PRESSURE 0u
#endif

#ifndef FCG_EVENT_REASON_OLDEST
#define FCG_EVENT_REASON_OLDEST 0u
#endif

#ifndef FCG_EVENT_REASON_FLUSH
#define FCG_EVENT_REASON_FLUSH 0u
#endif

#ifndef FCG_EVENT_REASON_ROLLBACK
#define FCG_EVENT_REASON_ROLLBACK 0u
#endif

/*===========================================================================
 * SIMD helper binding (file scope, applied to all GENERATE expansions)
 * Prefer AVX-512 helpers in AVX-512 tiers, otherwise fall back to AVX2.
 *===========================================================================*/
#if defined(__AVX512F__)
#undef RIX_HASH_FIND_U32X16
#undef RIX_HASH_FIND_U32X16_2
#define RIX_HASH_FIND_U32X16(arr, val) \
    rix_hash_find_u32x16_AVX512((arr), (val))
#define RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
    rix_hash_find_u32x16_2_AVX512((arr), (val0), (val1), (mask0), (mask1))
#elif defined(__AVX2__)
#undef RIX_HASH_FIND_U32X16
#undef RIX_HASH_FIND_U32X16_2
#define RIX_HASH_FIND_U32X16(arr, val) \
    rix_hash_find_u32x16_AVX2((arr), (val))
#define RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
    rix_hash_find_u32x16_2_AVX2((arr), (val0), (val1), (mask0), (mask1))
#endif

/*===========================================================================
 * Pipeline geometry defaults
 *===========================================================================*/
/* Pipeline geometry (single source of truth for all variants) */
#ifndef FLOW_CACHE_LOOKUP_STEP_KEYS
#define FLOW_CACHE_LOOKUP_STEP_KEYS   8u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_STEPS
#define FLOW_CACHE_LOOKUP_AHEAD_STEPS 4u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_KEYS
#define FLOW_CACHE_LOOKUP_AHEAD_KEYS \
    (FLOW_CACHE_LOOKUP_STEP_KEYS * FLOW_CACHE_LOOKUP_AHEAD_STEPS)
#endif

RIX_STATIC_ASSERT((FC_CACHE_BULK_CTX_COUNT &
                   (FC_CACHE_BULK_CTX_COUNT - 1u)) == 0u,
                  "FC_CACHE_BULK_CTX_COUNT must be a power of 2");
RIX_STATIC_ASSERT(FC_CACHE_BULK_CTX_COUNT >=
                  (3u * FLOW_CACHE_LOOKUP_AHEAD_KEYS +
                   FLOW_CACHE_LOOKUP_STEP_KEYS),
                  "FC_CACHE_BULK_CTX_COUNT too small for lookup pipeline");

#ifndef _FC_RELIEF_STAGE_SLOTS
#define _FC_RELIEF_STAGE_SLOTS 4u
#endif

#define _FCG_TS_SHIFT(fc) ((fc)->ts_shift)
#define _FCG_TS_NOW(fc, now) \
    flow_timestamp_encode((now), _FCG_TS_SHIFT(fc))
#define _FCG_TS_TIMEOUT(fc, timeout) \
    flow_timestamp_timeout_encode((timeout), _FCG_TS_SHIFT(fc))

/* Maximum buckets per maintain_step filter+reclaim pass. */
/* 256 entries * 4B = 1 KB - safe for any stack. */
#ifndef _FC_MAINT_STEP_MAX_BKS
#define _FC_MAINT_STEP_MAX_BKS 256u
#endif

#ifndef _FC_MAINT_STEP_PREFETCH_AHEAD
#define _FC_MAINT_STEP_PREFETCH_AHEAD 4u
#endif

RIX_STATIC_ASSERT(_FC_MAINT_STEP_MAX_BKS > 0u,
                  "_FC_MAINT_STEP_MAX_BKS must be non-zero");
RIX_STATIC_ASSERT(_FC_MAINT_STEP_PREFETCH_AHEAD > 0u,
                  "_FC_MAINT_STEP_PREFETCH_AHEAD must be non-zero");
RIX_STATIC_ASSERT(_FC_MAINT_STEP_MAX_BKS >= _FC_MAINT_STEP_PREFETCH_AHEAD,
                  "_FC_MAINT_STEP_MAX_BKS too small for maintain prefetch");

/*===========================================================================
 * Sub-macro 1: Hash-table GENERATE
 *===========================================================================*/
#define _FC_GENERATE_HT(p, hash_fn, cmp_fn)                               \
RIX_HASH_GENERATE_STATIC_SLOT_EX(                                         \
    _FCG_CAT(fc_, _FCG_CAT(p, _ht)),                                      \
    _FCG_CAT(fc_, _FCG_CAT(p, _entry)),                                   \
    hdr.key, hdr.meta.cur_hash, hdr.meta.slot,                            \
    cmp_fn, hash_fn)

/*===========================================================================
 * Sub-macro 2: Internal helper functions
 *===========================================================================*/
#define _FC_GENERATE_INTERNAL(p)                                           \
                                                                           \
static inline void __attribute__((unused))                                 \
_FCG_INT(p, prefetch_insert_hash)(const _FCG_CACHE_T(p) *fc,              \
                                  union rix_hash_hash_u h)                 \
{                                                                          \
    unsigned bk0, bk1;                                                     \
    u32 fp;                                                           \
    unsigned mask = fc->ht_head.rhh_mask;                                  \
    rix_hash_buckets(h, mask, &bk0, &bk1, &fp);                          \
    (void)fp;                                                              \
    rix_hash_prefetch_bucket_of(fc->buckets + bk0);                       \
    rix_hash_prefetch_bucket_of(fc->buckets + bk1);                       \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, result_set_hit)(_FCG_RESULT_T(p) *result,                     \
                            u32 entry_idx)                            \
{                                                                          \
    result->entry_idx = entry_idx;                                         \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, result_set_miss)(_FCG_RESULT_T(p) *result)                    \
{                                                                          \
    result->entry_idx = 0u;                                                \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, result_set_filled)(_FCG_RESULT_T(p) *result,                  \
                               u32 entry_idx)                         \
{                                                                          \
    result->entry_idx = entry_idx;                                         \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, update_eff_timeout)(_FCG_CACHE_T(p) *fc)                      \
{                                                                          \
    unsigned live = fc->ht_head.rhh_nb;                                    \
    u64 max_tsc = fc->timeout_tsc;                                    \
    unsigned lo = fc->timeout_lo_entries;                                  \
    unsigned hi = fc->timeout_hi_entries;                                  \
    if (fc->total_slots == 0u || max_tsc == 0u) {                          \
        fc->eff_timeout_tsc = max_tsc;                                     \
        return;                                                            \
    }                                                                      \
    if (live <= lo) {                                                      \
        fc->eff_timeout_tsc = max_tsc;                                     \
    } else if (live >= hi) {                                               \
        fc->eff_timeout_tsc = fc->timeout_min_tsc;                         \
    } else {                                                               \
        u64 span_entries = (u64)(hi - lo);                       \
        u64 used_entries = (u64)(live - lo);                     \
        u64 span_tsc = max_tsc - fc->timeout_min_tsc;                \
        u64 shrink = (used_entries * span_tsc) / span_entries;        \
        fc->eff_timeout_tsc = max_tsc - shrink;                            \
    }                                                                      \
    if (fc->eff_timeout_tsc == 0u)                                         \
        fc->eff_timeout_tsc = 1u;                                          \
}                                                                          \
                                                                           \
static inline unsigned                                                     \
_FCG_INT(p, threshold64)(unsigned total_slots, unsigned parts64)          \
{                                                                          \
    return (unsigned)(((u64)total_slots * parts64) >> 6);             \
}                                                                          \
                                                                           \
static void                                                                \
_FCG_INT(p, init_thresholds)(_FCG_CACHE_T(p) *fc)                       \
{                                                                          \
    fc->timeout_lo_entries =                                               \
        _FCG_INT(p, threshold64)(fc->total_slots, 38u);                   \
    fc->timeout_hi_entries =                                               \
        _FCG_INT(p, threshold64)(fc->total_slots, 48u);                   \
    fc->timeout_min_tsc = fc->timeout_tsc >> 3;                            \
    if (fc->timeout_min_tsc == 0u)                                         \
        fc->timeout_min_tsc = 1u;                                          \
    fc->relief_mid_entries =                                               \
        _FCG_INT(p, threshold64)(fc->total_slots, 45u);                   \
    fc->relief_hi_entries =                                                \
        _FCG_INT(p, threshold64)(fc->total_slots, 48u);                   \
}                                                                          \
                                                                           \
static inline unsigned                                                     \
_FCG_INT(p, relief_empty_slots)(const _FCG_CACHE_T(p) *fc)                \
{                                                                          \
    unsigned live = fc->ht_head.rhh_nb;                                    \
    unsigned empty_slots = fc->pressure_empty_slots;                       \
    if (live >= fc->relief_hi_entries) {                                   \
        if (empty_slots < 3u)                                              \
            empty_slots = 3u;                                              \
    } else if (live >= fc->relief_mid_entries) {                           \
        if (empty_slots < 2u)                                              \
            empty_slots = 2u;                                              \
    }                                                                      \
    return empty_slots;                                                    \
}                                                                          \
                                                                           \
static inline _FCG_ENTRY_T(p) *                                            \
_FCG_INT(p, alloc_entry)(_FCG_CACHE_T(p) *fc)                             \
{                                                                          \
    unsigned idx;                                                           \
    _FCG_ENTRY_T(p) *entry;                                                \
    FCG_FREE_LIST_POP_ENTRY(fc, entry, idx);                               \
    if (RIX_UNLIKELY(idx == RIX_NIL))                                       \
        return NULL;                                                       \
    FCG_EVENT_EMIT_ALLOC(fc, idx);                                         \
    return entry;                                                          \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, free_entry)(_FCG_CACHE_T(p) *fc,                              \
                        _FCG_ENTRY_T(p) *entry,                            \
                        unsigned reason)                                   \
{                                                                          \
    unsigned idx;                                                          \
    RIX_ASSUME_NONNULL(entry);                                             \
    idx = FCG_LAYOUT_ENTRY_INDEX(fc, entry);                               \
    FCG_EVENT_EMIT_FREE(fc, idx, reason);                                  \
    flow_timestamp_clear(&entry->hdr.meta);                                \
    FCG_FREE_LIST_PUSH_ENTRY(fc, entry, idx);                              \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, scan_bucket_slots)(_FCG_CACHE_T(p) *fc,                       \
                               unsigned bk_idx,                            \
                               u64 now_tsc,                           \
                               u64 timeout_tsc,                       \
                               unsigned *expired_slots,                    \
                               int *oldest_slot)                           \
{                                                                          \
    struct rix_hash_bucket_s *bucket = fc->buckets + bk_idx;              \
    u64 now_ts = _FCG_TS_NOW(fc, now_tsc);                            \
    u64 timeout_ts = _FCG_TS_TIMEOUT(fc, timeout_tsc);                \
    u64 oldest_ts = FLOW_TIMESTAMP_MASK;                              \
    _FCG_ENTRY_T(p) *entries[RIX_HASH_BUCKET_ENTRY_SZ];                    \
    unsigned slots[RIX_HASH_BUCKET_ENTRY_SZ];                              \
    unsigned cur_base = 0u;                                                \
    unsigned cur_count;                                                    \
    unsigned used = 0u;                                                    \
    unsigned expired_count = 0u;                                           \
    unsigned next_base = 0u;                                               \
    unsigned next_count = 0u;                                              \
    int dummy_oldest_slot = -1;                                            \
    int *oldest_slotp = (oldest_slot != NULL) ?                            \
        oldest_slot : &dummy_oldest_slot;                                  \
    for (unsigned slot = 0; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {    \
        unsigned idx = bucket->idx[slot];                                  \
        _FCG_ENTRY_T(p) *entry;                                          \
        if (idx == (unsigned)RIX_NIL)                                      \
            continue;                                                      \
        entry = FCG_LAYOUT_ENTRY_PTR(fc, idx);                            \
        slots[used] = slot;                                                \
        entries[used] = entry;                                             \
        used++;                                                            \
    }                                                                      \
    *oldest_slotp = -1;                                                    \
    if (RIX_UNLIKELY(used == 0u))                                          \
        return 0u;                                                         \
    cur_count = (used < _FC_RELIEF_STAGE_SLOTS) ?                          \
        used : _FC_RELIEF_STAGE_SLOTS;                                     \
    while (next_count < cur_count) {                                       \
        __builtin_prefetch(entries[next_count], 0, 3);                     \
        next_count++;                                                      \
    }                                                                      \
    next_base = cur_count;                                                 \
    while (cur_count != 0u) {                                              \
        next_count = 0u;                                                   \
        while (next_count < _FC_RELIEF_STAGE_SLOTS &&                      \
               next_base < used) {                                         \
            __builtin_prefetch(entries[next_base], 0, 3);                  \
            next_count++;                                                  \
            next_base++;                                                   \
        }                                                                  \
        for (unsigned s = 0; s < cur_count; s++) {                         \
            unsigned idx = cur_base + s;                                   \
            unsigned slot = slots[idx];                                    \
            _FCG_ENTRY_T(p) *entry = entries[idx];                        \
            if (!flow_timestamp_is_expired_raw(                            \
                    flow_timestamp_get(&entry->hdr.meta),                 \
                    now_ts, timeout_ts))                                   \
                continue;                                                  \
            expired_slots[expired_count] = slot;                           \
            expired_count++;                                               \
            if (flow_timestamp_get(&entry->hdr.meta) < oldest_ts) {        \
                oldest_ts = flow_timestamp_get(&entry->hdr.meta);          \
                *oldest_slotp = (int)slot;                                 \
            }                                                              \
        }                                                                  \
        cur_base += cur_count;                                             \
        cur_count = next_count;                                            \
    }                                                                      \
    return expired_count;                                                  \
}                                                                          \
                                                                           \
static int                                                                 \
_FCG_INT(p, reclaim_bucket)(_FCG_CACHE_T(p) *fc,                          \
                            unsigned bk_idx,                               \
                            u64 now_tsc,                              \
                            u64 timeout_tsc,                          \
                            unsigned free_reason)                          \
{                                                                          \
    _FCG_ENTRY_T(p) *victim;                                              \
    unsigned removed_idx;                                                  \
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];                      \
    unsigned expired_count;                                                \
    int victim_slot;                                                       \
    expired_count = _FCG_INT(p, scan_bucket_slots)(fc, bk_idx,             \
        now_tsc, timeout_tsc, expired_slots, &victim_slot);                \
    if (RIX_UNLIKELY(expired_count == 0u))                                 \
        return 0;                                                          \
    removed_idx = _FCG_HT(p, remove_at)(&fc->ht_head, fc->buckets,         \
                                         bk_idx, (unsigned)victim_slot);   \
    RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);                          \
    victim = FCG_LAYOUT_ENTRY_PTR(fc, removed_idx);                        \
    RIX_ASSERT(victim != NULL);                                            \
    RIX_ASSUME_NONNULL(victim);                                            \
    _FCG_INT(p, free_entry)(fc, victim, free_reason);                     \
    return 1;                                                              \
}                                                                          \
                                                                           \
static int                                                                 \
_FCG_INT(p, reclaim_oldest_global)(_FCG_CACHE_T(p) *fc,                   \
                                   u64 now,                           \
                                   unsigned free_reason)                   \
{                                                                          \
    u64 now_ts = _FCG_TS_NOW(fc, now);                                \
    u64 best_ts = FLOW_TIMESTAMP_MASK;                                \
    _FCG_ENTRY_T(p) *victim = NULL;                                        \
    unsigned max = fc->max_entries;                                        \
    unsigned scan_limit = max >> 3;                                        \
    unsigned start;                                                        \
    fc->stats.oldest_reclaim_calls++;                                      \
    /* Small tables (< 512 entries): full scan is cheap enough */          \
    if (scan_limit < 64u)                                                  \
        scan_limit = max;                                                  \
    /* Tile the entry space: successive calls scan adjacent windows.    */\
    /* tiles = ceil(max / scan_limit); start cycles through tile starts.*/\
    {                                                                      \
        unsigned tiles = (max + scan_limit - 1u) / scan_limit;             \
        start = (unsigned)(                                                \
            (fc->stats.oldest_reclaim_calls - 1u) % tiles)                 \
            * scan_limit;                                                   \
    }                                                                      \
    for (unsigned k = 0; k < scan_limit; k++) {                            \
        unsigned i = start + k;                                            \
        if (i >= max)                                                      \
            i -= max;                                                      \
        _FCG_ENTRY_T(p) *entry = FCG_LAYOUT_ENTRY_AT(fc, i);               \
        if (entry == NULL ||                                               \
            flow_timestamp_is_zero(&entry->hdr.meta) ||                    \
            flow_timestamp_get(&entry->hdr.meta) == now_ts)                \
            continue;                                                      \
        if (flow_timestamp_get(&entry->hdr.meta) < best_ts) {              \
            best_ts = flow_timestamp_get(&entry->hdr.meta);                \
            victim = entry;                                                \
        }                                                                  \
    }                                                                      \
    if (victim == NULL)                                                    \
        return 0;                                                          \
    victim = _FCG_HT(p, remove)(&fc->ht_head, fc->buckets,                 \
                                FCG_LAYOUT_HASH_BASE(fc), victim);         \
    RIX_ASSERT(victim != NULL);                                            \
    RIX_ASSUME_NONNULL(victim);                                            \
    _FCG_INT(p, free_entry)(fc, victim, free_reason);                      \
    fc->stats.oldest_reclaim_evictions++;                                  \
    return 1;                                                              \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, reclaim_bucket_all)(_FCG_CACHE_T(p) *fc,                      \
                                unsigned bk_idx,                           \
                                u64 now_tsc,                          \
                                u64 timeout_tsc,                      \
                                unsigned free_reason)                      \
{                                                                          \
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];                      \
    unsigned evicted;                                                      \
    evicted = _FCG_INT(p, scan_bucket_slots)(fc, bk_idx,                   \
                                             now_tsc, timeout_tsc,         \
                                             expired_slots, NULL);         \
    for (unsigned i = 0; i < evicted; i++) {                               \
        _FCG_ENTRY_T(p) *victim;                                           \
        unsigned removed_idx;                                              \
        removed_idx = _FCG_HT(p, remove_at)(&fc->ht_head, fc->buckets,     \
                                             bk_idx, expired_slots[i]);    \
        RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);                       \
        victim = FCG_LAYOUT_ENTRY_PTR(fc, removed_idx);                   \
        RIX_ASSERT(victim != NULL);                                        \
        RIX_ASSUME_NONNULL(victim);                                        \
        _FCG_INT(p, free_entry)(fc, victim, free_reason);                 \
    }                                                                      \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, maintain_grouped)(_FCG_CACHE_T(p) *fc,                        \
                              unsigned start_bk,                           \
                              unsigned bucket_count,                       \
                              u64 now_tsc)                            \
{                                                                          \
    unsigned evicted = 0u;                                                 \
    unsigned cur_bk;                                                       \
    unsigned mask;                                                         \
    unsigned next_bk;                                                      \
    RIX_ASSERT(fc->nb_bk != 0u);                                           \
    mask = fc->ht_head.rhh_mask;                                           \
    next_bk = start_bk & mask;                                             \
    rix_hash_prefetch_bucket_indices_of_idx(fc->buckets, next_bk);        \
    while (bucket_count-- != 0u) {                                         \
        unsigned reclaimed;                                                \
        cur_bk = next_bk;                                                  \
        next_bk = (next_bk + 1u) & mask;                                  \
        rix_hash_prefetch_bucket_indices_of_idx(fc->buckets, next_bk);    \
        fc->stats.maint_bucket_checks++;                                   \
        reclaimed = _FCG_INT(p, reclaim_bucket_all)(fc, cur_bk,           \
                                                     now_tsc,              \
                                                     fc->eff_timeout_tsc,  \
                                                     FCG_EVENT_REASON_TIMEOUT); \
        fc->stats.maint_evictions += reclaimed;                            \
        evicted += reclaimed;                                              \
    }                                                                      \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static inline unsigned                                                     \
_FCG_INT(p, bucket_used_slots)(const struct rix_hash_bucket_s *bucket)     \
{                                                                          \
    u32 zero_mask = rix_hash_arch->find_u32x16(bucket->idx, 0u);      \
    return 16u - (unsigned)__builtin_popcount(zero_mask);                  \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, maintain_step_filter_reclaim)(                                 \
    _FCG_CACHE_T(p) *fc,                                                   \
    unsigned start_bk,                                                     \
    unsigned bucket_count,                                                 \
    u64 now_tsc,                                                      \
    u64 timeout_tsc,                                                  \
    unsigned skip_threshold)                                               \
{                                                                          \
    unsigned evicted = 0u;                                                 \
    unsigned mask = fc->ht_head.rhh_mask;                                  \
    unsigned work[_FC_MAINT_STEP_MAX_BKS];                                 \
    RIX_ASSERT(bucket_count <= _FC_MAINT_STEP_MAX_BKS);                    \
    /* Pass 1 - filter: SIMD-scan idx[] to collect non-sparse buckets. */ \
    /* Touches only the idx[] cache line per bucket (sequential). */       \
    unsigned work_count = 0u;                                              \
    unsigned scan_bk = start_bk;                                           \
    for (unsigned i = 0; i < bucket_count; i++) {                          \
        unsigned used_slots;                                               \
        fc->stats.maint_bucket_checks++;                                   \
        used_slots = _FCG_INT(p, bucket_used_slots)(                       \
            &fc->buckets[scan_bk]);                                        \
        if (used_slots <= skip_threshold) {                                \
            fc->stats.maint_step_skipped_bks++;                            \
        } else {                                                           \
            work[work_count++] = scan_bk;                                  \
        }                                                                  \
        scan_bk = (scan_bk + 1u) & mask;                                   \
    }                                                                      \
    /* Pass 2 - reclaim: process only candidate buckets with N-ahead */   \
    /* prefetch.  Prefetch distance is stable (no skip branches). */       \
    for (unsigned j = 0;                                                   \
         j < _FC_MAINT_STEP_PREFETCH_AHEAD && j < work_count;              \
         j++)                                                              \
        rix_hash_prefetch_bucket_of(&fc->buckets[work[j]]);                \
    for (unsigned i = 0; i < work_count; i++) {                            \
        unsigned reclaimed;                                                \
        if (i + _FC_MAINT_STEP_PREFETCH_AHEAD < work_count)                \
            rix_hash_prefetch_bucket_of(                                   \
                &fc->buckets[work[i + _FC_MAINT_STEP_PREFETCH_AHEAD]]);   \
        reclaimed = _FCG_INT(p, reclaim_bucket_all)(fc, work[i],           \
                                                     now_tsc, timeout_tsc, \
                                                     FCG_EVENT_REASON_TIMEOUT); \
        fc->stats.maint_evictions += reclaimed;                            \
        evicted += reclaimed;                                              \
    }                                                                      \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, maintain_step_grouped)(_FCG_CACHE_T(p) *fc,                   \
                                   unsigned bucket_count,                  \
                                   u64 now_tsc,                       \
                                   unsigned skip_threshold)                \
{                                                                          \
    unsigned evicted = 0u;                                                 \
    unsigned mask;                                                         \
    RIX_ASSERT(fc->nb_bk != 0u);                                           \
    mask = fc->ht_head.rhh_mask;                                           \
    if (bucket_count > fc->nb_bk)                                          \
        bucket_count = fc->nb_bk;                                          \
    unsigned start_bk = fc->maint_cursor & mask;                           \
    unsigned cur_bk = start_bk;                                            \
    unsigned swept = bucket_count;                                         \
    while (bucket_count > 0u) {                                            \
        unsigned chunk = (bucket_count > _FC_MAINT_STEP_MAX_BKS) ?         \
            _FC_MAINT_STEP_MAX_BKS : bucket_count;                        \
        evicted += _FCG_INT(p, maintain_step_filter_reclaim)(              \
            fc, cur_bk, chunk, now_tsc, fc->eff_timeout_tsc,               \
            skip_threshold);                                               \
        cur_bk = (cur_bk + chunk) & mask;                                  \
        bucket_count -= chunk;                                             \
    }                                                                      \
    fc->maint_cursor = cur_bk;                                             \
    fc->last_maint_start_bk = start_bk;                                   \
    fc->last_maint_sweep_bk = swept;                                       \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static void __attribute__((unused))                                        \
_FCG_INT(p, insert_relief_hashed)(_FCG_CACHE_T(p) *fc,                    \
                                  union rix_hash_hash_u h,                 \
                                  u64 now_tsc)                        \
{                                                                          \
    unsigned bk0, bk1;                                                     \
    u32 fp;                                                           \
    u32 hits_fp;                                                      \
    u32 hits_zero;                                                    \
    unsigned pressure_empty_slots;                                         \
    if (fc->total_slots == 0u)                                             \
        return;                                                            \
    fc->stats.relief_calls++;                                              \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    rix_hash_buckets(h, fc->ht_head.rhh_mask, &bk0, &bk1, &fp);            \
    pressure_empty_slots = _FCG_INT(p, relief_empty_slots)(fc);            \
    fc->stats.relief_bucket_checks++;                                      \
    rix_hash_arch->find_u32x16_2(fc->buckets[bk0].hash, fp, 0u,            \
                                 &hits_fp, &hits_zero);                    \
    (void)hits_fp;                                                         \
    if ((unsigned)__builtin_popcount(hits_zero) <=                         \
            pressure_empty_slots &&                                        \
        _FCG_INT(p, reclaim_bucket)(fc, bk0, now_tsc,                      \
                                    fc->eff_timeout_tsc,                   \
                                    FCG_EVENT_REASON_PRESSURE)) {          \
        fc->stats.relief_evictions++;                                      \
        fc->stats.relief_bk0_evictions++;                                  \
        return;                                                            \
    }                                                                      \
    if (bk1 != bk0) {                                                      \
        fc->stats.relief_bucket_checks++;                                  \
        rix_hash_arch->find_u32x16_2(fc->buckets[bk1].hash, fp, 0u,        \
                                     &hits_fp, &hits_zero);                \
        if ((unsigned)__builtin_popcount(hits_zero) <=                     \
                pressure_empty_slots &&                                    \
            _FCG_INT(p, reclaim_bucket)(fc, bk1, now_tsc,                  \
                                        fc->eff_timeout_tsc,               \
                                        FCG_EVENT_REASON_PRESSURE)) {      \
            fc->stats.relief_evictions++;                                  \
            fc->stats.relief_bk1_evictions++;                              \
        }                                                                  \
    }                                                                      \
}

/*===========================================================================
 * Sub-macro 3a: Forward declarations for API functions
 *
 * When FC_ARCH_SUFFIX is defined, the suffixed function names have no
 * prototypes in any header.  Emit forward declarations to satisfy
 * -Wmissing-prototypes.
 *===========================================================================*/
#ifdef FC_ARCH_SUFFIX
#define _FC_GENERATE_API_DECLS(p)                                          \
static void _FCG_API(p, flush)(_FCG_CACHE_T(p) *);                         \
static unsigned _FCG_API(p, nb_entries)(const _FCG_CACHE_T(p) *);          \
static void _FCG_API(p, find_bulk)(_FCG_CACHE_T(p) *,                      \
                                   const _FCG_KEY_T(p) *, unsigned,        \
                                   u64, _FCG_RESULT_T(p) *);          \
static void _FCG_API(p, findadd_bulk)(_FCG_CACHE_T(p) *,                   \
                                      const _FCG_KEY_T(p) *, unsigned,     \
                                      u64, _FCG_RESULT_T(p) *);       \
static void _FCG_API(p, add_bulk)(_FCG_CACHE_T(p) *,                       \
                                  const _FCG_KEY_T(p) *, unsigned,         \
                                  u64, _FCG_RESULT_T(p) *);           \
static void _FCG_API(p, del_bulk)(_FCG_CACHE_T(p) *,                       \
                                  const _FCG_KEY_T(p) *, unsigned);        \
static void _FCG_API(p, del_idx_bulk)(_FCG_CACHE_T(p) *,                   \
                                      const u32 *, unsigned);         \
static unsigned _FCG_API(p, maintain)(_FCG_CACHE_T(p) *,                   \
                                      unsigned, unsigned, u64);       \
static unsigned _FCG_API(p, maintain_step_ex)(_FCG_CACHE_T(p) *,           \
                                              unsigned, unsigned,          \
                                              unsigned, u64);         \
static unsigned _FCG_API(p, maintain_step)(_FCG_CACHE_T(p) *,              \
                                           u64, int);                 \
static int _FCG_API(p, remove_idx)(_FCG_CACHE_T(p) *, u32);           \
static void _FCG_API(p, stats)(const _FCG_CACHE_T(p) *, _FCG_STATS_T(p) *); \
static int _FCG_API(p, walk)(_FCG_CACHE_T(p) *,                            \
                             int (*)(u32, void *), void *);
#else
#define _FC_GENERATE_API_DECLS(p) /* prototypes in variant header */
#endif

/*===========================================================================
 * Shared findadd stage-4 resolve logic (used by findadd_bulk & burst32)
 *
 * Parameters:
 *   p            - variant prefix (flow4 / flow6 / flowu)
 *   fc           - cache pointer
 *   keys         - key array
 *   now          - current timestamp
 *   results      - result array
 *   ctx_ptr      - pointer to a single rix_hash_find_ctx_s
 *   idx          - index into keys[] / results[]
 *   hit_counter  - lvalue incremented on hit
 *   miss_counter - lvalue incremented on miss
 *===========================================================================*/
#define _FCG_FINDADD_RESOLVE_ONE(p, fc, keys, now, results, ctx_ptr, idx,  \
                                  hit_counter, miss_counter)                \
do {                                                                        \
    unsigned _bk0i =                                                        \
        (unsigned)((ctx_ptr)->bk[0] - (fc)->buckets);                       \
    unsigned _bk1i =                                                        \
        (unsigned)((ctx_ptr)->bk[1] - (fc)->buckets);                       \
    _FCG_ENTRY_T(p) *_entry;                                                \
    u32 _hit_idx;                                                      \
    _hit_idx = _FCG_HT(p, cmp_key_empties)((ctx_ptr),                       \
                                           FCG_LAYOUT_HASH_BASE(fc), 0u);    \
    if (RIX_LIKELY(_hit_idx != (u32)RIX_NIL)) {                        \
        /* --- HIT --- */                                                    \
        _entry = FCG_LAYOUT_ENTRY_PTR((fc), _hit_idx);                       \
        RIX_ASSUME_NONNULL(_entry);                                          \
        flow_timestamp_store(&_entry->hdr.meta, (now),                     \
                             _FCG_TS_SHIFT(fc));                           \
        _FCG_INT(p, result_set_hit)(&(results)[(idx)],                       \
            _hit_idx);                                                       \
        (hit_counter)++;                                                     \
        break;                                                               \
    }                                                                        \
    _FCG_HT(p, scan_bk_empties)((ctx_ptr), 1u);                              \
    _hit_idx = _FCG_HT(p, cmp_key_empties)((ctx_ptr),                        \
                                           FCG_LAYOUT_HASH_BASE(fc), 1u);    \
    if (RIX_LIKELY(_hit_idx != (u32)RIX_NIL)) {                        \
        /* --- HIT in bk1 --- */                                             \
        _entry = FCG_LAYOUT_ENTRY_PTR((fc), _hit_idx);                       \
        RIX_ASSUME_NONNULL(_entry);                                          \
        flow_timestamp_store(&_entry->hdr.meta, (now),                     \
                             _FCG_TS_SHIFT(fc));                           \
        _FCG_INT(p, result_set_hit)(&(results)[(idx)],                       \
            _hit_idx);                                                       \
        (hit_counter)++;                                                     \
        break;                                                               \
    }                                                                        \
    /* --- MISS: inline insert --- */                                        \
    (miss_counter)++;                                                        \
    /* Relief: use empties[] popcount (no re-scan) */                        \
    if ((fc)->total_slots != 0u) {                                           \
        unsigned _pe =                                                       \
            _FCG_INT(p, relief_empty_slots)((fc));                           \
        (fc)->stats.relief_calls++;                                          \
        (fc)->stats.relief_bucket_checks++;                                  \
        if ((unsigned)__builtin_popcount(                                     \
                (ctx_ptr)->empties[0]) <= _pe) {                             \
            _FCG_INT(p, update_eff_timeout)((fc));                           \
            if (_FCG_INT(p, reclaim_bucket)(                                 \
                    (fc), _bk0i, (now), (fc)->eff_timeout_tsc,              \
                    FCG_EVENT_REASON_PRESSURE)) {                             \
                (fc)->stats.relief_evictions++;                               \
                (fc)->stats.relief_bk0_evictions++;                          \
            } else {                                                         \
                (fc)->stats.relief_bucket_checks++;                          \
                if ((unsigned)__builtin_popcount(                             \
                        (ctx_ptr)->empties[1]) <= _pe &&                     \
                    _bk1i != _bk0i &&                                        \
                    _FCG_INT(p, reclaim_bucket)(                             \
                        (fc), _bk1i, (now), (fc)->eff_timeout_tsc,          \
                        FCG_EVENT_REASON_PRESSURE)) {                        \
                    (fc)->stats.relief_evictions++;                           \
                    (fc)->stats.relief_bk1_evictions++;                      \
                }                                                            \
            }                                                                \
        }                                                                    \
    }                                                                        \
    /* Alloc from free list (head already prefetched) */                     \
    _entry = _FCG_INT(p, alloc_entry)((fc));                                 \
    if (RIX_UNLIKELY(_entry == NULL) &&                                      \
        _FCG_INT(p, reclaim_oldest_global)(                                  \
            (fc), (now), FCG_EVENT_REASON_OLDEST))                           \
        _entry = _FCG_INT(p, alloc_entry)((fc));                             \
    if (RIX_UNLIKELY(_entry == NULL)) {                                      \
        (fc)->stats.fill_full++;                                             \
        _FCG_INT(p, result_set_miss)(&(results)[(idx)]);                     \
        break;                                                               \
    }                                                                        \
    _entry->hdr.key = (keys)[(idx)];                                         \
    flow_timestamp_store(&_entry->hdr.meta, (now),                         \
                         _FCG_TS_SHIFT(fc));                               \
    /* insert_hashed: buckets in L1 from cmp_key,      */                   \
    /* hash reused from ctx (no rehash), dup-safe.     */                   \
    {                                                                        \
        _FCG_ENTRY_T(p) *_ret;                                               \
        _ret = _FCG_HT(p, insert_hashed)(                                   \
            &(fc)->ht_head, (fc)->buckets,                                   \
            FCG_LAYOUT_HASH_BASE(fc),                                        \
            _entry, (ctx_ptr)->hash);                                        \
        if (RIX_LIKELY(_ret == NULL)) {                                      \
            (fc)->stats.fills++;                                             \
            _FCG_INT(p, result_set_filled)(&(results)[(idx)],               \
                FCG_LAYOUT_ENTRY_INDEX((fc), _entry));                       \
        } else {                                                             \
            if (_ret == _entry &&                                            \
                _FCG_INT(p, reclaim_oldest_global)(                          \
                    (fc), (now), FCG_EVENT_REASON_OLDEST)) {                  \
                /* reclaim freed an entry globally but target buckets */     \
                /* may still be full -- evict timed-out entries from   */    \
                /* bk[0]/bk[1] using the same cutoff as pressure relief */  \
                {                                                            \
                    _FCG_INT(p, reclaim_bucket)(                             \
                        (fc), _bk0i, (now), (fc)->eff_timeout_tsc,          \
                        FCG_EVENT_REASON_PRESSURE);                          \
                    if (_bk1i != _bk0i)                                      \
                        _FCG_INT(p, reclaim_bucket)(                         \
                            (fc), _bk1i, (now), (fc)->eff_timeout_tsc,      \
                            FCG_EVENT_REASON_PRESSURE);                      \
                }                                                            \
                _ret = _FCG_HT(p, insert_hashed)(                           \
                    &(fc)->ht_head, (fc)->buckets,                           \
                    FCG_LAYOUT_HASH_BASE(fc),                                \
                    _entry, (ctx_ptr)->hash);                                 \
            }                                                                \
            if (_ret == NULL) {                                              \
                (fc)->stats.fills++;                                          \
                _FCG_INT(p, result_set_filled)(&(results)[(idx)],           \
                    FCG_LAYOUT_ENTRY_INDEX((fc), _entry));                    \
            } else if (_ret != _entry) {                                     \
                RIX_ASSUME_NONNULL(_entry);                                  \
                _FCG_INT(p, free_entry)((fc), _entry,                        \
                                        FCG_EVENT_REASON_ROLLBACK);          \
                /* duplicate found */                                        \
                RIX_ASSUME_NONNULL(_ret);                                    \
                flow_timestamp_store(&_ret->hdr.meta, (now),               \
                                     _FCG_TS_SHIFT(fc));                   \
                _FCG_INT(p, result_set_filled)(                              \
                    &(results)[(idx)],                                        \
                    FCG_LAYOUT_ENTRY_INDEX((fc), _ret));                      \
            } else {                                                         \
                RIX_ASSUME_NONNULL(_entry);                                  \
                _FCG_INT(p, free_entry)((fc), _entry,                        \
                                        FCG_EVENT_REASON_ROLLBACK);          \
                /* table full */                                             \
                (fc)->stats.fill_full++;                                      \
                _FCG_INT(p, result_set_miss)(                                \
                    &(results)[(idx)]);                                       \
            }                                                                \
        }                                                                    \
    }                                                                        \
    /* Prefetch next free list head for future miss */                       \
    {                                                                        \
        _FCG_ENTRY_T(p) *_nf = FCG_FREE_LIST_FIRST_PTR((fc));               \
        if (_nf != NULL)                                                     \
            rix_hash_prefetch_entry_of(_nf);                                 \
    }                                                                        \
} while (0)

/*===========================================================================
 * init_ex body macro (shared across per-variant .c files)
 *
 * Parameters:
 *   p               - variant prefix
 *   default_pressure - default pressure_empty_slots constant
 *   fc, buckets, nb_bk, array, max_entries, stride, entry_offset, cfg
 *                    - forwarded from init_ex signature
 *===========================================================================*/
#define _FCG_INIT_EX_BODY(p, default_pressure, fc, buckets, nb_bk,          \
                           array, max_entries, stride, entry_offset, cfg)    \
do {                                                                        \
    _FCG_CONFIG_T(p) _defcfg = {                                            \
        .timeout_tsc = UINT64_C(1000000),                                    \
        .pressure_empty_slots = (default_pressure),                          \
    };                                                                       \
    const _FCG_CONFIG_T(p) *_cfg = (cfg);                                    \
    if (_cfg == NULL)                                                        \
        _cfg = &_defcfg;                                                     \
    memset((fc), 0, sizeof(*(fc)));                                          \
    memset((buckets), 0, (size_t)(nb_bk) * sizeof(*(buckets)));              \
    FCG_LAYOUT_INIT_STORAGE((fc), (array), (stride), (entry_offset));        \
    (fc)->buckets = (buckets);                                               \
    (fc)->nb_bk = (nb_bk);                                                   \
    (fc)->max_entries = (max_entries);                                        \
    (fc)->total_slots = (nb_bk) * RIX_HASH_BUCKET_ENTRY_SZ;                  \
    (fc)->timeout_tsc = _cfg->timeout_tsc;                                   \
    (fc)->eff_timeout_tsc = _cfg->timeout_tsc ? _cfg->timeout_tsc : 1u;      \
    (fc)->ts_shift = (u8)((_cfg->ts_shift != 0u)                        \
        ? flow_timestamp_shift_clamp(_cfg->ts_shift)                         \
        : FLOW_TIMESTAMP_DEFAULT_SHIFT);                                     \
    (fc)->pressure_empty_slots = _cfg->pressure_empty_slots ?                \
        _cfg->pressure_empty_slots : (default_pressure);                     \
    (fc)->maint_interval_tsc = _cfg->maint_interval_tsc;                     \
    (fc)->maint_base_bk = _cfg->maint_base_bk ?                              \
        _cfg->maint_base_bk : (nb_bk);                                      \
    (fc)->maint_fill_threshold = _cfg->maint_fill_threshold;                 \
    _FCG_INT(p, init_thresholds)((fc));                                      \
    FCG_FREE_LIST_INIT((fc));                                                \
    _FCG_HT(p, init)(&(fc)->ht_head, (nb_bk));                               \
    for (unsigned _ii = (max_entries); _ii > 0u; _ii--) {                     \
        _FCG_ENTRY_T(p) *_ent = FCG_LAYOUT_ENTRY_PTR((fc), _ii);             \
        RIX_ASSUME_NONNULL(_ent);                                            \
        FCG_LAYOUT_ENTRY_CLEAR((fc), _ent);                                  \
        FCG_FREE_LIST_PUSH_ENTRY((fc), _ent, _ii);                           \
    }                                                                        \
} while (0)

/*===========================================================================
 * findadd_burst32 body macro (dynamic pipeline geometry, nb_keys <= 32)
 *
 * Used by per-variant .c files.  The function signature and forward
 * declaration are still written in each .c file so that FC_ARCH_SUFFIX
 * name mangling works normally.
 *===========================================================================*/
#define _FCG_FINDADD_BURST32_BODY(p, fc, keys, nb_keys, now, results)      \
do {                                                                        \
    struct rix_hash_find_ctx_s _stack_ctx[FC_CACHE_BULK_CTX_COUNT];         \
    struct rix_hash_find_ctx_s *_ctx =                                      \
        ((fc)->bulk_ctx != NULL &&                                           \
         (fc)->bulk_ctx_count >= FC_CACHE_BULK_CTX_COUNT) ?                  \
        (fc)->bulk_ctx : _stack_ctx;                                         \
    u64 _hit_count = 0u;                                                \
    u64 _miss_count = 0u;                                               \
    unsigned _step_keys;                                                     \
    unsigned _ahead_keys;                                                    \
    unsigned _total;                                                         \
                                                                             \
    if ((nb_keys) == 0u)                                                     \
        break;                                                               \
                                                                             \
    {                                                                        \
        _FCG_ENTRY_T(p) *_fh = FCG_FREE_LIST_FIRST_PTR((fc));               \
        if (_fh != NULL)                                                     \
            rix_hash_prefetch_entry_of(_fh);                                 \
    }                                                                        \
                                                                             \
    if ((nb_keys) <= 4u) {                                                   \
        _step_keys = 1u;                                                     \
        _ahead_keys = 1u;                                                    \
    } else if ((nb_keys) <= 8u) {                                            \
        _step_keys = 2u;                                                     \
        _ahead_keys = 2u;                                                    \
    } else if ((nb_keys) <= 16u) {                                           \
        _step_keys = 4u;                                                     \
        _ahead_keys = 4u;                                                    \
    } else {                                                                 \
        _step_keys = 8u;                                                     \
        _ahead_keys = 8u;                                                    \
    }                                                                        \
                                                                             \
    _total = (nb_keys) + 3u * _ahead_keys;                                   \
    for (unsigned _i = 0; _i < _total; _i += _step_keys) {                   \
        if (_i < (nb_keys)) {                                                \
            unsigned _n = (_i + _step_keys <= (nb_keys)) ?                   \
                _step_keys : ((nb_keys) - _i);                               \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCG_HT(p, hash_key_2bk_masked)(                             \
                    &_ctx[(_i + _j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],       \
                    (fc)->buckets, &(keys)[_i + _j],                         \
                    (fc)->ht_head.rhh_mask, (fc)->ht_head.rhh_mask);         \
        }                                                                    \
        if (_i >= _ahead_keys && _i - _ahead_keys < (nb_keys)) {             \
            unsigned _base = _i - _ahead_keys;                               \
            unsigned _n = (_base + _step_keys <= (nb_keys)) ?                \
                _step_keys : ((nb_keys) - _base);                            \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCG_HT(p, scan_bk_empties)(                                 \
                    &_ctx[(_base + _j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],    \
                    0u);                                                     \
        }                                                                    \
        if (_i >= 2u * _ahead_keys &&                                        \
            _i - 2u * _ahead_keys < (nb_keys)) {                             \
            unsigned _base = _i - 2u * _ahead_keys;                          \
            unsigned _n = (_base + _step_keys <= (nb_keys)) ?                \
                _step_keys : ((nb_keys) - _base);                            \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCG_HT(p, prefetch_node)(                                   \
                    &_ctx[(_base + _j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],    \
                    FCG_LAYOUT_HASH_BASE(fc));                               \
        }                                                                    \
        if (_i >= 3u * _ahead_keys &&                                        \
            _i - 3u * _ahead_keys < (nb_keys)) {                             \
            unsigned _base = _i - 3u * _ahead_keys;                          \
            unsigned _n = (_base + _step_keys <= (nb_keys)) ?                \
                _step_keys : ((nb_keys) - _base);                            \
            for (unsigned _j = 0; _j < _n; _j++)                             \
                _FCG_FINDADD_RESOLVE_ONE(p, (fc), (keys), (now),             \
                    (results),                                               \
                    &_ctx[(_base + _j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],    \
                    _base + _j, _hit_count, _miss_count);                    \
        }                                                                    \
    }                                                                        \
    (fc)->stats.lookups += (nb_keys);                                         \
    (fc)->stats.hits += _hit_count;                                           \
    (fc)->stats.misses += _miss_count;                                        \
} while (0)

/*===========================================================================
 * Sub-macro 3: Public API functions
 *===========================================================================*/
#define _FC_GENERATE_API(p, pressure, hash_fn)                             \
_FC_GENERATE_API_DECLS(p)                                                  \
                                                                           \
static void                                                                \
_FCG_API(p, flush)(_FCG_CACHE_T(p) *fc)                                    \
{                                                                          \
    memset(fc->buckets, 0, (size_t)fc->nb_bk * sizeof(*fc->buckets));      \
    FCG_FREE_LIST_INIT(fc);                                                \
    _FCG_HT(p, init)(&fc->ht_head, fc->nb_bk);                             \
    for (unsigned i = fc->max_entries; i > 0u; i--) {                      \
        _FCG_ENTRY_T(p) *entry = FCG_LAYOUT_ENTRY_PTR(fc, i);              \
        RIX_ASSUME_NONNULL(entry);                                         \
        if (!flow_timestamp_is_zero(&entry->hdr.meta))                     \
            FCG_EVENT_EMIT_FREE(fc, i, FCG_EVENT_REASON_FLUSH);            \
        FCG_LAYOUT_ENTRY_CLEAR(fc, entry);                                 \
        FCG_FREE_LIST_PUSH_ENTRY(fc, entry, i);                            \
    }                                                                      \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_API(p, nb_entries)(const _FCG_CACHE_T(p) *fc)                        \
{                                                                          \
    return fc->ht_head.rhh_nb;                                             \
}                                                                          \
                                                                           \
/* ----- find_bulk: search only, no insert ----------------------------- */ \
static void                                                                \
_FCG_API(p, find_bulk)(_FCG_CACHE_T(p) *fc,                                \
                       const _FCG_KEY_T(p) *keys,                          \
                       unsigned nb_keys,                                   \
                       u64 now,                                       \
                       _FCG_RESULT_T(p) *results)                          \
{                                                                          \
    struct rix_hash_find_ctx_s stack_ctx[FC_CACHE_BULK_CTX_COUNT];         \
    struct rix_hash_find_ctx_s *ctx =                                      \
        (fc->bulk_ctx != NULL &&                                           \
         fc->bulk_ctx_count >= FC_CACHE_BULK_CTX_COUNT) ?                  \
        fc->bulk_ctx : stack_ctx;                                          \
    u64 hit_count = 0u;                                               \
    u64 miss_count = 0u;                                              \
    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;              \
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;                \
    const unsigned total = nb_keys + 3u * ahead_keys;                      \
    for (unsigned i = 0; i < total; i += step_keys) {                      \
        /* Stage 1: hash_key_2bk */                                        \
        if (i < nb_keys) {                                                 \
            unsigned n = (i + step_keys <= nb_keys) ?                      \
                step_keys : (nb_keys - i);                                 \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, hash_key_2bk_masked)(                           \
                    &ctx[(i + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],        \
                    fc->buckets, &keys[i + j],                             \
                    fc->ht_head.rhh_mask, fc->ht_head.rhh_mask);           \
        }                                                                  \
        /* Stage 2: scan_bk (no empty tracking needed) */                  \
        if (i >= ahead_keys && i - ahead_keys < nb_keys) {                 \
            unsigned base = i - ahead_keys;                                \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, scan_bk)(                                       \
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],     \
                    &fc->ht_head, fc->buckets);                            \
        }                                                                  \
        /* Stage 3: prefetch_node */                                       \
        if (i >= 2u * ahead_keys &&                                        \
            i - 2u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 2u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, prefetch_node)(                                 \
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],     \
                    FCG_LAYOUT_HASH_BASE(fc));                              \
        }                                                                  \
        /* Stage 4: cmp_key - hit or miss, no insert */                    \
        if (i >= 3u * ahead_keys &&                                        \
            i - 3u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 3u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++) {                             \
                unsigned idx = base + j;                                   \
                _FCG_ENTRY_T(p) *entry;                                   \
                entry = _FCG_HT(p, cmp_key)(                               \
                    &ctx[idx & (FC_CACHE_BULK_CTX_COUNT - 1u)],            \
                    FCG_LAYOUT_HASH_BASE(fc));                              \
                if (RIX_LIKELY(entry != NULL)) {                           \
                    if (now)                                                \
                        flow_timestamp_store(&entry->hdr.meta, (now),      \
                                             _FCG_TS_SHIFT(fc));           \
                    _FCG_INT(p, result_set_hit)(&results[idx],            \
                        FCG_LAYOUT_ENTRY_INDEX(fc, entry));                \
                    hit_count++;                                           \
                } else {                                                   \
                    _FCG_INT(p, result_set_miss)(&results[idx]);          \
                    miss_count++;                                          \
                }                                                          \
            }                                                              \
        }                                                                  \
    }                                                                      \
    fc->stats.lookups += nb_keys;                                          \
    fc->stats.hits += hit_count;                                           \
    fc->stats.misses += miss_count;                                        \
}                                                                          \
                                                                           \
/* ----- findadd_bulk: search + insert on miss ------------------------- */ \
static void                                                                \
_FCG_API(p, findadd_bulk)(_FCG_CACHE_T(p) *fc,                             \
                          const _FCG_KEY_T(p) *keys,                       \
                          unsigned nb_keys,                                \
                          u64 now,                                    \
                          _FCG_RESULT_T(p) *results)                       \
{                                                                          \
    struct rix_hash_find_ctx_s stack_ctx[FC_CACHE_BULK_CTX_COUNT];         \
    struct rix_hash_find_ctx_s *ctx =                                      \
        (fc->bulk_ctx != NULL &&                                           \
         fc->bulk_ctx_count >= FC_CACHE_BULK_CTX_COUNT) ?                  \
        fc->bulk_ctx : stack_ctx;                                          \
    u64 hit_count = 0u;                                               \
    u64 miss_count = 0u;                                              \
    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;              \
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;                \
    const unsigned total = nb_keys + 3u * ahead_keys;                      \
    /* Prefetch free list head so first miss insert is warm */             \
    {                                                                      \
        _FCG_ENTRY_T(p) *_fh = FCG_FREE_LIST_FIRST_PTR(fc);                \
        if (_fh != NULL)                                                   \
            rix_hash_prefetch_entry_of(_fh);                              \
    }                                                                      \
    /* 4-stage N-ahead pipeline: lookup + inline insert on miss */         \
    for (unsigned i = 0; i < total; i += step_keys) {                      \
        /* Stage 1: hash_key_2bk (prefetch both bk[0] and bk[1]) */       \
        if (i < nb_keys) {                                                 \
            unsigned n = (i + step_keys <= nb_keys) ?                      \
                step_keys : (nb_keys - i);                                 \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, hash_key_2bk_masked)(                           \
                    &ctx[(i + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],        \
                    fc->buckets, &keys[i + j],                             \
                    fc->ht_head.rhh_mask, fc->ht_head.rhh_mask);           \
        }                                                                  \
        /* Stage 2: scan_bk_empties (bk[0] fp + empty scan) */            \
        if (i >= ahead_keys && i - ahead_keys < nb_keys) {                \
            unsigned base = i - ahead_keys;                                \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, scan_bk_empties)(                               \
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],     \
                    0u);                                                    \
        }                                                                  \
        /* Stage 3: prefetch_node */                                       \
        if (i >= 2u * ahead_keys &&                                        \
            i - 2u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 2u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, prefetch_node)(                                 \
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],     \
                    FCG_LAYOUT_HASH_BASE(fc));                              \
        }                                                                  \
        /* Stage 4: cmp_key_empties + inline insert on miss */             \
        if (i >= 3u * ahead_keys &&                                        \
            i - 3u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 3u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++) {                             \
                unsigned idx = base + j;                                   \
                _FCG_FINDADD_RESOLVE_ONE(p, fc, keys, now, results,        \
                    &ctx[idx & (FC_CACHE_BULK_CTX_COUNT - 1u)],            \
                    idx, hit_count, miss_count);                            \
            }                                                              \
        }                                                                  \
    }                                                                      \
    fc->stats.lookups += nb_keys;                                          \
    fc->stats.hits += hit_count;                                           \
    fc->stats.misses += miss_count;                                        \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_API(p, maintain)(_FCG_CACHE_T(p) *fc,                              \
                       unsigned start_bk,                                  \
                       unsigned bucket_count,                              \
                       u64 now)                                       \
{                                                                          \
    fc->stats.maint_calls++;                                               \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    return _FCG_INT(p, maintain_grouped)(fc, start_bk,                 \
                                            bucket_count, now);            \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_API(p, maintain_step_ex)(_FCG_CACHE_T(p) *fc,                       \
                               unsigned start_bk,                           \
                               unsigned bucket_count,                       \
                               unsigned skip_threshold,                     \
                               u64 now)                                \
{                                                                          \
    fc->stats.maint_step_calls++;                                          \
    fc->stats.maint_calls++;                                               \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    fc->maint_cursor = start_bk & fc->ht_head.rhh_mask;                   \
    return _FCG_INT(p, maintain_step_grouped)(fc, bucket_count, now,      \
                                               skip_threshold);             \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_API(p, maintain_step)(_FCG_CACHE_T(p) *fc,                          \
                            u64 now,                                   \
                            int idle)                                       \
{                                                                          \
    unsigned sweep;                                                        \
    unsigned skip_threshold;                                               \
    fc->stats.maint_step_calls++;                                          \
    if (idle) {                                                            \
        sweep = fc->nb_bk;                                                 \
        skip_threshold = 0u;                                               \
    } else {                                                               \
        u64 elapsed = now - fc->last_maint_tsc;                       \
        u64 added   = fc->stats.fills - fc->last_maint_fills;         \
        unsigned time_scale = 0u;                                          \
        unsigned entry_scale = 0u;                                         \
        unsigned scale;                                                    \
        if (fc->maint_interval_tsc != 0u)                                  \
            time_scale = (unsigned)(elapsed / fc->maint_interval_tsc);     \
        else                                                               \
            time_scale = 1u;                                               \
        if (fc->maint_fill_threshold != 0u)                                \
            entry_scale = (unsigned)(added / fc->maint_fill_threshold);    \
        scale = (time_scale > entry_scale) ? time_scale : entry_scale;     \
        if (scale == 0u)                                                   \
            return 0u;                                                     \
        sweep = fc->maint_base_bk * scale;                                 \
        if (sweep > fc->nb_bk)                                             \
            sweep = fc->nb_bk;                                             \
        skip_threshold = fc->pressure_empty_slots;                         \
    }                                                                      \
    fc->last_maint_tsc   = now;                                            \
    fc->last_maint_fills = fc->stats.fills;                                \
    fc->stats.maint_calls++;                                               \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    return _FCG_INT(p, maintain_step_grouped)(fc, sweep, now,             \
                                               skip_threshold);             \
}                                                                          \
                                                                           \
static int                                                                 \
_FCG_API(p, remove_idx)(_FCG_CACHE_T(p) *fc, u32 entry_idx)        \
{                                                                          \
    _FCG_ENTRY_T(p) *entry;                                               \
    if (RIX_UNLIKELY(entry_idx == 0u || entry_idx > fc->max_entries))      \
        return 0;                                                          \
    entry = FCG_LAYOUT_ENTRY_PTR(fc, entry_idx);                           \
    if (entry == NULL)                                                     \
        return 0;                                                          \
    if (_FCG_HT(p, remove)(&fc->ht_head, fc->buckets,                    \
                           FCG_LAYOUT_HASH_BASE(fc), entry) == NULL)       \
        return 0;                                                          \
    _FCG_INT(p, free_entry)(fc, entry, FCG_EVENT_REASON_DELETE);          \
    return 1;                                                              \
}                                                                          \
                                                                           \
static void                                                                \
_FCG_API(p, stats)(const _FCG_CACHE_T(p) *fc, _FCG_STATS_T(p) *out)   \
{                                                                          \
    *out = fc->stats;                                                      \
}                                                                          \
                                                                           \
/* ----- walk: iterate all live entries -------------------------------- */\
static int                                                                 \
_FCG_API(p, walk)(_FCG_CACHE_T(p) *fc,                                  \
                   int (*cb)(u32 entry_idx, void *arg), void *arg)    \
{                                                                          \
    for (unsigned i = 0; i < fc->max_entries; i++) {                       \
        if (!flow_timestamp_is_zero(&FCG_LAYOUT_ENTRY_AT(fc, i)->hdr.meta)) { \
            int rc = cb(i + 1u, arg);                                      \
            if (rc < 0)                                                    \
                return rc;                                                 \
        }                                                                  \
    }                                                                      \
    return 0;                                                              \
}                                                                          \
                                                                           \
/* ----- add_bulk: insert only (no prior search) ----------------------- */\
static void                                                                \
_FCG_API(p, add_bulk)(_FCG_CACHE_T(p) *fc,                              \
                       const _FCG_KEY_T(p) *keys,                         \
                       unsigned nb_keys,                                   \
                       u64 now,                                       \
                       _FCG_RESULT_T(p) *results)                         \
{                                                                          \
    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;              \
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;                \
    const unsigned total = nb_keys + ahead_keys;                           \
    union rix_hash_hash_u hashes[FC_CACHE_BULK_CTX_COUNT];                 \
    /* Prefetch free list head */                                          \
    {                                                                      \
        _FCG_ENTRY_T(p) *_fh = FCG_FREE_LIST_FIRST_PTR(fc);                \
        if (_fh != NULL)                                                   \
            rix_hash_prefetch_entry_of(_fh);                              \
    }                                                                      \
    /* 2-stage pipeline: hash+prefetch both candidate buckets, then        \
     * alloc+insert. add_bulk still performs duplicate checks inside       \
     * insert_hashed(), so bk1 matters too. */                             \
    for (unsigned i = 0; i < total; i += step_keys) {                      \
        /* Stage 1: hash + prefetch buckets */                             \
        if (i < nb_keys) {                                                 \
            unsigned n = (i + step_keys <= nb_keys) ?                      \
                step_keys : (nb_keys - i);                                 \
            for (unsigned j = 0; j < n; j++) {                             \
                unsigned idx = i + j;                                      \
                unsigned _bk0;                                             \
                unsigned _bk1;                                             \
                u32 _fp_unused;                                            \
                hashes[idx & (FC_CACHE_BULK_CTX_COUNT - 1u)] =             \
                    hash_fn(&keys[idx], fc->ht_head.rhh_mask);             \
                rix_hash_buckets(                                          \
                    hashes[idx & (FC_CACHE_BULK_CTX_COUNT - 1u)],          \
                    fc->ht_head.rhh_mask, &_bk0, &_bk1, &_fp_unused);      \
                rix_hash_prefetch_bucket_of(&fc->buckets[_bk0]);           \
                rix_hash_prefetch_bucket_of(&fc->buckets[_bk1]);           \
            }                                                              \
        }                                                                  \
        /* Stage 2: alloc + insert */                                      \
        if (i >= ahead_keys && i - ahead_keys < nb_keys) {                \
            unsigned base = i - ahead_keys;                                \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++) {                             \
                unsigned idx = base + j;                                   \
                _FCG_ENTRY_T(p) *entry =                                  \
                    _FCG_INT(p, alloc_entry)(fc);                          \
                if (RIX_UNLIKELY(entry == NULL) &&                         \
                    _FCG_INT(p, reclaim_oldest_global)(                    \
                        fc, now, FCG_EVENT_REASON_OLDEST))                 \
                    entry = _FCG_INT(p, alloc_entry)(fc);                  \
                if (RIX_UNLIKELY(entry == NULL)) {                         \
                    fc->stats.fill_full++;                                 \
                    _FCG_INT(p, result_set_miss)(&results[idx]);          \
                    continue;                                              \
                }                                                          \
                entry->hdr.key = keys[idx];                                \
                flow_timestamp_store(&entry->hdr.meta, (now),              \
                                     _FCG_TS_SHIFT(fc));                   \
                {                                                          \
                    _FCG_ENTRY_T(p) *_ret;                                \
                    _ret = _FCG_HT(p, insert_hashed)(                     \
                        &fc->ht_head, fc->buckets,                        \
                        FCG_LAYOUT_HASH_BASE(fc),                         \
                        entry, hashes[idx & (FC_CACHE_BULK_CTX_COUNT - 1u)]); \
                    if (RIX_LIKELY(_ret == NULL)) {                        \
                        fc->stats.fills++;                                 \
                        _FCG_INT(p, result_set_filled)(&results[idx],     \
                            FCG_LAYOUT_ENTRY_INDEX(fc, entry));            \
                    } else {                                               \
                        if (_ret == entry &&                               \
                            _FCG_INT(p, reclaim_oldest_global)(            \
                                fc, now, FCG_EVENT_REASON_OLDEST))         \
                            _ret = _FCG_HT(p, insert_hashed)(             \
                                &fc->ht_head, fc->buckets,                 \
                                FCG_LAYOUT_HASH_BASE(fc),                  \
                                entry,                                     \
                                hashes[idx & (FC_CACHE_BULK_CTX_COUNT - 1u)]); \
                        if (_ret == NULL) {                                \
                            fc->stats.fills++;                             \
                            _FCG_INT(p, result_set_filled)(&results[idx], \
                                FCG_LAYOUT_ENTRY_INDEX(fc, entry));        \
                        } else if (_ret != entry) {                        \
                            RIX_ASSUME_NONNULL(entry);                     \
                            _FCG_INT(p, free_entry)(fc, entry,            \
                                                    FCG_EVENT_REASON_ROLLBACK); \
                            RIX_ASSUME_NONNULL(_ret);                      \
                            flow_timestamp_store(&_ret->hdr.meta,          \
                                                 (now),                    \
                                                 _FCG_TS_SHIFT(fc));       \
                            _FCG_INT(p, result_set_filled)(               \
                                &results[idx],                              \
                                FCG_LAYOUT_ENTRY_INDEX(fc, _ret));         \
                        } else {                                           \
                            RIX_ASSUME_NONNULL(entry);                     \
                            _FCG_INT(p, free_entry)(fc, entry,            \
                                                    FCG_EVENT_REASON_ROLLBACK); \
                            fc->stats.fill_full++;                         \
                            _FCG_INT(p, result_set_miss)(                 \
                                &results[idx]);                             \
                        }                                                  \
                    }                                                      \
                }                                                          \
                /* Prefetch next free list head */                         \
                {                                                          \
                    _FCG_ENTRY_T(p) *_nf = FCG_FREE_LIST_FIRST_PTR(fc);    \
                    if (_nf != NULL)                                       \
                        rix_hash_prefetch_entry_of(_nf);                  \
                }                                                          \
            }                                                              \
        }                                                                  \
    }                                                                      \
}                                                                          \
                                                                           \
/* ----- del_bulk: remove by key --------------------------------------- */\
static void                                                                \
_FCG_API(p, del_bulk)(_FCG_CACHE_T(p) *fc,                              \
                       const _FCG_KEY_T(p) *keys,                         \
                       unsigned nb_keys)                                   \
{                                                                          \
    struct rix_hash_find_ctx_s stack_ctx[FC_CACHE_BULK_CTX_COUNT];         \
    struct rix_hash_find_ctx_s *ctx =                                      \
        (fc->bulk_ctx != NULL &&                                           \
         fc->bulk_ctx_count >= FC_CACHE_BULK_CTX_COUNT) ?                  \
        fc->bulk_ctx : stack_ctx;                                          \
    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;              \
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;                \
    const unsigned total = nb_keys + 3u * ahead_keys;                      \
    /* 4-stage pipeline: hash -> scan -> prefetch -> cmp+remove */           \
    for (unsigned i = 0; i < total; i += step_keys) {                      \
        /* Stage 1: hash_key_2bk */                                        \
        if (i < nb_keys) {                                                 \
            unsigned n = (i + step_keys <= nb_keys) ?                      \
                step_keys : (nb_keys - i);                                 \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, hash_key_2bk_masked)(                           \
                    &ctx[(i + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],        \
                    fc->buckets, &keys[i + j],                             \
                    fc->ht_head.rhh_mask, fc->ht_head.rhh_mask);           \
        }                                                                  \
        /* Stage 2: scan_bk (no empty tracking needed) */                   \
        if (i >= ahead_keys && i - ahead_keys < nb_keys) {                \
            unsigned base = i - ahead_keys;                                \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, scan_bk)(                                       \
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],     \
                    &fc->ht_head, fc->buckets);                            \
        }                                                                  \
        /* Stage 3: prefetch_node */                                       \
        if (i >= 2u * ahead_keys &&                                        \
            i - 2u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 2u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, prefetch_node)(                                 \
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],     \
                    FCG_LAYOUT_HASH_BASE(fc));                              \
        }                                                                  \
        /* Stage 4: cmp_key + remove on hit */                             \
        if (i >= 3u * ahead_keys &&                                        \
            i - 3u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 3u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++) {                             \
                unsigned idx = base + j;                                   \
                _FCG_ENTRY_T(p) *entry;                                   \
                entry = _FCG_HT(p, cmp_key)(                               \
                    &ctx[idx & (FC_CACHE_BULK_CTX_COUNT - 1u)],            \
                    FCG_LAYOUT_HASH_BASE(fc));                              \
                if (entry != NULL) {                                       \
                    _FCG_HT(p, remove)(&fc->ht_head, fc->buckets,        \
                                        FCG_LAYOUT_HASH_BASE(fc), entry);  \
                    _FCG_INT(p, free_entry)(fc, entry,                    \
                                            FCG_EVENT_REASON_DELETE);      \
                }                                                          \
            }                                                              \
        }                                                                  \
    }                                                                      \
}                                                                          \
                                                                           \
/* ----- del_idx_bulk: remove by pool index ---------------------------- */\
static void                                                                \
_FCG_API(p, del_idx_bulk)(_FCG_CACHE_T(p) *fc,                          \
                           const u32 *idxs,                           \
                           unsigned nb_idxs)                               \
{                                                                          \
    const unsigned ahead = FLOW_CACHE_LOOKUP_AHEAD_KEYS;                   \
    const unsigned step = FLOW_CACHE_LOOKUP_STEP_KEYS;                     \
    const unsigned total = nb_idxs + ahead;                                \
    /* 2-stage pipeline: prefetch entry, then remove */                    \
    for (unsigned i = 0; i < total; i += step) {                           \
        /* Stage 1: prefetch entry */                                      \
        if (i < nb_idxs) {                                                 \
            unsigned n = (i + step <= nb_idxs) ?                           \
                step : (nb_idxs - i);                                      \
            for (unsigned j = 0; j < n; j++) {                             \
                if (idxs[i + j] != 0u &&                                   \
                    idxs[i + j] <= fc->max_entries)                        \
                    rix_hash_prefetch_entry_of(                           \
                        FCG_LAYOUT_ENTRY_PTR(fc, idxs[i + j]));           \
            }                                                              \
        }                                                                  \
        /* Stage 2: remove */                                              \
        if (i >= ahead && i - ahead < nb_idxs) {                           \
            unsigned base = i - ahead;                                     \
            unsigned n = (base + step <= nb_idxs) ?                        \
                step : (nb_idxs - base);                                   \
            for (unsigned j = 0; j < n; j++) {                             \
                u32 eidx = idxs[base + j];                            \
                _FCG_ENTRY_T(p) *entry;                                   \
                if (eidx == 0u || eidx > fc->max_entries)                  \
                    continue;                                              \
                entry = FCG_LAYOUT_ENTRY_PTR(fc, eidx);                    \
                if (entry == NULL || flow_timestamp_is_zero(&entry->hdr.meta)) \
                    continue;                                              \
                _FCG_HT(p, remove)(&fc->ht_head, fc->buckets,            \
                                    FCG_LAYOUT_HASH_BASE(fc), entry);      \
                _FCG_INT(p, free_entry)(fc, entry,                        \
                                        FCG_EVENT_REASON_DELETE);          \
            }                                                              \
        }                                                                  \
    }                                                                      \
}

/*===========================================================================
 * Per-TU rix_hash_arch auto-initialization (constructor)
 *
 * Each arch-specific TU has its own static rix_hash_arch pointer.
 * Without this, rix_hash_bytes_fast() falls back to Generic
 * even when compiled with -mavx2.  The constructor runs once per
 * shared-library load (or at program start for static linking).
 *===========================================================================*/
#ifdef FC_ARCH_SUFFIX
#define _FC_RIX_ARCH_CTOR(prefix)                                         \
    __attribute__((constructor)) static void                               \
    _FCG_CAT(_fc_rix_arch_init_, prefix)(void)                            \
    {                                                                      \
        rix_hash_arch_init(RIX_HASH_ARCH_AUTO);                            \
    }
#else
#define _FC_RIX_ARCH_CTOR(prefix) /* no-op without FC_ARCH_SUFFIX */
#endif

/*===========================================================================
 * Top-level GENERATE macro
 *===========================================================================*/
#define FC_CACHE_GENERATE(prefix, pressure, hash_fn, cmp_fn)              \
    _FC_RIX_ARCH_CTOR(prefix)                                             \
    _FC_GENERATE_HT(prefix, hash_fn, cmp_fn)                              \
    _FC_GENERATE_INTERNAL(prefix)                                          \
    _FC_GENERATE_API(prefix, pressure, hash_fn)

/*===========================================================================
 * Ops table instance generation (for arch-specific builds)
 *
 * Usage in each arch .c file, after FC_CACHE_GENERATE:
 *   FC_OPS_TABLE(flow4, _avx2)
 *
 * This defines a const struct fc_flow4_ops fc_flow4_ops_avx2 = { ... }
 * pointing to the suffixed function names.
 *===========================================================================*/
#ifdef FC_ARCH_SUFFIX

#define _FC_OPS_FNAME(prefix, name) \
    _FCG_CAT(_FCG_CAT(fc_, _FCG_CAT(prefix, _cache_##name)), FC_ARCH_SUFFIX)

#define _FC_OPS_TNAME(prefix, suffix) \
    _FCG_CAT(_FCG_CAT(fc_, _FCG_CAT(prefix, _ops)), suffix)

#define FC_OPS_TABLE(prefix, suffix)                                           \
const struct fc_##prefix##_ops _FC_OPS_TNAME(prefix, suffix) = {               \
    .flush            = _FC_OPS_FNAME(prefix, flush),                          \
    .nb_entries       = _FC_OPS_FNAME(prefix, nb_entries),                     \
    .remove_idx       = _FC_OPS_FNAME(prefix, remove_idx),                     \
    .stats            = _FC_OPS_FNAME(prefix, stats),                          \
    .walk             = _FC_OPS_FNAME(prefix, walk),                           \
    .find_bulk        = _FC_OPS_FNAME(prefix, find_bulk),                      \
    .findadd_bulk     = _FC_OPS_FNAME(prefix, findadd_bulk),                   \
    .add_bulk         = _FC_OPS_FNAME(prefix, add_bulk),                       \
    .del_bulk         = _FC_OPS_FNAME(prefix, del_bulk),                       \
    .del_idx_bulk     = _FC_OPS_FNAME(prefix, del_idx_bulk),                   \
    .maintain         = _FC_OPS_FNAME(prefix, maintain),                       \
    .maintain_step_ex = _FC_OPS_FNAME(prefix, maintain_step_ex),               \
    .maintain_step    = _FC_OPS_FNAME(prefix, maintain_step),                  \
}

#endif /* FC_ARCH_SUFFIX */

#endif /* _FC_CACHE_GENERATE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
