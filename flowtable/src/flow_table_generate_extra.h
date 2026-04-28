/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_table_generate_extra.h - GENERATE macro for slot_extra variant table
 * implementations.
 *
 * Usage in .c files:
 *
 *   #include "flow4_extra_table.h"
 *   // define layout hooks, hash_fn, cmp ...
 *   #include "flow_table_generate_extra.h"
 *
 *   FT_TABLE_EXTRA_GENERATE(flow4, ft_table_extra, fc_flow4x_ht,
 *                           ft_flow4_hash_fn, ft_flow4_cmp)
 */

#ifndef _FLOW_TABLE_GENERATE_EXTRA_H_
#define _FLOW_TABLE_GENERATE_EXTRA_H_

#include <string.h>
#include <rix/rix_hash_slot_extra.h>

#include "flow_extra_common.h"

/*===========================================================================
 * Token-paste helpers
 *===========================================================================*/
#define _FTG_CAT2(a, b)  a##b
#define _FTG_CAT(a, b)   _FTG_CAT2(a, b)

#define _FTG_INT(p, name)   _FTG_CAT(ft_, _FTG_CAT(p, _extra_##name))

#ifdef FT_ARCH_SUFFIX
#define _FTG_API(p, name)  _FTG_CAT(_FTG_CAT(ft_, _FTG_CAT(p, _extra_table_##name)), FT_ARCH_SUFFIX)
#else
#define _FTG_API(p, name)  _FTG_CAT(ft_, _FTG_CAT(p, _extra_table_##name))
#endif

#define _FTG_KEY_T(p)       struct _FTG_CAT(p, _extra_key)
#define _FTG_RESULT_T(p)    struct ft_table_result
#ifndef FTG_ENTRY_TYPE
#define FTG_ENTRY_TYPE(p)   struct _FTG_CAT(ft_, _FTG_CAT(p, _entry))
#endif
#define _FTG_ENTRY_T(p)     FTG_ENTRY_TYPE(p)
#define _FTG_TABLE_T(p)     struct ft_table_extra
#define _FTG_CONFIG_T(p)    struct ft_table_extra_config
#define _FTG_STATS_T(p)     struct ft_table_extra_stats
#define _FTG_HT_T(ht)       struct ht

/*===========================================================================
 * FCORE_EXTRA function name helper
 *===========================================================================*/
#define _FTG_FCORE_EXTRA(p, name) \
    _FCORE_EXTRA_CAT(_fcore_extra_, _FCORE_EXTRA_CAT(p, _##name))

/*===========================================================================
 * Extra HT function name helper (ht##_name pattern used by slot_extra)
 *===========================================================================*/
#define _FTG_HT_EXTRA(ht, name) _FTG_CAT(ht, _##name)

/*===========================================================================
 * Layout hooks (overridable per-variant before #include)
 *===========================================================================*/
#ifndef FTG_LAYOUT_HASH_BASE
#define FTG_LAYOUT_HASH_BASE(ft) ((ft)->pool)
#endif

#ifndef FTG_LAYOUT_ENTRY_PTR
#define FTG_LAYOUT_ENTRY_PTR(ft, idx)                                         \
    (((unsigned)(idx) != RIX_NIL) ? &((ft)->pool[(unsigned)(idx) - 1u]) : NULL)
#endif

#ifndef FTG_LAYOUT_ENTRY_INDEX
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry) RIX_IDX_FROM_PTR((ft)->pool, (entry))
#endif

/*===========================================================================
 * Bucket carve helper for extra variant (returns rix_hash_bucket_extra_s *)
 *===========================================================================*/
static inline struct rix_hash_bucket_extra_s *
ft_table_extra_bucket_carve_(void *raw, size_t raw_size, unsigned *nb_bk_out)
{
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + (_Alignof(struct rix_hash_bucket_extra_s) - 1u))
                      & ~(uintptr_t)(_Alignof(struct rix_hash_bucket_extra_s) - 1u);
    size_t lost = (size_t)(aligned - addr);
    size_t usable = raw_size > lost ? raw_size - lost : 0u;
    unsigned nb = (unsigned)(usable / sizeof(struct rix_hash_bucket_extra_s));

    nb = ft_rounddown_pow2_u32(nb);
    *nb_bk_out = nb;
    return (struct rix_hash_bucket_extra_s *)aligned;
}

