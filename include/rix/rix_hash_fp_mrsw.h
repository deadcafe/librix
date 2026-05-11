/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _RIX_HASH_FP_MRSW_H_
#  define _RIX_HASH_FP_MRSW_H_

#  include "rix_hash_mrsw_core.h"

struct rix_hash_mrsw_find_ctx_s {
    union rix_hash_hash_u          hash;
    struct rix_hash_bucket_s *bk[2];
    struct rix_hash_bucket_s *buckets;
    const void                    *key;
    u32                            ctrl[2];
    unsigned                       hash_mask;
    unsigned                       bk_mask;
    u32                            fp;
    u32                            fp_hits[2];
};

#  define RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, attr) \
    attr void name##_init(struct name *head,                                  \
                          struct rix_hash_bucket_s *buckets,            \
                          unsigned nb_bk);                                    \
    attr struct type *name##_insert(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,   \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_s *buckets,    \
                                   unsigned bk,                               \
                                   unsigned slot);                            \
    attr struct type *name##_remove(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,   \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    /* walk: writer-only iteration.  If the per-bucket ctrl word changes      \
     * between the begin/end snapshots the walk aborts and returns -1.        \
     * Callers must serialize with the writer (or call from the writer        \
     * thread itself) to obtain a consistent traversal. */                    \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,              \
                         struct type *base,                                   \
                         int (*cb)(struct type *, void *),                    \
                         void *arg);

/* All public PROTOTYPE_* macros expand to the same external symbols (the FP
 * and SLOT variants generate identical signatures; slot_field is internal).
 * Therefore _EX, _STATIC, _SLOT and _STATIC_SLOT variants are thin wrappers
 * around the same _INTERNAL macro -- only the linkage attribute differs. */
#  define RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)  \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, hash_field,       \
                                     cmp_fn, )
#  define RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, hash_field,       \
                                     cmp_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_PROTOTYPE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn)

#  define RIX_HASH_MRSW_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn, hash_fn, )

#  define RIX_HASH_MRSW_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn, hash_fn, RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE(name, type, key_field, hash_field, cmp_fn)   \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn,                                   \
                                    RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_MRSW_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn,                                   \
                                    RIX_HASH_DEFAULT_HASH_FN_NAME(name),      \
                                    RIX_UNUSED static)

/* SLOT variant: requires slot_field of an integer type that can represent   \
 * 0..RIX_HASH_MRSW_BUCKET_ENTRY_SZ-1.  Insert/kickout updates the field; the \
 * remove path uses elm->slot_field for direct O(1) bucket-slot lookup.       \
 * The slot_field is only mutated by the writer; readers do not touch it. */
#  define RIX_HASH_MRSW_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn, hash_fn, )

#  define RIX_HASH_MRSW_GENERATE_SLOT_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn, hash_fn,         \
                                         RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn,                  \
                                         RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_MRSW_GENERATE_SLOT_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn,                  \
                                         RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                         RIX_UNUSED static)

