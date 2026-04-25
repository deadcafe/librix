/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_slot_extra.h - slot variant with a third per-slot u32 (extra[]).
 *
 * Bucket layout (192B, 3 cache lines):
 *   hash [16]  (64B, cl0) - fingerprints
 *   idx  [16]  (64B, cl1) - 1-origin node idx
 *   extra[16]  (64B, cl2) - user-defined u32
 *
 * Opt-in only. Not included from rix_hash.h umbrella.
 */

#ifndef _RIX_HASH_SLOT_EXTRA_H_
#  define _RIX_HASH_SLOT_EXTRA_H_

#  include "rix_hash_common.h"

struct rix_hash_bucket_extra_s {
    u32 hash [RIX_HASH_BUCKET_ENTRY_SZ];
    u32 idx  [RIX_HASH_BUCKET_ENTRY_SZ];
    u32 extra[RIX_HASH_BUCKET_ENTRY_SZ];
} __attribute__((aligned(64)));

struct rix_hash_find_ctx_extra_s {
    union rix_hash_hash_u            hash;
    struct rix_hash_bucket_extra_s  *bk[2];
    const void                      *key;
    u32  fp;
    u32  fp_hits[2];
    u32  empties[2];
};

/* ---- accessor / prefetch helpers --------------------------------------- */

/** @brief Pointer to the bucket at index @p bk_idx. */
#  ifndef rix_hash_extra_bucket_of_idx
#    define rix_hash_extra_bucket_of_idx(buckets, bk_idx) \
        (&(buckets)[(unsigned)(bk_idx)])
#  endif

/** @brief Pointer to the @c hash[] line of the bucket at index @p bk_idx. */
#  ifndef rix_hash_extra_bucket_hashes_of_idx
#    define rix_hash_extra_bucket_hashes_of_idx(buckets, bk_idx) \
        (rix_hash_extra_bucket_of_idx((buckets), (bk_idx))->hash)
#  endif

/** @brief Pointer to the @c idx[] line of the bucket at index @p bk_idx. */
#  ifndef rix_hash_extra_bucket_indices_of_idx
#    define rix_hash_extra_bucket_indices_of_idx(buckets, bk_idx) \
        (rix_hash_extra_bucket_of_idx((buckets), (bk_idx))->idx)
#  endif

/** @brief Pointer to the @c extra[] line of the bucket at index @p bk_idx. */
#  ifndef rix_hash_extra_bucket_extras_of_idx
#    define rix_hash_extra_bucket_extras_of_idx(buckets, bk_idx) \
        (rix_hash_extra_bucket_of_idx((buckets), (bk_idx))->extra)
#  endif

/** @brief Prefetch the hash[] cache line (T1) of @p bucket. */
static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_hashes_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->hash[0], 0, 1);
}

/** @brief Prefetch the idx[] cache line (T1) of @p bucket. */
static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_indices_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->idx[0], 0, 1);
}

/** @brief Prefetch the extra[] cache line (T1) of @p bucket. */
static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_extras_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->extra[0], 0, 1);
}

/** @brief Prefetch the hash[] and idx[] cache lines of @p bucket
 *         (find / scan path).
 */
static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    rix_hash_prefetch_extra_bucket_hashes_of(bucket);
    rix_hash_prefetch_extra_bucket_indices_of(bucket);
}

/** @brief Prefetch all three cache lines of @p bucket
 *         (maintain / timestamp-aware path).
 */
static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_full_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    rix_hash_prefetch_extra_bucket_hashes_of(bucket);
    rix_hash_prefetch_extra_bucket_indices_of(bucket);
    rix_hash_prefetch_extra_bucket_extras_of(bucket);
}

/** @brief Index-form variant of rix_hash_prefetch_extra_bucket_hashes_of(). */
#  define rix_hash_prefetch_extra_bucket_hashes_of_idx(buckets, bk_idx)       \
    rix_hash_prefetch_extra_bucket_hashes_of(&(buckets)[(unsigned)(bk_idx)])

