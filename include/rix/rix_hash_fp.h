/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_fp.h - fingerprint variant (hash_field in node).
 *
 * Requires: rix_hash_common.h
 *
 * Generated functions (section order):
 *   init, staged find (x1, xN), find, insert, remove, remove_at, walk
 */

#ifndef _RIX_HASH_FP_H_
#  define _RIX_HASH_FP_H_

#  include "rix_hash_common.h"

#  define RIX_HASH_PROTOTYPE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)


#  define RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, )

#  define RIX_HASH_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, RIX_UNUSED static)

#  define RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)         \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn,      \
                               _RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn,      \
                               _RIX_HASH_DEFAULT_HASH_FN_NAME(name),           \
                               RIX_UNUSED static)

#  define RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
                                                                              \
/* ================================================================== */      \
/* Init                                                               */      \
/* ================================================================== */      \
attr void                                                                     \
name##_init(struct name *head,                                                \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    head->rhh_nb   = 0u;                                                      \
}                                                                             \
                                                                              \
/* ------------------------------------------------------------------ */      \
/* Internal helpers: 1-origin index <-> pointer                       */      \
/* ------------------------------------------------------------------ */      \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p) {                        \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i) {                                  \
    return (struct type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);    \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - x1                                                   */      \
/* ================================================================== */      \
                                                                              \
/* Stage 1: compute hash, resolve bucket pointers, issue prefetches. */       \
/* Must be called before name_scan_bk.                               */       \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_find_ctx_s *ctx,                              \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets,                            \
                const _RIX_HASH_KEY_TYPE(type, key_field) *key)               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn(key, mask);                                                   \
    unsigned _bk0, _bk1;                                                      \
    u32 _fp;                                                             \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    rix_hash_prefetch_bucket_of(ctx->bk[0]);                                  \
    /* bk_1 not prefetched: bk_0 miss path fetches it lazily in cmp_key. */   \
}                                                                             \
                                                                              \
/* Stage 2: scan bk_0 fingerprints only; produce fp_hits[0] bitmask.  */      \
/* fp != 0 (XOR-based), no _nilm filter needed; fp_hits[1] = 0 here.  */      \
/* Call prefetch_node to hide node-fetch latency before cmp_key.      */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_find_ctx_s *ctx,                               \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused)))     \
{                                                                             \
    /* fp != 0: no spurious empty-slot matches; no _nilm filter needed. */    \
    ctx->fp_hits[0] = _RIX_HASH_FIND_U32X16(ctx->bk[0]->hash, ctx->fp);       \
    ctx->fp_hits[1] = 0u; /* bk_1 deferred to cmp_key on bk_0 miss */         \
}                                                                             \
                                                                              \
/* Stage 3: prefetch node data for all fp_hits positions.             */      \
/* Hides node-fetch DRAM latency; call for all N contexts before cmp_key. */  \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node(struct rix_hash_find_ctx_s *ctx,                         \
                     struct type *base)                                       \
{                                                                             \
    for (int _i = 0; _i < 2; _i++) {                                          \
        u32 _hits = ctx->fp_hits[_i];                                    \
        while (_hits) {                                                       \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            _hits &= _hits - 1u;                                              \
            rix_hash_prefetch_entry_of_idx(base, ctx->bk[_i]->idx[_bit]);     \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/* Stage 4: compare keys for bk_0 hits; lazily scan bk_1 on bk_0 miss. */     \
/* fp != 0 (XOR-based), so no _nilm filter is needed in bk_1 scan.    */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_find_ctx_s *ctx,                               \
               struct type *base)                                             \
{                                                                             \
    /* Fast path: bk_0 (primary bucket) */                                    \
    u32 _hits = ctx->fp_hits[0];                                         \
    while (_hits) {                                                           \
        unsigned      _bit  = (unsigned)__builtin_ctz(_hits);                 \
        _hits &= _hits - 1u;                                                  \
        unsigned      _nidx = ctx->bk[0]->idx[_bit];                          \
        if (_nidx == (unsigned)RIX_NIL) continue; /* removed slot */         \
        struct type  *_node = name##_hptr(base, _nidx);                       \
        if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,    \
                   &_node->key_field) == 0)                                   \
            return _node;                                                     \
    }                                                                         \
    /* Slow path: bk_0 miss -> lazily fetch and scan bk_1 (secondary) */       \
    {                                                                         \
        _hits = _RIX_HASH_FIND_U32X16(ctx->bk[1]->hash, ctx->fp);             \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            unsigned      _nidx = ctx->bk[1]->idx[_bit];                      \
            if (_nidx == (unsigned)RIX_NIL) continue; /* removed slot */     \
            struct type  *_node = name##_hptr(base, _nidx);                   \
            if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key, \
                       &_node->key_field) == 0)                               \
                return _node;                                                 \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Lookup pipeline variants that also collect empty-slot bitmasks.     */      \
