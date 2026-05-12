/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _RIX_HASH_U32_MR_H_
#  define _RIX_HASH_U32_MR_H_

#  include "rix_hash_mr_core.h"

/* MRSW U32 stores u32 keys directly in bk->hash[]; valid bits in the
 * shared ctrl word decide which slots are visible to readers. */
struct rix_hash_mrsw_u32_find_ctx_s {
    struct rix_hash_bucket_s *bk[2];
    struct rix_hash_bucket_s *buckets;
    u32                       ctrl[2];
    u32                       hits[2];
    u32                       key;
    unsigned                  bk_mask;
};

/* U32 variant: 32-bit keys stored directly in bk->hash[] (reinterpreted as  \
 * keys[]).  No fingerprint, no cmp_fn needed (key match in bucket scan is   \
 * authoritative once filtered by valid).  Hash function:                    \
 *   rix_hash_arch->hash_u32 (shared with pure U32).                          \
 * Empty slots have valid bit = 0; hash[] of unused slots may contain        \
 * stale keys that are ignored.                                              \
 */
#  define RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, attr)   \
    attr void name##_init(struct name *head,                                  \
                          struct rix_hash_bucket_s *buckets,                  \
                          unsigned nb_bk);                                    \
    attr type *name##_insert(struct name *head,                               \
                             struct rix_hash_bucket_s *buckets,               \
                             type *base, type *elm);                          \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_s *buckets,         \
                                   unsigned bk, unsigned slot);               \
    attr type *name##_remove(struct name *head,                               \
                             struct rix_hash_bucket_s *buckets,               \
                             type *base, type *elm);                          \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,                   \
                         type *base, int (*cb)(type *, void *), void *arg);

#  define RIX_HASH_MRSW_PROTOTYPE_U32(name, type, key_field)                  \
    RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRSW_PROTOTYPE_U32_STATIC(name, type, key_field)           \
    RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, RIX_UNUSED static)

#  define RIX_HASH_MRMW_PROTOTYPE_U32(name, type, key_field)                  \
    RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRMW_PROTOTYPE_U32_STATIC(name, type, key_field)           \
    RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE_U32(name, type, key_field)                   \
    RIX_HASH_MRSW_GENERATE_U32_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRSW_GENERATE_U32_STATIC(name, type, key_field)            \
    RIX_HASH_MRSW_GENERATE_U32_INTERNAL(name, type, key_field, RIX_UNUSED static)

#  define RIX_HASH_MRMW_GENERATE_U32(name, type, key_field)                   \
    RIX_HASH_MRMW_GENERATE_U32_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRMW_GENERATE_U32_STATIC(name, type, key_field)            \
    RIX_HASH_MRMW_GENERATE_U32_INTERNAL(name, type, key_field, RIX_UNUSED static)


/* ---- U32 variant generator -------------------------------------------- *
 * Structurally distinct from FP/SLOT/KEYONLY: bk->hash[] is reinterpreted   *
 * as a u32 keys[] array; no fingerprint, no cmp_fn.  The MRSW ctrl/valid   *
 * protocol is identical.  This generator does not share the common-pre     *
 * macros above because the find pipeline omits the fp/cmp_fn step.         */