/* KEYONLY variant: no hash_field, no slot_field in the node.  Kickout re-    \
 * hashes the victim's key to find its alternate bucket; remove re-hashes    \
 * the element key and probes both candidate buckets.  Find/insert behave    \
 * like the FP variant but with no node-side hash_field maintenance. */
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY(name, type, key_field, cmp_fn)      \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, /*hash_field*/_,  \
                                     cmp_fn, )
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY_STATIC(name, type, key_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, /*hash_field*/_,  \
                                     cmp_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_KEYONLY(name, type, key_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_KEYONLY_STATIC(name, type, key_field, cmp_fn)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            hash_fn, )

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            hash_fn, RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY(name, type, key_field, cmp_fn)       \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_STATIC(name, type, key_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                            RIX_UNUSED static)


/* ---------------------------------------------------------------------- *\
 * Internal generators are split into reusable building blocks so that the   *
 * FP and SLOT variants can share readers, find pipeline, walk, and the      *
 * remove_at_inner / remove_at primitives, while the writer-side ops         *
 * (flipflop, kickout, insert_hashed_idx, insert, remove) differ in slot     *
 * field maintenance.                                                        *
 *                                                                           *
 *   COMMON_PRE  : init, indexers, scan helpers, staged find, find,          *
 *                 remove_at_inner, remove_at                                *
 *   FP_OPS      : flipflop, kickout, insert_hashed_idx, insert_hashed,      *
 *                 insert, remove (linear bucket scan)                       *
 *   SLOT_OPS    : flipflop+slot_field, kickout, insert_hashed_idx+slot,     *
 *                 insert_hashed, insert, remove (direct slot lookup)        *
 *   COMMON_POST : walk                                                      *
 *                                                                           *
 *   GENERATE_INTERNAL      = COMMON_PRE + FP_OPS  + COMMON_POST             *
 *   GENERATE_SLOT_INTERNAL = COMMON_PRE + SLOT_OPS + COMMON_POST             *
\* ---------------------------------------------------------------------- */
#  define RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
attr void                                                                     \
name##_init(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                           \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    rix_hash_mrsw_buckets_init(buckets, nb_bk);                               \
}                                                                             \
RIX_HASH_MRSW_DEFINE_INDEXERS(name, type)                                     \
/* Intentional benign race: hash[] is non-atomic payload validated by         \
 * the bucket ctrl seq+valid protocol.  See rix_hash_mrsw.h header. */        \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_hashes(struct rix_hash_bucket_s *bk,                  \
                          u32 fp,                                             \
                          u32 valid)                                          \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, fp);                            \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_find_empty(struct rix_hash_bucket_s *buckets,                     \
                  unsigned bk_idx)                                            \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                     \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                          \
    return empty ? (int)__builtin_ctz(empty) : -1;                            \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_find_ctx_s *ctx,                  \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_s *buckets,                \
                       const RIX_HASH_KEY_TYPE(type, key_field) *key,         \
                       unsigned hash_mask,                                    \
                       unsigned bk_mask)                                      \
{                                                                             \
    union rix_hash_hash_u h = hash_fn(key, hash_mask);                        \
    unsigned bk0, bk1;                                                        \
    u32 fp = rix_hash_fp(h, bk_mask, &bk0, &bk1);                             \
    ctx->hash      = h;                                                       \
    ctx->fp        = fp;                                                      \
    ctx->key       = (const void *)key;                                       \
    ctx->buckets   = buckets;                                                 \
    ctx->hash_mask = hash_mask;                                               \
    ctx->bk_mask   = bk_mask;                                                 \
    ctx->bk[0]     = buckets + bk0;                                           \
    ctx->bk[1]     = buckets + bk1;                                           \
    __builtin_prefetch(ctx->bk[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_find_ctx_s *ctx,                         \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets,                       \
                const RIX_HASH_KEY_TYPE(type, key_field) *key)                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_masked(ctx, head, buckets, key, mask, mask);              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_find_ctx_s *ctx,                          \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused))) \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_bucket_read_begin(ctx->bk[0]);               \
    ctx->fp_hits[0] = name##_scan_bucket_hashes(ctx->bk[0], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[0]));           \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_find_ctx_s *ctx,                    \
                     struct type *base)                                       \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = ctx->fp_hits[i];                                           \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = ctx->bk[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                struct type *node = name##_hptr(base, idx);                   \
                if (node != NULL)                                             \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
/* Unified bucket-half comparison; FORCE_INLINE folds the constant 'which'. */\
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD struct type *       \
name##_cmp_key_bk_once(struct rix_hash_mrsw_find_ctx_s *ctx,                  \
                       struct type *base, unsigned which)                     \
{                                                                             \
    u32 hits = ctx->fp_hits[which];                                           \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        struct type *node = name##_hptr(base, idx);                           \
        if (cmp_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,      \
                   &node->key_field) == 0)                                    \
            return node;                                                      \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_bk0_once(struct rix_hash_mrsw_find_ctx_s *ctx,                 \
                        struct type *base)                                    \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 0u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_bk1(struct rix_hash_mrsw_find_ctx_s *ctx)                     \
{                                                                             \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_find_ctx_s *ctx)                         \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_bucket_read_begin(ctx->bk[1]);               \
    ctx->fp_hits[1] = name##_scan_bucket_hashes(ctx->bk[1], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[1]));           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_bk1_once(struct rix_hash_mrsw_find_ctx_s *ctx,                 \
                        struct type *base)                                    \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_once(struct rix_hash_mrsw_find_ctx_s *ctx,                     \
                    struct type *base)                                        \
{                                                                             \
    struct type *ret = name##_cmp_key_bk0_once(ctx, base);                    \
    if (ret != NULL)                                                          \
        return ret;                                                           \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk1_once(ctx, base);                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_miss_retry(struct rix_hash_mrsw_find_ctx_s *ctx)                       \
{                                                                             \
    int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0], ctx->ctrl[0]);   \
    int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1], ctx->ctrl[1]);   \
    return retry0 || retry1;                                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_mrsw_find_ctx_s *ctx,                          \
               struct type *base)                                             \
{                                                                             \
    for (;;) {                                                                \
        struct type *ret = name##_cmp_key_once(ctx, base);                    \
        if (ret != NULL)                                                      \
            return ret;                                                       \
        RIX_HASH_MRSW_HOOK(#name, "reader_before_verify", NULL,              \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0], ctx->ctrl[0]); \
        RIX_HASH_MRSW_HOOK(#name, "reader_between_verify", NULL,             \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1], ctx->ctrl[1]); \
        if (!retry0 && !retry1)                                               \
            return ret;                                                       \
        RIX_HASH_MRSW_HOOK(#name, "reader_retry", NULL, ctx->buckets,        \
                           retry0 ? (unsigned)(ctx->bk[0] - ctx->buckets)    \
                                  : (unsigned)(ctx->bk[1] - ctx->buckets),    \
                           0u);                                               \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_find_ctx_s *ctx,                \
                         unsigned n,                                          \
                         struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,              \
                         const RIX_HASH_KEY_TYPE(type, key_field) * const *keys, \
                         unsigned hash_mask,                                  \
                         unsigned bk_mask)                                    \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_find_ctx_s *ctx,                       \
                  unsigned n,                                                 \
                  struct name *head,                                          \
                  struct rix_hash_bucket_s *buckets,                     \
                  const RIX_HASH_KEY_TYPE(type, key_field) * const *keys)     \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, mask, mask);        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_find_ctx_s *ctx,                        \
                 unsigned n,                                                  \
                 struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets)                      \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_find_ctx_s *ctx,                  \
                       unsigned n,                                            \
                       struct type *base)                                     \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_find_ctx_s *ctx,                        \
                 unsigned n,                                                  \
                 struct type *base,                                           \
                 struct type **results)                                       \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr struct type *                                                            \
