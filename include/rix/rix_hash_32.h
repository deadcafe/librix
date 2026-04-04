/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * rix_hash_32.h - index-based cuckoo hash table for 32-bit keys, C11
 *
 * Key type: u32, stored directly in bucket (no fingerprint array).
 * No hash_field required in the node struct.
 * Runtime SIMD dispatch via rix_hash_arch (shared with rix_hash.h).
 * Index-based: RIX_NIL=0, 1-origin indices (no raw pointers stored).
 *
 * Bucket layout: key[16] (actual 32-bit keys) + idx[16] (node indices).
 * Empty slot sentinel: idx[s] == RIX_NIL (0).
 *
 * scan_bk performs exact key match against bk->key[].
 * cmp_key filters hits by idx != RIX_NIL (handles key=0 edge case).
 * remove re-hashes elm->key_field to locate the bucket (O(1), 2 buckets).
 * kickout re-hashes the victim key to find the alternate bucket.
 */

#ifndef _RIX_HASH32_H_
#  define _RIX_HASH32_H_

#  include "rix_hash_common.h"

/*===========================================================================
 * Bucket layout
 * 128 bytes = 2 x 64-byte cache lines, aligned to 64 bytes.
 *===========================================================================*/
struct rix_hash32_bucket_s {
    u32 key[RIX_HASH_BUCKET_ENTRY_SZ]; /* 64 bytes: 32-bit keys         */
    u32 idx[RIX_HASH_BUCKET_ENTRY_SZ]; /* 64 bytes: 1-origin node idx   */
} __attribute__((aligned(64)));

/*===========================================================================
 * Head struct
 *===========================================================================*/
#  define RIX_HASH32_HEAD(name)                                               \
    struct name {                                                             \
        unsigned rhh_mask;                                                    \
        unsigned rhh_nb;                                                      \
    }

/*===========================================================================
 * Find context (staged pipeline)
 *===========================================================================*/
struct rix_hash32_find_ctx_s {
    struct rix_hash32_bucket_s *bk[2];
    u32                    key;        /* 32-bit search key             */
    u32                    hits[2];    /* bitmask: key match in bk[i]   */
};

/*===========================================================================
 * RIX_HASH32_GENERATE(name, type, key_field, invalid_key)
 *
 *   name        - head struct tag AND generated-function prefix (must match
 *                 the tag given to RIX_HASH32_HEAD)
 *   type        - element struct type
 *   key_field   - field name inside type; must be u32
 *   invalid_key - compile-time constant written to bk->key[] for empty/
 *                 removed slots.  Must not equal any key the caller will
 *                 ever insert or search.
 *
 * The hash function is provided internally via rix_hash_arch->hash_u32.
 * No hash_field is required in the node struct.
 *
 * Invalid key contract:
 *   name_init() fills every bk->key[] slot with invalid_key.
 *   name_remove() restores the freed slot's key to invalid_key.
 *   With this contract:
 *     - scan_bk never hits an empty/removed slot for a valid search key,
 *       so cmp_key / prefetch_node need no idx != RIX_NIL guard.
 *     - cmp_key reduces to a single __builtin_ctz on the hit mask.
 *     - invalid_key is a compile-time constant; no runtime load from head.
 *
 * Generated functions:
 *
 *   Initialisation (must be called before any other operation):
 *     void  name_init (head, buckets, nb_bk)
 *
 *   Staged find (x1):
 *     void  name_hash_key      (ctx, head, buckets, key)  key: u32
 *     void  name_scan_bk       (ctx, head, buckets)
 *     void  name_prefetch_node (ctx, base)
 *     type *name_cmp_key       (ctx, base)
 *
 *   Staged find (xN bulk; FORCE_INLINE + constant n -> compiler unrolls):
 *     void  name_hash_key_n    (ctx, n, head, buckets, keys)
 *     void  name_scan_bk_n     (ctx, n, head, buckets)
 *     void  name_prefetch_node_n(ctx, n, base)
 *     void  name_cmp_key_n     (ctx, n, base, results)
 *
 *   Single-shot ops:
 *     type *name_find  (head, buckets, base, key)   key is u32
 *     type *name_insert(head, buckets, base, elm)
 *     type *name_remove(head, buckets, base, elm)   elm is type *
 *     int   name_walk  (head, buckets, base, cb, arg)
 *===========================================================================*/