/*===========================================================================
 * Pipeline geometry
 *===========================================================================*/
#ifndef FT_TABLE_EXTRA_BULK_STEP_KEYS
#define FT_TABLE_EXTRA_BULK_STEP_KEYS 8u
#endif

#ifndef FT_TABLE_EXTRA_BULK_AHEAD_STEPS
#define FT_TABLE_EXTRA_BULK_AHEAD_STEPS 4u
#endif

#define FT_TABLE_EXTRA_BULK_AHEAD_KEYS                                        \
    (FT_TABLE_EXTRA_BULK_STEP_KEYS * FT_TABLE_EXTRA_BULK_AHEAD_STEPS)

#ifndef FT_TABLE_EXTRA_BULK_CTX_RING
#define FT_TABLE_EXTRA_BULK_CTX_RING 128u
#endif

#ifndef FT_TABLE_EXTRA_GROW_OLD_BK_AHEAD
#define FT_TABLE_EXTRA_GROW_OLD_BK_AHEAD 2u
#endif

#ifndef FT_TABLE_EXTRA_GROW_REINSERT_AHEAD
#define FT_TABLE_EXTRA_GROW_REINSERT_AHEAD 8u
#endif

#ifndef FT_TABLE_EXTRA_GROW_CTX_RING
#define FT_TABLE_EXTRA_GROW_CTX_RING 64u
#endif

#ifndef FT_TABLE_EXTRA_FIND_BULK_MIN_KEYS
#define FT_TABLE_EXTRA_FIND_BULK_MIN_KEYS 32u
#endif

/*===========================================================================
 * FT_TABLE_EXTRA_GENERATE(prefix, ot, ht, hash_fn, cmp_fn)
 *
 * p       : variant prefix (flow4, flow6, flowu)
 * ot      : owner type name without "struct" (e.g. ft_table_extra)
 * ht      : rix_hash_slot_extra name prefix (e.g. fc_flow4x_ht)
 * hash_fn : hash function for this variant
 * cmp_fn  : compare function for this variant
 *
 * Bulk operations delegate to the FCORE_EXTRA_GENERATE functions
 * (_fcore_extra_<p>_*).
 *===========================================================================*/
#define FT_TABLE_EXTRA_GENERATE(p, ot, ht, hash_fn, cmp_fn)                  \
                                                                               \
/* --- Internal helpers -------------------------------------------------- */ \
                                                                               \
static RIX_UNUSED RIX_FORCE_INLINE _FTG_ENTRY_T(p) *                          \
_FTG_INT(p, hash_base_)(_FTG_TABLE_T(p) *ft)                                  \
{                                                                             \
    return (_FTG_ENTRY_T(p) *)(void *)ft;                                     \
}                                                                             \
                                                                               \
static RIX_UNUSED RIX_FORCE_INLINE size_t                                     \
_FTG_INT(p, bucket_bytes_)(unsigned nb_bk)                                    \
{                                                                             \
    return (size_t)nb_bk * sizeof(struct rix_hash_bucket_extra_s);            \
}                                                                             \
                                                                               \
