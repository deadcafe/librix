/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_keyonly.h - key-only variant (no hash_field or slot_field in node).
 *
 * Requires: rix_hash_common.h
 *
 * Node struct needs only the key field.  Trade-off: remove requires re-hashing
 * the key to find the bucket (no O(1) shortcut via hash_field).
 *
 * Generated functions (section order):
 *   init, staged find (x1, xN), find, insert, remove, remove_at, walk
 */

#ifndef _RIX_HASH_KEYONLY_H_
#  define _RIX_HASH_KEYONLY_H_

#  include "rix_hash_common.h"

/*===========================================================================
 * RIX_HASH_GENERATE_KEYONLY(name, type, key_field, cmp_fn)
 * RIX_HASH_GENERATE_KEYONLY_EX(name, type, key_field, cmp_fn, hash_fn)
 *
 * Variant of RIX_HASH_GENERATE without hash_field in the node struct.
 * All find operations are identical to RIX_HASH_GENERATE.
 * insert: no hash_field writes; cuckoo kickout re-hashes victim's key
 *         to locate the alternate bucket (one extra hash call per kickout).
 * remove: re-hashes elm's key to try bk_0 then bk_1 (two scans worst case).
 * Benefit: smaller node struct - no u32 hash_field member required.
 *===========================================================================*/
#  define RIX_HASH_KEYONLY_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, attr) \
    attr void name##_init(struct name *head, unsigned nb_bk);                 \
    attr struct type *name##_insert(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,        \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_s *buckets,         \
                                   unsigned bk,                               \
                                   unsigned slot);                            \
    attr struct type *name##_remove(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,        \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,                   \
                         struct type *base,                                   \
                         int (*cb)(struct type *, void *),                    \
                         void *arg);

#  define RIX_HASH_KEYONLY_PROTOTYPE_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_KEYONLY_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, )

#  define RIX_HASH_KEYONLY_PROTOTYPE_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_KEYONLY_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_KEYONLY_PROTOTYPE(name, type, key_field, cmp_fn) \
    RIX_HASH_KEYONLY_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, )

#  define RIX_HASH_KEYONLY_PROTOTYPE_STATIC(name, type, key_field, cmp_fn) \
    RIX_HASH_KEYONLY_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_GENERATE_KEYONLY_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn, hash_fn, )

#  define RIX_HASH_GENERATE_KEYONLY_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn, hash_fn, RIX_UNUSED static)

#  define RIX_HASH_GENERATE_KEYONLY(name, type, key_field, cmp_fn)            \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                   \
    RIX_HASH_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,         \
                                    RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_GENERATE_KEYONLY_STATIC(name, type, key_field, cmp_fn)     \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                   \
    RIX_HASH_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,         \
                                    RIX_HASH_DEFAULT_HASH_FN_NAME(name),     \
                                    RIX_UNUSED static)

