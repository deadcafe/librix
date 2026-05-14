/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _RIX_HASH_SLOT_EXTRA_MR_H_
#  define _RIX_HASH_SLOT_EXTRA_MR_H_

#  include "rix_hash_mr_core.h"
#  include "rix_hash_slot_extra.h"

/* MRSW SLOT_EXTRA reuses struct rix_hash_bucket_extra_s (192 B, 3 CL).
 * Slot 15 of hash[] / idx[] aliases ctrl / reserved (same as the other
 * MRSW variants); extra[0..14] holds caller-defined u32 values that the
 * writer maintains across kickout.  Slot 15 of extra[] is unused. */
/*
 * MRSW/MRMW SLOT_EXTRA reuses the shared rix_hash_mrsw_find_ctx_s
 * (rix_hash_fp_mr.h).  Generators access bk_ex[2] and buckets_ex, which
 * alias bk[2] / buckets in that struct.
 */

/* SLOT_EXTRA bucket ctrl helpers (operate on rix_hash_bucket_extra_s).      */
static RIX_FORCE_INLINE u32
rix_hash_mrsw_extra_bucket_valid_load(struct rix_hash_bucket_extra_s *bk,
                                      memory_order order)
{
    return rix_hash_mrsw_ctrl_valid(atomic_load_explicit(&bk->ctrl, order));
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_extra_bucket_valid_set(struct rix_hash_bucket_extra_s *bk,
                                     unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) | bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_extra_bucket_valid_clear(struct rix_hash_bucket_extra_s *bk,
                                       unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) & ~bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_extra_bucket_read_begin(struct rix_hash_bucket_extra_s *bk)
{
    return atomic_load_explicit(&bk->ctrl, memory_order_acquire);
}

static RIX_FORCE_INLINE int
rix_hash_mrsw_extra_bucket_read_retry(struct rix_hash_bucket_extra_s *bk,
                                      u32 ctrl)
{
    u32 now = atomic_load_explicit(&bk->ctrl, memory_order_acquire);
    return now != ctrl;
}

/*
 * Only ctrl (with wlock/reserved aliased at idx[15]) needs to be cleared.
 * hash[], idx[], and extra[] are read only under a matching valid bit, so
 * uninitialised payload of valid=0 slots is never observed.  See the
 * commentary on rix_hash_mrsw_buckets_init().
 */
static RIX_FORCE_INLINE void
rix_hash_mrsw_extra_buckets_init(struct rix_hash_bucket_extra_s *buckets,
                                 unsigned nb_bk)
{
    for (unsigned b = 0u; b < nb_bk; b++) {
        struct rix_hash_bucket_extra_s *bk = buckets + b;
        atomic_init(&bk->ctrl, 0u);
        atomic_init(&bk->wlock, 0u);
    }
}

/* SLOT_EXTRA variant: extends SLOT with per-slot u32 extra[].  Writer copies
 * extra on kickout and sets it on insert.  Reader does not touch extra[] in
 * find; explicit get/set helpers are provided for caller-controlled access. */
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, attr) \
    attr void name##_init(struct name *head,                                  \
                          struct rix_hash_bucket_extra_s *buckets,            \
                          unsigned nb_bk);                                    \
    attr struct type *name##_insert(struct name *head,                        \
                                    struct rix_hash_bucket_extra_s *buckets,  \
                                    struct type *base, struct type *elm,      \
                                    u32 extra);                               \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_extra_s *buckets,   \
                                   unsigned bk, unsigned slot);               \
    attr struct type *name##_remove(struct name *head,                        \
                                    struct rix_hash_bucket_extra_s *buckets,  \
                                    struct type *base, struct type *elm);     \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         struct type *base,                                   \
                         int (*cb)(struct type *, void *), void *arg);

#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, )
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn)

#  define RIX_HASH_MRMW_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, )
#  define RIX_HASH_MRMW_PROTOTYPE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, RIX_UNUSED static)
#  define RIX_HASH_MRMW_PROTOTYPE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRMW_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn)
#  define RIX_HASH_MRMW_PROTOTYPE_SLOT_EXTRA_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRMW_PROTOTYPE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn)

#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, )
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, \
                                               RIX_HASH_DEFAULT_HASH_FN_NAME(name), )
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, \
                                               RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                               RIX_UNUSED static)

#  define RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, )
#  define RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, RIX_UNUSED static)
#  define RIX_HASH_MRMW_GENERATE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, \
                                               RIX_HASH_DEFAULT_HASH_FN_NAME(name), )