name##_find(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                           \
            struct type *base,                                                \
            const RIX_HASH_KEY_TYPE(type, key_field) *key)                    \
{                                                                             \
    struct rix_hash_mrsw_find_ctx_s ctx;                                      \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head,                                     \
                       struct rix_hash_bucket_s *buckets,                \
                       unsigned bk,                                           \
                       unsigned slot)                                         \
{                                                                             \
    struct rix_hash_bucket_s *b = buckets + bk;                          \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed);     \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_bucket_valid_clear(b, slot);                                \
    RIX_HASH_MRSW_HOOK(#name, "remove_hash", head, buckets, bk, slot);        \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                      \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    return name##_remove_at_inner(head, buckets, bk, slot);                   \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                           \
            struct type *base,                                                \
            int (*cb)(struct type *, void *),                                 \
            void *arg)                                                        \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_s *bk = buckets + b;                      \
        u32 ctrl = rix_hash_mrsw_bucket_read_begin(bk);                       \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 idx = bk->idx[s];                                             \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            struct type *node = name##_hptr(base, idx);                       \
            int rc = cb(node, arg);                                           \
            if (rc)                                                           \
                return rc;                                                    \
        }                                                                     \
        if (rix_hash_mrsw_bucket_read_retry(bk, ctrl))                        \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}

/* ---- Writer ops factored: FP and SLOT share everything except slot_field  *
 *      maintenance and the _remove body.                                    *
 *      slot_set(elm, val, type, slot_field) writes elm->slot_field for SLOT *
 *      and is a no-op for FP.  remove_body(name, type, hash_field,          *
 *      slot_field, attr) expands to the variant-specific _remove body.      */
/* ---- per-variant node-side write & alt-bucket helpers ---------------- *
 * Three orthogonal slots specialize _RHM_OPS_INTERNAL per variant:        *
 *   slot_set      : writes node->slot_field        (SLOT only)            *
 *   node_hash_flip: nd->hash_field ^= fp on kickout (FP/SLOT, no-op KO)   *
 *   node_hash_set : elm->hash_field = h.val32[N] on insert (FP/SLOT only) *
 *   alt_bk_expr   : compute alt bucket index                              *
 *                   FP/SLOT use the XOR trick on stored hash_field;       *
 *                   KEYONLY re-hashes the node's key.                     *
 */