/* Use when miss -> insert is the expected follow-up path.             */      \
/* ================================================================== */      \
                                                                              \
/* Stage 1: like hash_key but prefetches both bk_0 and bk_1.         */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_2bk(struct rix_hash_find_ctx_s *ctx,                          \
                    struct name *head,                                        \
                    struct rix_hash_bucket_s *buckets,                        \
                    const _RIX_HASH_KEY_TYPE(type, key_field) *key)           \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn(key, mask);                                                   \
    unsigned _bk0, _bk1;                                                      \
    u32 _fp;                                                             \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    rix_hash_prefetch_bucket_of(ctx->bk[0]);                                  \
    rix_hash_prefetch_bucket_of(ctx->bk[1]);                                  \
}                                                                             \
                                                                              \
/* Stage 2: scan bk_0 for fp matches AND empty slots in one pass.     */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_empties(struct rix_hash_find_ctx_s *ctx,                       \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_s *buckets                      \
                           __attribute__((unused)))                           \
{                                                                             \
    _RIX_HASH_FIND_U32X16_2(ctx->bk[0]->hash, ctx->fp, 0u,                    \
                              &ctx->fp_hits[0], &ctx->empties[0]);              \
    ctx->fp_hits[1] = 0u;                                                      \
    ctx->empties[1] = 0u;                                                      \
}                                                                             \
                                                                              \
/* Stage 4: like cmp_key but also produces empties[1] on bk_1 scan.  */      \
/* bk_1 is already warm (prefetched by hash_key_2bk).                 */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_empties(struct rix_hash_find_ctx_s *ctx,                       \
                       struct type *base)                                     \
{                                                                             \
    /* Fast path: bk_0 */                                                     \
    u32 _hits = ctx->fp_hits[0];                                         \
    while (_hits) {                                                           \
        unsigned      _bit  = (unsigned)__builtin_ctz(_hits);                 \
        _hits &= _hits - 1u;                                                  \
        unsigned      _nidx = ctx->bk[0]->idx[_bit];                          \
        if (_nidx == (unsigned)RIX_NIL) continue;                             \
        struct type  *_node = name##_hptr(base, _nidx);                       \
        if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,    \
                   &_node->key_field) == 0)                                   \
            return _node;                                                     \
    }                                                                         \
    /* bk_1: warm from hash_key_2bk; collect fp_hits[1] + empties[1]. */       \
    {                                                                         \
        _RIX_HASH_FIND_U32X16_2(ctx->bk[1]->hash, ctx->fp, 0u,               \
                                  &ctx->fp_hits[1], &ctx->empties[1]);          \
        _hits = ctx->fp_hits[1];                                               \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            unsigned      _nidx = ctx->bk[1]->idx[_bit];                      \
            if (_nidx == (unsigned)RIX_NIL) continue;                         \
            struct type  *_node = name##_hptr(base, _nidx);                   \
            if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key, \
                       &_node->key_field) == 0)                               \
                return _node;                                                 \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - xN bulk                                              */      \