#  define RIX_HASH_MRSW_GENERATE_U32_INTERNAL(name, type, key_field, attr)    \
attr void                                                                     \
name##_init(struct name *head, struct rix_hash_bucket_s *buckets,             \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    rix_hash_mrsw_buckets_init(buckets, nb_bk);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(type *base, const type *p)                                        \
{                                                                             \
    return RIX_IDX_FROM_PTR(base, (type *)(uintptr_t)p);                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_hptr(type *base, unsigned i)                                           \
{                                                                             \
    return (type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);           \
}                                                                             \
/* Intentional benign race: hash[] (u32 keys) is non-atomic; the bucket      \
 * ctrl seq+valid protocol guards observations. */                            \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_keys(struct rix_hash_bucket_s *bk, u32 key, u32 valid)     \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, key);                           \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_find_empty(struct rix_hash_bucket_s *buckets, unsigned bk_idx)         \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                          \
    return empty ? (int)__builtin_ctz(empty) : -1;                            \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_u32_find_ctx_s *ctx,              \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_s *buckets,                     \
                       u32 key,                                               \
                       unsigned hash_mask __attribute__((unused)),             \
                       unsigned bk_mask)                                      \
{                                                                             \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, bk_mask);          \
    ctx->key     = key;                                                       \
    ctx->buckets = buckets;                                                   \
    ctx->bk_mask = bk_mask;                                                   \
    ctx->bk[0]   = buckets + (h.val32[0] & bk_mask);                          \
    ctx->bk[1]   = buckets + (h.val32[1] & bk_mask);                          \
    __builtin_prefetch(ctx->bk[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_u32_find_ctx_s *ctx,                     \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets, u32 key)                   \
{                                                                             \
    name##_hash_key_masked(ctx, head, buckets, key, head->rhh_mask,           \
                           head->rhh_mask);                                   \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_u32_find_ctx_s *ctx,                      \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused)))     \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_bucket_read_begin(ctx->bk[0]);               \
    ctx->hits[0] = name##_scan_bucket_keys(ctx->bk[0], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[0]));               \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->hits[1] = 0u;                                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)    \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 h = ctx->hits[i];                                                 \
        while (h) {                                                           \
            unsigned bit = (unsigned)__builtin_ctz(h);                        \
            h &= h - 1u;                                                      \
            u32 idx = ctx->bk[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                type *node = name##_hptr(base, idx);                          \
                if (node)                                                     \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD type *              \
name##_cmp_key_bk_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base,  \
                       unsigned which)                                        \
{                                                                             \
    u32 hits = ctx->hits[which];                                              \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        return name##_hptr(base, idx);                                        \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk0_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 0u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_bk1(struct rix_hash_mrsw_u32_find_ctx_s *ctx)                 \
{                                                                             \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_u32_find_ctx_s *ctx)                     \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_bucket_read_begin(ctx->bk[1]);               \
    ctx->hits[1] = name##_scan_bucket_keys(ctx->bk[1], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[1]));               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk1_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)     \
{                                                                             \
    type *r = name##_cmp_key_bk0_once(ctx, base);                             \
    if (r != NULL)                                                            \
        return r;                                                             \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk1_once(ctx, base);                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_miss_retry(struct rix_hash_mrsw_u32_find_ctx_s *ctx)                   \
{                                                                             \
    int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0], ctx->ctrl[0]);   \
    int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1], ctx->ctrl[1]);   \
    return retry0 || retry1;                                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)          \
{                                                                             \
    for (;;) {                                                                \
        type *r = name##_cmp_key_once(ctx, base);                             \
        if (r != NULL)                                                        \
            return r;                                                         \
        RIX_HASH_MRSW_HOOK(#name, "reader_before_verify", NULL,               \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0],              \
                                                     ctx->ctrl[0]);           \
        RIX_HASH_MRSW_HOOK(#name, "reader_between_verify", NULL,              \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1],              \
                                                     ctx->ctrl[1]);           \
        if (!retry0 && !retry1)                                               \
            return NULL;                                                      \
        RIX_HASH_MRSW_HOOK(#name, "reader_retry", NULL, ctx->buckets,         \
                           retry0 ? (unsigned)(ctx->bk[0] - ctx->buckets)     \
                                  : (unsigned)(ctx->bk[1] - ctx->buckets),    \
                           0u);                                               \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_u32_find_ctx_s *ctx,            \
                         unsigned n, struct name *head,                       \
                         struct rix_hash_bucket_s *buckets,                   \
                         const u32 *keys,                                     \
                         unsigned hash_mask, unsigned bk_mask)                \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,       \
                  struct name *head, struct rix_hash_bucket_s *buckets,       \
                  const u32 *keys)                                            \
{                                                                             \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, head->rhh_mask,     \
                             head->rhh_mask);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,        \
                 struct name *head, struct rix_hash_bucket_s *buckets)        \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,  \
                       type *base)                                            \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,        \
                 type *base, type **results)                                  \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr type *                                                                   \