#  define _RHM_NO_SLOT_SET(elm, val, sf_type, sf)                             \
    ((void)(elm), (void)(val))
#  define _RHM_DO_SLOT_SET(elm, val, sf_type, sf)                             \
    ((elm)->sf = (RIX_HASH_SLOT_TYPE(sf_type, sf))(unsigned)(val))

#  define _RHM_NO_HASH_FLIP(target, fp, hf)    ((void)(target), (void)(fp))
#  define _RHM_DO_HASH_FLIP(target, fp, hf)    ((target)->hf ^= (fp))

#  define _RHM_NO_HASH_SET(target, val, hf)    ((void)(target), (void)(val))
#  define _RHM_DO_HASH_SET(target, val, hf)    ((target)->hf = (val))

#  define _RHM_ALT_BK_HASHED(fp, nd, bk_idx, mask, type, key_field, hash_field, hash_fn) \
    (((fp) ^ (nd)->hash_field) & (mask))

#  define _RHM_ALT_BK_REHASH(fp, nd, bk_idx, mask, type, key_field, hash_field, hash_fn) \
    (__extension__ ({                                                         \
        union rix_hash_hash_u _ab_h = hash_fn(                                \
            (const RIX_HASH_KEY_TYPE(type, key_field) *)&(nd)->key_field,     \
            (mask));                                                          \
        unsigned _ab_b0 = _ab_h.val32[0] & (mask);                            \
        unsigned _ab_b1 = _ab_h.val32[1] & (mask);                            \
        ((bk_idx) == _ab_b0) ? _ab_b1 : _ab_b0;                               \
    }))

/* remove body variants.  Signature is uniform; unused parameters are        \
 * silently dropped by each macro. */
#  define _RHM_REMOVE_FP(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned bk = (unsigned)(elm->hash_field & head->rhh_mask);               \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
    struct type *ret = NULL;                                                  \
    u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed);     \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u32 idx = b->idx[s];                                                  \
        if (idx == (u32)node_idx &&                                           \
            name##_remove_at_inner(head, buckets, bk, s) !=                   \
                (unsigned)RIX_NIL) {                                          \
            ret = elm;                                                        \
            break;                                                            \
        }                                                                     \
    }                                                                         \
    return ret;                                                               \
}

#  define _RHM_REMOVE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned bk = (unsigned)(elm->hash_field & head->rhh_mask);               \
    unsigned slot = (unsigned)elm->slot_field;                                \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return NULL;                                                          \
    if (b->idx[slot] != (u32)node_idx)                                        \
        return NULL;                                                          \
    if (name##_remove_at_inner(head, buckets, bk, slot) ==                    \
        (unsigned)RIX_NIL)                                                    \
        return NULL;                                                          \
    return elm;                                                               \
}

/* KEYONLY: re-hash key, probe both candidate buckets. */
#  define _RHM_REMOVE_KEYONLY(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u h = hash_fn(                                        \
        (const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, mask);   \
    unsigned bks[2] = { h.val32[0] & mask, h.val32[1] & mask };               \
    for (int i = 0; i < 2; i++) {                                             \
        struct rix_hash_bucket_s *b = buckets + bks[i];                       \
        u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed); \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            if (b->idx[s] == (u32)node_idx &&                                 \
                name##_remove_at_inner(head, buckets, bks[i], s) !=           \
                    (unsigned)RIX_NIL)                                        \
                return elm;                                                   \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}