/** @brief Index-form variant of rix_hash_prefetch_extra_bucket_indices_of(). */
#  define rix_hash_prefetch_extra_bucket_indices_of_idx(buckets, bk_idx)      \
    rix_hash_prefetch_extra_bucket_indices_of(&(buckets)[(unsigned)(bk_idx)])

/** @brief Index-form variant of rix_hash_prefetch_extra_bucket_extras_of(). */
#  define rix_hash_prefetch_extra_bucket_extras_of_idx(buckets, bk_idx)       \
    rix_hash_prefetch_extra_bucket_extras_of(&(buckets)[(unsigned)(bk_idx)])

/**
 * @brief True (non-zero) if @p slot is a valid bucket-slot index
 *        (< RIX_HASH_BUCKET_ENTRY_SZ).
 */
static RIX_FORCE_INLINE int
rix_hash_slot_extra_slot_valid(unsigned slot)
{
    return slot < RIX_HASH_BUCKET_ENTRY_SZ;
}

/**
 * @brief Resolve the bucket pointer for @p cur_hash given mask @p mask.
 */
static RIX_FORCE_INLINE struct rix_hash_bucket_extra_s *
rix_hash_slot_extra_bucket_by_hash(struct rix_hash_bucket_extra_s *buckets,
                                   unsigned mask,
                                   u32 cur_hash)
{
    return &buckets[cur_hash & mask];
}

/** @brief @c const variant of rix_hash_slot_extra_bucket_by_hash(). */
static RIX_FORCE_INLINE const struct rix_hash_bucket_extra_s *
rix_hash_slot_extra_bucket_by_hash_const(
    const struct rix_hash_bucket_extra_s *buckets,
    unsigned mask,
    u32 cur_hash)
{
    return &buckets[cur_hash & mask];
}

/**
 * Validated read of bucket->extra[slot].
 *
 * Confirms that the bucket resolved from @cur_hash & @mask still holds
 * @expected_idx at @slot before reading.  @expected_idx must be a real
 * 1-origin entry index; passing @c RIX_NIL (the empty-slot sentinel) is
 * rejected so that callers cannot accidentally read the extra[] of an
 * unoccupied slot.
 *
 * Returns 0 on success, -1 if @buckets / @out is NULL, @slot is out of
 * range, @expected_idx is RIX_NIL, or the slot does not hold @expected_idx.
 */
static RIX_FORCE_INLINE int
rix_hash_slot_extra_get(const struct rix_hash_bucket_extra_s *buckets,
                        unsigned mask,
                        u32 cur_hash,
                        unsigned slot,
                        u32 expected_idx,
                        u32 *out)
{
    const struct rix_hash_bucket_extra_s *bk;

    if (buckets == NULL || out == NULL)
        return -1;
    if (!rix_hash_slot_extra_slot_valid(slot))
        return -1;
    if (expected_idx == (u32)RIX_NIL)
        return -1;
    bk = rix_hash_slot_extra_bucket_by_hash_const(buckets, mask, cur_hash);
    if (bk->idx[slot] != expected_idx)
        return -1;
    *out = bk->extra[slot];
    return 0;
}

/**
 * Validated write of bucket->extra[slot].  See rix_hash_slot_extra_get()
 * for the contract; @expected_idx == @c RIX_NIL is rejected to avoid writing
 * into empty slots by mistake.
 */
static RIX_FORCE_INLINE int
rix_hash_slot_extra_set(struct rix_hash_bucket_extra_s *buckets,
                        unsigned mask,
                        u32 cur_hash,
                        unsigned slot,
                        u32 expected_idx,
                        u32 value)
{
    struct rix_hash_bucket_extra_s *bk;

    if (buckets == NULL)
        return -1;
    if (!rix_hash_slot_extra_slot_valid(slot))
        return -1;
    if (expected_idx == (u32)RIX_NIL)
        return -1;
    bk = rix_hash_slot_extra_bucket_by_hash(buckets, mask, cur_hash);
    if (bk->idx[slot] != expected_idx)
        return -1;
    bk->extra[slot] = value;
    return 0;
}