#  define RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, \
                                               RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                               RIX_UNUSED static)


/* ---- SLOT_EXTRA variant generator ------------------------------------- *
 * Structurally the SLOT MRSW pipeline with extra[] writes added on the    *
 * writer side; reader path leaves extra[] untouched (caller-controlled    *
 * extra_get / extra_set).  Bucket type is rix_hash_bucket_extra_s         *
 * (192 B, 3 cache lines) shared with pure SLOT_EXTRA.                      */
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr void                                                                     \
name##_init(struct name *head,                                                \
            struct rix_hash_bucket_extra_s *buckets,                          \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    rix_hash_mrsw_extra_buckets_init(buckets, nb_bk);                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    return (struct type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);    \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_hashes(struct rix_hash_bucket_extra_s *bk, u32 fp,         \
                          u32 valid)                                          \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, fp);                            \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_find_empty(struct rix_hash_bucket_extra_s *buckets, unsigned bk_idx)   \
{                                                                             \
    struct rix_hash_bucket_extra_s *bk = buckets + bk_idx;                    \
    u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bk,                     \
                                                      memory_order_relaxed);  \
    u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                          \
    return empty ? (int)__builtin_ctz(empty) : -1;                            \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_find_ctx_s *ctx,            \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_extra_s *buckets,               \
                       const RIX_HASH_KEY_TYPE(type, key_field) *key,         \
                       unsigned hash_mask, unsigned bk_mask)                  \
{                                                                             \
    union rix_hash_hash_u h = hash_fn(key, hash_mask);                        \
    unsigned bk0, bk1;                                                        \
    u32 fp = rix_hash_fp(h, bk_mask, &bk0, &bk1);                             \
    ctx->hash = h;                                                            \
    ctx->fp = fp;                                                             \
    ctx->key = (const void *)key;                                             \
    ctx->buckets_ex = buckets;                                                \
    ctx->hash_mask = hash_mask;                                               \
    ctx->bk_mask = bk_mask;                                                   \
    ctx->bk_ex[0] = buckets + bk0;                                               \
    ctx->bk_ex[1] = buckets + bk1;                                               \
    __builtin_prefetch(ctx->bk_ex[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk_ex[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_find_ctx_s *ctx,                   \
                struct name *head,                                            \
                struct rix_hash_bucket_extra_s *buckets,                      \
                const RIX_HASH_KEY_TYPE(type, key_field) *key)                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_masked(ctx, head, buckets, key, mask, mask);              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_find_ctx_s *ctx,                    \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_extra_s *buckets __attribute__((unused))) \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_extra_bucket_read_begin(ctx->bk_ex[0]);         \
    ctx->fp_hits[0] = name##_scan_bucket_hashes(ctx->bk_ex[0], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[0]));           \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_find_ctx_s *ctx,              \
                     struct type *base)                                       \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = ctx->fp_hits[i];                                           \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = ctx->bk_ex[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                struct type *node = name##_hptr(base, idx);                   \
                if (node)                                                     \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD struct type *       \
name##_cmp_key_bk_once(struct rix_hash_mrsw_find_ctx_s *ctx,            \
                       struct type *base, unsigned which)                     \
{                                                                             \
    u32 hits = ctx->fp_hits[which];                                           \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk_ex[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        struct type *node = name##_hptr(base, idx);                           \
        if (cmp_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,      \
                   &node->key_field) == 0)                                    \
            return node;                                                      \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_find_ctx_s *ctx)                   \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_extra_bucket_read_begin(ctx->bk_ex[1]);         \
    ctx->fp_hits[1] = name##_scan_bucket_hashes(ctx->bk_ex[1], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[1]));           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_once(struct rix_hash_mrsw_find_ctx_s *ctx,               \
                    struct type *base)                                        \
{                                                                             \
    struct type *r = name##_cmp_key_bk_once(ctx, base, 0u);                   \
    if (r != NULL)                                                            \
        return r;                                                             \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_mrsw_find_ctx_s *ctx, struct type *base) \
{                                                                             \
    for (;;) {                                                                \
        struct type *r = name##_cmp_key_once(ctx, base);                      \
        if (r != NULL)                                                        \
            return r;                                                         \
        int retry0 = rix_hash_mrsw_extra_bucket_read_retry(ctx->bk_ex[0],        \
                                                           ctx->ctrl[0]);     \
        int retry1 = rix_hash_mrsw_extra_bucket_read_retry(ctx->bk_ex[1],        \
                                                           ctx->ctrl[1]);     \
        if (!retry0 && !retry1)                                               \
            return NULL;                                                      \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_find_ctx_s *ctx,          \
                         unsigned n, struct name *head,                       \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         const RIX_HASH_KEY_TYPE(type, key_field) * const *keys, \
                         unsigned hash_mask, unsigned bk_mask)                \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_find_ctx_s *ctx, unsigned n,     \
                  struct name *head, struct rix_hash_bucket_extra_s *buckets, \
                  const RIX_HASH_KEY_TYPE(type, key_field) * const *keys)     \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, mask, mask);        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_find_ctx_s *ctx, unsigned n,      \
                 struct name *head, struct rix_hash_bucket_extra_s *buckets)  \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_find_ctx_s *ctx,            \
                       unsigned n, struct type *base)                         \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_find_ctx_s *ctx, unsigned n,      \
                 struct type *base, struct type **results)                   \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr struct type *                                                            \