#  define _RHM_OPS_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr, slot_set, hash_flip, hash_set, alt_bk_expr, remove_body) \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_flipflop(struct rix_hash_bucket_s *buckets,                            \
                struct type *base,                                            \
                unsigned mask,                                                \
                unsigned bk_idx,                                              \
                unsigned slot)                                                \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 fp = bk->hash[slot];                                                  \
    u32 idx = bk->idx[slot];                                                  \
    if (fp == 0u || idx == (u32)RIX_NIL)                                      \
        return -1;                                                            \
    struct type *nd = name##_hptr(base, idx);                                 \
    unsigned ab = alt_bk_expr(fp, nd, bk_idx, mask, type, key_field,          \
                              hash_field, hash_fn);                           \
    int alt_slot = name##_find_empty(buckets, ab);                            \
    if (alt_slot < 0)                                                         \
        return -1;                                                            \
    struct rix_hash_bucket_s *alt = buckets + ab;                             \
    hash_flip(nd, fp, hash_field);                                            \
    slot_set(nd, alt_slot, type, slot_field);                                 \
    /* Publish-before-unpublish: alt is set, then bk is cleared.  Readers     \
     * may see the entry in both buckets transiently; reverse order would    \
     * create a false-negative window. */                                     \
    alt->idx[alt_slot] = idx;                                                 \
    RIX_HASH_MRSW_HOOK(#name, "move_idx", NULL, buckets, ab,                  \
                       (unsigned)alt_slot);                                   \
    alt->hash[alt_slot] = fp;                                                 \
    rix_hash_mrsw_bucket_valid_set(alt, (unsigned)alt_slot);                  \
    RIX_HASH_MRSW_HOOK(#name, "move_hash", NULL, buckets, ab,                 \
                       (unsigned)alt_slot);                                   \
    RIX_HASH_MRSW_HOOK(#name, "move_before_old_clear", NULL, buckets,         \
                       bk_idx, slot);                                         \
    rix_hash_mrsw_bucket_valid_clear(bk, slot);                               \
    return (int)slot;                                                         \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_kickout(struct rix_hash_bucket_s *buckets,                             \
               struct type *base,                                             \
               unsigned mask,                                                 \
               unsigned bk_idx,                                               \
               int depth)                                                     \
{                                                                             \
    if (depth <= 0)                                                           \
        return -1;                                                            \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        if (name##_flipflop(buckets, base, mask, bk_idx, s) >= 0)             \
            return (int)s;                                                    \
    }                                                                         \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u32 fp = bk->hash[s];                                                 \
        u32 si = bk->idx[s];                                                  \
        if (si == (u32)RIX_NIL)                                               \
            continue;                                                         \
        struct type *sn = name##_hptr(base, si);                              \
        unsigned ab = alt_bk_expr(fp, sn, bk_idx, mask, type, key_field,      \
                                  hash_field, hash_fn);                       \
        if (name##_kickout(buckets, base, mask, ab, depth - 1) >= 0) {        \
            u32 now_fp = bk->hash[s];                                         \
            u32 now_idx = bk->idx[s];                                         \
            u32 now_valid = rix_hash_mrsw_bucket_valid_load(bk,               \
                                                            memory_order_relaxed); \
            if (now_fp != fp || now_idx != si ||                              \
                (now_valid & (UINT32_C(1) << s)) == 0u) {                     \
                int fs = name##_find_empty(buckets, bk_idx);                  \
                if (fs >= 0)                                                  \
                    return fs;                                                \
                valid = now_valid;                                            \
                continue;                                                     \
            }                                                                 \
            name##_flipflop(buckets, base, mask, bk_idx, s);                  \
            return (int)s;                                                    \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_insert_hashed_idx(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,                   \
                         struct type *base,                                   \
                         struct type *elm,                                    \
                         union rix_hash_hash_u h)                             \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned bk0, bk1;                                                        \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 fp = rix_hash_fp(h, mask, &bk0, &bk1);                                \
    struct rix_hash_bucket_s *bks[2] = { buckets + bk0, buckets + bk1 };      \
    u32 fp_hits_v[2];                                                         \
    int empty_slot_v[2];                                                      \
    hash_set(elm, h.val32[0], hash_field);                                    \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bks[i],                   \
                                                    memory_order_acquire);    \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        fp_hits_v[i] = name##_scan_bucket_hashes(bks[i], fp, valid);          \
        empty_slot_v[i] = empty ? (int)__builtin_ctz(empty) : -1;             \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = fp_hits_v[i];                                              \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 node_idx = bks[i]->idx[bit];                                  \
            if (node_idx == (u32)RIX_NIL)                                     \
                continue;                                                     \
            struct type *node = name##_hptr(base, node_idx);                  \
            if (cmp_fn(&elm->key_field, &node->key_field) == 0)               \
                return node_idx;                                              \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        int slot = empty_slot_v[i];                                           \
        if (slot >= 0) {                                                      \
            unsigned bki = (i == 0) ? bk0 : bk1;                              \
            struct rix_hash_bucket_s *bk = bks[i];                            \
            if (i == 1)                                                       \
                hash_set(elm, h.val32[1], hash_field);                        \
            slot_set(elm, slot, type, slot_field);                            \
            bk->idx[slot] = elm_idx;                                          \
            RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,       \
                               (unsigned)slot);                               \
            bk->hash[slot] = fp;                                              \
            rix_hash_mrsw_bucket_valid_set(bk, (unsigned)slot);               \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            return 0u;                                                        \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = name##_kickout(buckets, base, mask, bk0,                        \
                             RIX_HASH_FOLLOW_DEPTH);                          \
        if (pos >= 0) {                                                       \
            bki = bk0;                                                        \
        } else {                                                              \
            pos = name##_kickout(buckets, base, mask, bk1,                    \
                                 RIX_HASH_FOLLOW_DEPTH);                      \
            if (pos < 0)                                                      \
                return elm_idx;                                               \
            bki = bk1;                                                        \
            hash_set(elm, h.val32[1], hash_field);                            \
        }                                                                     \
        struct rix_hash_bucket_s *bk = buckets + bki;                         \
        slot_set(elm, pos, type, slot_field);                                 \
        bk->idx[pos] = elm_idx;                                               \
        RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,           \
                           (unsigned)pos);                                    \
        bk->hash[pos] = fp;                                                   \
        rix_hash_mrsw_bucket_valid_set(bk, (unsigned)pos);                    \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        return 0u;                                                            \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_insert_hashed(struct name *head,                                       \
                     struct rix_hash_bucket_s *buckets,                       \
                     struct type *base,                                       \
                     struct type *elm,                                        \
                     union rix_hash_hash_u h)                                 \
{                                                                             \
    u32 ret_idx = name##_insert_hashed_idx(head, buckets, base, elm, h);      \
    return (ret_idx == 0u) ? NULL : name##_hptr(base, ret_idx);               \
}                                                                             \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    union rix_hash_hash_u h =                                                 \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field,  \
                head->rhh_mask);                                              \
    return name##_insert_hashed(head, buckets, base, elm, h);                 \
}                                                                             \
remove_body(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr)

