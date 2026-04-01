/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * ft_table_generate.h - GENERATE macro for per-variant table implementations.
 *
 * Usage in .c files:
 *
 *   #include "flow4_table.h"
 *   // define layout hooks, hash_fn, cmp ...
 *   #include "ft_table_generate.h"
 *
 *   FT_TABLE_GENERATE(flow4, ft_flow4_hash_fn, ft_flow4_cmp)
 */

#ifndef _FT_TABLE_GENERATE_H_
#define _FT_TABLE_GENERATE_H_

#include <string.h>
#include <rix/rix_hash_slot.h>

/*===========================================================================
 * Token-paste helpers
 *===========================================================================*/
#define _FTG_CAT2(a, b)  a##b
#define _FTG_CAT(a, b)   _FTG_CAT2(a, b)

#define _FTG_HT(p, name)   _FTG_CAT(ft_, _FTG_CAT(p, _ht_##name))
#define _FTG_INT(p, name)   _FTG_CAT(ft_, _FTG_CAT(p, _##name))

#ifdef FT_ARCH_SUFFIX
#define _FTG_API(p, name)  _FTG_CAT(_FTG_CAT(ft_, _FTG_CAT(p, _table_##name)), FT_ARCH_SUFFIX)
#else
#define _FTG_API(p, name)  _FTG_CAT(ft_, _FTG_CAT(p, _table_##name))
#endif

#define _FTG_KEY_T(p)       struct _FTG_CAT(p, _key)
#define _FTG_RESULT_T(p)    struct _FTG_CAT(ft_, _FTG_CAT(p, _result))
#define _FTG_ENTRY_T(p)     struct _FTG_CAT(ft_, _FTG_CAT(p, _entry))
#define _FTG_TABLE_T(p)     struct _FTG_CAT(ft_, _FTG_CAT(p, _table))
#define _FTG_CONFIG_T(p)    struct _FTG_CAT(ft_, _FTG_CAT(p, _config))
#define _FTG_STATS_T(p)     struct _FTG_CAT(ft_, _FTG_CAT(p, _stats))
#define _FTG_HT_T(p)        struct _FTG_CAT(ft_, _FTG_CAT(p, _ht))

/*===========================================================================
 * FCORE function name helper (available when FTG_USE_FCORE is defined)
 *===========================================================================*/
#ifdef FTG_USE_FCORE
# define _FTG_FCORE(p, name) _FCORE_CAT(_fcore_, _FCORE_CAT(p, _##name))
#endif

/*===========================================================================
 * Entry validity / metadata hooks (overridable per-variant)
 *
 * FTG_USE_FCORE path:  cur_hash != 0 means "linked in a bucket".
 * Legacy path:         flags & flag_active.
 *===========================================================================*/
#ifndef FTG_ENTRY_IS_ACTIVE
# ifdef FTG_USE_FCORE
#  define FTG_ENTRY_IS_ACTIVE(entry, flag_active) \
       ((entry)->cur_hash != 0u)
# else
#  define FTG_ENTRY_IS_ACTIVE(entry, flag_active) \
       (((entry)->flags & (flag_active)) != 0u)
# endif
#endif

#ifndef FTG_ON_INSERT_SUCCESS
# ifdef FTG_USE_FCORE
#  define FTG_ON_INSERT_SUCCESS(entry, flag_active) (void)0
# else
#  define FTG_ON_INSERT_SUCCESS(entry, flag_active) \
       (entry)->flags = (flag_active)
# endif
#endif

#ifndef FTG_ENTRY_META_CLEAR_TAIL
# ifdef FTG_USE_FCORE
#  define FTG_ENTRY_META_CLEAR_TAIL(entry) \
       memset((entry)->reserved0, 0, sizeof((entry)->reserved0))
# else
#  define FTG_ENTRY_META_CLEAR_TAIL(entry) do {                                \
       (entry)->flags = 0u;                                                    \
       memset((entry)->reserved0, 0, sizeof((entry)->reserved0));              \
   } while (0)
# endif
#endif

/*===========================================================================
 * Layout hooks (overridable per-variant before #include)
 *===========================================================================*/
#ifndef FTG_LAYOUT_INIT_STORAGE
#define FTG_LAYOUT_INIT_STORAGE(ft, array, stride, entry_offset)               \
    do {                                                                       \
        (void)(stride);                                                        \
        (void)(entry_offset);                                                  \
        (ft)->pool = (array);                                                  \
    } while (0)
#endif

#ifndef FTG_LAYOUT_HASH_BASE
#define FTG_LAYOUT_HASH_BASE(ft) ((ft)->pool)
#endif

#ifndef FTG_LAYOUT_ENTRY_PTR
#define FTG_LAYOUT_ENTRY_PTR(ft, idx)                                          \
    (((unsigned)(idx) != RIX_NIL) ? &((ft)->pool[(unsigned)(idx) - 1u]) : NULL)
#endif

#ifndef FTG_LAYOUT_ENTRY_INDEX
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry) RIX_IDX_FROM_PTR((ft)->pool, (entry))
#endif

#ifndef FTG_LAYOUT_ENTRY_AT
#define FTG_LAYOUT_ENTRY_AT(ft, off0) (&((ft)->pool[(off0)]))
#endif

/*===========================================================================
 * Pipeline geometry
 *===========================================================================*/
#ifndef FT_TABLE_BULK_STEP_KEYS
#define FT_TABLE_BULK_STEP_KEYS 8u
#endif

#ifndef FT_TABLE_BULK_AHEAD_STEPS
#define FT_TABLE_BULK_AHEAD_STEPS 4u
#endif

#define FT_TABLE_BULK_AHEAD_KEYS \
    (FT_TABLE_BULK_STEP_KEYS * FT_TABLE_BULK_AHEAD_STEPS)

#ifndef FT_TABLE_BULK_CTX_RING
#define FT_TABLE_BULK_CTX_RING 128u
#endif

#ifndef FT_TABLE_GROW_OLD_BK_AHEAD
#define FT_TABLE_GROW_OLD_BK_AHEAD 2u
#endif

#ifndef FT_TABLE_GROW_REINSERT_AHEAD
#define FT_TABLE_GROW_REINSERT_AHEAD 8u
#endif

#ifndef FT_TABLE_GROW_CTX_RING
#define FT_TABLE_GROW_CTX_RING 64u
#endif

/*===========================================================================
 * Bulk-op body macros (selected by FTG_USE_FCORE before #include)
 *
 * These are called from inside FT_TABLE_GENERATE via _FTG_*_BODY macros.
 * The FCORE path delegates to _fcore_<p>_*_ functions generated by
 * FCORE_GENERATE.  The legacy path uses the original 2-stage pipeline.
 *===========================================================================*/
