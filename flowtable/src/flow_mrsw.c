/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash.h>

#include "flowtable/flow_mrsw_table.h"
#include "flow_mrsw_dispatch.h"
#include "flow_hash.h"

#define FT_MRSW_CAT2(a, b) a##b
#define FT_MRSW_CAT(a, b)  FT_MRSW_CAT2(a, b)

#ifdef FT_ARCH_SUFFIX
#define FT_MRSW_API(name) FT_MRSW_CAT(name, FT_ARCH_SUFFIX)
#else
#define FT_MRSW_API(name) name
#endif

#ifndef FT_MRSW_ARCH_ENABLE
#define FT_MRSW_ARCH_ENABLE 0u
#endif

#define FT_MRSW_FUNC(prefix, name)                                            \
    FT_MRSW_API(FT_MRSW_CAT(FT_MRSW_CAT(ft_, prefix), _mrsw_table_##name))
#define FT_MRSW_OPS(prefix)                                                   \
    FT_MRSW_API(FT_MRSW_CAT(FT_MRSW_CAT(ft_, prefix), _mrsw_ops))

#define ft_flow4_mrsw_hash_fn flow4_key_hash
#define ft_flow6_mrsw_hash_fn flow6_key_hash
#define ft_flowu_mrsw_hash_fn flowu_key_hash
#define ft_flow4_mrsw_cmp     flow4_key_cmp
#define ft_flow6_mrsw_cmp     flow6_key_cmp
#define ft_flowu_mrsw_cmp     flowu_key_cmp

static inline void
ft_mrsw_stat_add_u64_(_Atomic u64 *p, u64 v)
{
    atomic_fetch_add_explicit(p, v, memory_order_relaxed);
}

static inline void
ft_mrsw_stat_add_u32_(_Atomic u32 *p, u32 v)
{
    atomic_fetch_add_explicit(p, v, memory_order_relaxed);
}

static RIX_UNUSED inline u64
ft_mrsw_load_u64_(const _Atomic u64 *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

static RIX_UNUSED inline u32
ft_mrsw_load_u32_(const _Atomic u32 *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

static RIX_UNUSED void
ft_mrsw_stats_clear_(struct ft_mrsw_table_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
}

static RIX_UNUSED void
ft_mrsw_status_clear_(struct ft_mrsw_flow_status *status)
{
    memset(status, 0, sizeof(*status));
}

static inline void *
ft_mrsw_record_member_ptr_(const struct ft_mrsw_table *ft,
                           unsigned idx)
{
    if (idx == RIX_NIL || idx == 0u || idx > ft->max_entries)
        return NULL;
    return FT_BYTE_PTR_ADD(ft->pool_base,
                           RIX_IDX_TO_OFF0(idx) * ft->pool_stride +
                           ft->pool_entry_offset);
}

static inline unsigned
ft_mrsw_record_member_idx_(const struct ft_mrsw_table *ft,
                           const void *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

#define FT_MRSW_ENTRY_PTR(ft, idx, entry_t)                                   \
    ((entry_t *)__builtin_assume_aligned(                                     \
        ft_mrsw_record_member_ptr_((ft), (idx)), _Alignof(entry_t)))

#define FT_MRSW_DEFINE_HASH(prefix, entry_t, cmp_fn, hash_fn)                 \
RIX_HASH_MRSW_HEAD(ft_##prefix##_mrsw_ht);                                    \
static inline entry_t *                                                       \
ft_##prefix##_mrsw_entry_ptr_(const struct ft_mrsw_table *ft, unsigned idx)   \
{                                                                             \
    return FT_MRSW_ENTRY_PTR(ft, idx, entry_t);                               \
}                                                                             \
static inline unsigned                                                        \
ft_##prefix##_mrsw_entry_idx_(const struct ft_mrsw_table *ft,                 \
                              const entry_t *entry)                           \
{                                                                             \
    return ft_mrsw_record_member_idx_(ft, entry);                             \
}                                                                             \
RIX_STATIC_ASSERT(sizeof(struct ft_##prefix##_mrsw_ht) ==                     \
                  sizeof(struct ft_mrsw_table_ht),                            \
                  "MRSW flow hash heads must stay layout-compatible");        \
RIX_STATIC_ASSERT(_Alignof(struct ft_##prefix##_mrsw_ht) ==                   \
                  _Alignof(struct ft_mrsw_table_ht),                          \
                  "MRSW flow hash heads must keep compatible alignment");     \
_Pragma("GCC diagnostic push")                                                \
_Pragma("GCC diagnostic ignored \"-Wstrict-aliasing\"")                      \
static inline struct ft_##prefix##_mrsw_ht *                                  \
ft_##prefix##_mrsw_head_(struct ft_mrsw_table *ft)                            \
{                                                                             \
    return (struct ft_##prefix##_mrsw_ht *)(void *)&ft->ht_head;              \
}                                                                             \
_Pragma("GCC diagnostic pop")                                                 \
static inline struct ft_##prefix##_mrsw_ht *                                  \
ft_##prefix##_mrsw_head_for_hash_(struct ft_mrsw_table *ft)                   \
{                                                                             \
    return ft_##prefix##_mrsw_head_(ft);                                      \
}

FT_MRSW_DEFINE_HASH(flow4, struct flow4_mrsw_entry,
                    ft_flow4_mrsw_cmp, ft_flow4_mrsw_hash_fn)

#undef RIX_HASH_MRSW_DEFINE_INDEXERS
#define RIX_HASH_MRSW_DEFINE_INDEXERS(name, type)                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_mrsw_table *ft =                                          \
        (const struct ft_mrsw_table *)(const void *)base;                     \
    return ft_flow4_mrsw_entry_idx_(ft, p);                                   \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_mrsw_table *ft =                                          \
        (const struct ft_mrsw_table *)(const void *)base;                     \
    return ft_flow4_mrsw_entry_ptr_(ft, i);                                   \
}
RIX_HASH_MRSW_GENERATE_STATIC_SLOT_EX(ft_flow4_mrsw_ht, flow4_mrsw_entry,
                                       key, meta.cur_hash, meta.slot,
                                       ft_flow4_mrsw_cmp,
                                       ft_flow4_mrsw_hash_fn)

FT_MRSW_DEFINE_HASH(flow6, struct flow6_mrsw_entry,
                    ft_flow6_mrsw_cmp, ft_flow6_mrsw_hash_fn)

#undef RIX_HASH_MRSW_DEFINE_INDEXERS
#define RIX_HASH_MRSW_DEFINE_INDEXERS(name, type)                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_mrsw_table *ft =                                          \
        (const struct ft_mrsw_table *)(const void *)base;                     \
    return ft_flow6_mrsw_entry_idx_(ft, p);                                   \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_mrsw_table *ft =                                          \
        (const struct ft_mrsw_table *)(const void *)base;                     \
    return ft_flow6_mrsw_entry_ptr_(ft, i);                                   \
}
RIX_HASH_MRSW_GENERATE_STATIC_SLOT_EX(ft_flow6_mrsw_ht, flow6_mrsw_entry,
                                       key, meta.cur_hash, meta.slot,
                                       ft_flow6_mrsw_cmp,
                                       ft_flow6_mrsw_hash_fn)

FT_MRSW_DEFINE_HASH(flowu, struct flowu_mrsw_entry,
                    ft_flowu_mrsw_cmp, ft_flowu_mrsw_hash_fn)

#undef RIX_HASH_MRSW_DEFINE_INDEXERS
#define RIX_HASH_MRSW_DEFINE_INDEXERS(name, type)                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_mrsw_table *ft =                                          \
        (const struct ft_mrsw_table *)(const void *)base;                     \
    return ft_flowu_mrsw_entry_idx_(ft, p);                                   \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_mrsw_table *ft =                                          \
        (const struct ft_mrsw_table *)(const void *)base;                     \
    return ft_flowu_mrsw_entry_ptr_(ft, i);                                   \
}
RIX_HASH_MRSW_GENERATE_STATIC_SLOT_EX(ft_flowu_mrsw_ht, flowu_mrsw_entry,
                                       key, meta.cur_hash, meta.slot,
                                       ft_flowu_mrsw_cmp,
                                       ft_flowu_mrsw_hash_fn)
#undef RIX_HASH_MRSW_DEFINE_INDEXERS

#define FT_MRSW_ENTRY_OPS(prefix, entry_t, key_t)                             \
struct ft_##prefix##_mrsw_walk_ctx_ {                                         \
    struct ft_mrsw_table *ft;                                                 \
    int (*cb)(u32, void *);                                                   \
    void *arg;                                                                \
};                                                                            \
static inline entry_t *                                                       \
ft_##prefix##_mrsw_entry_from_idx_(const struct ft_mrsw_table *ft,            \
                                   u32 entry_idx)                             \
{                                                                             \
    return ft_##prefix##_mrsw_entry_ptr_(ft, entry_idx);                      \
}                                                                             \
static inline u32                                                             \
ft_##prefix##_mrsw_idx_from_entry_(const struct ft_mrsw_table *ft,            \
                                   const entry_t *entry)                      \
{                                                                             \
    return (u32)ft_##prefix##_mrsw_entry_idx_(ft, entry);                     \
}                                                                             \
static entry_t *                                                              \
ft_##prefix##_mrsw_find_entry_(struct ft_mrsw_table *ft,                      \
                               const key_t *key)                              \
{                                                                             \
    return ft_##prefix##_mrsw_ht_find(ft_##prefix##_mrsw_head_for_hash_(ft),  \
                                      ft->buckets,                            \
                                      (entry_t *)(void *)ft, key);            \
}                                                                             \
static u32                                                                    \
ft_##prefix##_mrsw_find_idx_(struct ft_mrsw_table *ft,                        \
                             const key_t *key, u64 now)                       \
{                                                                             \
    entry_t *entry;                                                           \
    u32 idx;                                                                  \
    (void)now;                                                                \
    if (ft == NULL || ft->buckets == NULL || key == NULL)                     \
        return RIX_NIL;                                                       \
    entry = ft_##prefix##_mrsw_find_entry_(ft, key);                          \
    if (entry == NULL)                                                        \
        return RIX_NIL;                                                       \
    idx = ft_##prefix##_mrsw_idx_from_entry_(ft, entry);                      \
    return idx;                                                               \
}                                                                             \
static void                                                                   \
ft_##prefix##_mrsw_find_bulk_(struct ft_mrsw_table *ft,                       \
                              const key_t *keys, unsigned nb_keys,            \
                              u64 now, struct ft_table_result *results)       \
{                                                                             \
    enum { FT_MRSW_FIND_BULK_BATCH = 64u };                                   \
    struct ft_##prefix##_mrsw_ht *head;                                       \
    entry_t *base;                                                            \
    (void)now;                                                                \
    if (results == NULL)                                                      \
        return;                                                               \
    if (ft == NULL || ft->buckets == NULL || keys == NULL) {                  \
        for (unsigned i = 0u; i < nb_keys; i++)                               \
            results[i].entry_idx = RIX_NIL;                                   \
        return;                                                               \
    }                                                                         \
    head = ft_##prefix##_mrsw_head_for_hash_(ft);                             \
    base = (entry_t *)(void *)ft;                                             \
    for (unsigned off = 0u; off < nb_keys; off += FT_MRSW_FIND_BULK_BATCH) {  \
        unsigned n = nb_keys - off;                                           \
        struct rix_hash_mrsw_find_ctx_s ctx[FT_MRSW_FIND_BULK_BATCH];         \
        struct rix_hash_mrsw_find_ctx_s *miss_ctx[FT_MRSW_FIND_BULK_BATCH];   \
        unsigned miss_pos[FT_MRSW_FIND_BULK_BATCH];                           \
        const key_t *keyv[FT_MRSW_FIND_BULK_BATCH];                           \
        entry_t *entries[FT_MRSW_FIND_BULK_BATCH];                            \
        unsigned nb_miss;                                                     \
        if (n > FT_MRSW_FIND_BULK_BATCH)                                      \
            n = FT_MRSW_FIND_BULK_BATCH;                                      \
        for (unsigned i = 0u; i < n; i++) {                                   \
            keyv[i] = &keys[off + i];                                         \
            rix_hash_prefetch_key(keyv[i]);                                   \
            entries[i] = NULL;                                                \
        }                                                                     \
        ft_##prefix##_mrsw_ht_hash_key_n(ctx, n, head, ft->buckets, keyv);    \
        ft_##prefix##_mrsw_ht_scan_bk_n(ctx, n, head, ft->buckets);           \
        ft_##prefix##_mrsw_ht_prefetch_node_n(ctx, n, base);                  \
        nb_miss = 0u;                                                         \
        for (unsigned i = 0u; i < n; i++) {                                   \
            entries[i] = ft_##prefix##_mrsw_ht_cmp_key_bk0_once(&ctx[i],      \
                                                                base);        \
            if (entries[i] == NULL) {                                         \
                miss_ctx[nb_miss] = &ctx[i];                                  \
                miss_pos[nb_miss] = i;                                        \
                nb_miss++;                                                    \
            }                                                                 \
        }                                                                     \
        for (unsigned i = 0u; i < nb_miss; i++)                               \
            ft_##prefix##_mrsw_ht_scan_bk1(miss_ctx[i]);                      \
        for (unsigned i = 0u; i < nb_miss; i++)                               \
            ft_##prefix##_mrsw_ht_prefetch_node(miss_ctx[i], base);           \
        for (unsigned i = 0u; i < nb_miss; i++) {                             \
            unsigned pos = miss_pos[i];                                       \
            entries[pos] = ft_##prefix##_mrsw_ht_cmp_key_bk1_once(miss_ctx[i],\
                                                                  base);      \
        }                                                                     \
        for (unsigned i = 0u; i < nb_miss; i++) {                             \
            unsigned pos = miss_pos[i];                                       \
            while (entries[pos] == NULL &&                                    \
                   ft_##prefix##_mrsw_ht_miss_retry(miss_ctx[i])) {           \
                ft_##prefix##_mrsw_ht_scan_bk(miss_ctx[i], head, ft->buckets);\
                ft_##prefix##_mrsw_ht_prefetch_node(miss_ctx[i], base);       \
                entries[pos] = ft_##prefix##_mrsw_ht_cmp_key(miss_ctx[i],     \
                                                             base);           \
            }                                                                 \
        }                                                                     \
        for (unsigned i = 0u; i < n; i++)                                     \
            results[off + i].entry_idx = entries[i] != NULL                   \
                ? ft_##prefix##_mrsw_idx_from_entry_(ft, entries[i])          \
                : RIX_NIL;                                                    \
    }                                                                         \
}                                                                             \
static u32                                                                    \
ft_##prefix##_mrsw_insert_dup_hashed_idx_(struct ft_mrsw_table *ft,           \
                                          entry_t *entry,                     \
                                          union rix_hash_hash_u h,            \
                                          unsigned *bucket_order_out,         \
                                          int *kickout_out)                   \
{                                                                             \
    struct ft_##prefix##_mrsw_ht *head = ft_##prefix##_mrsw_head_(ft);        \
    unsigned mask = head->rhh_mask;                                           \
    unsigned bk0, bk1;                                                        \
    u32 entry_idx = ft_##prefix##_mrsw_idx_from_entry_(ft, entry);            \
    u32 fp = rix_hash_fp(h, mask, &bk0, &bk1);                                \
    entry->meta.cur_hash = h.val32[0];                                        \
    if (bucket_order_out != NULL)                                             \
        *bucket_order_out = 0u;                                               \
    if (kickout_out != NULL)                                                  \
        *kickout_out = 0;                                                     \
    for (unsigned i = 0u; i < 2u; i++) {                                      \
        unsigned bki = (i == 0u) ? bk0 : bk1;                                 \
        struct rix_hash_mrsw_bucket_s *bk = ft->buckets + bki;               \
        int slot = ft_##prefix##_mrsw_ht_find_empty(ft->buckets, bki);        \
        if (slot < 0)                                                         \
            continue;                                                         \
        if (i == 1u)                                                          \
            entry->meta.cur_hash = h.val32[1];                                \
        entry->meta.slot = (u16)(unsigned)slot;                               \
        bk->idx[slot] = entry_idx;                                            \
        bk->hash[slot] = fp;                                                  \
        rix_hash_mrsw_bucket_valid_set(bk, (unsigned)slot);                   \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        if (bucket_order_out != NULL)                                         \
            *bucket_order_out = i;                                            \
        return 0u;                                                            \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = ft_##prefix##_mrsw_ht_kickout(ft->buckets,                      \
                                            (entry_t *)(void *)ft,            \
                                            mask, bk0,                        \
                                            RIX_HASH_FOLLOW_DEPTH);           \
        if (pos >= 0) {                                                       \
            bki = bk0;                                                        \
        } else {                                                              \
            pos = ft_##prefix##_mrsw_ht_kickout(ft->buckets,                  \
                                                (entry_t *)(void *)ft,        \
                                                mask, bk1,                    \
                                                RIX_HASH_FOLLOW_DEPTH);       \
            if (pos < 0)                                                      \
                return entry_idx;                                             \
            bki = bk1;                                                        \
            entry->meta.cur_hash = h.val32[1];                                \
            if (bucket_order_out != NULL)                                     \
                *bucket_order_out = 1u;                                       \
        }                                                                     \
        entry->meta.slot = (u16)(unsigned)pos;                                \
        ft->buckets[bki].idx[pos] = entry_idx;                                \
        ft->buckets[bki].hash[pos] = fp;                                      \
        rix_hash_mrsw_bucket_valid_set(&ft->buckets[bki], (unsigned)pos);     \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        if (kickout_out != NULL)                                              \
            *kickout_out = 1;                                                 \
        return 0u;                                                            \
    }                                                                         \
}                                                                             \
static u32                                                                    \
ft_##prefix##_mrsw_add_idx_(struct ft_mrsw_table *ft, u32 entry_idx,          \
                            enum ft_add_policy policy, u64 now,               \
                            u32 *unused_idx_out)                              \
{                                                                             \
    entry_t *entry;                                                           \
    struct ft_##prefix##_mrsw_ht *head;                                       \
    u32 ret_idx;                                                              \
    union rix_hash_hash_u h;                                                  \
    unsigned bucket_order = 0u;                                               \
    int used_kickout = 0;                                                     \
    (void)now;                                                                \
    if (unused_idx_out != NULL)                                               \
        *unused_idx_out = RIX_NIL;                                            \
    if (ft == NULL || ft->buckets == NULL ||                                  \
        !RIX_IDX_IS_VALID(entry_idx, ft->max_entries))                        \
        return RIX_NIL;                                                       \
    entry = ft_##prefix##_mrsw_entry_from_idx_(ft, entry_idx);                \
    if (entry == NULL)                                                        \
        return RIX_NIL;                                                       \
    head = ft_##prefix##_mrsw_head_(ft);                                      \
    h = ft_##prefix##_mrsw_hash_fn(&entry->key, head->rhh_mask);              \
    ret_idx = ft_##prefix##_mrsw_ht_insert_hashed_idx(                        \
        head, ft->buckets, (entry_t *)(void *)ft, entry, h);                  \
    if (ret_idx == 0u) {                                                      \
        ft_mrsw_stat_add_u64_(&ft->stats.core.adds, 1u);                      \
        if (entry->meta.cur_hash == h.val32[1])                               \
            ft_mrsw_stat_add_u32_(&ft->status.add_bk1, 1u);                   \
        else                                                                  \
            ft_mrsw_stat_add_u32_(&ft->status.add_bk0, 1u);                   \
        return entry_idx;                                                     \
    }                                                                         \
    if (ret_idx != entry_idx) {                                               \
        ft_mrsw_stat_add_u64_(&ft->stats.core.add_existing, 1u);              \
        if (((unsigned)policy & 1u) != 0u) {                                  \
            u32 ins_ret = ft_##prefix##_mrsw_insert_dup_hashed_idx_(          \
                ft, entry, h, &bucket_order, &used_kickout);                  \
            if (ins_ret == 0u) {                                              \
                entry_t *old_entry = ft_##prefix##_mrsw_entry_from_idx_(      \
                    ft, ret_idx);                                             \
                if (old_entry != NULL &&                                      \
                    ft_##prefix##_mrsw_ht_remove(ft_##prefix##_mrsw_head_(ft), \
                                                 ft->buckets,                 \
                                                 (entry_t *)(void *)ft,       \
                                                 old_entry) != NULL) {        \
                    ft_mrsw_stat_add_u64_(&ft->stats.core.dels, 1u);          \
                }                                                             \
                if (unused_idx_out != NULL)                                   \
                    *unused_idx_out = ret_idx;                                \
                if (bucket_order == 1u)                                       \
                    ft_mrsw_stat_add_u32_(&ft->status.add_bk1, 1u);           \
                else                                                          \
                    ft_mrsw_stat_add_u32_(&ft->status.add_bk0, 1u);           \
                if (used_kickout)                                             \
                    ft_mrsw_stat_add_u32_(&ft->status.kickouts, 1u);          \
                return entry_idx;                                             \
            }                                                                 \
        }                                                                     \
        if (unused_idx_out != NULL)                                           \
            *unused_idx_out = entry_idx;                                      \
        return ret_idx;                                                       \
    }                                                                         \
    ft_mrsw_stat_add_u64_(&ft->stats.core.add_failed, 1u);                    \
    if (unused_idx_out != NULL)                                               \
        *unused_idx_out = entry_idx;                                          \
    return RIX_NIL;                                                           \
}                                                                             \
static u32                                                                    \
ft_##prefix##_mrsw_del_idx_(struct ft_mrsw_table *ft, u32 entry_idx)          \
{                                                                             \
    entry_t *entry;                                                           \
    entry_t *ret;                                                             \
    if (ft == NULL || ft->buckets == NULL ||                                  \
        !RIX_IDX_IS_VALID(entry_idx, ft->max_entries)) {                      \
        if (ft != NULL)                                                       \
            ft_mrsw_stat_add_u64_(&ft->stats.core.del_miss, 1u);              \
        return RIX_NIL;                                                       \
    }                                                                         \
    entry = ft_##prefix##_mrsw_entry_from_idx_(ft, entry_idx);                \
    if (entry == NULL) {                                                      \
        ft_mrsw_stat_add_u64_(&ft->stats.core.del_miss, 1u);                  \
        return RIX_NIL;                                                       \
    }                                                                         \
    ret = ft_##prefix##_mrsw_ht_remove(ft_##prefix##_mrsw_head_(ft),          \
                                       ft->buckets,                           \
                                       (entry_t *)(void *)ft, entry);         \
    if (ret == NULL) {                                                        \
        ft_mrsw_stat_add_u64_(&ft->stats.core.del_miss, 1u);                  \
        return RIX_NIL;                                                       \
    }                                                                         \
    ft_mrsw_stat_add_u64_(&ft->stats.core.dels, 1u);                          \
    return entry_idx;                                                         \
}                                                                             \
static unsigned                                                               \
ft_##prefix##_mrsw_del_key_bulk_(struct ft_mrsw_table *ft,                   \
                                 const key_t *keys, unsigned nb_keys,         \
                                 u32 *unused_idxv)                            \
{                                                                             \
    unsigned count = 0u;                                                      \
    if (ft == NULL || ft->buckets == NULL || keys == NULL ||                  \
        unused_idxv == NULL)                                                  \
        return 0u;                                                            \
    for (unsigned i = 0u; i < nb_keys; i++) {                                 \
        entry_t *entry = ft_##prefix##_mrsw_find_entry_(ft, &keys[i]);        \
        if (entry == NULL) {                                                  \
            ft_mrsw_stat_add_u64_(&ft->stats.core.del_miss, 1u);              \
            continue;                                                         \
        }                                                                     \
        unused_idxv[count++] = ft_##prefix##_mrsw_idx_from_entry_(ft, entry); \
        (void)ft_##prefix##_mrsw_del_idx_(ft, unused_idxv[count - 1u]);       \
    }                                                                         \
    return count;                                                             \
}                                                                             \
static int                                                                    \
ft_##prefix##_mrsw_walk_cb_(entry_t *entry, void *arg)                        \
{                                                                             \
    struct ft_##prefix##_mrsw_walk_ctx_ *ctx =                                \
        (struct ft_##prefix##_mrsw_walk_ctx_ *)arg;                           \
    return ctx->cb(ft_##prefix##_mrsw_idx_from_entry_(ctx->ft, entry),        \
                   ctx->arg);                                                 \
}                                                                             \
static int                                                                    \
ft_##prefix##_mrsw_walk_(struct ft_mrsw_table *ft,                            \
                         int (*cb)(u32 entry_idx, void *arg), void *arg)      \
{                                                                             \
    struct ft_##prefix##_mrsw_walk_ctx_ w = { ft, cb, arg };                  \
    if (ft == NULL || ft->buckets == NULL || cb == NULL)                      \
        return -1;                                                            \
    return ft_##prefix##_mrsw_ht_walk(ft_##prefix##_mrsw_head_(ft),           \
                                      ft->buckets,                            \
                                      (entry_t *)(void *)ft,                  \
                                      ft_##prefix##_mrsw_walk_cb_, &w);       \
}                                                                             \
static int                                                                    \
ft_##prefix##_mrsw_migrate_(struct ft_mrsw_table *ft,                         \
                            void *new_buckets_raw, size_t new_bucket_size)    \
{                                                                             \
    struct rix_hash_mrsw_bucket_s *new_buckets;                               \
    struct ft_##prefix##_mrsw_ht new_head;                                    \
    unsigned new_nb_bk;                                                       \
    if (ft == NULL || ft->buckets == NULL || new_buckets_raw == NULL ||       \
        new_bucket_size == 0u)                                                \
        return -1;                                                            \
    new_buckets = ft_mrsw_table_bucket_carve(new_buckets_raw,                 \
                                             new_bucket_size, &new_nb_bk);    \
    if (new_nb_bk < FT_TABLE_MIN_NB_BK)                                       \
        return -1;                                                            \
    ft_##prefix##_mrsw_ht_init(&new_head, new_buckets, new_nb_bk);            \
    for (unsigned b = 0u; b < ft->nb_bk; b++) {                               \
        struct rix_hash_mrsw_bucket_s *bk = &ft->buckets[b];                  \
        u32 ctrl = rix_hash_mrsw_bucket_read_begin(bk);                       \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            u32 idx;                                                          \
            entry_t *entry;                                                   \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            idx = bk->idx[s];                                                 \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            entry = ft_##prefix##_mrsw_entry_from_idx_(ft, idx);              \
            if (entry == NULL ||                                              \
                ft_##prefix##_mrsw_ht_insert(&new_head, new_buckets,          \
                                             (entry_t *)(void *)ft, entry)    \
                    != NULL) {                                                \
                ft_mrsw_stat_add_u64_(&ft->stats.grow_failures, 1u);          \
                return -1;                                                    \
            }                                                                 \
        }                                                                     \
        if (rix_hash_mrsw_bucket_read_retry(bk, ctrl)) {                      \
            ft_mrsw_stat_add_u64_(&ft->stats.grow_failures, 1u);              \
            return -1;                                                        \
        }                                                                     \
    }                                                                         \
    ft->buckets = new_buckets;                                                \
    memcpy(&ft->ht_head, &new_head, sizeof(ft->ht_head));                     \
    ft->nb_bk = new_nb_bk;                                                    \
    ft->start_mask = new_nb_bk - 1u;                                          \
    ft_mrsw_stat_add_u64_(&ft->stats.grow_execs, 1u);                         \
    return 0;                                                                 \
}