/* find_hashed_: single-key lookup using precomputed hash */                  \
static RIX_UNUSED RIX_FORCE_INLINE _FTG_ENTRY_T(p) *                          \
_FTG_INT(p, find_hashed_)(_FTG_TABLE_T(p) *ft,                                \
                          const _FTG_KEY_T(p) *key,                           \
                          union rix_hash_hash_u h)                            \
{                                                                             \
    unsigned bk0, bk1;                                                        \
    u32 fp;                                                                   \
    u32 hits;                                                                 \
                                                                               \
    fp = rix_hash_fp(h, ft->ht_head.rhh_mask, &bk0, &bk1);                   \
    hits = RIX_HASH_FIND_U32X16(ft->buckets[bk0].hash, fp);                  \
    while (hits != 0u) {                                                      \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        unsigned idx = ft->buckets[bk0].idx[bit];                             \
        _FTG_ENTRY_T(p) *entry;                                               \
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
    hits = RIX_HASH_FIND_U32X16(ft->buckets[bk1].hash, fp);                  \
    while (hits != 0u) {                                                      \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        unsigned idx = ft->buckets[bk1].idx[bit];                             \
        _FTG_ENTRY_T(p) *entry;                                               \
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
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
_FTG_INT(p, find_small_)(_FTG_TABLE_T(p) *ft,                                 \
                         const _FTG_KEY_T(p) *keys,                           \
                         unsigned nb_keys,                                    \
                         u64 now,                                             \
                         _FTG_RESULT_T(p) *results)                           \
{                                                                             \
    const u32 hash_mask = ft->start_mask;                                     \
    u64 hit_count = 0u;                                                       \
    u64 miss_count = 0u;                                                      \
                                                                              \
    if (nb_keys > 1u && nb_keys < 4u) {                                       \
        struct rix_hash_find_ctx_extra_s ctx[3];                              \
                                                                              \
        for (unsigned i = 0u; i < nb_keys; i++)                               \
            rix_hash_prefetch_key(&keys[i]);                                  \
        for (unsigned i = 0u; i < nb_keys; i++)                               \
            _FTG_HT_EXTRA(ht, hash_key_2bk_masked)(&ctx[i], ft->buckets,      \
                                            &keys[i],                         \
                                            hash_mask, ft->ht_head.rhh_mask); \
        for (unsigned i = 0u; i < nb_keys; i++) {                             \
            _FTG_HT_EXTRA(ht, scan_bk)(&ctx[i],                               \
                                       (_FTG_HT_T(ht) *)0, ft->buckets);      \
            _FTG_HT_EXTRA(ht, prefetch_node)(&ctx[i],                         \
                                             FTG_LAYOUT_HASH_BASE(ft));       \
        }                                                                     \
        for (unsigned i = 0u; i < nb_keys; i++) {                             \
            _FTG_ENTRY_T(p) *entry =                                          \
                _FTG_HT_EXTRA(ht, cmp_key)(&ctx[i],                           \
                                           FTG_LAYOUT_HASH_BASE(ft));         \
                                                                              \
            if (RIX_LIKELY(entry != NULL)) {                                  \
                u32 entry_idx = FTG_LAYOUT_ENTRY_INDEX(ft, entry);            \
                                                                              \
                /* scan_bk has already proven entry_idx lives in bk[0]/bk[1], \
                 * so touch must succeed here.  Assert in debug builds.       \
                 */                                                           \
                if ((now) != 0u) {                                            \
                    int _ts_rc = rix_hash_slot_extra_touch_2bk(               \
                        ctx[i].bk[0], ctx[i].bk[1],                           \
                        (unsigned)entry->meta.slot, entry_idx,                \
                        flow_extra_timestamp_encode((now), ft->ts_shift));    \
                    RIX_ASSERT(_ts_rc == 0);                                  \
                    (void)_ts_rc;                                             \
                }                                                             \
                FCORE_EXTRA_TOUCH_TIMESTAMP(ft, entry, now);                  \
                FCORE_EXTRA_ON_HIT(ft, entry, entry_idx);                     \
                results[i].entry_idx = entry_idx;                             \
                hit_count++;                                                  \
            } else {                                                          \
                results[i].entry_idx = 0u;                                    \
                miss_count++;                                                 \
            }                                                                 \
        }                                                                     \
        ft->stats.core.lookups += nb_keys;                                    \
        ft->stats.core.hits += hit_count;                                     \
        ft->stats.core.misses += miss_count;                                  \
        return;                                                               \
    }                                                                         \
                                                                              \
    for (unsigned i = 0u; i < nb_keys; i++) {                                 \
        struct rix_hash_find_ctx_extra_s ctx;                                 \
        _FTG_ENTRY_T(p) *entry;                                               \
                                                                              \
        _FTG_HT_EXTRA(ht, hash_key_2bk_masked)(&ctx, ft->buckets, &keys[i],   \
                                        hash_mask, ft->ht_head.rhh_mask);     \
        _FTG_HT_EXTRA(ht, scan_bk)(&ctx, (_FTG_HT_T(ht) *)0, ft->buckets);   \
        _FTG_HT_EXTRA(ht, prefetch_node)(&ctx, FTG_LAYOUT_HASH_BASE(ft));     \
        entry = _FTG_HT_EXTRA(ht, cmp_key)(&ctx, FTG_LAYOUT_HASH_BASE(ft));   \
                                                                              \
        if (RIX_LIKELY(entry != NULL)) {                                      \
            u32 entry_idx = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                \
                                                                              \
            /* scan_bk has already proven entry_idx lives in bk[0]/bk[1].     \
             * Assert in debug builds; release path drops the check.          \
             */                                                               \
            if ((now) != 0u) {                                                \
                int _ts_rc = rix_hash_slot_extra_touch_2bk(                   \
                    ctx.bk[0], ctx.bk[1],                                     \
                    (unsigned)entry->meta.slot, entry_idx,                    \
                    flow_extra_timestamp_encode((now), ft->ts_shift));        \
                RIX_ASSERT(_ts_rc == 0);                                      \
                (void)_ts_rc;                                                 \
            }                                                                 \
            FCORE_EXTRA_TOUCH_TIMESTAMP(ft, entry, now);                      \
            FCORE_EXTRA_ON_HIT(ft, entry, entry_idx);                         \
            results[i].entry_idx = entry_idx;                                 \
            hit_count++;                                                      \
        } else {                                                              \
            results[i].entry_idx = 0u;                                        \
            miss_count++;                                                     \
        }                                                                     \
    }                                                                         \
    ft->stats.core.lookups += nb_keys;                                        \
    ft->stats.core.hits += hit_count;                                         \
    ft->stats.core.misses += miss_count;                                      \
}                                                                             \
                                                                               \
/* insert_hashed_: insert pre-hashed entry into hash table */                 \
static RIX_UNUSED int                                                         \
_FTG_INT(p, insert_hashed_)(_FTG_TABLE_T(p) *ft,                              \
                            _FTG_ENTRY_T(p) *entry,                           \
                            const union rix_hash_hash_u h,                    \
                            u32 *entry_idx_out)                               \
{                                                                             \
    _FTG_ENTRY_T(p) *ret;                                                     \
    _Pragma("GCC diagnostic push")                                            \
    _Pragma("GCC diagnostic ignored \"-Wstrict-aliasing\"")                   \
    ret = _FTG_HT_EXTRA(ht, insert_hashed)(                                   \
                            (_FTG_HT_T(ht) *)(void *)&ft->ht_head,            \
                            ft->buckets,                                      \
                            _FTG_INT(p, hash_base_)(ft),                      \
                            entry, h, 0u);                                    \
    _Pragma("GCC diagnostic pop")                                             \
    if (ret == NULL) {                                                        \
        *entry_idx_out = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                   \
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
static RIX_UNUSED int                                                         \
_FTG_INT(p, rehash_insert_hashed_)(_FTG_HT_T(ht) *head,                       \
                                   struct rix_hash_bucket_extra_s *buckets,   \
                                   _FTG_TABLE_T(p) *ft,                       \
                                   _FTG_ENTRY_T(p) *entry,                    \
                                   union rix_hash_hash_u h)                   \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned bk0, bk1;                                                        \
    u32 fp;                                                                   \
    fp = rix_hash_fp(h, mask, &bk0, &bk1);                                   \
    entry->meta.cur_hash = h.val32[0];                                        \
    for (unsigned pass = 0u; pass < 2u; pass++) {                             \
        unsigned bki = (pass == 0u) ? bk0 : bk1;                              \
        struct rix_hash_bucket_extra_s *bk = &buckets[bki];                   \
        u32 nilm = RIX_HASH_FIND_U32X16(bk->hash, 0u);                       \
        if (nilm != 0u) {                                                     \
            unsigned s = (unsigned)__builtin_ctz(nilm);                       \
            bk->hash[s] = fp;                                                 \
            bk->idx[s] = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                   \
            if (pass == 1u)                                                   \
                entry->meta.cur_hash = h.val32[1];                            \
            entry->meta.slot = (u16)s;                                        \
            head->rhh_nb++;                                                   \
            return 0;                                                         \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        struct rix_hash_bucket_extra_s *bk;                                   \
        pos = _FTG_HT_EXTRA(ht, kickout)(buckets,                              \
                                  _FTG_INT(p, hash_base_)(ft),                \
                                  mask, bk0, RIX_HASH_FOLLOW_DEPTH);          \
        if (pos >= 0) {                                                       \
            bki = bk0;                                                        \
        } else {                                                              \
            pos = _FTG_HT_EXTRA(ht, kickout)(buckets,                          \
                                      _FTG_INT(p, hash_base_)(ft),            \
                                      mask, bk1, RIX_HASH_FOLLOW_DEPTH);      \
            if (pos < 0)                                                      \
                return -1;                                                    \
            bki = bk1;                                                        \
            entry->meta.cur_hash = h.val32[1];                                \
        }                                                                     \
        bk = &buckets[bki];                                                   \
        bk->hash[pos] = fp;                                                   \
        bk->idx[pos] = FTG_LAYOUT_ENTRY_INDEX(ft, entry);                     \
        entry->meta.slot = (u16)(unsigned)pos;                                \
        head->rhh_nb++;                                                       \
        return 0;                                                             \
    }                                                                         \
}                                                                             \
                                                                               \
/* --- destroy ----------------------------------------------------------- */ \
                                                                               \
static void                                                                          \
_FTG_API(p, destroy)(_FTG_TABLE_T(p) *ft)                                     \
{                                                                             \
    if (ft == NULL)                                                           \
        return;                                                               \
    memset(ft, 0, sizeof(*ft));                                               \
}                                                                             \
                                                                               \
/* --- flush ------------------------------------------------------------- */ \
                                                                               \
static void                                                                          \
_FTG_API(p, flush)(_FTG_TABLE_T(p) *ft)                                       \
{                                                                             \
    if (ft == NULL || ft->buckets == NULL)                                    \
        return;                                                               \
    _FTG_FCORE_EXTRA(p, flush_)(ft);                                          \
    flow_status_reset(&ft->status, 0u);                                       \
}                                                                             \
                                                                               \
/* --- nb_entries -------------------------------------------------------- */ \
                                                                               \
static unsigned                                                                      \
_FTG_API(p, nb_entries)(const _FTG_TABLE_T(p) *ft)                            \
{                                                                             \
    return ft == NULL ? 0u : ft->ht_head.rhh_nb;                              \
}                                                                             \
                                                                               \
static unsigned                                                                      \
_FTG_API(p, nb_bk)(const _FTG_TABLE_T(p) *ft)                                 \
{                                                                             \
    return ft == NULL ? 0u : ft->nb_bk;                                       \
}                                                                             \
                                                                               \
static void                                                                          \
_FTG_API(p, stats)(const _FTG_TABLE_T(p) *ft,                                 \
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
static void                                                                          \
_FTG_API(p, status)(const _FTG_TABLE_T(p) *ft,                                \
                    struct flow_status *out)                                  \
{                                                                             \
    if (out == NULL)                                                          \
        return;                                                               \
    if (ft == NULL) {                                                         \
        memset(out, 0, sizeof(*out));                                         \
        return;                                                               \
    }                                                                         \
    *out = ft->status;                                                        \
}                                                                             \
                                                                               \
/* --- find_bulk --------------------------------------------------------- */ \
                                                                               \
static void                                                                          \
_FTG_API(p, find_bulk)(_FTG_TABLE_T(p) *ft,                                   \
                       const _FTG_KEY_T(p) *keys,                             \
                       unsigned nb_keys,                                      \
                       u64 now,                                               \
                       _FTG_RESULT_T(p) *results)                             \
{                                                                             \
    if (results == NULL)                                                      \
        return;                                                               \
    if (ft == NULL || ft->buckets == NULL || keys == NULL) {                  \
        for (unsigned i = 0; i < nb_keys; i++)                                \
            results[i].entry_idx = 0u;                                        \
        return;                                                               \
    }                                                                         \
    if (nb_keys < FT_TABLE_EXTRA_FIND_BULK_MIN_KEYS) {                        \
        _FTG_INT(p, find_small_)(ft, keys, nb_keys, now, results);            \
        return;                                                               \
    }                                                                         \
    _FTG_FCORE_EXTRA(p, find_key_bulk_)((ft), (keys), (nb_keys), (now),       \
                                  &(results)[0].entry_idx);                   \
}                                                                             \
                                                                               \
/* --- add_idx_bulk ------------------------------------------------------ */ \
                                                                               \
static unsigned                                                                      \
_FTG_API(p, add_idx_bulk)(_FTG_TABLE_T(p) *ft,                                \
                           u32 *entry_idxv,                                   \
                           unsigned nb_keys,                                  \
                           enum ft_add_policy policy,                         \
                           u64 now,                                           \
                           u32 *unused_idxv)                                  \
{                                                                             \
    if (unused_idxv == NULL)                                                  \
        return 0u;                                                            \
    if (ft == NULL || ft->buckets == NULL || entry_idxv == NULL) {            \
        return 0u;                                                            \
    }                                                                         \
    return _FTG_FCORE_EXTRA(p, add_idx_bulk_)((ft), (entry_idxv), (nb_keys),  \
                                        (policy), (now), (unused_idxv));      \
}                                                                             \
                                                                               \
/* --- add_idx_bulk_maint ----------------------------------------------- */  \
                                                                               \
static unsigned                                                               \
_FTG_API(p, add_idx_bulk_maint)(_FTG_TABLE_T(p) *ft,                          \
                                u32 *entry_idxv,                               \
                                unsigned nb_keys,                              \
                                enum ft_add_policy policy,                     \
                                u64 now,                                        \
                                u64 timeout,                                    \
                                u32 *unused_idxv,                              \
                                unsigned max_unused,                            \
                                unsigned min_bk_used)                           \
{                                                                             \
    if (unused_idxv == NULL)                                                  \
        return 0u;                                                            \
    if (ft == NULL || ft->buckets == NULL || entry_idxv == NULL)              \
        return 0u;                                                            \
    return _FTG_FCORE_EXTRA(p, add_idx_bulk_maint_)(                          \
                                              (ft), (entry_idxv), (nb_keys),  \
                                              (policy), (now), (timeout),     \
                                              (unused_idxv), (max_unused),    \
                                              (min_bk_used));                 \
}                                                                             \
                                                                               \
/* --- del_key_bulk ------------------------------------------------------ */ \
                                                                               \
static unsigned                                                               \
_FTG_API(p, del_key_bulk)(_FTG_TABLE_T(p) *ft,                                \
                          const _FTG_KEY_T(p) *keys,                          \
                          unsigned nb_keys,                                   \
                          u32 *unused_idxv)                                   \
{                                                                             \
    if (ft == NULL || keys == NULL || ft->buckets == NULL ||                  \
        unused_idxv == NULL)                                                  \
        return 0u;                                                            \
    return _FTG_FCORE_EXTRA(p, del_key_bulk_)((ft), (keys), (nb_keys),        \
                                        (unused_idxv));                       \
}                                                                             \
                                                                               \
/* --- del_idx_bulk ------------------------------------------------ */ \
                                                                               \
static unsigned                                                               \
_FTG_API(p, del_idx_bulk)(_FTG_TABLE_T(p) *ft,                                \
                                const u32 *entry_idxv,                        \
                                unsigned nb_keys,                             \
                                u32 *unused_idxv)                             \
{                                                                             \
    if (ft == NULL || ft->buckets == NULL || entry_idxv == NULL)              \
        return 0u;                                                            \
    return _FTG_FCORE_EXTRA(p, del_idx_bulk_)((ft), (entry_idxv), (nb_keys),  \
                                        (unused_idxv));                       \
}                                                                             \
                                                                               \
/* --- walk -------------------------------------------------------------- */ \
                                                                               \
static int                                                                           \
_FTG_API(p, walk)(_FTG_TABLE_T(p) *ft,                                        \
                  int (*cb)(u32 entry_idx, void *arg),                        \
                  void *arg)                                                  \
{                                                                             \
    if (ft == NULL || cb == NULL)                                             \
        return -1;                                                            \
    return _FTG_FCORE_EXTRA(p, walk_)((ft), (cb), (arg));                     \
}                                                                             \
                                                                               \
/* --- migrate: move entries to user-provided new buckets ----------------*/ \
                                                                               \
static int                                                                     \
_FTG_API(p, migrate)(_FTG_TABLE_T(p) *ft,                                     \
                     void *new_buckets_raw,                                   \
                     size_t new_bucket_size)                                  \
{                                                                             \
    struct rix_hash_bucket_extra_s *new_buckets;                              \
    _FTG_HT_T(ht) new_head;                                                   \
    unsigned new_nb_bk;                                                       \
    enum {                                                                    \
        _GROW_RING_SZ      = 128u,                                            \
        _GROW_RING_MASK    = _GROW_RING_SZ - 1u,                              \
        _GROW_BATCH        = 8u,                                              \
        _GROW_ENTRY_AHEAD  = 48u,                                             \
        _GROW_INSERT_AHEAD = 32u,                                             \
        _GROW_OLD_BK_AHEAD = 8u                                               \
    };                                                                        \
    struct {                                                                  \
        _FTG_ENTRY_T(p) *entry;                                               \
        u32 old_fp;                                                           \
        union rix_hash_hash_u h;                                              \
    } ring[_GROW_RING_SZ];                                                    \
    unsigned new_mask, old_mask;                                              \
    unsigned scanned = 0u, hashed = 0u, inserted = 0u;                       \
                                                                               \
    if (ft == NULL || ft->buckets == NULL)                                    \
        return -1;                                                            \
    if (new_buckets_raw == NULL || new_bucket_size == 0u)                     \
        return -1;                                                            \
    new_buckets = ft_table_extra_bucket_carve_(new_buckets_raw,               \
                                       new_bucket_size, &new_nb_bk);         \
    if (new_nb_bk <= ft->start_mask)                                          \
        return -1;                                                            \
    memset(new_buckets, 0, (size_t)new_nb_bk * sizeof(*new_buckets));         \
                                                                               \
    _FTG_HT_EXTRA(ht, init)(&new_head, new_nb_bk);                            \
    new_mask = new_head.rhh_mask;                                             \
    old_mask = ft->ht_head.rhh_mask;                                          \
                                                                               \
    for (unsigned bk = 0u;                                                    \
         bk < _GROW_OLD_BK_AHEAD && bk <= old_mask;                           \
         bk++)                                                                \
        rix_hash_prefetch_extra_bucket_of(&ft->buckets[bk]);                        \
                                                                               \
    for (unsigned bk = 0u; bk <= old_mask; bk++) {                            \
        const struct rix_hash_bucket_extra_s *old_bk = &ft->buckets[bk];      \
        unsigned pfbk = bk + _GROW_OLD_BK_AHEAD;                              \
        if (pfbk <= old_mask)                                                 \
            rix_hash_prefetch_extra_bucket_of(&ft->buckets[pfbk]);                  \
                                                                               \
        for (unsigned s = 0u; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {            \
            unsigned idx = old_bk->idx[s];                                    \
            _FTG_ENTRY_T(p) *entry;                                           \
            if (idx == (unsigned)RIX_NIL)                                     \
                continue;                                                     \
            entry = FTG_LAYOUT_ENTRY_PTR(ft, idx);                            \
            RIX_ASSUME_NONNULL(entry);                                        \
            rix_hash_prefetch_entry_of(entry);                                \
            ring[scanned & _GROW_RING_MASK].entry = entry;                    \
            ring[scanned & _GROW_RING_MASK].old_fp = old_bk->hash[s];        \
            scanned++;                                                        \
                                                                               \
            if (scanned - hashed >= _GROW_ENTRY_AHEAD + _GROW_BATCH) {        \
                for (unsigned b = 0u; b < _GROW_BATCH; b++) {                 \
                    unsigned ri = hashed & _GROW_RING_MASK;                   \
                    u32 cur = ring[ri].entry->meta.cur_hash;                  \
                    ring[ri].h.val32[0] = cur;                                \
                    ring[ri].h.val32[1] = ring[ri].old_fp ^ cur;              \
                    rix_hash_prefetch_extra_bucket_of(                              \
                        &new_buckets[cur & new_mask]);                        \
                    hashed++;                                                 \
                }                                                             \
                if (hashed - inserted >= _GROW_INSERT_AHEAD + _GROW_BATCH) {  \
                    for (unsigned b = 0u; b < _GROW_BATCH; b++) {             \
                        unsigned ri = inserted & _GROW_RING_MASK;             \
                        if (RIX_UNLIKELY(                                     \
                            _FTG_INT(p, rehash_insert_hashed_)(               \
                                &new_head, new_buckets, ft,                   \
                                ring[ri].entry, ring[ri].h) != 0))            \
                            goto _migrate_fail;                               \
                        inserted++;                                           \
                    }                                                         \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
                                                                               \
    while (hashed < scanned) {                                                \
        unsigned ri = hashed & _GROW_RING_MASK;                               \
        u32 cur = ring[ri].entry->meta.cur_hash;                              \
        ring[ri].h.val32[0] = cur;                                            \
        ring[ri].h.val32[1] = ring[ri].old_fp ^ cur;                          \
        rix_hash_prefetch_extra_bucket_of(                                          \
            &new_buckets[cur & new_mask]);                                    \
        hashed++;                                                             \
    }                                                                         \
    while (inserted < hashed) {                                               \
        unsigned ri = inserted & _GROW_RING_MASK;                             \
        if (RIX_UNLIKELY(                                                     \
            _FTG_INT(p, rehash_insert_hashed_)(                               \
                &new_head, new_buckets, ft,                                   \
                ring[ri].entry, ring[ri].h) != 0))                            \
            goto _migrate_fail;                                               \
        inserted++;                                                           \
    }                                                                         \
    goto _migrate_done;                                                       \
                                                                               \
_migrate_fail:                                                                \
    ft->stats.grow_failures++;                                                \
    return -1;                                                                \
_migrate_done: (void)0;                                                       \
                                                                               \
    ft->buckets = new_buckets;                                                \
    memcpy(&ft->ht_head, &new_head, sizeof(ft->ht_head));                     \
    ft->nb_bk = new_nb_bk;                                                    \
    flow_status_reset(&ft->status, (u32)new_head.rhh_nb);                    \
    ft->stats.grow_execs++;                                                   \
    return 0;                                                                 \
}                                                                             \
/* end FT_TABLE_EXTRA_GENERATE */

/*===========================================================================
 * OPS table macro (used in each arch .c after FT_TABLE_EXTRA_GENERATE)
 *===========================================================================*/
#define _FT_OPS_EXTRA_FNAME(prefix, name)                                     \
    _FTG_CAT(_FTG_CAT(ft_, _FTG_CAT(prefix, _extra_table_##name)),            \
             FT_ARCH_SUFFIX)

#define _FT_OPS_EXTRA_TNAME(prefix, suffix)                                   \
    _FTG_CAT(_FTG_CAT(ft_, _FTG_CAT(prefix, _extra_ops)), suffix)

#define FT_OPS_TABLE_EXTRA(prefix, suffix)                                    \
const struct ft_##prefix##_extra_ops _FT_OPS_EXTRA_TNAME(prefix, suffix) = { \
    /* cold-path */                                                            \
    .destroy         = _FT_OPS_EXTRA_FNAME(prefix, destroy),                  \
    .flush           = _FT_OPS_EXTRA_FNAME(prefix, flush),                    \
    .nb_entries      = _FT_OPS_EXTRA_FNAME(prefix, nb_entries),               \
    .nb_bk           = _FT_OPS_EXTRA_FNAME(prefix, nb_bk),                   \
    .stats           = _FT_OPS_EXTRA_FNAME(prefix, stats),                    \
    .status          = _FT_OPS_EXTRA_FNAME(prefix, status),                   \
    .walk            = _FT_OPS_EXTRA_FNAME(prefix, walk),                     \
    .migrate         = _FT_OPS_EXTRA_FNAME(prefix, migrate),                  \
    /* hot-path */                                                             \
    .find_bulk       = _FT_OPS_EXTRA_FNAME(prefix, find_bulk),                \
    .add_idx_bulk    = _FT_OPS_EXTRA_FNAME(prefix, add_idx_bulk),             \
    .add_idx_bulk_maint = _FT_OPS_EXTRA_FNAME(prefix, add_idx_bulk_maint),   \
    .del_key_bulk    = _FT_OPS_EXTRA_FNAME(prefix, del_key_bulk),             \
    .del_idx_bulk    = _FT_OPS_EXTRA_FNAME(prefix, del_idx_bulk),             \
}

#endif /* _FLOW_TABLE_GENERATE_EXTRA_H_ */