name##_find(struct name *head, struct rix_hash_bucket_extra_s *buckets,       \
            struct type *base,                                                \
            const RIX_HASH_KEY_TYPE(type, key_field) *key)                    \
{                                                                             \
    struct rix_hash_mrsw_find_ctx_s ctx;                                \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head,                                     \
                       struct rix_hash_bucket_extra_s *buckets,               \
                       unsigned bk, unsigned slot)                            \
{                                                                             \
    struct rix_hash_bucket_extra_s *b = buckets + bk;                         \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_extra_bucket_valid_load(b,                      \
                                                      memory_order_relaxed);  \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_extra_bucket_valid_clear(b, slot);                          \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head, struct rix_hash_bucket_extra_s *buckets,  \
                 unsigned bk, unsigned slot)                                  \
{                                                                             \
    return name##_remove_at_inner(head, buckets, bk, slot);                   \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head, struct rix_hash_bucket_extra_s *buckets,       \
            struct type *base,                                                \
            int (*cb)(struct type *, void *), void *arg)                      \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_extra_s *bk = buckets + b;                     \
        u32 ctrl = rix_hash_mrsw_extra_bucket_read_begin(bk);                 \
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
        if (rix_hash_mrsw_extra_bucket_read_retry(bk, ctrl))                  \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_flipflop(struct rix_hash_bucket_extra_s *buckets,                      \
                struct type *base,                                            \
                unsigned mask, unsigned bk_idx, unsigned slot)                \
{                                                                             \
    struct rix_hash_bucket_extra_s *bk = buckets + bk_idx;                    \
    u32 fp = bk->hash[slot];                                                  \
    u32 idx = bk->idx[slot];                                                  \
    u32 extra_save = bk->extra[slot];                                         \
    if (fp == 0u || idx == (u32)RIX_NIL)                                      \
        return -1;                                                            \
    struct type *nd = name##_hptr(base, idx);                                 \
    unsigned ab = (fp ^ nd->hash_field) & mask;                               \
    int alt_slot = name##_find_empty(buckets, ab);                            \
    if (alt_slot < 0)                                                         \
        return -1;                                                            \
    struct rix_hash_bucket_extra_s *alt = buckets + ab;                       \
    nd->hash_field ^= fp;                                                     \
    nd->slot_field =                                                          \
        (RIX_HASH_SLOT_TYPE(type, slot_field))(unsigned)alt_slot;             \
    alt->idx[alt_slot] = idx;                                                 \
    alt->extra[alt_slot] = extra_save;                                        \
    alt->hash[alt_slot] = fp;                                                 \
    rix_hash_mrsw_extra_bucket_valid_set(alt, (unsigned)alt_slot);            \
    rix_hash_mrsw_extra_bucket_valid_clear(bk, slot);                         \
    return (int)slot;                                                         \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_kickout(struct rix_hash_bucket_extra_s *buckets, struct type *base,    \
               unsigned mask, unsigned bk_idx, int depth)                     \
{                                                                             \
    if (depth <= 0)                                                           \
        return -1;                                                            \
    struct rix_hash_bucket_extra_s *bk = buckets + bk_idx;                    \
    u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bk,                     \
                                                      memory_order_relaxed);  \
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
        unsigned ab = (fp ^ sn->hash_field) & mask;                           \
        if (name##_kickout(buckets, base, mask, ab, depth - 1) >= 0) {        \
            u32 now_fp = bk->hash[s];                                         \
            u32 now_idx = bk->idx[s];                                         \
            u32 now_valid = rix_hash_mrsw_extra_bucket_valid_load(bk,         \
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
attr struct type *                                                            \
name##_insert(struct name *head, struct rix_hash_bucket_extra_s *buckets,     \
              struct type *base, struct type *elm, u32 extra)                 \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u h =                                                 \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field,  \
                mask);                                                        \
    unsigned bk0, bk1;                                                        \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 fp = rix_hash_fp(h, mask, &bk0, &bk1);                                \
    struct rix_hash_bucket_extra_s *bks[2] = {                                \
        buckets + bk0, buckets + bk1 };                                       \
    u32 fp_hits_v[2];                                                         \
    int empty_slot_v[2];                                                      \
    elm->hash_field = h.val32[0];                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bks[i],             \
                                                          memory_order_acquire); \
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
                return node;                                                  \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        int slot = empty_slot_v[i];                                           \
        if (slot >= 0) {                                                      \
            unsigned bki = (i == 0) ? bk0 : bk1;                              \
            struct rix_hash_bucket_extra_s *bk = bks[i];                      \
            (void)bki;                                                        \
            if (i == 1)                                                       \
                elm->hash_field = h.val32[1];                                 \
            elm->slot_field =                                                 \
                (RIX_HASH_SLOT_TYPE(type, slot_field))(unsigned)slot;         \
            bk->idx[slot] = elm_idx;                                          \
            bk->extra[slot] = extra;                                          \
            bk->hash[slot] = fp;                                              \
            rix_hash_mrsw_extra_bucket_valid_set(bk, (unsigned)slot);         \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = name##_kickout(buckets, base, mask, bk0, RIX_HASH_FOLLOW_DEPTH);\
        if (pos >= 0)                                                         \
            bki = bk0;                                                        \
        else {                                                                \
            pos = name##_kickout(buckets, base, mask, bk1,                    \
                                 RIX_HASH_FOLLOW_DEPTH);                      \
            if (pos < 0)                                                      \
                return elm;                                                   \
            bki = bk1;                                                        \
            elm->hash_field = h.val32[1];                                     \
        }                                                                     \
        struct rix_hash_bucket_extra_s *bk = buckets + bki;                   \
        elm->slot_field =                                                     \
            (RIX_HASH_SLOT_TYPE(type, slot_field))(unsigned)pos;              \
        bk->idx[pos] = elm_idx;                                               \
        bk->extra[pos] = extra;                                               \
        bk->hash[pos] = fp;                                                   \
        rix_hash_mrsw_extra_bucket_valid_set(bk, (unsigned)pos);              \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        return NULL;                                                          \
    }                                                                         \
}                                                                             \
attr struct type *                                                            \
name##_remove(struct name *head, struct rix_hash_bucket_extra_s *buckets,     \
              struct type *base, struct type *elm)                            \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned bk = (unsigned)(elm->hash_field & head->rhh_mask);               \
    unsigned slot = (unsigned)elm->slot_field;                                \
    struct rix_hash_bucket_extra_s *b = buckets + bk;                         \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return NULL;                                                          \
    if (b->idx[slot] != (u32)node_idx)                                        \
        return NULL;                                                          \
    if (name##_remove_at_inner(head, buckets, bk, slot) ==                    \
        (unsigned)RIX_NIL)                                                    \
        return NULL;                                                          \
    return elm;                                                               \
}