/**
 * Touch the bucket-side extra[] for an entry that lives in one of two
 * candidate buckets (the bk[0]/bk[1] pair held in
 * struct rix_hash_find_ctx_extra_s after scan).  The helper writes @value to
 * extra[slot] in whichever of bk0 / bk1 actually holds @expected_idx at @slot.
 *
 * @expected_idx must be a real 1-origin entry index; @c RIX_NIL (empty-slot
 * sentinel) is rejected so a caller cannot accidentally match an empty slot.
 *
 * Returns 0 on success, -1 if @slot is out of range, @expected_idx is
 * @c RIX_NIL, or no candidate bucket matches @expected_idx.
 */
static RIX_FORCE_INLINE int
rix_hash_slot_extra_touch_2bk(struct rix_hash_bucket_extra_s *bk0,
                              struct rix_hash_bucket_extra_s *bk1,
                              unsigned slot,
                              u32 expected_idx,
                              u32 value)
{
    struct rix_hash_bucket_extra_s *bk;

    if (!rix_hash_slot_extra_slot_valid(slot))
        return -1;
    if (expected_idx == (u32)RIX_NIL)
        return -1;
    bk = bk0;
    if (bk == NULL || bk->idx[slot] != expected_idx) {
        bk = bk1;
        if (bk == NULL || bk->idx[slot] != expected_idx)
            return -1;
    }
    bk->extra[slot] = value;
    return 0;
}

/* ---- PROTOTYPE macros -------------------------------------------------- */

/*
 * Declare the public slot_extra API surface. Mirrors the signatures emitted
 * by RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL (192B bucket type, 5-arg insert).
 * Use this when splitting declaration (.h) and definition (.c) across TUs;
 * single-TU users can skip PROTOTYPE and call GENERATE directly.
 */
#  define RIX_HASH_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, cmp_fn, attr) \
    attr void name##_init(struct name *head, unsigned nb_bk);                  \
    attr struct type *name##_insert(struct name *head,                         \
                                    struct rix_hash_bucket_extra_s *buckets,   \
                                    struct type *base,                         \
                                    struct type *elm,                          \
                                    u32 extra);                                \
    attr unsigned name##_remove_at(struct name *head,                          \
                                   struct rix_hash_bucket_extra_s *buckets,    \
                                   unsigned bk,                                \
                                   unsigned slot);                             \
    attr struct type *name##_remove(struct name *head,                         \
                                    struct rix_hash_bucket_extra_s *buckets,   \
                                    struct type *base,                         \
                                    struct type *elm);                         \
    attr int name##_walk(struct name *head,                                    \
                         struct rix_hash_bucket_extra_s *buckets,              \
                         struct type *base,                                    \
                         int (*cb)(struct type *, void *),                     \
                         void *arg);

#  define RIX_HASH_PROTOTYPE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

/* ---- GENERATE wrappers ------------------------------------------------- */

#  define RIX_HASH_GENERATE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn, hash_fn, )

#  define RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn, hash_fn,        \
                                          RIX_UNUSED static)

#  define RIX_HASH_GENERATE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                     \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn,                  \
                                          RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_GENERATE_STATIC_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                     \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn,                  \
                                          RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                          RIX_UNUSED static)

/* ---- INDEXERS helper --------------------------------------------------- */

#  ifndef RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS
#    define RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS(name, type)                    \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p) {                        \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i) {                                  \
    return (struct type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);    \
}
#  endif

/* ---- INTERNAL macro (slot_extra variant) ------------------------------- */

#  define RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
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
RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS(name, type)                               \
                                                                              \