FT_MRSW_ENTRY_OPS(flow4, struct flow4_mrsw_entry, struct flow4_key)
FT_MRSW_ENTRY_OPS(flow6, struct flow6_mrsw_entry, struct flow6_key)
FT_MRSW_ENTRY_OPS(flowu, struct flowu_mrsw_entry, struct flowu_key)

void
FT_MRSW_API(ft_mrsw_arch_variant_init)(void)
{
    rix_hash_arch_init(FT_MRSW_ARCH_ENABLE);
}

#define FT_MRSW_ARCH_API(prefix, key_t)                                       \
u32                                                                           \
FT_MRSW_FUNC(prefix, find)(struct ft_mrsw_table *ft,                          \
                           const key_t *key, u64 now)                         \
{                                                                             \
    return ft_##prefix##_mrsw_find_idx_(ft, key, now);                        \
}                                                                             \
void                                                                          \
FT_MRSW_FUNC(prefix, find_bulk)(struct ft_mrsw_table *ft,                     \
                                const key_t *keys, unsigned nb_keys,          \
                                u64 now, struct ft_table_result *results)     \
{                                                                             \
    ft_##prefix##_mrsw_find_bulk_(ft, keys, nb_keys, now, results);           \
}                                                                             \
unsigned                                                                      \
FT_MRSW_FUNC(prefix, add_idx_bulk)(struct ft_mrsw_table *ft,                  \
                                   u32 *entry_idxv, unsigned nb_keys,         \
                                   enum ft_add_policy policy, u64 now,        \
                                   u32 *unused_idxv)                          \
{                                                                             \
    unsigned unused_count = 0u;                                                \
    if (ft == NULL || entry_idxv == NULL || unused_idxv == NULL)              \
        return 0u;                                                            \
    for (unsigned i = 0u; i < nb_keys; i++) {                                 \
        u32 unused = RIX_NIL;                                                 \
        u32 ret = ft_##prefix##_mrsw_add_idx_(ft, entry_idxv[i], policy,      \
                                              now, &unused);                  \
        entry_idxv[i] = ret;                                                  \
        if (unused != RIX_NIL)                                                \
            unused_idxv[unused_count++] = unused;                             \
    }                                                                         \
    return unused_count;                                                      \
}                                                                             \
unsigned                                                                      \
FT_MRSW_FUNC(prefix, del_key_bulk)(struct ft_mrsw_table *ft,                  \
                                   const key_t *keys, unsigned nb_keys,       \
                                   u32 *unused_idxv)                          \
{                                                                             \
    return ft_##prefix##_mrsw_del_key_bulk_(ft, keys, nb_keys, unused_idxv);  \
}                                                                             \
unsigned                                                                      \
FT_MRSW_FUNC(prefix, del_idx_bulk)(struct ft_mrsw_table *ft,                  \
                                   const u32 *entry_idxv, unsigned nb_keys,   \
                                   u32 *unused_idxv)                          \
{                                                                             \
    unsigned count = 0u;                                                       \
    if (ft == NULL || entry_idxv == NULL || unused_idxv == NULL)              \
        return 0u;                                                            \
    for (unsigned i = 0u; i < nb_keys; i++) {                                 \
        u32 ret = ft_##prefix##_mrsw_del_idx_(ft, entry_idxv[i]);             \
        if (ret != RIX_NIL)                                                   \
            unused_idxv[count++] = ret;                                       \
    }                                                                         \
    return count;                                                             \
}                                                                             \
int                                                                           \
FT_MRSW_FUNC(prefix, walk)(struct ft_mrsw_table *ft,                          \
                           int (*cb)(u32 entry_idx, void *arg), void *arg)    \
{                                                                             \
    return ft_##prefix##_mrsw_walk_(ft, cb, arg);                             \
}                                                                             \
int                                                                           \
FT_MRSW_FUNC(prefix, migrate)(struct ft_mrsw_table *ft,                       \
                              void *new_buckets, size_t new_bucket_size)      \
{                                                                             \
    return ft_##prefix##_mrsw_migrate_(ft, new_buckets, new_bucket_size);     \
}                                                                             \
const struct ft_##prefix##_mrsw_ops FT_MRSW_OPS(prefix) = {                   \
    .find         = FT_MRSW_FUNC(prefix, find),                               \
    .find_bulk    = FT_MRSW_FUNC(prefix, find_bulk),                          \
    .add_idx_bulk = FT_MRSW_FUNC(prefix, add_idx_bulk),                       \
    .del_key_bulk = FT_MRSW_FUNC(prefix, del_key_bulk),                       \
    .del_idx_bulk = FT_MRSW_FUNC(prefix, del_idx_bulk),                       \
    .walk         = FT_MRSW_FUNC(prefix, walk),                               \
    .migrate      = FT_MRSW_FUNC(prefix, migrate),                            \
}

FT_MRSW_ARCH_API(flow4, struct flow4_key);
FT_MRSW_ARCH_API(flow6, struct flow6_key);
FT_MRSW_ARCH_API(flowu, struct flowu_key);

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