#  define RIX_HASH_MRSW_GENERATE_FP_OPS_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
    _RHM_OPS_INTERNAL(name, type, key_field, hash_field,                      \
                      /* slot_field unused */ key_field,                      \
                      cmp_fn, hash_fn, attr,                                  \
                      _RHM_NO_SLOT_SET, _RHM_DO_HASH_FLIP, _RHM_DO_HASH_SET,  \
                      _RHM_ALT_BK_HASHED, _RHM_REMOVE_FP)

#  define RIX_HASH_MRSW_GENERATE_SLOT_OPS_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
    _RHM_OPS_INTERNAL(name, type, key_field, hash_field, slot_field,          \
                      cmp_fn, hash_fn, attr,                                  \
                      _RHM_DO_SLOT_SET, _RHM_DO_HASH_FLIP, _RHM_DO_HASH_SET,  \
                      _RHM_ALT_BK_HASHED, _RHM_REMOVE_SLOT)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_OPS_INTERNAL(name, type, key_field, cmp_fn, hash_fn, attr) \
    _RHM_OPS_INTERNAL(name, type, key_field,                                  \
                      /* hash_field unused */ key_field,                      \
                      /* slot_field unused */ key_field,                      \
                      cmp_fn, hash_fn, attr,                                  \
                      _RHM_NO_SLOT_SET, _RHM_NO_HASH_FLIP, _RHM_NO_HASH_SET,  \
                      _RHM_ALT_BK_REHASH, _RHM_REMOVE_KEYONLY)

/* ---- public composed generators -------------------------------------- */
#  define RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
    RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field,         \
                                               hash_field, cmp_fn, hash_fn,   \
                                               attr)                          \
    RIX_HASH_MRSW_GENERATE_FP_OPS_INTERNAL(name, type, key_field, hash_field, \
                                           cmp_fn, hash_fn, attr)

#  define RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
    RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field,         \
                                               hash_field, cmp_fn, hash_fn,   \
                                               attr)                          \
    RIX_HASH_MRSW_GENERATE_SLOT_OPS_INTERNAL(name, type, key_field,           \
                                             hash_field, slot_field, cmp_fn,  \
                                             hash_fn, attr)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn, hash_fn, attr) \
    RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field,         \
                                               /* hash_field */ key_field,    \
                                               cmp_fn, hash_fn, attr)         \
    RIX_HASH_MRSW_GENERATE_KEYONLY_OPS_INTERNAL(name, type, key_field,        \
                                                cmp_fn, hash_fn, attr)


#endif /* _RIX_HASH_FP_MRSW_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