/* ================================================================== */      \
/* Staged find - x1                                                   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_find_ctx_extra_s *ctx,                 \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_extra_s *buckets,               \
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
    rix_hash_prefetch_extra_bucket_of(ctx->bk[0]);                            \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_find_ctx_extra_s *ctx,                        \
                struct name *head,                                            \
                struct rix_hash_bucket_extra_s *buckets,                      \
                const RIX_HASH_KEY_TYPE(type, key_field) *key)               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_masked(ctx, head, buckets, key, mask, mask);              \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_find_ctx_extra_s *ctx,                         \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_extra_s *buckets __attribute__((unused))) \
{                                                                             \
    ctx->fp_hits[0] = RIX_HASH_FIND_U32X16(ctx->bk[0]->hash, ctx->fp);       \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node(struct rix_hash_find_ctx_extra_s *ctx,                   \
                     struct type *base)                                       \
{                                                                             \
    u32 _hits = ctx->fp_hits[0];                                              \
    if (_hits) {                                                              \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        unsigned _nidx = ctx->bk[0]->idx[_bit];                               \
        rix_hash_prefetch_entry_of_idx(base, _nidx);                          \
    }                                                                         \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_find_ctx_extra_s *ctx,                         \
               struct type *base)                                             \
{                                                                             \
    u32 _hits = ctx->fp_hits[0];                                              \
    while (_hits) {                                                           \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        _hits &= _hits - 1u;                                                  \
        unsigned _nidx = ctx->bk[0]->idx[_bit];                               \
        if (_nidx == (unsigned)RIX_NIL) continue;                             \
        struct type *_node = name##_hptr(base, _nidx);                        \
        if (cmp_fn(&_node->key_field,                                         \
                   (const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key) == 0)\
            return _node;                                                     \
    }                                                                         \
    _hits = RIX_HASH_FIND_U32X16(ctx->bk[1]->hash, ctx->fp);                 \
    while (_hits) {                                                           \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        _hits &= _hits - 1u;                                                  \
        unsigned _nidx = ctx->bk[1]->idx[_bit];                               \
        if (_nidx == (unsigned)RIX_NIL) continue;                             \
        struct type *_node = name##_hptr(base, _nidx);                        \
        if (cmp_fn(&_node->key_field,                                         \
                   (const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key) == 0)\
            return _node;                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Lookup pipeline variants that also collect empty-slot bitmasks.    */      \