/* EXTRA MRMW insert_slow.  See rix_hash_fp_mr.h for the algorithm
 * description; this is the SLOT_EXTRA specialization (carries the
 * variant-specific extra[] payload through the cuckoo path). */
#  define _RHM_MRMW_EXTRA_INSERT_SLOW(name, type, key_field, hash_field, slot_field, cmp_fn, attr) \
static RIX_UNUSED struct type *                                               \
name##_insert_slow(struct name *head,                                         \
                   struct rix_hash_bucket_extra_s *buckets,                   \
                   struct type *base, struct type *elm, u32 extra,            \
                   union rix_hash_hash_u h, u32 fp,                           \
                   unsigned bk0, unsigned bk1)                                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned nb_bk = mask + 1u;                                               \
    unsigned nil = nb_bk;                                                     \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    struct type *ret = elm;                                                   \
    unsigned *mem = head->rhh_kickout_scratch;                                \
    if (mem == NULL)                                                          \
        return ret;                                                           \
    unsigned *queue       = mem;                                              \
    unsigned *parent_bk   = mem + nb_bk;                                      \
    unsigned *parent_slot = mem + nb_bk * 2u;                                 \
    unsigned *visit_set   = mem + nb_bk * 3u;                                 \
    unsigned *ctrl_snap   = mem + nb_bk * 4u;                                 \
    RIX_HASH_MRSW_HOOK(#name, "insert_slow_before_lock", head, buckets,       \
                       bk0, bk1);                                             \
    RIX_HASH_MR_KICKOUT_LOCK_MRMW(head);                                      \
    for (unsigned attempt = 0u; attempt < 3u; attempt++) {                    \
        for (unsigned i = 0u; i < nb_bk; i++) {                               \
            parent_bk[i] = nil;                                               \
            parent_slot[i] = nil;                                             \
        }                                                                     \
        unsigned qh = 0u, qt = 0u;                                            \
        unsigned nv = 0u;                                                     \
        parent_bk[bk0] = bk0;                                                 \
        visit_set[nv++] = bk0;                                                \
        ctrl_snap[bk0] = atomic_load_explicit(&buckets[bk0].ctrl,             \
                                              memory_order_acquire);          \
        queue[qt++] = bk0;                                                    \
        if (bk1 != bk0) {                                                     \
            parent_bk[bk1] = bk1;                                             \
            visit_set[nv++] = bk1;                                            \
            ctrl_snap[bk1] = atomic_load_explicit(&buckets[bk1].ctrl,         \
                                                  memory_order_acquire);      \
            queue[qt++] = bk1;                                                \
        }                                                                     \
        struct type *dup_ret = NULL;                                          \
        unsigned free_bk = nil, free_slot = nil;                              \
        for (int si = 0; si < (int)qt; si++) {                                \
            unsigned scan_bk = queue[si];                                     \
            u32 sv = rix_hash_mrsw_ctrl_valid(ctrl_snap[scan_bk]);            \
            u32 hits = name##_scan_bucket_hashes(buckets + scan_bk, fp, sv);  \
            while (hits) {                                                    \
                unsigned bit = (unsigned)__builtin_ctz(hits);                 \
                hits &= hits - 1u;                                            \
                u32 node_idx = buckets[scan_bk].idx[bit];                     \
                if (node_idx == (u32)RIX_NIL) continue;                       \
                struct type *node = name##_hptr(base, node_idx);              \
                if (cmp_fn(&elm->key_field, &node->key_field) == 0) {         \
                    dup_ret = node;                                           \
                    break;                                                    \
                }                                                             \
            }                                                                 \
            if (dup_ret != NULL) break;                                       \
        }                                                                     \
        if (dup_ret == NULL) {                                                \
            while (qh < qt && free_bk == nil) {                               \
                unsigned cur_bk = queue[qh++];                                \
                u32 valid = rix_hash_mrsw_ctrl_valid(ctrl_snap[cur_bk]);      \
                u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;              \
                if (empty) {                                                  \
                    free_bk = cur_bk;                                         \
                    free_slot = (unsigned)__builtin_ctz(empty);               \
                    break;                                                    \
                }                                                             \
                struct rix_hash_bucket_extra_s *bk = buckets + cur_bk;        \
                for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ;      \
                     s++) {                                                   \
                    if ((valid & (UINT32_C(1) << s)) == 0u) continue;         \
                    u32 idx = bk->idx[s];                                     \
                    if (idx == (u32)RIX_NIL) continue;                        \
                    u32 move_fp = bk->hash[s];                                \
                    struct type *node = name##_hptr(base, idx);               \
                    unsigned ab = (move_fp ^ node->hash_field) & mask;        \
                    if (parent_bk[ab] != nil) continue;                       \
                    parent_bk[ab] = cur_bk;                                   \
                    parent_slot[ab] = s;                                      \
                    visit_set[nv++] = ab;                                     \
                    ctrl_snap[ab] = atomic_load_explicit(                     \
                        &buckets[ab].ctrl, memory_order_acquire);             \
                    queue[qt++] = ab;                                         \
                }                                                             \
            }                                                                 \
        }                                                                     \
        for (unsigned i = 1u; i < nv; i++) {                                  \
            unsigned x = visit_set[i];                                        \
            unsigned j = i;                                                   \
            while (j > 0u && visit_set[j - 1u] > x) {                         \
                visit_set[j] = visit_set[j - 1u];                             \
                j--;                                                          \
            }                                                                 \
            visit_set[j] = x;                                                 \
        }                                                                     \
        for (unsigned i = 0u; i < nv; i++)                                    \
            rix_hash_mrmw_lock(&buckets[visit_set[i]].wlock);                 \
        unsigned valid_scout = 1u;                                            \
        for (unsigned i = 0u; i < nv; i++) {                                  \
            unsigned bk_i = visit_set[i];                                     \
            u32 now = atomic_load_explicit(&buckets[bk_i].ctrl,               \
                                           memory_order_acquire);             \
            if (now != ctrl_snap[bk_i]) {                                     \
                valid_scout = 0u;                                             \
                break;                                                        \
            }                                                                 \
        }                                                                     \
        if (!valid_scout) {                                                   \
            for (unsigned i = nv; i > 0u; i--)                                \
                rix_hash_mrmw_unlock(&buckets[visit_set[i - 1u]].wlock);      \
            continue;                                                         \
        }                                                                     \
        if (dup_ret != NULL) {                                                \
            ret = dup_ret;                                                    \
            goto v_unlock;                                                    \
        }                                                                     \
        if (free_bk == nil)                                                   \
            goto v_unlock;                                                    \
        while (parent_slot[free_bk] != nil) {                                 \
            unsigned src_bk = parent_bk[free_bk];                             \
            unsigned src_slot = parent_slot[free_bk];                         \
            struct rix_hash_bucket_extra_s *src = buckets + src_bk;           \
            struct rix_hash_bucket_extra_s *dst = buckets + free_bk;          \
            u32 move_fp = src->hash[src_slot];                                \
            u32 move_idx = src->idx[src_slot];                                \
            u32 move_extra = src->extra[src_slot];                            \
            struct type *node = name##_hptr(base, move_idx);                  \
            node->hash_field ^= move_fp;                                      \
            node->slot_field =                                                \
                (RIX_HASH_SLOT_TYPE(type, slot_field))free_slot;              \
            dst->idx[free_slot] = move_idx;                                   \
            dst->extra[free_slot] = move_extra;                               \
            dst->hash[free_slot] = move_fp;                                   \
            rix_hash_mrsw_extra_bucket_valid_set(dst, free_slot);             \
            rix_hash_mrsw_extra_bucket_valid_clear(src, src_slot);            \
            free_bk = src_bk;                                                 \
            free_slot = src_slot;                                             \
        }                                                                     \
        elm->hash_field = (free_bk == bk1) ? h.val32[1] : h.val32[0];         \
        elm->slot_field = (RIX_HASH_SLOT_TYPE(type, slot_field))free_slot;    \
        buckets[free_bk].idx[free_slot] = elm_idx;                            \
        buckets[free_bk].extra[free_slot] = extra;                            \
        buckets[free_bk].hash[free_slot] = fp;                                \
        rix_hash_mrsw_extra_bucket_valid_set(&buckets[free_bk], free_slot);   \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        ret = NULL;                                                           \