name##_find(struct name *head, struct rix_hash_bucket_s *buckets,             \
            type *base, u32 key)                                              \
{                                                                             \
    struct rix_hash_mrsw_u32_find_ctx_s ctx;                                  \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head, struct rix_hash_bucket_s *buckets,  \
                       unsigned bk, unsigned slot)                            \
{                                                                             \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
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
name##_remove_at(struct name *head, struct rix_hash_bucket_s *buckets,        \
                 unsigned bk, unsigned slot)                                  \
{                                                                             \
    return name##_remove_at_inner(head, buckets, bk, slot);                   \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head, struct rix_hash_bucket_s *buckets,             \
            type *base, int (*cb)(type *, void *), void *arg)                 \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_s *bk = buckets + b;                           \
        u32 ctrl = rix_hash_mrsw_bucket_read_begin(bk);                       \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 idx = bk->idx[s];                                             \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            type *node = name##_hptr(base, idx);                              \
            int rc = cb(node, arg);                                           \
            if (rc)                                                           \
                return rc;                                                    \
        }                                                                     \
        if (rix_hash_mrsw_bucket_read_retry(bk, ctrl))                        \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_flipflop(struct rix_hash_bucket_s *buckets, unsigned mask,             \
                unsigned bk_idx, unsigned slot)                               \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 key = bk->hash[slot];                                                 \
    u32 idx = bk->idx[slot];                                                  \
    if (idx == (u32)RIX_NIL)                                                  \
        return -1;                                                            \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
    unsigned b0 = h.val32[0] & mask;                                          \
    unsigned b1 = h.val32[1] & mask;                                          \
    unsigned ab = (bk_idx == b0) ? b1 : b0;                                   \
    int alt_slot = name##_find_empty(buckets, ab);                            \
    if (alt_slot < 0)                                                         \
        return -1;                                                            \
    struct rix_hash_bucket_s *alt = buckets + ab;                             \
    /* Publish-before-unpublish */                                            \
    alt->idx[alt_slot] = idx;                                                 \
    RIX_HASH_MRSW_HOOK(#name, "move_idx", NULL, buckets, ab,                  \
                       (unsigned)alt_slot);                                   \
    alt->hash[alt_slot] = key;                                                \
    rix_hash_mrsw_bucket_valid_set(alt, (unsigned)alt_slot);                  \
    RIX_HASH_MRSW_HOOK(#name, "move_hash", NULL, buckets, ab,                 \
                       (unsigned)alt_slot);                                   \
    RIX_HASH_MRSW_HOOK(#name, "move_before_old_clear", NULL, buckets,         \
                       bk_idx, slot);                                         \
    rix_hash_mrsw_bucket_valid_clear(bk, slot);                               \
    return (int)slot;                                                         \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_kickout(struct rix_hash_bucket_s *buckets, unsigned mask,              \
               unsigned bk_idx, int depth)                                    \
{                                                                             \
    if (depth <= 0)                                                           \
        return -1;                                                            \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        if (name##_flipflop(buckets, mask, bk_idx, s) >= 0)                   \
            return (int)s;                                                    \
    }                                                                         \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u32 key = bk->hash[s];                                                \
        u32 si = bk->idx[s];                                                  \
        if (si == (u32)RIX_NIL)                                               \
            continue;                                                         \
        union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);         \
        unsigned b0 = h.val32[0] & mask;                                      \
        unsigned b1 = h.val32[1] & mask;                                      \
        unsigned ab = (bk_idx == b0) ? b1 : b0;                               \
        if (name##_kickout(buckets, mask, ab, depth - 1) >= 0) {              \
            u32 now_key = bk->hash[s];                                        \
            u32 now_idx = bk->idx[s];                                         \
            u32 now_valid = rix_hash_mrsw_bucket_valid_load(bk,               \
                                                            memory_order_relaxed); \
            if (now_key != key || now_idx != si ||                            \
                (now_valid & (UINT32_C(1) << s)) == 0u) {                     \
                int fs = name##_find_empty(buckets, bk_idx);                  \
                if (fs >= 0)                                                  \
                    return fs;                                                \
                valid = now_valid;                                            \
                continue;                                                     \
            }                                                                 \
            name##_flipflop(buckets, mask, bk_idx, s);                        \
            return (int)s;                                                    \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
attr type *                                                                   \
name##_insert(struct name *head, struct rix_hash_bucket_s *buckets,           \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
    unsigned bk0 = h.val32[0] & mask;                                         \
    unsigned bk1 = h.val32[1] & mask;                                         \
    struct rix_hash_bucket_s *bks[2] = { buckets + bk0, buckets + bk1 };      \
    u32 hits_v[2];                                                            \
    int empty_slot_v[2];                                                      \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bks[i],                   \
                                                    memory_order_acquire);    \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        hits_v[i] = name##_scan_bucket_keys(bks[i], key, valid);              \
        empty_slot_v[i] = empty ? (int)__builtin_ctz(empty) : -1;             \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = hits_v[i];                                                 \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = bks[i]->idx[bit];                                       \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            return name##_hptr(base, idx);                                    \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        int slot = empty_slot_v[i];                                           \
        if (slot >= 0) {                                                      \
            unsigned bki = (i == 0) ? bk0 : bk1;                              \
            struct rix_hash_bucket_s *bk = bks[i];                            \
            bk->idx[slot] = elm_idx;                                          \
            RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,       \
                               (unsigned)slot);                               \
            bk->hash[slot] = key;                                             \
            rix_hash_mrsw_bucket_valid_set(bk, (unsigned)slot);               \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = name##_kickout(buckets, mask, bk0, RIX_HASH_FOLLOW_DEPTH);      \
        if (pos >= 0)                                                         \
            bki = bk0;                                                        \
        else {                                                                \
            pos = name##_kickout(buckets, mask, bk1, RIX_HASH_FOLLOW_DEPTH);  \
            if (pos < 0)                                                      \
                return elm;                                                   \
            bki = bk1;                                                        \
        }                                                                     \
        struct rix_hash_bucket_s *bk = buckets + bki;                         \
        bk->idx[pos] = elm_idx;                                               \
        RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,           \
                           (unsigned)pos);                                    \
        bk->hash[pos] = key;                                                  \
        rix_hash_mrsw_bucket_valid_set(bk, (unsigned)pos);                    \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        return NULL;                                                          \
    }                                                                         \
}                                                                             \
attr type *                                                                   \
name##_remove(struct name *head, struct rix_hash_bucket_s *buckets,           \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned node_idx = name##_hidx(base, elm);                               \
    u32 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
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