#ifdef FTG_USE_FCORE

/* sizeof(ft_flow4_result) == sizeof(uint32_t): safe to pass &results[0].entry_idx */
#define _FTG_FIND_BULK_BODY(p, ft, keys, nb_keys, results, hash_fn, cmp_fn)  \
    _FTG_FCORE(p, find_bulk_)((ft), (keys), (nb_keys), 0u,                   \
                              &(results)[0].entry_idx)

#define _FTG_ADD_ENTRY_BULK_BODY(p, ft, entry_idxv, nb_keys, results,         \
                                 hash_fn, cmp_fn)                              \
    _FTG_FCORE(p, add_idx_bulk_)((ft), (entry_idxv), (nb_keys), 0u,           \
                                 &(results)[0].entry_idx)

#define _FTG_DEL_BULK_BODY(p, ft, keys, nb_keys, results, hash_fn, cmp_fn)   \
    _FTG_FCORE(p, del_bulk_)((ft), (keys), (nb_keys),                         \
                             &(results)[0].entry_idx)

#define _FTG_WALK_BODY(p, ft, cb, arg)                                        \
    return _FTG_FCORE(p, walk_)((ft), (cb), (arg))

#define _FTG_FLUSH_BODY(p, ft)                                                \
    _FTG_FCORE(p, flush_)(ft)

#else /* !FTG_USE_FCORE — legacy 2-stage pipeline */