v_unlock:                                                                     \
        for (unsigned i = nv; i > 0u; i--)                                    \
            rix_hash_mrmw_unlock(&buckets[visit_set[i - 1u]].wlock);          \
        RIX_HASH_MR_KICKOUT_UNLOCK_MRMW(head);                                \
        return ret;                                                           \
    }                                                                         \
    RIX_HASH_MR_KICKOUT_UNLOCK_MRMW(head);                                    \
    return ret;                                                               \
}

#  define RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr void                                                                     \
name##_init(struct name *head,                                                \
            struct rix_hash_bucket_extra_s *buckets,                          \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    RIX_HASH_MR_HEAD_INIT_MRMW(head);                                         \
    rix_hash_mrsw_extra_buckets_init(buckets, nb_bk);                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    return (struct type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);    \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_hashes(struct rix_hash_bucket_extra_s *bk, u32 fp,         \
                          u32 valid)                                          \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, fp);                            \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_find_ctx_s *ctx,            \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_extra_s *buckets,               \
                       const RIX_HASH_KEY_TYPE(type, key_field) *key,         \
                       unsigned hash_mask, unsigned bk_mask)                  \
{                                                                             \
    union rix_hash_hash_u h = hash_fn(key, hash_mask);                        \
    unsigned bk0, bk1;                                                        \
    u32 fp = rix_hash_fp(h, bk_mask, &bk0, &bk1);                             \
    ctx->hash = h;                                                            \
    ctx->fp = fp;                                                             \
    ctx->key = (const void *)key;                                             \
    ctx->buckets_ex = buckets;                                                \
    ctx->hash_mask = hash_mask;                                               \
    ctx->bk_mask = bk_mask;                                                   \
    ctx->bk_ex[0] = buckets + bk0;                                               \
    ctx->bk_ex[1] = buckets + bk1;                                               \
    __builtin_prefetch(ctx->bk_ex[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk_ex[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_find_ctx_s *ctx,                   \
                struct name *head,                                            \
                struct rix_hash_bucket_extra_s *buckets,                      \
                const RIX_HASH_KEY_TYPE(type, key_field) *key)                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_masked(ctx, head, buckets, key, mask, mask);              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_find_ctx_s *ctx,                    \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_extra_s *buckets __attribute__((unused))) \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_extra_bucket_read_begin(ctx->bk_ex[0]);         \
    ctx->fp_hits[0] = name##_scan_bucket_hashes(ctx->bk_ex[0], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[0]));           \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_find_ctx_s *ctx,              \
                     struct type *base)                                       \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = ctx->fp_hits[i];                                           \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = ctx->bk_ex[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                struct type *node = name##_hptr(base, idx);                   \
                if (node)                                                     \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD struct type *       \
name##_cmp_key_bk_once(struct rix_hash_mrsw_find_ctx_s *ctx,            \
                       struct type *base, unsigned which)                     \
{                                                                             \
    u32 hits = ctx->fp_hits[which];                                           \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk_ex[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        struct type *node = name##_hptr(base, idx);                           \
        if (cmp_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,      \
                   &node->key_field) == 0)                                    \
            return node;                                                      \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_find_ctx_s *ctx)                   \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_extra_bucket_read_begin(ctx->bk_ex[1]);         \
    ctx->fp_hits[1] = name##_scan_bucket_hashes(ctx->bk_ex[1], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[1]));           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_once(struct rix_hash_mrsw_find_ctx_s *ctx,               \
                    struct type *base)                                        \
{                                                                             \
    struct type *r = name##_cmp_key_bk_once(ctx, base, 0u);                   \
    if (r != NULL)                                                            \
        return r;                                                             \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_mrsw_find_ctx_s *ctx, struct type *base) \
{                                                                             \
    for (;;) {                                                                \
        struct type *r = name##_cmp_key_once(ctx, base);                      \
        if (r != NULL)                                                        \
            return r;                                                         \
        int retry0 = rix_hash_mrsw_extra_bucket_read_retry(ctx->bk_ex[0],        \
                                                           ctx->ctrl[0]);     \
        int retry1 = rix_hash_mrsw_extra_bucket_read_retry(ctx->bk_ex[1],        \
                                                           ctx->ctrl[1]);     \
        if (!retry0 && !retry1)                                               \
            return NULL;                                                      \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_find_ctx_s *ctx,          \
                         unsigned n, struct name *head,                       \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         const RIX_HASH_KEY_TYPE(type, key_field) * const *keys, \
                         unsigned hash_mask, unsigned bk_mask)                \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_find_ctx_s *ctx, unsigned n,     \
                  struct name *head, struct rix_hash_bucket_extra_s *buckets, \
                  const RIX_HASH_KEY_TYPE(type, key_field) * const *keys)     \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, mask, mask);        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_find_ctx_s *ctx, unsigned n,      \
                 struct name *head, struct rix_hash_bucket_extra_s *buckets)  \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_find_ctx_s *ctx,            \
                       unsigned n, struct type *base)                         \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_find_ctx_s *ctx, unsigned n,      \
                 struct type *base, struct type **results)                   \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr struct type *                                                            \