/* U32 MRMW insert_slow body macros.  See rix_hash_fp_mr.h for the algorithm
 * description; this is the U32 specialization (no fingerprint - bk->hash[]
 * stores the full u32 key, alt bucket comes from a rehash). */
#  define _RHM_MRMW_U32_INSERT_SLOW_ALLLOCK(name, type, key_field, attr)      \
static RIX_UNUSED type *                                                      \
name##_insert_slow(struct name *head,                                         \
                   struct rix_hash_bucket_s *buckets,                         \
                   type *base, type *elm, u32 key,                            \
                   unsigned bk0, unsigned bk1)                                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned nb_bk = mask + 1u;                                               \
    unsigned nil = nb_bk;                                                     \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    type *ret = elm;                                                          \
    unsigned *mem = head->rhh_kickout_scratch;                                \
    if (mem == NULL)                                                          \
        return ret;                                                           \
    unsigned *queue = mem;                                                    \
    unsigned *parent_bk = mem + nb_bk;                                        \
    unsigned *parent_slot = mem + nb_bk * 2u;                                 \
    RIX_HASH_MRSW_HOOK(#name, "insert_slow_before_lock", head, buckets,       \
                       bk0, bk1);                                             \
    RIX_HASH_MR_KICKOUT_LOCK_MRMW(head);                                      \
    RIX_HASH_MR_BK_LOCK_ALL_MRMW(buckets, nb_bk);                             \
    struct rix_hash_bucket_s *start_bks[2] = { buckets + bk0, buckets + bk1 };\
    for (int i = 0; i < 2; i++) {                                             \
        struct rix_hash_bucket_s *bk = start_bks[i];                          \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_acquire);\
        u32 hits = name##_scan_bucket_keys(bk, key, valid);                   \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = bk->idx[bit];                                           \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            ret = name##_hptr(base, idx);                                     \
            goto out;                                                         \
        }                                                                     \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        if (empty) {                                                          \
            unsigned slot = (unsigned)__builtin_ctz(empty);                   \
            bk->idx[slot] = elm_idx;                                          \
            bk->hash[slot] = key;                                             \
            rix_hash_mrsw_bucket_valid_set(bk, slot);                         \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            ret = NULL;                                                       \
            goto out;                                                         \
        }                                                                     \
    }                                                                         \
    for (unsigned i = 0u; i < nb_bk; i++) {                                   \
        parent_bk[i] = nil;                                                   \
        parent_slot[i] = nil;                                                 \
    }                                                                         \
    unsigned qh = 0u, qt = 0u;                                                \
    parent_bk[bk0] = bk0;                                                     \
    parent_slot[bk0] = nil;                                                   \
    queue[qt++] = bk0;                                                        \
    if (bk1 != bk0) {                                                         \
        parent_bk[bk1] = bk1;                                                 \
        parent_slot[bk1] = nil;                                               \
        queue[qt++] = bk1;                                                    \
    }                                                                         \
    unsigned free_bk = nil, free_slot = nil;                                  \
    while (qh < qt && free_bk == nil) {                                       \
        unsigned cur_bk = queue[qh++];                                        \
        struct rix_hash_bucket_s *bk = buckets + cur_bk;                      \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);\
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        if (empty) {                                                          \
            free_bk = cur_bk;                                                 \
            free_slot = (unsigned)__builtin_ctz(empty);                       \
            break;                                                            \
        }                                                                     \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 move_key = bk->hash[s];                                       \
            union rix_hash_hash_u mh = rix_hash_arch->hash_u32(move_key, mask);\
            unsigned mb0 = mh.val32[0] & mask;                                \
            unsigned mb1 = mh.val32[1] & mask;                                \
            unsigned ab = (cur_bk == mb0) ? mb1 : mb0;                        \
            if (parent_bk[ab] != nil)                                         \
                continue;                                                     \
            parent_bk[ab] = cur_bk;                                           \
            parent_slot[ab] = s;                                              \
            queue[qt++] = ab;                                                 \
        }                                                                     \
    }                                                                         \
    if (free_bk == nil)                                                       \
        goto out;                                                             \
    while (parent_slot[free_bk] != nil) {                                     \
        unsigned src_bk = parent_bk[free_bk];                                 \
        unsigned src_slot = parent_slot[free_bk];                             \
        struct rix_hash_bucket_s *src = buckets + src_bk;                     \
        struct rix_hash_bucket_s *dst = buckets + free_bk;                    \
        u32 move_idx = src->idx[src_slot];                                    \
        u32 move_key = src->hash[src_slot];                                   \
        dst->idx[free_slot] = move_idx;                                       \
        dst->hash[free_slot] = move_key;                                      \
        rix_hash_mrsw_bucket_valid_set(dst, free_slot);                       \
        rix_hash_mrsw_bucket_valid_clear(src, src_slot);                      \
        free_bk = src_bk;                                                     \
        free_slot = src_slot;                                                 \
    }                                                                         \
    buckets[free_bk].idx[free_slot] = elm_idx;                                \
    buckets[free_bk].hash[free_slot] = key;                                   \
    rix_hash_mrsw_bucket_valid_set(&buckets[free_bk], free_slot);             \
    atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    ret = NULL;                                                               \