#  define RIX_HASH32_PROTOTYPE_INTERNAL(name, type, key_field, invalid_key, attr) \
    attr void name##_init(struct name *head,                                   \
                          struct rix_hash32_bucket_s *buckets,                 \
                          unsigned nb_bk);                                     \
    attr type *name##_insert(struct name *head,                                \
                             struct rix_hash32_bucket_s *buckets,              \
                             type *base, type *elm);                           \
    attr type *name##_remove(struct name *head,                                \
                             struct rix_hash32_bucket_s *buckets,              \
                             type *base, type *elm);                           \
    attr int name##_walk(struct name *head,                                    \
                         struct rix_hash32_bucket_s *buckets,                  \
                         type *base,                                           \
                         int (*cb)(type *, void *),                            \
                         void *arg);

#  define RIX_HASH32_PROTOTYPE(name, type, key_field, invalid_key) \
    RIX_HASH32_PROTOTYPE_INTERNAL(name, type, key_field, invalid_key, )

#  define RIX_HASH32_PROTOTYPE_STATIC(name, type, key_field, invalid_key) \
    RIX_HASH32_PROTOTYPE_INTERNAL(name, type, key_field, invalid_key, RIX_UNUSED static)

#  define RIX_HASH32_GENERATE(name, type, key_field, invalid_key) \
    RIX_HASH32_GENERATE_INTERNAL(name, type, key_field, invalid_key, )

#  define RIX_HASH32_GENERATE_STATIC(name, type, key_field, invalid_key) \
    RIX_HASH32_GENERATE_INTERNAL(name, type, key_field, invalid_key, RIX_UNUSED static)

#  define RIX_HASH32_GENERATE_INTERNAL(name, type, key_field, invalid_key, attr) \
                                                                              \