#define _FTG_FIND_BULK_BODY(p, ft, keys, nb_keys, results, hash_fn, cmp_fn)  \
    do {                                                                       \
    const unsigned _step_keys = FT_TABLE_BULK_STEP_KEYS;                       \
    const unsigned _ahead_keys = FT_TABLE_BULK_AHEAD_KEYS;                     \
    const unsigned _total = (nb_keys) + _ahead_keys;                           \
    union rix_hash_hash_u _hashes[FT_TABLE_BULK_CTX_RING];                     \
    for (unsigned _i = 0; _i < _total; _i += _step_keys) {                    \
        if (_i < (nb_keys)) {                                                  \
            unsigned _n = (_i + _step_keys <= (nb_keys))                       \
                       ? _step_keys : ((nb_keys) - _i);                        \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _i + _j;                                       \
                union rix_hash_hash_u _h =                                     \
                    hash_fn(&(keys)[_idx], (ft)->start_mask);                  \
                unsigned _bk0, _bk1; u32 _fp_unused;                          \
                _hashes[_idx & (FT_TABLE_BULK_CTX_RING - 1u)] = _h;           \
                _rix_hash_buckets(_h, (ft)->ht_head.rhh_mask,                  \
                                  &_bk0, &_bk1, &_fp_unused);                 \
                rix_hash_prefetch_bucket_of(&(ft)->buckets[_bk0]);             \
                if (_bk1 != _bk0)                                             \
                    rix_hash_prefetch_bucket_of(&(ft)->buckets[_bk1]);         \
            }                                                                  \
        }                                                                      \
        if (_i >= _ahead_keys && _i - _ahead_keys < (nb_keys)) {              \
            unsigned _base = _i - _ahead_keys;                                 \
            unsigned _n = (_base + _step_keys <= (nb_keys))                    \
                       ? _step_keys : ((nb_keys) - _base);                     \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _base + _j;                                    \
                _FTG_ENTRY_T(p) *_entry;                                       \
                (ft)->stats.lookups++;                                         \
                _entry = _FTG_INT(p, find_hashed_)(                            \
                    (ft), &(keys)[_idx],                                       \
                    _hashes[_idx & (FT_TABLE_BULK_CTX_RING - 1u)]);            \
                if (_entry != NULL) {                                          \
                    (ft)->stats.hits++;                                         \
                    (results)[_idx].entry_idx =                                \
                        FTG_LAYOUT_ENTRY_INDEX((ft), _entry);                  \
                } else {                                                       \
                    (ft)->stats.misses++;                                       \
                    (results)[_idx].entry_idx = 0u;                            \
                }                                                              \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    } while (0)

#define _FTG_ADD_ENTRY_BULK_BODY(p, ft, entry_idxv, nb_keys, results,         \
                                 hash_fn, cmp_fn)                              \
    do {                                                                       \
    const unsigned _step_keys = FT_TABLE_BULK_STEP_KEYS;                       \
    const unsigned _ahead_keys = FT_TABLE_BULK_AHEAD_KEYS;                     \
    const unsigned _total = (nb_keys) + _ahead_keys;                           \
    union rix_hash_hash_u _hashes[FT_TABLE_BULK_CTX_RING];                     \
    for (unsigned _i = 0; _i < (nb_keys); _i++) {                             \
        _FTG_ENTRY_T(p) *_entry;                                              \
        (results)[_i].entry_idx = 0u;                                          \
        if ((entry_idxv)[_i] == 0u || (entry_idxv)[_i] > (ft)->max_entries)   \
            continue;                                                          \
        _entry = FTG_LAYOUT_ENTRY_PTR((ft), (entry_idxv)[_i]);                \
        RIX_ASSUME_NONNULL(_entry);                                            \
        rix_hash_prefetch_key(&_entry->key);                                   \
    }                                                                          \
    for (unsigned _i = 0; _i < _total; _i += _step_keys) {                    \
        if (_i < (nb_keys)) {                                                  \
            unsigned _n = (_i + _step_keys <= (nb_keys))                       \
                       ? _step_keys : ((nb_keys) - _i);                        \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _i + _j;                                       \
                uint32_t _eidx = (entry_idxv)[_idx];                           \
                _FTG_ENTRY_T(p) *_entry; unsigned _bk0, _bk1;                 \
                u32 _fp_unused; union rix_hash_hash_u _h;                      \
                if (_eidx == 0u || _eidx > (ft)->max_entries) continue;        \
                _entry = FTG_LAYOUT_ENTRY_PTR((ft), _eidx);                    \
                RIX_ASSUME_NONNULL(_entry);                                    \
                _h = hash_fn(&_entry->key, (ft)->start_mask);                  \
                _hashes[_idx & (FT_TABLE_BULK_CTX_RING - 1u)] = _h;           \
                _rix_hash_buckets(_h, (ft)->ht_head.rhh_mask,                  \
                                  &_bk0, &_bk1, &_fp_unused);                 \
                rix_hash_prefetch_bucket_of(&(ft)->buckets[_bk0]);             \
                if (_bk1 != _bk0)                                             \
                    rix_hash_prefetch_bucket_of(&(ft)->buckets[_bk1]);         \
            }                                                                  \
        }                                                                      \
        if (_i >= _ahead_keys && _i - _ahead_keys < (nb_keys)) {              \
            unsigned _base = _i - _ahead_keys;                                 \
            unsigned _n = (_base + _step_keys <= (nb_keys))                    \
                       ? _step_keys : ((nb_keys) - _base);                     \
            for (unsigned _j = 0; _j < _n; _j++) {                            \
                unsigned _idx = _base + _j;                                    \
                uint32_t _eidx = (entry_idxv)[_idx];                           \
                _FTG_ENTRY_T(p) *_entry; uint32_t _out_idx; int _rc;           \
                if (_eidx == 0u || _eidx > (ft)->max_entries) continue;        \
                _entry = FTG_LAYOUT_ENTRY_PTR((ft), _eidx);                    \
                RIX_ASSUME_NONNULL(_entry);                                    \
                if (_FTG_INT(p, entry_is_active_)(_entry)) {                   \
                    (ft)->stats.add_existing++;                                \
                    (results)[_idx].entry_idx = _eidx;                         \
                    continue;                                                  \
                }                                                              \
                _FTG_INT(p, entry_meta_clear_)(_entry);                        \
                _out_idx = _eidx;                                              \
                _rc = _FTG_INT(p, insert_hashed_)(                             \
                    (ft), _entry,                                              \
                    _hashes[_idx & (FT_TABLE_BULK_CTX_RING - 1u)],             \
                    &_out_idx);                                                \
                if (_rc == 0) {                                                \
                    (ft)->stats.adds++;                                        \
                    (results)[_idx].entry_idx = _out_idx;                      \
                } else {                                                       \
                    _FTG_INT(p, entry_meta_clear_)(_entry);                    \
                    if (_rc > 0) {                                             \
                        (ft)->stats.add_existing++;                            \
                        (results)[_idx].entry_idx = _out_idx;                  \
                    } else {                                                   \
                        (ft)->stats.add_failed++;                              \
                        if ((ft)->nb_bk < (ft)->max_nb_bk)                    \
                            _FTG_INT(p, mark_need_grow_)((ft));               \
                        (results)[_idx].entry_idx = 0u;                        \
                    }                                                          \
                }                                                              \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    } while (0)

#define _FTG_DEL_BULK_BODY(p, ft, keys, nb_keys, results, hash_fn, cmp_fn)   \
    do {                                                                       \
    for (unsigned _i = 0; _i < (nb_keys); _i++)                               \
        (results)[_i].entry_idx = _FTG_API(p, del)((ft), &(keys)[_i]);       \
    } while (0)

#define _FTG_WALK_BODY(p, ft, cb, arg)                                        \
    do {                                                                       \
    for (unsigned _i = 1u; _i <= (ft)->max_entries; _i++) {                   \
        const _FTG_ENTRY_T(p) *_entry = FTG_LAYOUT_ENTRY_PTR((ft), _i);      \
        if (_entry == NULL) continue;                                          \
        RIX_ASSUME_NONNULL(_entry);                                            \
        if (!_FTG_INT(p, entry_is_active_)(_entry)) continue;                 \
        if ((cb)(_i, (arg)) != 0) return 1;                                   \
    }                                                                          \
    } while (0)

#define _FTG_FLUSH_BODY(p, ft)                                                \
    do {                                                                       \
    memset((ft)->buckets, 0,                                                   \
           _FTG_INT(p, bucket_bytes_)((ft)->nb_bk));                          \
    _FTG_HT(p, init)(&(ft)->ht_head, (ft)->nb_bk);                           \
    (ft)->need_grow = 0u;                                                      \
    for (unsigned _i = 1u; _i <= (ft)->max_entries; _i++) {                   \
        _FTG_ENTRY_T(p) *_entry = FTG_LAYOUT_ENTRY_PTR((ft), _i);            \
        if (_entry != NULL) _FTG_INT(p, entry_meta_clear_)(_entry);           \
    }                                                                          \
    } while (0)

#endif /* FTG_USE_FCORE */

/*===========================================================================
 * FT_TABLE_GENERATE(prefix, default_min_nb_bk, default_max_nb_bk,
 *                   default_grow_fill_pct, flag_active, hash_fn, cmp_fn)
 *
 * When FTG_USE_FCORE is defined, flag_active is ignored and bulk
 * operations delegate to the FCORE_GENERATE functions (_fcore_<p>_*).
 * Entry validity is determined by cur_hash != 0 (bucket linkage).
 *===========================================================================*/
#define FT_TABLE_GENERATE(p, default_min_nb_bk, default_max_nb_bk,             \
                          default_grow_fill_pct, flag_active, hash_fn, cmp_fn)  \
                                                                               \
/* --- Internal helpers -------------------------------------------------- */ \
                                                                               \
static inline _FTG_ENTRY_T(p) *                                               \
_FTG_INT(p, hash_base_)(_FTG_TABLE_T(p) *ft)                                 \
{                                                                             \
    return (_FTG_ENTRY_T(p) *)(void *)ft;                                     \
}                                                                             \
                                                                               \
static inline int                                                             \
_FTG_INT(p, entry_is_active_)(const _FTG_ENTRY_T(p) *entry)                  \
{                                                                             \
    return FTG_ENTRY_IS_ACTIVE(entry, flag_active);                            \
}                                                                             \
                                                                               \
static inline void                                                            \
_FTG_INT(p, entry_meta_clear_)(_FTG_ENTRY_T(p) *entry)                       \
{                                                                             \
    entry->cur_hash = 0u;                                                     \
    entry->hash0 = 0u;                                                        \
    entry->hash1 = 0u;                                                        \
    entry->slot = 0u;                                                         \
    FTG_ENTRY_META_CLEAR_TAIL(entry);                                         \
}                                                                             \
                                                                               \
static inline unsigned                                                        \
_FTG_INT(p, default_start_nb_bk_)(unsigned max_entries)                       \
{                                                                             \
    unsigned hinted;                                                          \
    hinted = (max_entries + (RIX_HASH_BUCKET_ENTRY_SZ - 1u))                  \
           / RIX_HASH_BUCKET_ENTRY_SZ;                                        \
    if (hinted < (default_min_nb_bk))                                         \
        hinted = (default_min_nb_bk);                                         \
    return ft_roundup_pow2_u32(hinted);                                       \
}                                                                             \
                                                                               \
static inline unsigned                                                        \
_FTG_INT(p, required_nb_bk_)(unsigned entries, unsigned fill_pct)             \
{                                                                             \
    uint64_t need_slots;                                                      \
    unsigned need_bk;                                                         \
    if (entries == 0u)                                                        \
        return (default_min_nb_bk);                                           \
    need_slots = ((uint64_t)entries * 100u + (uint64_t)fill_pct - 1u)         \
               / (uint64_t)fill_pct;                                          \
    need_bk = (unsigned)((need_slots                                          \
               + (uint64_t)RIX_HASH_BUCKET_ENTRY_SZ - 1u)                    \
               / (uint64_t)RIX_HASH_BUCKET_ENTRY_SZ);                        \
    if (need_bk < (default_min_nb_bk))                                       \
        need_bk = (default_min_nb_bk);                                       \
    return ft_roundup_pow2_u32(need_bk);                                      \
}                                                                             \
                                                                               \
static inline unsigned                                                        \
_FTG_INT(p, fill_pct_)(const _FTG_TABLE_T(p) *ft)                            \
{                                                                             \
    uint64_t slots;                                                           \
    if (ft->nb_bk == 0u)                                                      \
        return 0u;                                                            \
    slots = (uint64_t)ft->nb_bk * (uint64_t)RIX_HASH_BUCKET_ENTRY_SZ;        \
    return (unsigned)(((uint64_t)ft->ht_head.rhh_nb * 100u) / slots);         \
}                                                                             \
                                                                               \
static inline void                                                            \
_FTG_INT(p, mark_need_grow_)(_FTG_TABLE_T(p) *ft)                            \
{                                                                             \
    if (ft->need_grow == 0u) {                                                \
        ft->need_grow = 1u;                                                   \
        ft->stats.grow_marks++;                                               \
    }                                                                         \
}                                                                             \
                                                                               \
static inline size_t                                                          \
_FTG_INT(p, bucket_bytes_)(unsigned nb_bk)                                    \
{                                                                             \
    return (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);                   \
}                                                                             \
                                                                               \
static int                                                                    \
_FTG_INT(p, alloc_buckets_)(_FTG_TABLE_T(p) *ft,                             \
                            unsigned nb_bk,                                   \
                            struct rix_hash_bucket_s **out)                    \
{                                                                             \
    size_t bytes = _FTG_INT(p, bucket_bytes_)(nb_bk);                         \
    void *ptr;                                                                \
    ptr = ft->bucket_alloc.alloc(bytes,                                       \
                                 _Alignof(struct rix_hash_bucket_s),          \
                                 ft->bucket_alloc.arg);                       \
    if (ptr == NULL)                                                          \
        return -1;                                                            \
    memset(ptr, 0, bytes);                                                    \
    *out = (struct rix_hash_bucket_s *)ptr;                                   \
    return 0;                                                                 \
}                                                                             \
                                                                               \
static inline void                                                            \
_FTG_INT(p, init_storage_)(_FTG_TABLE_T(p) *ft, void *array,                 \
                           size_t stride, size_t entry_offset)                \
{                                                                             \
    FTG_LAYOUT_INIT_STORAGE(ft, array, stride, entry_offset);                 \
}                                                                             \
                                                                               \
/* find_hashed_: single-key lookup using precomputed hash */                  \
static inline _FTG_ENTRY_T(p) *                                               \
_FTG_INT(p, find_hashed_)(_FTG_TABLE_T(p) *ft,                               \
                          const _FTG_KEY_T(p) *key,                           \
                          union rix_hash_hash_u h)                            \
{                                                                             \
    unsigned bk0, bk1;                                                        \
    u32 fp;                                                                   \
    u32 hits;                                                                 \
                                                                               \
    _rix_hash_buckets(h, ft->ht_head.rhh_mask, &bk0, &bk1, &fp);            \
    hits = _RIX_HASH_FIND_U32X16(ft->buckets[bk0].hash, fp);                \
    while (hits != 0u) {                                                      \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        unsigned idx = ft->buckets[bk0].idx[bit];                             \
        _FTG_ENTRY_T(p) *entry;                                              \
        hits &= hits - 1u;                                                    \
        if (idx == (unsigned)RIX_NIL)                                         \
            continue;                                                         \
        entry = FTG_LAYOUT_ENTRY_PTR(ft, idx);                                \
        RIX_ASSUME_NONNULL(entry);                                            \
        if (cmp_fn(&entry->key, key) == 0)                                    \
            return entry;                                                     \
    }                                                                         \
    if (bk1 == bk0)                                                           \
        return NULL;                                                          \
    hits = _RIX_HASH_FIND_U32X16(ft->buckets[bk1].hash, fp);                \
    while (hits != 0u) {                                                      \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        unsigned idx = ft->buckets[bk1].idx[bit];                             \
        _FTG_ENTRY_T(p) *entry;                                              \
        hits &= hits - 1u;                                                    \
        if (idx == (unsigned)RIX_NIL)                                         \
            continue;                                                         \
        entry = FTG_LAYOUT_ENTRY_PTR(ft, idx);                                \
        RIX_ASSUME_NONNULL(entry);                                            \
        if (cmp_fn(&entry->key, key) == 0)                                    \
            return entry;                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                               \
/* insert_hashed_: insert pre-hashed entry into hash table */                 \
static inline int                                                             \
_FTG_INT(p, insert_hashed_)(_FTG_TABLE_T(p) *ft,                             \
                            _FTG_ENTRY_T(p) *entry,                           \
                            const union rix_hash_hash_u h,                    \
                            uint32_t *entry_idx_out)                          \
{                                                                             \
    _FTG_ENTRY_T(p) *ret;                                                    \
    entry->hash0 = h.val32[0];                                                \
    entry->hash1 = h.val32[1];                                                \
    ret = _FTG_HT(p, insert_hashed)(&ft->ht_head, ft->buckets,               \
                                     _FTG_INT(p, hash_base_)(ft),             \
                                     entry, h);                               \
    if (ret == NULL) {                                                        \
        FTG_ON_INSERT_SUCCESS(entry, flag_active);                             \
        *entry_idx_out = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                   \
        if (_FTG_INT(p, fill_pct_)(ft) >= ft->grow_fill_pct &&                \
            ft->nb_bk < ft->max_nb_bk)                                       \
            _FTG_INT(p, mark_need_grow_)(ft);                                 \
        return 0;                                                             \
    }                                                                         \
    if (ret != entry) {                                                       \
        *entry_idx_out = FTG_LAYOUT_ENTRY_INDEX(ft, ret);                     \
        return 1; /* duplicate found */                                       \
    }                                                                         \
    *entry_idx_out = 0u;                                                      \
    return -1; /* table full */                                               \
}                                                                             \
                                                                               \
/* rehash_insert_hashed_: for grow_2x, insert into new bucket array */        \
static inline int                                                             \
_FTG_INT(p, rehash_insert_hashed_)(_FTG_HT_T(p) *head,                       \
                                   struct rix_hash_bucket_s *buckets,         \
                                   _FTG_TABLE_T(p) *ft,                       \
                                   _FTG_ENTRY_T(p) *entry,                    \
                                   union rix_hash_hash_u h)                   \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned bk0, bk1;                                                        \
    u32 fp;                                                                   \
    _rix_hash_buckets(h, mask, &bk0, &bk1, &fp);                             \
    entry->cur_hash = h.val32[0];                                             \
    for (unsigned pass = 0u; pass < 2u; pass++) {                             \
        unsigned bki = (pass == 0u) ? bk0 : bk1;                             \
        struct rix_hash_bucket_s *bk = &buckets[bki];                         \
        u32 nilm = _RIX_HASH_FIND_U32X16(bk->hash, 0u);                     \
        if (nilm != 0u) {                                                     \
            unsigned s = (unsigned)__builtin_ctz(nilm);                       \
            bk->hash[s] = fp;                                                 \
            bk->idx[s] = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                   \
            if (pass == 1u)                                                   \
                entry->cur_hash = h.val32[1];                                 \
            entry->slot = (uint16_t)s;                                        \
            FTG_ON_INSERT_SUCCESS(entry, flag_active);              \
            head->rhh_nb++;                                                   \
            return 0;                                                         \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        struct rix_hash_bucket_s *bk;                                         \
        pos = _FTG_HT(p, kickout)(buckets, _FTG_INT(p, hash_base_)(ft),      \
                                  mask, bk0, RIX_HASH_FOLLOW_DEPTH);          \
        if (pos >= 0) {                                                       \
            bki = bk0;                                                        \
        } else {                                                              \
            pos = _FTG_HT(p, kickout)(buckets,                                \
                                      _FTG_INT(p, hash_base_)(ft),            \
                                      mask, bk1, RIX_HASH_FOLLOW_DEPTH);      \
            if (pos < 0)                                                      \
                return -1;                                                    \
            bki = bk1;                                                        \
            entry->cur_hash = h.val32[1];                                     \
        }                                                                     \
        bk = &buckets[bki];                                                   \
        bk->hash[pos] = fp;                                                   \
        bk->idx[pos] = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                     \
        entry->slot = (uint16_t)(unsigned)pos;                                \
        FTG_ON_INSERT_SUCCESS(entry, flag_active);                             \
        head->rhh_nb++;                                                       \
        return 0;                                                             \
    }                                                                         \
}                                                                             \
                                                                               \
/* --- init_ex ----------------------------------------------------------- */ \
                                                                               \
int                                                                           \
_FTG_API(p, init_ex)(_FTG_TABLE_T(p) *ft,                                    \
                     void *array,                                             \
                     unsigned max_entries,                                     \
                     size_t stride,                                           \
                     size_t entry_offset,                                     \
                     const _FTG_CONFIG_T(p) *cfg)                             \
{                                                                             \
    _FTG_CONFIG_T(p) defcfg;                                                  \
    struct rix_hash_bucket_s *buckets;                                        \
    unsigned start_nb_bk, max_nb_bk;                                          \
                                                                               \
    if (ft == NULL || array == NULL || max_entries == 0u)                      \
        return -1;                                                            \
    memset(&defcfg, 0, sizeof(defcfg));                                       \
    if (cfg == NULL)                                                          \
        cfg = &defcfg;                                                        \
    if (cfg->bucket_alloc.alloc == NULL ||                                    \
        cfg->bucket_alloc.free == NULL)                                       \
        return -1;                                                            \
                                                                               \
    RIX_ASSERT(stride >= sizeof(_FTG_ENTRY_T(p)));                            \
    RIX_ASSERT(entry_offset + sizeof(_FTG_ENTRY_T(p)) <= stride);             \
    RIX_ASSERT(FT_PTR_IS_ALIGNED(FT_BYTE_PTR_ADD(array, entry_offset),       \
                                 _Alignof(_FTG_ENTRY_T(p))));                 \
                                                                               \
    start_nb_bk = cfg->start_nb_bk                                           \
        ? ft_roundup_pow2_u32(cfg->start_nb_bk)                              \
        : _FTG_INT(p, default_start_nb_bk_)(max_entries);                     \
    max_nb_bk = cfg->max_nb_bk                                               \
        ? ft_roundup_pow2_u32(cfg->max_nb_bk)                                \
        : (default_max_nb_bk);                                              \
    if (start_nb_bk < _FTG_INT(p, default_start_nb_bk_)(max_entries))         \
        start_nb_bk = _FTG_INT(p, default_start_nb_bk_)(max_entries);         \
    if (max_nb_bk < start_nb_bk)                                             \
        max_nb_bk = start_nb_bk;                                             \
                                                                               \
    memset(ft, 0, sizeof(*ft));                                               \
    ft->bucket_alloc = cfg->bucket_alloc;                                     \
    ft->grow_fill_pct = cfg->grow_fill_pct                                    \
        ? cfg->grow_fill_pct                                                  \
        : (default_grow_fill_pct);                                           \
    ft->max_entries = max_entries;                                            \
    ft->max_nb_bk = max_nb_bk;                                               \
    ft->start_mask = start_nb_bk - 1u;                                       \
    _FTG_INT(p, init_storage_)(ft, array, stride, entry_offset);              \
                                                                               \
    if (_FTG_INT(p, alloc_buckets_)(ft, start_nb_bk, &buckets) != 0)          \
        return -1;                                                            \
    ft->buckets = buckets;                                                    \
    ft->nb_bk = start_nb_bk;                                                 \
    _FTG_HT(p, init)(&ft->ht_head, start_nb_bk);                             \
    ft->need_grow = 0u;                                                       \
    return 0;                                                                 \
}                                                                             \
                                                                               \
/* --- init -------------------------------------------------------------- */ \
                                                                               \
int                                                                           \
_FTG_API(p, init)(_FTG_TABLE_T(p) *ft,                                       \
                  _FTG_ENTRY_T(p) *pool,                                      \
                  unsigned max_entries,                                        \
                  const _FTG_CONFIG_T(p) *cfg)                                \
{                                                                             \
    return _FTG_API(p, init_ex)(ft, pool, max_entries,                        \
                                sizeof(*pool), 0u, cfg);                      \
}                                                                             \
                                                                               \
/* --- destroy ----------------------------------------------------------- */ \
                                                                               \
void                                                                          \
_FTG_API(p, destroy)(_FTG_TABLE_T(p) *ft)                                    \
{                                                                             \
    if (ft == NULL)                                                           \
        return;                                                               \
    if (ft->buckets != NULL && ft->bucket_alloc.free != NULL) {               \
        ft->bucket_alloc.free(ft->buckets,                                    \
                              _FTG_INT(p, bucket_bytes_)(ft->nb_bk),          \
                              _Alignof(struct rix_hash_bucket_s),             \
                              ft->bucket_alloc.arg);                          \
    }                                                                         \
    memset(ft, 0, sizeof(*ft));                                               \
}                                                                             \
                                                                               \
/* --- flush ------------------------------------------------------------- */ \
                                                                               \
void                                                                          \
_FTG_API(p, flush)(_FTG_TABLE_T(p) *ft)                                      \
{                                                                             \
    if (ft == NULL || ft->buckets == NULL)                                     \
        return;                                                               \
    _FTG_FLUSH_BODY(p, ft);                                                                         \
}                                                                             \
                                                                               \
/* --- nb_entries -------------------------------------------------------- */ \
                                                                               \
unsigned                                                                      \
_FTG_API(p, nb_entries)(const _FTG_TABLE_T(p) *ft)                            \
{                                                                             \
    return ft == NULL ? 0u : ft->ht_head.rhh_nb;                              \
}                                                                             \
                                                                               \
unsigned                                                                      \
_FTG_API(p, nb_bk)(const _FTG_TABLE_T(p) *ft)                                \
{                                                                             \
    return ft == NULL ? 0u : ft->nb_bk;                                       \
}                                                                             \
                                                                               \
unsigned                                                                      \
_FTG_API(p, need_grow)(const _FTG_TABLE_T(p) *ft)                             \
{                                                                             \
    return ft == NULL ? 0u : ft->need_grow;                                   \
}                                                                             \
                                                                               \
void                                                                          \
_FTG_API(p, stats)(const _FTG_TABLE_T(p) *ft,                                \
                   _FTG_STATS_T(p) *out)                                      \
{                                                                             \
    if (out == NULL)                                                          \
        return;                                                               \
    if (ft == NULL) {                                                         \
        memset(out, 0, sizeof(*out));                                         \
        return;                                                               \
    }                                                                         \
    *out = ft->stats;                                                         \
}                                                                             \
                                                                               \
/* --- find (single) ----------------------------------------------------- */ \
                                                                               \
uint32_t                                                                      \
_FTG_API(p, find)(_FTG_TABLE_T(p) *ft,                                       \
                  const _FTG_KEY_T(p) *key)                                   \
{                                                                             \
    _FTG_ENTRY_T(p) *entry;                                                   \
    union rix_hash_hash_u h;                                                  \
                                                                               \
    if (ft == NULL || key == NULL || ft->buckets == NULL)                      \
        return 0u;                                                            \
    ft->stats.lookups++;                                                      \
    h = hash_fn(key, ft->start_mask);                                         \
    entry = _FTG_INT(p, find_hashed_)(ft, key, h);                            \
    if (entry == NULL) {                                                      \
        ft->stats.misses++;                                                   \
        return 0u;                                                            \
    }                                                                         \
    ft->stats.hits++;                                                         \
    return FTG_LAYOUT_ENTRY_INDEX(ft, entry);                                 \
}                                                                             \
                                                                               \
/* --- add_entry (single, by index) -------------------------------------- */ \
                                                                               \
uint32_t                                                                      \
_FTG_API(p, add_entry)(_FTG_TABLE_T(p) *ft,                                  \
                       uint32_t entry_idx)                                    \
{                                                                             \
    _FTG_ENTRY_T(p) *entry;                                                   \
    union rix_hash_hash_u h;                                                  \
    uint32_t out_idx = entry_idx;                                             \
    int rc;                                                                   \
                                                                               \
    if (ft == NULL || ft->buckets == NULL ||                                   \
        entry_idx == 0u || entry_idx > ft->max_entries)                       \
        return 0u;                                                            \
    entry = FTG_LAYOUT_ENTRY_PTR(ft, entry_idx);                              \
    RIX_ASSUME_NONNULL(entry);                                                \
    if (_FTG_INT(p, entry_is_active_)(entry)) {                               \
        ft->stats.add_existing++;                                             \
        return entry_idx;                                                     \
    }                                                                         \
    _FTG_INT(p, entry_meta_clear_)(entry);                                    \
    h = hash_fn(&entry->key, ft->start_mask);                                 \
    rc = _FTG_INT(p, insert_hashed_)(ft, entry, h, &out_idx);                 \
    if (rc == 0) {                                                            \
        ft->stats.adds++;                                                     \
        return out_idx;                                                       \
    }                                                                         \
    _FTG_INT(p, entry_meta_clear_)(entry);                                    \
    if (rc > 0) {                                                             \
        ft->stats.add_existing++;                                             \
        return out_idx;                                                       \
    }                                                                         \
    ft->stats.add_failed++;                                                   \
    if (ft->nb_bk < ft->max_nb_bk)                                           \
        _FTG_INT(p, mark_need_grow_)(ft);                                     \
    return 0u;                                                                \
}                                                                             \
                                                                               \
/* --- del (single, by key) ---------------------------------------------- */ \
                                                                               \
uint32_t                                                                      \
_FTG_API(p, del)(_FTG_TABLE_T(p) *ft,                                        \
                 const _FTG_KEY_T(p) *key)                                    \
{                                                                             \
    _FTG_ENTRY_T(p) *entry;                                                   \
    union rix_hash_hash_u h;                                                  \
    unsigned idx;                                                             \
                                                                               \
    if (ft == NULL || key == NULL || ft->buckets == NULL)                      \
        return 0u;                                                            \
    h = hash_fn(key, ft->start_mask);                                         \
    entry = _FTG_INT(p, find_hashed_)(ft, key, h);                            \
    if (entry == NULL) {                                                      \
        ft->stats.del_miss++;                                                 \
        return 0u;                                                            \
    }                                                                         \
    idx = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                                  \
    if (_FTG_HT(p, remove)(&ft->ht_head, ft->buckets,                        \
                           _FTG_INT(p, hash_base_)(ft), entry) == NULL)       \
        return 0u;                                                            \
    ft->stats.dels++;                                                         \
    _FTG_INT(p, entry_meta_clear_)(entry);                                    \
    return idx;                                                               \
}                                                                             \
                                                                               \
/* --- del_idx (single, by index) ---------------------------------------- */ \
                                                                               \
uint32_t                                                                      \
_FTG_API(p, del_idx)(_FTG_TABLE_T(p) *ft,                                    \
                     uint32_t entry_idx)                                      \
{                                                                             \
    _FTG_ENTRY_T(p) *entry;                                                   \
                                                                               \
    if (ft == NULL || ft->buckets == NULL ||                                   \
        entry_idx == 0u || entry_idx > ft->max_entries)                       \
        return 0u;                                                            \
    entry = FTG_LAYOUT_ENTRY_PTR(ft, entry_idx);                              \
    RIX_ASSUME_NONNULL(entry);                                                \
    if (!_FTG_INT(p, entry_is_active_)(entry))                                \
        return 0u;                                                            \
    if (_FTG_HT(p, remove)(&ft->ht_head, ft->buckets,                        \
                           _FTG_INT(p, hash_base_)(ft), entry) == NULL)       \
        return 0u;                                                            \
    ft->stats.dels++;                                                         \
    _FTG_INT(p, entry_meta_clear_)(entry);                                    \
    return entry_idx;                                                         \
}                                                                             \
                                                                               \
/* --- find_bulk --------------------------------------------------------- */ \
                                                                               \
void                                                                          \
_FTG_API(p, find_bulk)(_FTG_TABLE_T(p) *ft,                                  \
                       const _FTG_KEY_T(p) *keys,                             \
                       unsigned nb_keys,                                       \
                       _FTG_RESULT_T(p) *results)                             \
{                                                                             \
    if (results == NULL)                                                       \
        return;                                                               \
    if (ft == NULL || ft->buckets == NULL || keys == NULL) {                   \
        for (unsigned i = 0; i < nb_keys; i++)                                \
            results[i].entry_idx = 0u;                                        \
        return;                                                               \
    }                                                                         \
    _FTG_FIND_BULK_BODY(p, ft, keys, nb_keys, results, hash_fn, cmp_fn);     \
}                                                                             \
                                                                               \
/* --- add_entry_bulk ---------------------------------------------------- */ \
                                                                               \
void                                                                          \
_FTG_API(p, add_entry_bulk)(_FTG_TABLE_T(p) *ft,                             \
                            const uint32_t *entry_idxv,                       \
                            unsigned nb_keys,                                  \
                            _FTG_RESULT_T(p) *results)                        \
{                                                                             \
    if (results == NULL)                                                       \
        return;                                                               \
    if (ft == NULL || ft->buckets == NULL || entry_idxv == NULL) {             \
        for (unsigned i = 0; i < nb_keys; i++)                                \
            results[i].entry_idx = 0u;                                        \
        return;                                                               \
    }                                                                         \
    _FTG_ADD_ENTRY_BULK_BODY(p, ft, entry_idxv, nb_keys, results,             \
                             hash_fn, cmp_fn);                                \
}                                                                             \
                                                                               \
/* --- del_bulk ---------------------------------------------------------- */ \
                                                                               \
void                                                                          \
_FTG_API(p, del_bulk)(_FTG_TABLE_T(p) *ft,                                   \
                      const _FTG_KEY_T(p) *keys,                              \
                      unsigned nb_keys,                                        \
                      _FTG_RESULT_T(p) *results)                              \
{                                                                             \
    if (results == NULL)                                                       \
        return;                                                               \
    if (ft == NULL || ft->buckets == NULL || keys == NULL) {                   \
        for (unsigned i = 0; i < nb_keys; i++)                                \
            results[i].entry_idx = 0u;                                        \
        return;                                                               \
    }                                                                         \
    _FTG_DEL_BULK_BODY(p, ft, keys, nb_keys, results, hash_fn, cmp_fn);      \
}                                                                             \
                                                                               \
/* --- walk -------------------------------------------------------------- */ \
                                                                               \
int                                                                           \
_FTG_API(p, walk)(_FTG_TABLE_T(p) *ft,                                       \
                  int (*cb)(uint32_t entry_idx, void *arg),                   \
                  void *arg)                                                  \
{                                                                             \
    if (ft == NULL || cb == NULL)                                              \
        return -1;                                                            \
    _FTG_WALK_BODY(p, ft, cb, arg);                                           \
    return 0;                                                                 \
}                                                                             \
                                                                               \
/* --- grow_2x ----------------------------------------------------------- */ \
                                                                               \
int                                                                           \
_FTG_API(p, grow_2x)(_FTG_TABLE_T(p) *ft)                                    \
{                                                                             \
    struct rix_hash_bucket_s *new_buckets;                                    \
    _FTG_HT_T(p) new_head;                                                   \
    struct { _FTG_ENTRY_T(p) *entry; union rix_hash_hash_u hash; }            \
        ctx[FT_TABLE_GROW_CTX_RING];                                          \
    unsigned new_nb_bk, new_mask, old_mask;                                   \
    unsigned produced = 0u, consumed = 0u;                                    \
                                                                               \
    if (ft == NULL || ft->buckets == NULL)                                     \
        return -1;                                                            \
    if (ft->nb_bk >= ft->max_nb_bk) {                                        \
        ft->stats.grow_failures++;                                            \
        return -1;                                                            \
    }                                                                         \
    new_nb_bk = ft->nb_bk << 1;                                              \
    if (new_nb_bk == 0u || new_nb_bk > ft->max_nb_bk) {                      \
        ft->stats.grow_failures++;                                            \
        return -1;                                                            \
    }                                                                         \
    if (_FTG_INT(p, alloc_buckets_)(ft, new_nb_bk, &new_buckets) != 0) {      \
        ft->stats.grow_failures++;                                            \
        return -1;                                                            \
    }                                                                         \
    _FTG_HT(p, init)(&new_head, new_nb_bk);                                  \
    new_mask = new_head.rhh_mask;                                             \
    old_mask = ft->ht_head.rhh_mask;                                          \
                                                                               \
    for (unsigned bk = 0u;                                                    \
         bk < FT_TABLE_GROW_OLD_BK_AHEAD && bk <= old_mask;                  \
         bk++)                                                                \
        rix_hash_prefetch_bucket_indices_of(&ft->buckets[bk]);                \
                                                                               \
    for (unsigned bk = 0u; bk <= old_mask; bk++) {                            \
        const struct rix_hash_bucket_s *old_bk = &ft->buckets[bk];           \
        unsigned pfbk = bk + FT_TABLE_GROW_OLD_BK_AHEAD;                     \
        if (pfbk <= old_mask)                                                 \
            rix_hash_prefetch_bucket_indices_of(&ft->buckets[pfbk]);          \
                                                                               \
        for (unsigned s = 0u; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {           \
            unsigned idx = old_bk->idx[s];                                    \
            _FTG_ENTRY_T(p) *entry;                                           \
            if (idx == (unsigned)RIX_NIL)                                     \
                continue;                                                     \
            entry = FTG_LAYOUT_ENTRY_PTR(ft, idx);                            \
            RIX_ASSUME_NONNULL(entry);                                        \
            rix_hash_prefetch_entry_of(entry);                                \
        }                                                                     \
                                                                               \
        for (unsigned s = 0u; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {           \
            unsigned idx = old_bk->idx[s];                                    \
            _FTG_ENTRY_T(p) *entry;                                           \
            union rix_hash_hash_u h;                                          \
            unsigned bk0;                                                     \
            u32 fp_unused;                                                    \
            if (idx == (unsigned)RIX_NIL)                                     \
                continue;                                                     \
            entry = FTG_LAYOUT_ENTRY_PTR(ft, idx);                            \
            RIX_ASSUME_NONNULL(entry);                                        \
            h.val32[0] = entry->hash0;                                        \
            h.val32[1] = entry->hash1;                                        \
            ctx[produced & (FT_TABLE_GROW_CTX_RING - 1u)].entry = entry;      \
            ctx[produced & (FT_TABLE_GROW_CTX_RING - 1u)].hash = h;          \
            {                                                                 \
                unsigned _bk1;                                                \
                _rix_hash_buckets(h, new_mask, &bk0, &_bk1, &fp_unused);     \
            }                                                                 \
            rix_hash_prefetch_bucket_hashes_of(&new_buckets[bk0]);            \
            produced++;                                                       \
                                                                               \
            if (produced - consumed > FT_TABLE_GROW_REINSERT_AHEAD) {         \
                unsigned ci = consumed & (FT_TABLE_GROW_CTX_RING - 1u);       \
                if (_FTG_INT(p, rehash_insert_hashed_)(                       \
                        &new_head, new_buckets, ft,                           \
                        ctx[ci].entry, ctx[ci].hash) != 0) {                  \
                    ft->bucket_alloc.free(                                    \
                        new_buckets,                                          \
                        _FTG_INT(p, bucket_bytes_)(new_nb_bk),                \
                        _Alignof(struct rix_hash_bucket_s),                   \
                        ft->bucket_alloc.arg);                                \
                    ft->stats.grow_failures++;                                 \
                    return -1;                                                \
                }                                                             \
                consumed++;                                                   \
            }                                                                 \
        }                                                                     \
    }                                                                         \
                                                                               \
    while (consumed < produced) {                                             \
        unsigned ci = consumed & (FT_TABLE_GROW_CTX_RING - 1u);               \
        if (_FTG_INT(p, rehash_insert_hashed_)(                               \
                &new_head, new_buckets, ft,                                   \
                ctx[ci].entry, ctx[ci].hash) != 0) {                          \
            ft->bucket_alloc.free(                                            \
                new_buckets,                                                  \
                _FTG_INT(p, bucket_bytes_)(new_nb_bk),                        \
                _Alignof(struct rix_hash_bucket_s),                           \
                ft->bucket_alloc.arg);                                        \
            ft->stats.grow_failures++;                                        \
            return -1;                                                        \
        }                                                                     \
        consumed++;                                                           \
    }                                                                         \
                                                                               \
    ft->bucket_alloc.free(ft->buckets,                                        \
                          _FTG_INT(p, bucket_bytes_)(ft->nb_bk),              \
                          _Alignof(struct rix_hash_bucket_s),                 \
                          ft->bucket_alloc.arg);                              \
    ft->buckets = new_buckets;                                                \
    ft->ht_head = new_head;                                                   \
    ft->nb_bk = new_nb_bk;                                                   \
    ft->need_grow =                                                           \
        _FTG_INT(p, fill_pct_)(ft) >= ft->grow_fill_pct ? 1u : 0u;           \
    ft->stats.grow_execs++;                                                   \
    return 0;                                                                 \
}                                                                             \
                                                                               \
/* --- reserve ----------------------------------------------------------- */ \
                                                                               \
int                                                                           \
_FTG_API(p, reserve)(_FTG_TABLE_T(p) *ft,                                    \
                     unsigned min_entries)                                     \
{                                                                             \
    unsigned required_nb_bk;                                                  \
    if (ft == NULL)                                                           \
        return -1;                                                            \
    ft->stats.reserve_calls++;                                                \
    required_nb_bk = _FTG_INT(p, required_nb_bk_)(                            \
        min_entries, ft->grow_fill_pct);                                      \
    if (required_nb_bk > ft->max_nb_bk)                                       \
        return -1;                                                            \
    while (ft->nb_bk < required_nb_bk) {                                     \
        if (_FTG_API(p, grow_2x)(ft) != 0)                                    \
            return -1;                                                        \
    }                                                                         \
    ft->need_grow =                                                           \
        _FTG_INT(p, fill_pct_)(ft) >= ft->grow_fill_pct ? 1u : 0u;           \
    return 0;                                                                 \
}                                                                             \
/* end FT_TABLE_GENERATE */

/*===========================================================================
 * OPS table macro (used in each arch .c after FT_TABLE_GENERATE)
 *===========================================================================*/
#define _FT_OPS_FNAME(prefix, name)                                            \
    _FTG_CAT(_FTG_CAT(ft_, _FTG_CAT(prefix, _table_##name)), FT_ARCH_SUFFIX)

#define _FT_OPS_TNAME(prefix, suffix)                                          \
    _FTG_CAT(_FTG_CAT(ft_, _FTG_CAT(prefix, _ops)), suffix)

#define FT_OPS_TABLE(prefix, suffix)                                           \
const struct ft_##prefix##_ops _FT_OPS_TNAME(prefix, suffix) = {              \
    .find            = _FT_OPS_FNAME(prefix, find),                           \
    .add_entry       = _FT_OPS_FNAME(prefix, add_entry),                      \
    .del             = _FT_OPS_FNAME(prefix, del),                            \
    .del_idx         = _FT_OPS_FNAME(prefix, del_idx),                        \
    .find_bulk       = _FT_OPS_FNAME(prefix, find_bulk),                      \
    .add_entry_bulk  = _FT_OPS_FNAME(prefix, add_entry_bulk),                 \
    .del_bulk        = _FT_OPS_FNAME(prefix, del_bulk),                       \
}

/*===========================================================================
 * Init-ex body macro (for dispatch layer)
 *===========================================================================*/
#define _FTG_INIT_EX_BODY(p, ft, array, max_entries, stride, entry_offset, cfg) \
    _FTG_API(p, init_ex)((ft), (array), (max_entries),                        \
                          (stride), (entry_offset), (cfg))

#endif /* _FT_TABLE_GENERATE_H_ */