out:                                                                          \
    RIX_HASH_MR_BK_UNLOCK_ALL_MRMW(buckets, nb_bk);                           \
    RIX_HASH_MR_KICKOUT_UNLOCK_MRMW(head);                                    \
    return ret;                                                               \
}

#  define _RHM_MRMW_U32_INSERT_SLOW_VISITED(name, type, key_field, attr)      \
static RIX_UNUSED type *                                                      \
name##_insert_slow(struct name *head,                                         \
                   struct rix_hash_bucket_s *buckets,                         \
                   type *base, type *elm, u32 key,                            \
                   unsigned bk0, unsigned bk1)                                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned nb_bk = mask + 1u;                                               \
    unsigned nil = nb_bk;                                                     \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    type *ret = elm;                                                          \
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
        type *dup_ret = NULL;                                                 \
        unsigned free_bk = nil, free_slot = nil;                              \
        for (int si = 0; si < (int)qt; si++) {                                \
            unsigned scan_bk = queue[si];                                     \
            u32 sv = rix_hash_mrsw_ctrl_valid(ctrl_snap[scan_bk]);            \
            u32 hits = name##_scan_bucket_keys(buckets + scan_bk, key, sv);   \
            while (hits) {                                                    \
                unsigned bit = (unsigned)__builtin_ctz(hits);                 \
                hits &= hits - 1u;                                            \
                u32 idx = buckets[scan_bk].idx[bit];                          \
                if (idx == (u32)RIX_NIL) continue;                            \
                dup_ret = name##_hptr(base, idx);                             \
                break;                                                        \
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
                struct rix_hash_bucket_s *bk = buckets + cur_bk;              \
                for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ;      \
                     s++) {                                                   \
                    if ((valid & (UINT32_C(1) << s)) == 0u) continue;         \
                    u32 mk = bk->hash[s];                                     \
                    union rix_hash_hash_u mh =                                \
                        rix_hash_arch->hash_u32(mk, mask);                    \
                    unsigned mb0 = mh.val32[0] & mask;                        \
                    unsigned mb1 = mh.val32[1] & mask;                        \
                    unsigned ab = (cur_bk == mb0) ? mb1 : mb0;                \
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
            struct rix_hash_bucket_s *src = buckets + src_bk;                 \
            struct rix_hash_bucket_s *dst = buckets + free_bk;                \
            u32 move_idx = src->idx[src_slot];                                \
            u32 move_key = src->hash[src_slot];                               \
            dst->idx[free_slot] = move_idx;                                   \
            dst->hash[free_slot] = move_key;                                  \
            rix_hash_mrsw_bucket_valid_set(dst, free_slot);                   \
            rix_hash_mrsw_bucket_valid_clear(src, src_slot);                  \
            free_bk = src_bk;                                                 \
            free_slot = src_slot;                                             \
        }                                                                     \
        buckets[free_bk].idx[free_slot] = elm_idx;                            \
        buckets[free_bk].hash[free_slot] = key;                               \
        rix_hash_mrsw_bucket_valid_set(&buckets[free_bk], free_slot);         \
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

#  ifdef RIX_HASH_MRMW_FP_ALLLOCK
#    define _RHM_MRMW_U32_INSERT_SLOW _RHM_MRMW_U32_INSERT_SLOW_ALLLOCK
#  else
#    define _RHM_MRMW_U32_INSERT_SLOW _RHM_MRMW_U32_INSERT_SLOW_VISITED
#  endif

#  define RIX_HASH_MRMW_GENERATE_U32_INTERNAL(name, type, key_field, attr)    \
attr void                                                                     \
name##_init(struct name *head, struct rix_hash_bucket_s *buckets,             \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    RIX_HASH_MR_HEAD_INIT_MRMW(head);                                         \
    rix_hash_mrsw_buckets_init(buckets, nb_bk);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(type *base, const type *p)                                        \
{                                                                             \
    return RIX_IDX_FROM_PTR(base, (type *)(uintptr_t)p);                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_hptr(type *base, unsigned i)                                           \
{                                                                             \
    return (type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_keys(struct rix_hash_bucket_s *bk, u32 key, u32 valid)     \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, key);                           \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_u32_find_ctx_s *ctx,              \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_s *buckets,                     \
                       u32 key,                                               \
                       unsigned hash_mask __attribute__((unused)),             \
                       unsigned bk_mask)                                      \
{                                                                             \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, bk_mask);          \
    ctx->key     = key;                                                       \
    ctx->buckets = buckets;                                                   \
    ctx->bk_mask = bk_mask;                                                   \
    ctx->bk[0]   = buckets + (h.val32[0] & bk_mask);                          \
    ctx->bk[1]   = buckets + (h.val32[1] & bk_mask);                          \
    __builtin_prefetch(ctx->bk[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_u32_find_ctx_s *ctx,                     \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets, u32 key)                   \
{                                                                             \
    name##_hash_key_masked(ctx, head, buckets, key, head->rhh_mask,           \
                           head->rhh_mask);                                   \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_u32_find_ctx_s *ctx,                      \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused)))     \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_bucket_read_begin(ctx->bk[0]);               \
    ctx->hits[0] = name##_scan_bucket_keys(ctx->bk[0], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[0]));               \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->hits[1] = 0u;                                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)    \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 h = ctx->hits[i];                                                 \
        while (h) {                                                           \
            unsigned bit = (unsigned)__builtin_ctz(h);                        \
            h &= h - 1u;                                                      \
            u32 idx = ctx->bk[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                type *node = name##_hptr(base, idx);                          \
                if (node)                                                     \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD type *              \
name##_cmp_key_bk_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base,  \
                       unsigned which)                                        \
{                                                                             \
    u32 hits = ctx->hits[which];                                              \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        return name##_hptr(base, idx);                                        \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk0_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 0u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_u32_find_ctx_s *ctx)                     \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_bucket_read_begin(ctx->bk[1]);               \
    ctx->hits[1] = name##_scan_bucket_keys(ctx->bk[1], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[1]));               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk1_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)     \
{                                                                             \
    type *r = name##_cmp_key_bk0_once(ctx, base);                             \
    if (r != NULL)                                                            \
        return r;                                                             \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk1_once(ctx, base);                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)          \
{                                                                             \
    for (;;) {                                                                \
        type *r = name##_cmp_key_once(ctx, base);                             \
        if (r != NULL)                                                        \
            return r;                                                         \
        int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0],              \
                                                     ctx->ctrl[0]);           \
        int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1],              \
                                                     ctx->ctrl[1]);           \
        if (!retry0 && !retry1)                                               \
            return NULL;                                                      \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_u32_find_ctx_s *ctx,            \
                         unsigned n, struct name *head,                       \
                         struct rix_hash_bucket_s *buckets,                   \
                         const u32 *keys,                                     \
                         unsigned hash_mask, unsigned bk_mask)                \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,       \
                  struct name *head, struct rix_hash_bucket_s *buckets,       \
                  const u32 *keys)                                            \
{                                                                             \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, head->rhh_mask,     \
                             head->rhh_mask);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,        \
                 struct name *head, struct rix_hash_bucket_s *buckets)        \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,  \
                       type *base)                                            \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,        \
                 type *base, type **results)                                  \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr type *                                                                   \