/* Use when miss -> insert is the expected follow-up path.            */      \
/* ================================================================== */      \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_2bk_masked(struct rix_hash_find_ctx_extra_s *ctx,             \
                           struct rix_hash_bucket_extra_s *buckets,           \
                           const RIX_HASH_KEY_TYPE(type, key_field) *key,    \
                           unsigned hash_mask,                                \
                           unsigned bk_mask)                                  \
{                                                                             \
    union rix_hash_hash_u _h = hash_fn(key, hash_mask);                       \
    unsigned _bk0, _bk1;                                                      \
    u32 _fp;                                                                  \
    _fp = rix_hash_fp(_h, bk_mask, &_bk0, &_bk1);                       \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    rix_hash_prefetch_extra_bucket_of(ctx->bk[0]);                            \
    rix_hash_prefetch_extra_bucket_of(ctx->bk[1]);                            \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_bk_masked(struct rix_hash_find_ctx_extra_s *ctx,              \
                          struct rix_hash_bucket_extra_s *buckets,            \
                          const RIX_HASH_KEY_TYPE(type, key_field) *key,     \
                          unsigned hash_mask,                                 \
                          unsigned bk_mask)                                   \
{                                                                             \
    union rix_hash_hash_u _h = hash_fn(key, hash_mask);                       \
    unsigned _bk0, _bk1;                                                      \
    u32 _fp;                                                                  \
    _fp = rix_hash_fp(_h, bk_mask, &_bk0, &_bk1);                       \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    rix_hash_prefetch_extra_bucket_of(ctx->bk[0]);                            \
}                                                                             \
                                                                              \
/* Stage 2: scan the selected bucket for fp matches and empty slots.  */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_empties(struct rix_hash_find_ctx_extra_s *ctx,                 \
                       unsigned which)                                        \
{                                                                             \
    RIX_HASH_FIND_U32X16_2(ctx->bk[which]->hash, ctx->fp, 0u,                \
                            &ctx->fp_hits[which], &ctx->empties[which]);      \
}                                                                             \
                                                                              \
/* Stage 4: compare candidates in the selected bucket.                */       \
static RIX_UNUSED RIX_FORCE_INLINE u32                                   \
name##_cmp_key_empties(struct rix_hash_find_ctx_extra_s *ctx,                 \
                       struct type *base,                                     \
                       unsigned which)                                        \
{                                                                             \
    u32 _hits = ctx->fp_hits[which];                                          \
    while (_hits) {                                                           \
        unsigned      _bit  = (unsigned)__builtin_ctz(_hits);                 \
        _hits &= _hits - 1u;                                                  \
        unsigned      _nidx = ctx->bk[which]->idx[_bit];                      \
        if (_nidx == (unsigned)RIX_NIL) continue;                             \
        struct type  *_node = name##_hptr(base, _nidx);                       \
        if (cmp_fn(&_node->key_field,                                         \
                   (const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key) == 0)\
            return (u32)_nidx;                                           \
    }                                                                         \
    return (u32)RIX_NIL;                                                 \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_find_ctx_extra_s *ctx,               \
                         unsigned n,                                          \
                         struct name *head,                                   \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         const RIX_HASH_KEY_TYPE(type, key_field) * const *keys, \
                         unsigned hash_mask,                                  \
                         unsigned bk_mask)                                    \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_find_ctx_extra_s *ctx,                      \
                  unsigned n,                                                 \
                  struct name *head,                                          \
                  struct rix_hash_bucket_extra_s *buckets,                    \
                  const RIX_HASH_KEY_TYPE(type, key_field) * const *keys)    \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, mask, mask);        \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_find_ctx_extra_s *ctx,                       \
                 unsigned n,                                                  \
                 struct name *head,                                           \
                 struct rix_hash_bucket_extra_s *buckets)                     \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_find_ctx_extra_s *ctx,                 \
                       unsigned n,                                            \
                       struct type *base)                                     \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_find_ctx_extra_s *ctx,                       \
                 unsigned n,                                                  \
                 struct type *base,                                           \
                 struct type **results)                                       \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
                                                                              \
attr struct type *                                                            \
name##_find(struct name *head,                                                \
            struct rix_hash_bucket_extra_s *buckets,                          \
            struct type *base,                                                \
            const RIX_HASH_KEY_TYPE(type, key_field) *key)                   \
{                                                                             \
    struct rix_hash_find_ctx_extra_s _ctx;                                    \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk(&_ctx, head, buckets);                                     \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* find_empty / flipflop / kickout (slot_extra variant)                */      \
/* ================================================================== */      \
static RIX_UNUSED int                                                         \
name##_find_empty(struct rix_hash_bucket_extra_s *buckets,                    \
                  unsigned bk_idx)                                            \
{                                                                             \
    u32 _nilm = RIX_HASH_FIND_U32X16(buckets[bk_idx].hash, 0u);              \
    if (!_nilm) return -1;                                                    \
    return (int)__builtin_ctz(_nilm);                                         \
}                                                                             \
                                                                              \
static RIX_UNUSED int                                                         \
name##_flipflop(struct rix_hash_bucket_extra_s *buckets,                      \
                struct type *base,                                            \
                unsigned mask,                                                \
                unsigned bk_idx,                                              \
                unsigned slot)                                                \
{                                                                             \
    struct rix_hash_bucket_extra_s *_bk = buckets + bk_idx;                   \
    u32     _fp  = _bk->hash[slot];                                           \
    unsigned     _idx = _bk->idx [slot];                                      \
    u32     _extra_save = _bk->extra[slot];                                   \
    struct type *_nd  = name##_hptr(base, _idx);                              \
    if (!_nd) return -1;                                                      \
    u32 _h  = _nd->hash_field;                                                \
    unsigned _ab = (_fp ^ _h) & mask;                                         \
    int _slot = name##_find_empty(buckets, _ab);                              \
    if (_slot < 0) return -1;                                                 \
    struct rix_hash_bucket_extra_s *_alt = buckets + _ab;                     \
    _alt->hash[_slot] = _fp;                                                  \
    _alt->idx [_slot] = _idx;                                                 \
    _alt->extra[_slot] = _extra_save;                                         \
    _nd->hash_field = _fp ^ _h;                                               \
    _nd->slot_field = (RIX_HASH_SLOT_TYPE(type, slot_field))_slot;           \
    _bk->hash[slot] = 0u;                                                     \
    _bk->idx [slot] = (u32)RIX_NIL;                                           \
    _bk->extra[slot] = 0u;                                                    \
    return (int)slot;                                                         \
}                                                                             \
                                                                              \