/* ================================================================== */      \
/* Initialisation                                                     */      \
/*                                                                    */      \
/* Must be called before any other operation on this table.           */      \
/* Fills every bk->key[] slot with invalid_key and every bk->idx[]    */      \
/* slot with RIX_NIL.  nb_bk must be a power of 2.                    */      \
/* ================================================================== */      \
attr void                                                                     \
name##_init(struct name *head,                                                \
            struct rix_hash32_bucket_s *buckets,                              \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    head->rhh_nb   = 0u;                                                      \
    for (unsigned _b = 0u; _b < nb_bk; _b++) {                                \
        struct rix_hash32_bucket_s *_bk = buckets + _b;                       \
        for (unsigned _s = 0u; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {         \
            _bk->key[_s] = (u32)(invalid_key);                                \
            _bk->idx[_s] = (u32)RIX_NIL;                                      \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/* ------------------------------------------------------------------ */      \
/* Internal helpers: 1-origin index <-> pointer                       */      \
/* ------------------------------------------------------------------ */      \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(type *base, const type *p) {                                      \
    return RIX_IDX_FROM_PTR(base, (type *)(uintptr_t)p);                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_hptr(type *base, unsigned i) {                                         \
    return (type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);           \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - x1                                                   */      \
/* ================================================================== */      \
                                                                              \
/* Stage 1: compute hash, resolve bucket pointers, issue bk[0] prefetch. */   \
/* Prefetch both cache lines of bk[0]:                                  */    \
/*   CL0 ( +0): key[ 0..15] - needed by scan_bk                         */    \
/*   CL1 (+64): idx[ 0..15] - needed by idx-only path and cmp_key       */    \
/* bk[1] is not prefetched; it is scanned lazily on bk[0] miss.         */    \
/* key is passed by value (u32), not as a pointer. */                         \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash32_find_ctx_s *ctx,                            \
                struct name *head,                                            \
                struct rix_hash32_bucket_s *buckets,                          \
                u32 key)                                                      \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h = rix_hash_arch->hash_u32(key, mask);            \
    ctx->key   = key;                                                         \
    ctx->bk[0] = buckets + (_h.val32[0] & mask);                              \
    ctx->bk[1] = buckets + (_h.val32[1] & mask);                              \
    __builtin_prefetch((const char *)ctx->bk[0] +  0, 0, 1); /* CL0: key */   \
    __builtin_prefetch((const char *)ctx->bk[0] + 64, 0, 1); /* CL1: idx */   \
    /* bk[1] not prefetched: most entries reside in bk[0] at moderate fill. */ \
    /* bk[1] is fetched lazily in cmp_key only on bk[0] miss. */              \
}                                                                             \
                                                                              \
/* Stage 2: scan bk[0]->key[] for ctx->key; produce hits[0] bitmask. */       \
/* Empty slots (idx=NIL) may appear in hits if their key[] happens to equal */ \
/* ctx->key (edge case: key=0 matches zero-initialized empty slots). */       \
/* The idx != NIL guard in cmp_key and prefetch_node filters them out. */     \
/* hits[1] = 0; bk[1] is scanned lazily in cmp_key on bk[0] miss. */          \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash32_find_ctx_s *ctx,                             \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash32_bucket_s *buckets __attribute__((unused)))   \
{                                                                             \
    ctx->hits[0] = rix_hash_arch->find_u32x16(ctx->bk[0]->key, ctx->key);     \
    ctx->hits[1] = 0u;                                                        \
}                                                                             \
                                                                              \
/* Stage 3: prefetch node data for all hits[0] positions. */                  \
/* Hides node-fetch latency (DRAM) before cmp_key is called. */               \
/* With invalid_key contract: every hit is an occupied slot; */               \
/* no idx != RIX_NIL guard is needed. */                                      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node(struct rix_hash32_find_ctx_s *ctx,                       \
                     type *base)                                              \
{                                                                             \
    u32 _hits = ctx->hits[0];                                                 \
    while (_hits) {                                                           \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        _hits &= _hits - 1u;                                                  \
        __builtin_prefetch(                                                   \
            name##_hptr(base, ctx->bk[0]->idx[_bit]), 0, 0);                  \
    }                                                                         \
}                                                                             \
                                                                              \
/* Stage 4: return the node for the hit; lazily scan bk[1] on miss. */        \
/* With invalid_key contract: any key match in scan_bk is an occupied */      \
/* slot, so no idx != RIX_NIL guard is needed.  Unique-key invariant */       \
/* guarantees at most one hit per bucket, so __builtin_ctz suffices. */       \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key(struct rix_hash32_find_ctx_s *ctx,                             \
               type *base)                                                    \
{                                                                             \
    /* Fast path: bk[0] */                                                    \
    u32 _hits = ctx->hits[0];                                                 \
    if (_hits) {                                                              \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        return name##_hptr(base, ctx->bk[0]->idx[_bit]);                      \
    }                                                                         \
    /* Slow path: bk[0] miss - lazily fetch and scan bk[1] */                 \
    {                                                                         \
        _hits = rix_hash_arch->find_u32x16(ctx->bk[1]->key, ctx->key);        \
        if (_hits) {                                                          \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            return name##_hptr(base, ctx->bk[1]->idx[_bit]);                  \
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
name##_hash_key_n(struct rix_hash32_find_ctx_s *ctx,                          \
                  int n,                                                      \
                  struct name *head,                                          \
                  struct rix_hash32_bucket_s *buckets,                        \
                  const u32 *keys)                                            \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_hash_key(&ctx[_j], head, buckets, keys[_j]);                   \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash32_find_ctx_s *ctx,                           \
                 int n,                                                       \
                 struct name *head,                                           \
                 struct rix_hash32_bucket_s *buckets)                         \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_scan_bk(&ctx[_j], head, buckets);                              \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash32_find_ctx_s *ctx,                     \
                       int n,                                                 \
                       type *base)                                            \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_prefetch_node(&ctx[_j], base);                                 \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash32_find_ctx_s *ctx,                           \
                 int n,                                                       \
                 type *base,                                                  \
                 type **results)                                              \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        results[_j] = name##_cmp_key(&ctx[_j], base);                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Single-shot find = hash_key + scan_bk + cmp_key                    */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_find(struct name *head,                                                \
            struct rix_hash32_bucket_s *buckets,                              \
            type *base,                                                       \
            u32 key)                                                          \
{                                                                             \
    struct rix_hash32_find_ctx_s _ctx;                                        \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk (&_ctx, head, buckets);                                    \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* find_empty: return free slot in bucket, or -1                      */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_find_empty(struct rix_hash32_bucket_s *buckets,                        \
                  unsigned bk_idx)                                            \
{                                                                             \
    u32 _nilm = rix_hash_arch->find_u32x16(                                   \
        buckets[bk_idx].key, (u32)(invalid_key));                             \
    if (!_nilm) return -1;                                                    \
    return (int)__builtin_ctz(_nilm);                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* flipflop: move bk[slot] to its alt bucket if alt has a free slot.  */      \
/* Returns freed slot on success, -1 on failure.                      */      \
/* Non-destructive on failure.                                        */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_flipflop(struct rix_hash32_bucket_s *buckets,                          \
                unsigned mask,                                                \
                unsigned bk_idx,                                              \
                unsigned slot)                                                \
{                                                                             \
    struct rix_hash32_bucket_s *_bk = buckets + bk_idx;                       \
    u32 _key = _bk->key[slot];                                                \
    unsigned _idx = _bk->idx[slot];                                           \
    if (_idx == (unsigned)RIX_NIL) return -1;                                 \
    union rix_hash_hash_u _rh = rix_hash_arch->hash_u32(_key, mask);          \
    unsigned _rb0 = _rh.val32[0] & mask;                                      \
    unsigned _rb1 = _rh.val32[1] & mask;                                      \
    unsigned _ab  = (bk_idx == _rb0) ? _rb1 : _rb0;                           \
    int _es = name##_find_empty(buckets, _ab);                                \
    if (_es < 0) return -1;                                                   \
    struct rix_hash32_bucket_s *_alt = buckets + _ab;                         \
    _alt->key[_es] = _key;                                                    \
    _alt->idx[_es] = _idx;                                                    \
    _bk->key[slot] = (u32)(invalid_key);                                      \
    _bk->idx[slot] = (u32)RIX_NIL;                                            \
    return (int)slot;                                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* kickout: make a free slot in bk by moving occupants.               */      \
/* Pass 1: try flipflop for each slot (direct move).                  */      \
/* Pass 2: recurse into occupant's alt bucket, then move.             */      \
/* Returns freed slot index, or -1 (table untouched on failure).      */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_kickout(struct rix_hash32_bucket_s *buckets,                           \
               unsigned mask,                                                 \
               unsigned bk_idx,                                               \
               int depth)                                                     \
{                                                                             \
    if (depth <= 0) return -1;                                                \
    struct rix_hash32_bucket_s *_bk = buckets + bk_idx;                       \
                                                                              \
    for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {              \
        if (name##_flipflop(buckets, mask, bk_idx, _s) >= 0)                  \
            return (int)_s;                                                   \
    }                                                                         \
    for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {              \
        u32 _key = _bk->key[_s];                                              \
        unsigned _si  = _bk->idx[_s];                                         \
        if (_si == (unsigned)RIX_NIL) continue;                               \
        union rix_hash_hash_u _sh = rix_hash_arch->hash_u32(_key, mask);      \
        unsigned _sb0 = _sh.val32[0] & mask;                                  \
        unsigned _sb1 = _sh.val32[1] & mask;                                  \
        unsigned _ab  = (bk_idx == _sb0) ? _sb1 : _sb0;                       \
        if (name##_kickout(buckets, mask, _ab, depth - 1) >= 0) {             \
            /* Re-check: recursive chain may have relocated bk[_s] */         \
            if (_bk->key[_s] != _key || _bk->idx[_s] != _si) {                \
                int _fs = name##_find_empty(buckets, bk_idx);                 \
                if (_fs >= 0) return _fs;                                     \
                continue;                                                     \
            }                                                                 \
            int _es = name##_find_empty(buckets, _ab);                        \
            struct rix_hash32_bucket_s *_alt = buckets + _ab;                 \
            _alt->key[_es] = _key;                                            \
            _alt->idx[_es] = _si;                                             \
            _bk->key[_s] = (u32)(invalid_key);                                \
            _bk->idx[_s] = (u32)RIX_NIL;                                      \
            return (int)_s;                                                   \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
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
attr type *                                                                   \
name##_insert(struct name *head,                                              \
              struct rix_hash32_bucket_s *buckets,                            \
              type *base,                                                     \
              type *elm)                                                      \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        rix_hash_arch->hash_u32((u32)elm->key_field, mask);                   \
    unsigned _bk0 = _h.val32[0] & mask;                                       \
    unsigned _bk1 = _h.val32[1] & mask;                                       \
                                                                              \
    /* Duplicate check */                                                     \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash32_bucket_s *_bk =                                     \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        u32 _hits = rix_hash_arch->find_u32x16(_bk->key,                      \
                                                    (u32)elm->key_field);     \
        if (_hits) {                                                          \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            return name##_hptr(base, _bk->idx[_bit]);                         \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Fast path: empty slot in bk0 or bk1 */                                 \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        int _slot = name##_find_empty(buckets, _bki);                         \
        if (_slot >= 0) {                                                     \
            struct rix_hash32_bucket_s *_bk = buckets + _bki;                 \
            _bk->key[_slot] = (u32)elm->key_field;                            \
            _bk->idx[_slot] = name##_hidx(base, elm);                         \
            head->rhh_nb++;                                                   \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Slow path: non-destructive recursive kickout */                        \
    {                                                                         \
        int _slot; unsigned _bki;                                             \
        _slot = name##_kickout(buckets, mask, _bk0,                           \
                               RIX_HASH_FOLLOW_DEPTH);                        \
        if (_slot >= 0) { _bki = _bk0; }                                      \
        else {                                                                \
            _slot = name##_kickout(buckets, mask, _bk1,                       \
                                   RIX_HASH_FOLLOW_DEPTH);                    \
            if (_slot < 0) return elm; /* table full - no modification */     \
            _bki = _bk1;                                                      \
        }                                                                     \
        struct rix_hash32_bucket_s *_bk = buckets + _bki;                     \
        _bk->key[_slot] = (u32)elm->key_field;                                \
        _bk->idx[_slot] = name##_hidx(base, elm);                             \
        head->rhh_nb++;                                                       \
        return NULL;                                                          \
    }                                                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Remove - evict a known node from the table                         */      \
/*                                                                    */      \
/* Takes the node pointer directly (not a key).                       */      \
/* Re-hashes elm->key_field to locate the two candidate buckets, then */      \
/* searches each by node index.  O(1): at most 2 find_u32x16 calls.   */      \
/* Returns elm on success, NULL if elm is not currently in the table. */      \
/* ================================================================== */      \
attr type *                                                                   \
name##_remove(struct name *head,                                              \
              struct rix_hash32_bucket_s *buckets,                            \
              type *base,                                                     \
              type *elm)                                                      \
{                                                                             \
    unsigned mask     = head->rhh_mask;                                       \
    unsigned node_idx = name##_hidx(base, elm);                               \
    union rix_hash_hash_u _h =                                                \
        rix_hash_arch->hash_u32((u32)elm->key_field, mask);                   \
    unsigned _bk0 = _h.val32[0] & mask;                                       \
    unsigned _bk1 = _h.val32[1] & mask;                                       \
                                                                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash32_bucket_s *_bk =                                     \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        u32 _hits = rix_hash_arch->find_u32x16(_bk->idx,                      \
                                                    (u32)node_idx);           \
        if (_hits) {                                                          \
            unsigned _slot    = (unsigned)__builtin_ctz(_hits);               \
            _bk->key[_slot] = (u32)(invalid_key);                             \
            _bk->idx[_slot] = (u32)RIX_NIL;                                   \
            head->rhh_nb--;                                                   \
            return elm;                                                       \
        }                                                                     \
    }                                                                         \
    return NULL; /* not in table */                                           \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Walk - iterate over all occupied slots                             */      \
/*                                                                    */      \
/* cb(node, arg): return 0 to continue, non-zero to stop.             */      \
/* Returns 0 when all entries are visited, or first non-zero cb rv.   */      \
/* ================================================================== */      \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash32_bucket_s *buckets,                              \
            type *base,                                                       \
            int (*cb)(type *, void *),                                        \
            void *arg)                                                        \
{                                                                             \
    unsigned _nb_bk = head->rhh_mask + 1u;                                    \
    for (unsigned _b = 0u; _b < _nb_bk; _b++) {                               \
        struct rix_hash32_bucket_s *_bk = buckets + _b;                       \
        for (unsigned _s = 0u; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {         \
            unsigned _nidx = _bk->idx[_s];                                    \
            if (_nidx == (unsigned)RIX_NIL)                                   \
                continue;                                                     \
            type *_node = name##_hptr(base, _nidx);                           \
            int   _r    = cb(_node, arg);                                     \
            if (_r)                                                           \
                return _r;                                                    \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}

/*===========================================================================
 * Convenience macro API
 *
 * Wraps the generated name##_xxx() functions with BSD queue-style macros
 * that embed the table-name prefix explicitly.  The name argument must
 * match the tag passed to RIX_HASH32_HEAD / RIX_HASH32_GENERATE.
 *
 * Single-shot ops:
 *   RIX_HASH32_INIT        (name, head, buckets, nb_bk)
 *   RIX_HASH32_FIND        (name, head, buckets, base, key)
 *   RIX_HASH32_INSERT      (name, head, buckets, base, elm)
 *   RIX_HASH32_REMOVE      (name, head, buckets, base, elm)
 *   RIX_HASH32_WALK        (name, head, buckets, base, cb, arg)
 *
 * Staged find - x1:
 *   RIX_HASH32_HASH_KEY    (name, ctx, head, buckets, key)
 *   RIX_HASH32_SCAN_BK     (name, ctx, head, buckets)
 *   RIX_HASH32_PREFETCH_NODE(name, ctx, base)
 *   RIX_HASH32_CMP_KEY     (name, ctx, base)
 *
 * Staged find - x2 / x4  (named shorthands; expand to _n internally):
 *   RIX_HASH32_HASH_KEY2   (name, ctx, head, buckets, keys)
 *   RIX_HASH32_SCAN_BK2    (name, ctx, head, buckets)
 *   RIX_HASH32_PREFETCH_NODE2(name, ctx, base)
 *   RIX_HASH32_CMP_KEY2    (name, ctx, base, results)
 *
 *   RIX_HASH32_HASH_KEY4   (name, ctx, head, buckets, keys)
 *   RIX_HASH32_SCAN_BK4    (name, ctx, head, buckets)
 *   RIX_HASH32_PREFETCH_NODE4(name, ctx, base)
 *   RIX_HASH32_CMP_KEY4    (name, ctx, base, results)
 *
 * Staged find - xN  (arbitrary n; FORCE_INLINE + constant n -> unrolled):
 *   RIX_HASH32_HASH_KEY_N  (name, ctx, n, head, buckets, keys)
 *   RIX_HASH32_SCAN_BK_N   (name, ctx, n, head, buckets)
 *   RIX_HASH32_PREFETCH_NODE_N(name, ctx, n, base)
 *   RIX_HASH32_CMP_KEY_N   (name, ctx, n, base, results)
 *===========================================================================*/

/* ---- single-shot ops ---------------------------------------------------- */
#  define RIX_HASH32_INIT(name, head, buckets, nb_bk)                          \
    name##_init(head, buckets, nb_bk)

#  define RIX_HASH32_FIND(name, head, buckets, base, key)                      \
    name##_find(head, buckets, base, key)

#  define RIX_HASH32_INSERT(name, head, buckets, base, elm)                    \
    name##_insert(head, buckets, base, elm)

#  define RIX_HASH32_REMOVE(name, head, buckets, base, elm)                    \
    name##_remove(head, buckets, base, elm)

#  define RIX_HASH32_WALK(name, head, buckets, base, cb, arg)                  \
    name##_walk(head, buckets, base, cb, arg)

/* ---- staged find - x1 --------------------------------------------------- */
#  define RIX_HASH32_HASH_KEY(name, ctx, head, buckets, key)                   \
    name##_hash_key(ctx, head, buckets, key)

#  define RIX_HASH32_SCAN_BK(name, ctx, head, buckets)                         \
    name##_scan_bk(ctx, head, buckets)

#  define RIX_HASH32_PREFETCH_NODE(name, ctx, base)                            \
    name##_prefetch_node(ctx, base)

#  define RIX_HASH32_CMP_KEY(name, ctx, base)                                  \
    name##_cmp_key(ctx, base)

/* ---- staged find - x2 --------------------------------------------------- */
#  define RIX_HASH32_HASH_KEY2(name, ctx, head, buckets, keys)                 \
    name##_hash_key_n(ctx, 2, head, buckets, keys)

#  define RIX_HASH32_SCAN_BK2(name, ctx, head, buckets)                        \
    name##_scan_bk_n(ctx, 2, head, buckets)

#  define RIX_HASH32_PREFETCH_NODE2(name, ctx, base)                           \
    name##_prefetch_node_n(ctx, 2, base)

#  define RIX_HASH32_CMP_KEY2(name, ctx, base, results)                        \
    name##_cmp_key_n(ctx, 2, base, results)

/* ---- staged find - x4 --------------------------------------------------- */
#  define RIX_HASH32_HASH_KEY4(name, ctx, head, buckets, keys)                 \
    name##_hash_key_n(ctx, 4, head, buckets, keys)

#  define RIX_HASH32_SCAN_BK4(name, ctx, head, buckets)                        \
    name##_scan_bk_n(ctx, 4, head, buckets)

#  define RIX_HASH32_PREFETCH_NODE4(name, ctx, base)                           \
    name##_prefetch_node_n(ctx, 4, base)

#  define RIX_HASH32_CMP_KEY4(name, ctx, base, results)                        \
    name##_cmp_key_n(ctx, 4, base, results)

/* ---- staged find - xN (arbitrary n) ------------------------------------- */
#  define RIX_HASH32_HASH_KEY_N(name, ctx, n, head, buckets, keys)             \
    name##_hash_key_n(ctx, n, head, buckets, keys)

#  define RIX_HASH32_SCAN_BK_N(name, ctx, n, head, buckets)                    \
    name##_scan_bk_n(ctx, n, head, buckets)

#  define RIX_HASH32_PREFETCH_NODE_N(name, ctx, n, base)                       \
    name##_prefetch_node_n(ctx, n, base)

#  define RIX_HASH32_CMP_KEY_N(name, ctx, n, base, results)                    \
    name##_cmp_key_n(ctx, n, base, results)

#endif /* _RIX_HASH32_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