name##_find(struct name *head, struct rix_hash_bucket_s *buckets,             \
            type *base, u32 key)                                              \
{                                                                             \
    struct rix_hash_mrsw_u32_find_ctx_s ctx;                                  \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head, struct rix_hash_bucket_s *buckets,  \
                       unsigned bk, unsigned slot)                            \
{                                                                             \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed);     \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_bucket_valid_clear(b, slot);                                \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head, struct rix_hash_bucket_s *buckets,        \
                 unsigned bk, unsigned slot)                                  \
{                                                                             \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
    RIX_HASH_MR_BK_LOCK_MRMW(b);                                              \
    unsigned ret = name##_remove_at_inner(head, buckets, bk, slot);           \
    RIX_HASH_MR_BK_UNLOCK_MRMW(b);                                            \
    return ret;                                                               \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head, struct rix_hash_bucket_s *buckets,             \
            type *base, int (*cb)(type *, void *), void *arg)                 \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_s *bk = buckets + b;                           \
        u32 ctrl = rix_hash_mrsw_bucket_read_begin(bk);                       \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 idx = bk->idx[s];                                             \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            type *node = name##_hptr(base, idx);                              \
            int rc = cb(node, arg);                                           \
            if (rc)                                                           \
                return rc;                                                    \
        }                                                                     \
        if (rix_hash_mrsw_bucket_read_retry(bk, ctrl))                        \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