#  define RIX_HASH_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn, hash_fn, attr) \
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
name##_hash_key_masked(struct rix_hash_find_ctx_s *ctx,                       \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_s *buckets,                     \
                       const RIX_HASH_KEY_TYPE(type, key_field) *key,        \
                       unsigned hash_mask,                                    \
                       unsigned bk_mask)                                      \
{                                                                             \
    union rix_hash_hash_u _h =                                                \
        hash_fn(key, hash_mask);                                              \
    unsigned _bk0, _bk1;                                                      \
    u32 _fp;                                                                  \
    _fp = rix_hash_fp(_h, bk_mask, &_bk0, &_bk1);                       \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    rix_hash_prefetch_bucket_of(ctx->bk[0]);                                  \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_find_ctx_s *ctx,                              \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets,                            \
                const RIX_HASH_KEY_TYPE(type, key_field) *key)               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_masked(ctx, head, buckets, key, mask, mask);              \
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
    ctx->fp_hits[0] = RIX_HASH_FIND_U32X16(ctx->bk[0]->hash, ctx->fp);       \
    ctx->fp_hits[1] = 0u;                                                     \
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
/* fp != 0 (XOR-based), so no _nilm filter is needed in bk_1 scan.     */     \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_find_ctx_s *ctx,                               \
               struct type *base)                                             \
{                                                                             \
    u32 _hits = ctx->fp_hits[0];                                              \
    while (_hits) {                                                           \
        unsigned      _bit  = (unsigned)__builtin_ctz(_hits);                 \
        _hits &= _hits - 1u;                                                  \
        unsigned      _nidx = ctx->bk[0]->idx[_bit];                          \
        struct type  *_node = name##_hptr(base, _nidx);                       \
        if (cmp_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,     \
                   &_node->key_field) == 0)                                   \
            return _node;                                                     \
    }                                                                         \
    {                                                                         \
        _hits = RIX_HASH_FIND_U32X16(ctx->bk[1]->hash, ctx->fp);             \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            unsigned      _nidx = ctx->bk[1]->idx[_bit];                      \
            struct type  *_node = name##_hptr(base, _nidx);                   \
            if (cmp_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key, \
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
name##_hash_key_n_masked(struct rix_hash_find_ctx_s *ctx,                     \
                         int n,                                               \
                         struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,                   \
                         const RIX_HASH_KEY_TYPE(type, key_field) * const *keys, \
                         unsigned hash_mask,                                  \
                         unsigned bk_mask)                                    \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_hash_key_masked(&ctx[_j], head, buckets, keys[_j],             \
                               hash_mask, bk_mask);                           \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_find_ctx_s *ctx,                            \
                  int n,                                                      \
                  struct name *head,                                          \
                  struct rix_hash_bucket_s *buckets,                          \
                  const RIX_HASH_KEY_TYPE(type, key_field) * const *keys)    \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, mask, mask);        \
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
            const RIX_HASH_KEY_TYPE(type, key_field) *key)                   \
{                                                                             \
    struct rix_hash_find_ctx_s _ctx;                                          \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk (&_ctx, head, buckets);                                    \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* find_empty: return free slot index in bucket, or -1                */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_find_empty(struct rix_hash_bucket_s *buckets,                          \
                  unsigned bk_idx)                                            \
{                                                                             \
    u32 _nilm = RIX_HASH_FIND_U32X16(buckets[bk_idx].hash, 0u);              \
    if (!_nilm) return -1;                                                    \
    return (int)__builtin_ctz(_nilm);                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* flipflop: move bk[slot] to its alt bucket if alt has a free slot.  */      \
/* Nohf: re-hash key to find alt bucket (no hash_field stored).       */      \
/* Returns freed slot on success, -1 on failure.                      */      \
/* Non-destructive on failure.                                        */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_flipflop(struct rix_hash_bucket_s *buckets,                            \
                struct type *base,                                            \
                unsigned mask,                                                \
                unsigned bk_idx,                                              \
                unsigned slot)                                                \
{                                                                             \
    struct rix_hash_bucket_s *_bk = buckets + bk_idx;                         \
    u32     _fp  = _bk->hash[slot];                                           \
    unsigned     _idx = _bk->idx [slot];                                      \
    struct type *_nd  = name##_hptr(base, _idx);                              \
    if (!_nd) return -1;                                                      \
    union rix_hash_hash_u _rh =                                               \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)                 \
                &_nd->key_field, mask);                                       \
    unsigned _rb0 = _rh.val32[0] & mask;                                      \
    unsigned _rb1 = _rh.val32[1] & mask;                                      \
    unsigned _ab  = (bk_idx == _rb0) ? _rb1 : _rb0;                           \
    int _slot = name##_find_empty(buckets, _ab);                              \
    if (_slot < 0) return -1;                                                 \
    struct rix_hash_bucket_s *_alt = buckets + _ab;                           \
    _alt->hash[_slot] = _fp;                                                  \
    _alt->idx [_slot] = _idx;                                                 \
    _bk->hash[slot] = 0u;                                                     \
    _bk->idx [slot] = (u32)RIX_NIL;                                           \
    return (int)slot;                                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* kickout: make a free slot in bk by moving occupants.               */      \
/*                                                                    */      \
/* Pass 1: try flipflop for each of 16 slots (direct move).           */      \
/* Pass 2: for each slot, recurse into occupant's alt bucket to make  */      \
/*         room there first, then flipflop (guaranteed to succeed).   */      \
/* Returns freed slot index, or -1 (table untouched on failure).      */      \
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
    for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {              \
        if (name##_flipflop(buckets, base, mask, bk_idx, _s) >= 0)            \
            return (int)_s;                                                   \
    }                                                                         \
    for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {              \
        u32     _fp = _bk->hash[_s];                                          \
        unsigned     _si = _bk->idx [_s];                                     \
        struct type *_sn = name##_hptr(base, _si);                            \
        if (!_sn) continue;                                                   \
        union rix_hash_hash_u _sh =                                           \
            hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)             \
                    &_sn->key_field, mask);                                   \
        unsigned _sb0 = _sh.val32[0] & mask;                                  \
        unsigned _sb1 = _sh.val32[1] & mask;                                  \
        unsigned _ab  = (bk_idx == _sb0) ? _sb1 : _sb0;                       \
        if (name##_kickout(buckets, base, mask, _ab, depth - 1) >= 0) {       \
            /* Re-check: recursive chain may have relocated bk[_s] */         \
            if (_bk->hash[_s] != _fp || _bk->idx[_s] != _si) {                \
                int _fs = name##_find_empty(buckets, bk_idx);                 \
                if (_fs >= 0) return _fs;                                     \
                continue;                                                     \
            }                                                                 \
            int _slot = name##_find_empty(buckets, _ab);                      \
            struct rix_hash_bucket_s *_alt = buckets + _ab;                   \
            _alt->hash[_slot] = _fp;                                          \
            _alt->idx [_slot] = _si;                                          \
            _bk->hash[_s] = 0u;                                               \
            _bk->idx [_s] = (u32)RIX_NIL;                                     \
            return (int)_s;                                                   \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Insert with cuckoo kickout (no hash_field)                         */      \
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
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    unsigned _bk0, _bk1;                                                      \
    u32 _fp;                                                                  \
    u32 _hits_fp[2];                                                          \
    u32 _hits_zero[2];                                                        \
    _fp = rix_hash_fp(_h, mask, &_bk0, &_bk1);                          \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        RIX_HASH_FIND_U32X16_2(_bk->hash, _fp, 0u,                           \
                                &_hits_fp[_i], &_hits_zero[_i]);              \
        u32 _hits = _hits_fp[_i];                                             \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            struct type  *_node = name##_hptr(base, _bk->idx[_bit]);          \
            RIX_ASSUME_NONNULL(_node);                                        \
            if (cmp_fn(&elm->key_field, &_node->key_field) == 0)              \
                return _node;                                                 \
        }                                                                     \
    }                                                                         \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_s *_bk = buckets + _bki;                       \
        u32 _nilm = _hits_zero[_i];                                           \
        if (_nilm) {                                                          \
            unsigned _slot   = (unsigned)__builtin_ctz(_nilm);                \
            _bk->hash[_slot] = _fp;                                           \
            _bk->idx [_slot] = name##_hidx(base, elm);                        \
            head->rhh_nb++;                                                   \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    /* Slow path: non-destructive recursive kickout */                        \
    {                                                                         \
        int _pos; unsigned _bki;                                              \
        _pos = name##_kickout(buckets, base, mask, _bk0,                      \
                              RIX_HASH_FOLLOW_DEPTH);                         \
        if (_pos >= 0) { _bki = _bk0; }                                       \
        else {                                                                \
            _pos = name##_kickout(buckets, base, mask, _bk1,                  \
                                  RIX_HASH_FOLLOW_DEPTH);                     \
            if (_pos < 0) return elm; /* table full - no modification */      \
            _bki = _bk1;                                                      \
        }                                                                     \
        struct rix_hash_bucket_s *_bk = buckets + _bki;                       \
        _bk->hash[_pos] = _fp;                                                \
        _bk->idx [_pos] = name##_hidx(base, elm);                             \
        head->rhh_nb++;                                                       \
        return NULL;                                                          \
    }                                                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Remove (re-hash key; scan bk_0 then bk_1)                          */      \
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
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    unsigned _bk0, _bk1;                                                      \
    (void)rix_hash_fp(_h, mask, &_bk0, &_bk1);                               \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_s *_b = buckets + _bki;                        \
        u32 _hits = RIX_HASH_FIND_U32X16(_b->idx,                            \
                                               (u32)node_idx);                \
        if (_hits) {                                                          \
            unsigned _slot   = (unsigned)__builtin_ctz(_hits);                \
            if (name##_remove_at(head, buckets, _bki, _slot) !=               \
                (unsigned)RIX_NIL)                                            \
                return elm;                                                   \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
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
    _b->idx [slot] = (u32)RIX_NIL;                                            \
    head->rhh_nb--;                                                           \
    return _idx;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Walk                                                               */      \
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

#endif /* _RIX_HASH_KEYONLY_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