static RIX_UNUSED int                                                         \
name##_kickout(struct rix_hash_bucket_extra_s *buckets,                       \
               struct type *base,                                             \
               unsigned mask,                                                 \
               unsigned bk_idx,                                               \
               int depth)                                                     \
{                                                                             \
    if (depth <= 0) return -1;                                                \
    struct rix_hash_bucket_extra_s *_bk = buckets + bk_idx;                   \
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
        u32 _sh = _sn->hash_field;                                            \
        unsigned _ab = (_fp ^ _sh) & mask;                                    \
        if (name##_kickout(buckets, base, mask, _ab, depth - 1) >= 0) {       \
            if (_bk->hash[_s] != _fp || _bk->idx[_s] != _si) {                \
                int _fs = name##_find_empty(buckets, bk_idx);                 \
                if (_fs >= 0) return _fs;                                     \
                continue;                                                     \
            }                                                                 \
            name##_flipflop(buckets, base, mask, bk_idx, _s);                 \
            return (int)_s;                                                   \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE u32                                   \
name##_insert_hashed_idx(struct name *head,                                   \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         struct type *base,                                   \
                         struct type *elm,                                    \
                         union rix_hash_hash_u _h,                            \
                         u32 extra)                                           \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned _bk0, _bk1;                                                      \
    u32 _elm_idx = name##_hidx(base, elm);                               \
    u32 _fp;                                                                  \
    u32 _hits_fp[2];                                                          \
    u32 _hits_zero[2];                                                        \
    _fp = rix_hash_fp(_h, mask, &_bk0, &_bk1);                          \
    elm->hash_field = _h.val32[0];                                            \
    rix_hash_prefetch_extra_bucket_of(buckets + _bk0);                        \
    rix_hash_prefetch_extra_bucket_of(buckets + _bk1);                        \
                                                                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_extra_s *_bk =                                 \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        RIX_HASH_FIND_U32X16_2(_bk->hash, _fp, 0u,                           \
                                &_hits_fp[_i], &_hits_zero[_i]);              \
    }                                                                         \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_extra_s *_bk =                                 \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        u32 _hits = _hits_fp[_i];                                             \
        while (_hits) {                                                       \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            _hits &= _hits - 1u;                                              \
            u32 _node_idx = _bk->idx[_bit];                              \
            struct type *_node = name##_hptr(base, _node_idx);                \
            RIX_ASSUME_NONNULL(_node);                                        \
            if (cmp_fn(&elm->key_field, &_node->key_field) == 0)              \
                return _node_idx;                                             \
        }                                                                     \
    }                                                                         \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_extra_s *_bk = buckets + _bki;                 \
        u32 _nilm = _hits_zero[_i];                                           \
        if (_nilm) {                                                          \
            unsigned _slot = (unsigned)__builtin_ctz(_nilm);                  \
            _bk->hash[_slot] = _fp;                                           \
            _bk->idx[_slot]  = _elm_idx;                                      \
            _bk->extra[_slot] = extra;                                        \
            if (_i == 1)                                                      \
                elm->hash_field = _h.val32[1];                                \
            elm->slot_field = (RIX_HASH_SLOT_TYPE(type, slot_field))_slot;   \
            head->rhh_nb++;                                                   \
            return 0u;                                                        \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Slow path: kickout */                                                  \
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
                return _elm_idx;                                              \
            _bki = _bk1;                                                      \
            elm->hash_field = _h.val32[1];                                    \
        }                                                                     \
        struct rix_hash_bucket_extra_s *_bk = buckets + _bki;                 \
        _bk->hash[_pos] = _fp;                                                \
        _bk->idx [_pos] = _elm_idx;                                           \
        _bk->extra[_pos] = extra;                                             \
        elm->slot_field = (RIX_HASH_SLOT_TYPE(type, slot_field))_pos;        \
        head->rhh_nb++;                                                       \
        return 0u;                                                            \
    }                                                                         \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_insert_hashed(struct name *head,                                       \
                     struct rix_hash_bucket_extra_s *buckets,                 \
                     struct type *base,                                       \
                     struct type *elm,                                        \
                     union rix_hash_hash_u _h,                                \
                     u32 extra)                                               \
{                                                                             \
    u32 _ret_idx =                                                       \
        name##_insert_hashed_idx(head, buckets, base, elm, _h, extra);       \
    return (_ret_idx == 0u) ? NULL : name##_hptr(base, _ret_idx);            \
}                                                                             \
                                                                              \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_extra_s *buckets,                        \
              struct type *base,                                              \
              struct type *elm,                                               \
              u32 extra)                                                      \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    return name##_insert_hashed(head, buckets, base, elm, _h, extra);         \
}                                                                             \
                                                                              \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_extra_s *buckets,                     \
                 unsigned bk,                                                 \
                 unsigned slot);                                              \
                                                                              \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_extra_s *buckets,                        \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned _bk = (unsigned)(elm->hash_field & head->rhh_mask);              \
    unsigned _slot = (unsigned)elm->slot_field;                               \
    struct rix_hash_bucket_extra_s *_b = buckets + _bk;                       \
    RIX_ASSERT(_slot < RIX_HASH_BUCKET_ENTRY_SZ);                             \
    if (_slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                    \
        return NULL;                                                          \
    if (_b->idx[_slot] != (u32)node_idx)                                      \
        return NULL;                                                          \
    if (name##_remove_at(head, buckets, _bk, _slot) !=                        \
        (unsigned)RIX_NIL)                                                    \
        return elm;                                                           \
    return NULL;                                                              \
}                                                                             \
                                                                              \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_extra_s *buckets,                     \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    struct rix_hash_bucket_extra_s *_b = buckets + bk;                        \
    unsigned _idx;                                                            \
    RIX_ASSERT(slot < RIX_HASH_BUCKET_ENTRY_SZ);                              \
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                     \
        return (unsigned)RIX_NIL;                                             \
    _idx = _b->idx[slot];                                                     \
    if (_idx == (unsigned)RIX_NIL)                                            \
        return (unsigned)RIX_NIL;                                             \
    _b->hash[slot] = 0u;                                                      \
    _b->idx [slot] = (u32)RIX_NIL;                                            \
    _b->extra[slot] = 0u;                                                     \
    head->rhh_nb--;                                                           \
    return _idx;                                                              \
}                                                                             \
                                                                              \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash_bucket_extra_s *buckets,                          \
            struct type *base,                                                \
            int (*cb)(struct type *, void *),                                 \
            void *arg)                                                        \
{                                                                             \
    for (unsigned _b = 0; _b <= head->rhh_mask; _b++) {                       \
        struct rix_hash_bucket_extra_s *_bk = buckets + _b;                   \
        for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {          \
            unsigned _idx = _bk->idx[_s];                                     \
            if (_idx == (unsigned)RIX_NIL) continue;                          \
            struct type *_node = name##_hptr(base, _idx);                     \
            int _rc = cb(_node, arg);                                         \
            if (_rc) return _rc;                                              \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}

/* ---- Convenience insert macro (extra variant) -------------------------- */

#  define RIX_HASH_INSERT_EXTRA(name, head, buckets, base, elm, extra)         \
    name##_insert(head, buckets, base, elm, extra)

#endif /* _RIX_HASH_SLOT_EXTRA_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