_RHM_MRMW_U32_INSERT_SLOW(name, type, key_field, attr)                        \
attr void                                                                     \
name##_attach_kickout_scratch(struct name *head, unsigned *scratch)           \
{                                                                             \
    head->rhh_kickout_scratch = scratch;                                      \
}                                                                             \
attr type *                                                                   \
name##_insert(struct name *head, struct rix_hash_bucket_s *buckets,           \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
    unsigned bk0 = h.val32[0] & mask;                                         \
    unsigned bk1 = h.val32[1] & mask;                                         \
    struct rix_hash_bucket_s *bks[2] = { buckets + bk0, buckets + bk1 };      \
    RIX_HASH_MR_BK_LOCK2_MRMW(bks[0], bks[1]);                                \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bks[i],                   \
                                                    memory_order_acquire);    \
        u32 hits = name##_scan_bucket_keys(bks[i], key, valid);               \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = bks[i]->idx[bit];                                       \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            type *ret = name##_hptr(base, idx);                               \
            RIX_HASH_MR_BK_UNLOCK2_MRMW(bks[0], bks[1]);                      \
            return ret;                                                       \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bks[i],                   \
                                                    memory_order_relaxed);    \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        if (empty) {                                                          \
            unsigned slot = (unsigned)__builtin_ctz(empty);                   \
            bks[i]->idx[slot] = elm_idx;                                      \
            bks[i]->hash[slot] = key;                                         \
            rix_hash_mrsw_bucket_valid_set(bks[i], slot);                    \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            RIX_HASH_MR_BK_UNLOCK2_MRMW(bks[0], bks[1]);                      \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    RIX_HASH_MR_BK_UNLOCK2_MRMW(bks[0], bks[1]);                              \
    return name##_insert_slow(head, buckets, base, elm, key, bk0, bk1);       \
}                                                                             \
attr type *                                                                   \
name##_remove(struct name *head, struct rix_hash_bucket_s *buckets,           \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned node_idx = name##_hidx(base, elm);                               \
    u32 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
    unsigned bk0 = h.val32[0] & mask;                                         \
    unsigned bk1 = h.val32[1] & mask;                                         \
    struct rix_hash_bucket_s *b0 = buckets + bk0;                             \
    struct rix_hash_bucket_s *b1 = buckets + bk1;                             \
    RIX_HASH_MR_BK_LOCK2_MRMW(b0, b1);                                        \
    struct rix_hash_bucket_s *bks[2] = { b0, b1 };                            \
    unsigned bki[2] = { bk0, bk1 };                                           \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bks[i],                   \
                                                    memory_order_relaxed);    \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            if (bks[i]->idx[s] == (u32)node_idx &&                            \
                name##_remove_at_inner(head, buckets, bki[i], s) !=           \
                    (unsigned)RIX_NIL) {                                      \
                RIX_HASH_MR_BK_UNLOCK2_MRMW(b0, b1);                          \
                return elm;                                                   \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    RIX_HASH_MR_BK_UNLOCK2_MRMW(b0, b1);                                      \
    return NULL;                                                              \
}


#endif /* _RIX_HASH_U32_MR_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