/* FORCE_INLINE + constant n -> compiler unrolls identically to xN.   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_find_ctx_s *ctx,                            \
                  int n,                                                      \
                  struct name *head,                                          \
                  struct rix_hash_bucket_s *buckets,                          \
                  const _RIX_HASH_KEY_TYPE(type, key_field) * const *keys)    \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_hash_key(&ctx[_j], head, buckets, keys[_j]);                   \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_find_ctx_s *ctx,                             \
                 int n,                                                       \
                 struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets)                           \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_scan_bk(&ctx[_j], head, buckets);                              \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_find_ctx_s *ctx,                       \
                       int n,                                                 \
                       struct type *base)                                     \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_prefetch_node(&ctx[_j], base);                                 \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_find_ctx_s *ctx,                             \
                 int n,                                                       \
                 struct type *base,                                           \
                 struct type **results)                                       \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        results[_j] = name##_cmp_key(&ctx[_j], base);                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Single-shot find  = hash_key + scan_bk + cmp_key                   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_find(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            const _RIX_HASH_KEY_TYPE(type, key_field) *key)                   \
{                                                                             \
    struct rix_hash_find_ctx_s _ctx;                                          \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk (&_ctx, head, buckets);                                    \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* find_empty: return any free slot in bucket, or -1                   */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_find_empty(struct rix_hash_bucket_s *buckets,                          \
                  unsigned bk_idx)                                            \
{                                                                             \
    u32 _nilm = _RIX_HASH_FIND_U32X16(buckets[bk_idx].hash, 0u);        \
    if (!_nilm) return -1;                                                    \
    return (int)__builtin_ctz(_nilm);                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* flipflop: move bk[slot] to its alt bucket if alt has a free slot.   */     \
/* Returns freed slot on success, -1 on failure.                        */     \
/* Non-destructive on failure.                                         */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_flipflop(struct rix_hash_bucket_s *buckets,                            \
                struct type *base,                                            \
                unsigned mask,                                                \
                unsigned bk_idx,                                              \
                unsigned slot)                                                \
{                                                                             \
    struct rix_hash_bucket_s *_bk = buckets + bk_idx;                         \
    u32     _fp  = _bk->hash[slot];                                      \
    unsigned     _idx = _bk->idx [slot];                                        \
    struct type *_nd  = name##_hptr(base, _idx);                              \
    if (!_nd) return -1;                                                      \
    u32 _h  = _nd->hash_field;                                            \
    unsigned _ab = (_fp ^ _h) & mask;                                          \
    int _slot = name##_find_empty(buckets, _ab);                              \
    if (_slot < 0) return -1;                                                 \
    struct rix_hash_bucket_s *_alt = buckets + _ab;                           \
    _alt->hash[_slot] = _fp;                                                   \
    _alt->idx [_slot] = _idx;                                                  \
    _nd->hash_field = _fp ^ _h;                                                \
    _bk->hash[slot] = 0u;                                                      \
    _bk->idx [slot] = (u32)RIX_NIL;                                      \
    return (int)slot;                                                          \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* kickout: make a free slot in bk by moving occupants.                */      \
/*                                                                    */      \
/* Pass 1: try flipflop for each of 16 slots (direct move).            */      \
/* Pass 2: for each slot, recurse into occupant's alt bucket to make  */      \
/*         room there first, then flipflop (guaranteed to succeed).    */      \
/* Returns freed slot index, or -1 (table untouched on failure).       */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_kickout(struct rix_hash_bucket_s *buckets,                             \
               struct type *base,                                             \
               unsigned mask,                                                 \
               unsigned bk_idx,                                               \
               int depth)                                                     \
{                                                                             \
    if (depth <= 0) return -1;                                                \
    struct rix_hash_bucket_s *_bk = buckets + bk_idx;                         \
                                                                              \
    for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {             \
        if (name##_flipflop(buckets, base, mask, bk_idx, _s) >= 0)           \
            return (int)_s;                                                   \
    }                                                                         \
                                                                              \
    for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {             \
        u32     _fp = _bk->hash[_s];                                     \
        unsigned     _si = _bk->idx [_s];                                      \
        struct type *_sn = name##_hptr(base, _si);                            \
        if (!_sn) continue;                                                   \
        u32 _sh = _sn->hash_field;                                        \
        unsigned _ab = (_fp ^ _sh) & mask;                                     \
        if (name##_kickout(buckets, base, mask, _ab, depth - 1) >= 0) {       \
            /* Re-check: recursive chain may have relocated bk[_s] */        \
            if (_bk->hash[_s] != _fp || _bk->idx[_s] != _si) {              \
                int _fs = name##_find_empty(buckets, bk_idx);                \
                if (_fs >= 0) return _fs;                                    \
                continue;                                                    \
            }                                                                \
            name##_flipflop(buckets, base, mask, bk_idx, _s);                \
            return (int)_s;                                                   \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Internal insert helper with precomputed hash                       */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_insert_hashed(struct name *head,                                       \
                     struct rix_hash_bucket_s *buckets,                       \
                     struct type *base,                                       \
                     struct type *elm,                                        \
                     union rix_hash_hash_u _h)                                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned _bk0, _bk1;                                                      \
    u32 _fp;                                                             \
    u32 _hits_fp[2];                                                     \
    u32 _hits_zero[2];                                                   \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    /* Store h.val32[0] in hash_field initially; updated later if bk1 chosen */\
    elm->hash_field = _h.val32[0];                                            \
    rix_hash_prefetch_bucket_of(buckets + _bk0);                              \
    if (_bk1 != _bk0)                                                         \
        rix_hash_prefetch_bucket_of(buckets + _bk1);                          \
                                                                              \
    /* Scan both candidate buckets once up front so duplicate detection and   \
     * empty-slot search reuse the same CL0 load. */                          \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        _RIX_HASH_FIND_U32X16_2(_bk->hash, _fp, 0u,                           \
                                &_hits_fp[_i], &_hits_zero[_i]);              \
    }                                                                         \
                                                                              \
    /* Duplicate check in both candidate buckets before inserting.            \
     * fp != 0, so no _nilm filter is needed. */                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        u32 _hits = _hits_fp[_i];                                        \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            struct type  *_node = name##_hptr(base, _bk->idx[_bit]);          \
            RIX_ASSUME_NONNULL(_node);                                       \
            if (cmp_fn(&elm->key_field, &_node->key_field) == 0)              \
                return _node; /* duplicate */                                 \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Fast path: find an empty slot in bk0 then bk1.                     */  \
    /* Empty slot: hash[s]==0 (fp is never 0 by construction). CL0 only.  */  \
    /* node->hash_field stores the hash corresponding to the bucket where */  \
    /* the node is placed: h.val32[0] (bk0) or h.val32[1] (bk1).          */  \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_s *_bk = buckets + _bki;                       \
        u32 _nilm = _hits_zero[_i];                                      \
        if (_nilm) {                                                          \
            unsigned _slot   = (unsigned)__builtin_ctz(_nilm);                \
            _bk->hash[_slot] = _fp;                                           \
            _bk->idx [_slot] = name##_hidx(base, elm);                        \
            /* update hash_field: store the hash of the chosen bucket */      \
            if (_i == 1) elm->hash_field = _h.val32[1];                       \
            head->rhh_nb++;                                                   \
            return NULL; /* success */                                        \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Slow path: kickout - make room in bk0 or bk1.                       */ \
    /* On failure, table is untouched.                                     */ \
    {                                                                         \
        int _pos;                                                             \
        unsigned _bki;                                                        \
        _pos = name##_kickout(buckets, base, mask, _bk0,                      \
                              RIX_HASH_FOLLOW_DEPTH);                         \
        if (_pos >= 0) {                                                      \
            _bki = _bk0;                                                      \
        } else {                                                              \
            _pos = name##_kickout(buckets, base, mask, _bk1,                  \
                                  RIX_HASH_FOLLOW_DEPTH);                     \
            if (_pos < 0)                                                     \
                return elm; /* table full - no modification */                \
            _bki = _bk1;                                                      \
            elm->hash_field = _h.val32[1];                                    \
        }                                                                     \
        struct rix_hash_bucket_s *_bk = buckets + _bki;                       \
        _bk->hash[_pos] = _fp;                                                \
        _bk->idx [_pos] = name##_hidx(base, elm);                            \
        head->rhh_nb++;                                                       \
        return NULL; /* success */                                            \
    }                                                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Insert with cuckoo kickout                                         */      \
/*                                                                    */      \
/* Returns:                                                           */      \
/*   NULL     - inserted successfully                                 */      \
/*   elm      - table full (kickout exhausted)                        */      \
/*   other    - duplicate found, returns the existing node            */      \
/* ================================================================== */      \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    return name##_insert_hashed(head, buckets, base, elm, _h);                \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Remove - evict a known node from the table                         */      \
/*                                                                    */      \
/* Takes the node pointer directly (not a key).                       */      \
/* elm->hash_field & mask always gives the current bucket index, so   */      \
/* removal is O(1): find node by idx, then zero-clear hash[slot].     */      \
/* No key comparison, no bk1 fallback.                                */      \
/* Returns elm on success, NULL if elm is not currently in the table. */      \
/* ================================================================== */      \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot);                                              \
                                                                              \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    /* hash_field & mask == current bucket (insert invariant) */              \
    unsigned _bk = (unsigned)(elm->hash_field & head->rhh_mask);              \
    struct rix_hash_bucket_s *_b = buckets + _bk;                             \
    u32 _hits = _RIX_HASH_FIND_U32X16(_b->idx, (u32)node_idx);      \
    if (_hits) {                                                              \
        unsigned _slot   = (unsigned)__builtin_ctz(_hits);                    \
        if (name##_remove_at(head, buckets, _bk, _slot) !=                    \
            (unsigned)RIX_NIL)                                                \
            return elm;                                                       \
    }                                                                         \
    return NULL; /* not in table */                                           \
}                                                                             \
                                                                              \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    struct rix_hash_bucket_s *_b = buckets + bk;                              \
    unsigned _idx;                                                            \
    RIX_ASSERT(slot < RIX_HASH_BUCKET_ENTRY_SZ);                              \
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                     \
        return (unsigned)RIX_NIL;                                             \
    _idx = _b->idx[slot];                                                     \
    if (_idx == (unsigned)RIX_NIL)                                            \
        return (unsigned)RIX_NIL;                                             \
    _b->hash[slot] = 0u;                                                      \
    _b->idx [slot] = (u32)RIX_NIL;                                       \
    head->rhh_nb--;                                                           \
    return _idx;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Walk - iterate over all occupied slots                             */      \
/*                                                                    */      \
/* cb(node, arg): return 0 to continue, non-zero to stop.             */      \
/* Returns 0 when all entries are visited, or the first non-zero cb   */      \
/* return value.                                                      */      \
/* ================================================================== */      \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            int (*cb)(struct type *, void *),                                 \
            void *arg)                                                        \
{                                                                             \
    unsigned _nb_bk = head->rhh_mask + 1u;                                    \
    for (unsigned _b = 0u; _b < _nb_bk; _b++) {                               \
        struct rix_hash_bucket_s *_bk = buckets + _b;                         \
        for (unsigned _s = 0u; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {         \
            unsigned     _nidx = _bk->idx[_s];                                \
            if (_nidx == (unsigned)RIX_NIL)                                   \
                continue;                                                     \
            struct type *_node = name##_hptr(base, _nidx);                    \
            int   _r    = cb(_node, arg);                                     \
            if (_r)                                                           \
                return _r;                                                    \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}


#endif /* _RIX_HASH_FP_H_ */