name##_find(struct name *head, struct rix_hash_bucket_extra_s *buckets,       \
            struct type *base,                                                \
            const RIX_HASH_KEY_TYPE(type, key_field) *key)                    \
{                                                                             \
    struct rix_hash_mrsw_find_ctx_s ctx;                                \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head,                                     \
                       struct rix_hash_bucket_extra_s *buckets,               \
                       unsigned bk, unsigned slot)                            \
{                                                                             \
    struct rix_hash_bucket_extra_s *b = buckets + bk;                         \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_extra_bucket_valid_load(b,                      \
                                                      memory_order_relaxed);  \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_extra_bucket_valid_clear(b, slot);                          \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head, struct rix_hash_bucket_extra_s *buckets,  \
                 unsigned bk, unsigned slot)                                  \
{                                                                             \
    struct rix_hash_bucket_extra_s *b = buckets + bk;                         \
    RIX_HASH_MR_BK_LOCK_MRMW(b);                                              \
    unsigned ret = name##_remove_at_inner(head, buckets, bk, slot);           \
    RIX_HASH_MR_BK_UNLOCK_MRMW(b);                                            \
    return ret;                                                               \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head, struct rix_hash_bucket_extra_s *buckets,       \
            struct type *base,                                                \
            int (*cb)(struct type *, void *), void *arg)                      \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_extra_s *bk = buckets + b;                     \
        u32 ctrl = rix_hash_mrsw_extra_bucket_read_begin(bk);                 \
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
        if (rix_hash_mrsw_extra_bucket_read_retry(bk, ctrl))                  \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
_RHM_MRMW_EXTRA_INSERT_SLOW(name, type, key_field, hash_field, slot_field, cmp_fn, attr)                        \
attr void                                                                     \
name##_attach_kickout_scratch(struct name *head, unsigned *scratch)           \
{                                                                             \
    head->rhh_kickout_scratch = scratch;                                      \
}                                                                             \
attr struct type *                                                            \
name##_insert(struct name *head, struct rix_hash_bucket_extra_s *buckets,     \
              struct type *base, struct type *elm, u32 extra)                 \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u h =                                                 \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field,  \
                mask);                                                        \
    unsigned bk0, bk1;                                                        \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 fp = rix_hash_fp(h, mask, &bk0, &bk1);                                \
    struct rix_hash_bucket_extra_s *bks[2] = {                                \
        buckets + bk0, buckets + bk1 };                                       \
    elm->hash_field = h.val32[0];                                             \
    RIX_HASH_MR_BK_LOCK2_MRMW(bks[0], bks[1]);                                \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bks[i],             \
                                                          memory_order_acquire); \
        u32 hits = name##_scan_bucket_hashes(bks[i], fp, valid);              \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 node_idx = bks[i]->idx[bit];                                  \
            if (node_idx == (u32)RIX_NIL)                                     \
                continue;                                                     \
            struct type *node = name##_hptr(base, node_idx);                  \
            if (cmp_fn(&elm->key_field, &node->key_field) == 0) {             \
                RIX_HASH_MR_BK_UNLOCK2_MRMW(bks[0], bks[1]);                  \
                return node;                                                  \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bks[i],             \
                                                          memory_order_relaxed); \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        if (empty) {                                                          \
            unsigned slot = (unsigned)__builtin_ctz(empty);                   \
            if (i == 1)                                                       \
                elm->hash_field = h.val32[1];                                 \
            elm->slot_field =                                                 \
                (RIX_HASH_SLOT_TYPE(type, slot_field))slot;                   \
            bks[i]->idx[slot] = elm_idx;                                      \
            bks[i]->extra[slot] = extra;                                      \
            bks[i]->hash[slot] = fp;                                          \
            rix_hash_mrsw_extra_bucket_valid_set(bks[i], slot);              \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            RIX_HASH_MR_BK_UNLOCK2_MRMW(bks[0], bks[1]);                      \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    RIX_HASH_MR_BK_UNLOCK2_MRMW(bks[0], bks[1]);                              \
    return name##_insert_slow(head, buckets, base, elm, extra, h, fp, bk0, bk1); \
}                                                                             \
attr struct type *                                                            \
name##_remove(struct name *head, struct rix_hash_bucket_extra_s *buckets,     \
              struct type *base, struct type *elm)                            \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u h = hash_fn(                                        \
        (const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, mask);   \
    unsigned bk0, bk1;                                                        \
    (void)rix_hash_fp(h, mask, &bk0, &bk1);                                   \
    struct rix_hash_bucket_extra_s *b0 = buckets + bk0;                       \
    struct rix_hash_bucket_extra_s *b1 = buckets + bk1;                       \
    struct type *ret = NULL;                                                  \
    RIX_HASH_MR_BK_LOCK2_MRMW(b0, b1);                                        \
    unsigned bks[2] = { bk0, bk1 };                                           \
    for (int i = 0; i < 2 && ret == NULL; i++) {                              \
        struct rix_hash_bucket_extra_s *b = buckets + bks[i];                 \
        u32 valid = rix_hash_mrsw_extra_bucket_valid_load(                    \
            b, memory_order_relaxed);                                         \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            if (b->idx[s] == (u32)node_idx &&                                 \
                name##_remove_at_inner(head, buckets, bks[i], s) !=           \
                    (unsigned)RIX_NIL) {                                      \
                ret = elm;                                                    \
                break;                                                        \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    RIX_HASH_MR_BK_UNLOCK2_MRMW(b0, b1);                                      \
    return ret;                                                               \
}


#endif /* _RIX_HASH_SLOT_EXTRA_MR_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
