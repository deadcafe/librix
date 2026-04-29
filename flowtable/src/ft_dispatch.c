/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_arch.h>

#include "flow_dispatch.h"

/*===========================================================================
 * Maintain arch-variant declarations
 *===========================================================================*/
typedef unsigned (*ft_maintain_fn_t)(const struct ft_maint_ctx *,
                                     unsigned, u64, u64,
                                     u32 *, unsigned, unsigned,
                                     unsigned *);
typedef unsigned (*ft_maintain_idx_bulk_fn_t)(const struct ft_maint_ctx *,
                                              const u32 *, unsigned,
                                              u64, u64,
                                              u32 *, unsigned, unsigned,
                                              int);

#define FT_MAINT_DECLARE(suffix)                                               \
    extern unsigned ft_table_maintain##suffix(                                  \
        const struct ft_maint_ctx *, unsigned, u64, u64,                       \
        u32 *, unsigned, unsigned, unsigned *);                                \
    extern unsigned ft_table_maintain_idx_bulk##suffix(                         \
        const struct ft_maint_ctx *, const u32 *, unsigned,                    \
        u64, u64, u32 *, unsigned, unsigned, int)

FT_MAINT_DECLARE(_gen);
FT_MAINT_DECLARE(_sse);
FT_MAINT_DECLARE(_avx2);
FT_MAINT_DECLARE(_avx512);

/*===========================================================================
 * Active ops pointers (default to _gen)
 *===========================================================================*/
static const struct ft_flow4_ops *ft_flow4_active = &ft_flow4_ops_gen;
static const struct ft_flow6_ops *ft_flow6_active = &ft_flow6_ops_gen;
static const struct ft_flowu_ops *ft_flowu_active = &ft_flowu_ops_gen;

static ft_maintain_fn_t ft_maintain_active = ft_table_maintain_gen;
static ft_maintain_idx_bulk_fn_t ft_maintain_idx_bulk_active =
    ft_table_maintain_idx_bulk_gen;

/*===========================================================================
 * Arch init
 *===========================================================================*/
static unsigned
ft_rix_hash_arch_enable_(unsigned arch_enable)
{
    unsigned enable = 0u;

    if (arch_enable == FT_ARCH_GEN)
        return 0u;
    if ((arch_enable & FT_ARCH_AVX512) != 0u)
        enable |= RIX_HASH_ARCH_AVX512 | RIX_HASH_ARCH_AVX2 | RIX_HASH_ARCH_SSE;
    else if ((arch_enable & FT_ARCH_AVX2) != 0u)
        enable |= RIX_HASH_ARCH_AVX2 | RIX_HASH_ARCH_SSE;
    else if ((arch_enable & FT_ARCH_SSE) != 0u)
        enable |= RIX_HASH_ARCH_SSE;
    return enable;
}

static void
ft_maint_select_(unsigned arch_enable)
{
    ft_maintain_active = ft_table_maintain_gen;
    ft_maintain_idx_bulk_active = ft_table_maintain_idx_bulk_gen;
#if defined(__x86_64__)
    __builtin_cpu_init();
    if ((arch_enable & FT_ARCH_AVX512) &&
        __builtin_cpu_supports("avx512f")) {
        ft_maintain_active = ft_table_maintain_avx512;
        ft_maintain_idx_bulk_active = ft_table_maintain_idx_bulk_avx512;
    } else if ((arch_enable & (FT_ARCH_AVX2 | FT_ARCH_AVX512)) &&
               __builtin_cpu_supports("avx2")) {
        ft_maintain_active = ft_table_maintain_avx2;
        ft_maintain_idx_bulk_active = ft_table_maintain_idx_bulk_avx2;
    } else if ((arch_enable & (FT_ARCH_SSE | FT_ARCH_AVX2 |
                                FT_ARCH_AVX512)) &&
               __builtin_cpu_supports("sse4.2")) {
        ft_maintain_active = ft_table_maintain_sse;
        ft_maintain_idx_bulk_active = ft_table_maintain_idx_bulk_sse;
    }
#else
    (void)arch_enable;
#endif
}

void
ft_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(ft_rix_hash_arch_enable_(arch_enable));
    FT_OPS_SELECT(flow4, arch_enable, &ft_flow4_active);
    FT_OPS_SELECT(flow6, arch_enable, &ft_flow6_active);
    FT_OPS_SELECT(flowu, arch_enable, &ft_flowu_active);
    ft_maint_select_(arch_enable);
}

/*===========================================================================
 * Dispatch: init (inlined - not in ops table, compiled once per variant)
 *===========================================================================*/
#define FT_DISPATCH_INIT(prefix, entry_type, table_variant)                    \
                                                                               \
int                                                                            \
ft_##prefix##_table_init(struct ft_table *ft,                                  \
                         void *array,                                          \
                         unsigned max_entries,                                  \
                         size_t stride,                                         \
                         size_t entry_offset,                                   \
                         void *buckets_raw,                                     \
                         size_t bucket_size,                                    \
                         const struct ft_table_config *cfg)                    \
{                                                                              \
    struct ft_table_config defcfg;                                             \
    struct rix_hash_bucket_s *buckets;                                         \
    unsigned nb_bk;                                                            \
                                                                               \
    if (ft == NULL || array == NULL || max_entries == 0u)                       \
        return -1;                                                             \
    if (buckets_raw == NULL || bucket_size == 0u)                               \
        return -1;                                                             \
    buckets = ft_table_bucket_carve(buckets_raw, bucket_size, &nb_bk);         \
    if (nb_bk < FT_TABLE_MIN_NB_BK)                                           \
        return -1;                                                             \
                                                                               \
    memset(&defcfg, 0, sizeof(defcfg));                                        \
    if (cfg == NULL)                                                           \
        cfg = &defcfg;                                                         \
                                                                               \
    RIX_ASSERT(stride >= sizeof(entry_type));                                  \
    RIX_ASSERT(entry_offset + sizeof(entry_type) <= stride);                   \
    RIX_ASSERT(FT_PTR_IS_ALIGNED(FT_BYTE_PTR_ADD(array, entry_offset),        \
                                 _Alignof(entry_type)));                       \
                                                                               \
    memset(ft, 0, sizeof(*ft));                                                \
    ft->variant = (u8)(table_variant);                                         \
    ft->ts_shift = (u8)((cfg->ts_shift != 0u)                                 \
        ? flow_timestamp_shift_clamp(cfg->ts_shift)                            \
        : FLOW_TIMESTAMP_DEFAULT_SHIFT);                                       \
    ft->max_entries = max_entries;                                              \
    ft->start_mask = nb_bk - 1u;                                               \
    ft->pool_base = (unsigned char *)array;                                     \
    ft->pool_stride = stride;                                                   \
    ft->pool_entry_offset = entry_offset;                                       \
                                                                               \
    memset(buckets, 0, (size_t)nb_bk * sizeof(*buckets));                      \
    ft->buckets = buckets;                                                     \
    ft->nb_bk = nb_bk;                                                         \
    ft->ht_head.rhh_mask = nb_bk - 1u;                                        \
    ft->ht_head.rhh_nb = 0u;                                                   \
    return 0;                                                                  \
}

FT_DISPATCH_INIT(flow4, struct flow4_entry, FT_TABLE_VARIANT_FLOW4)
FT_DISPATCH_INIT(flow6, struct flow6_entry, FT_TABLE_VARIANT_FLOW6)
FT_DISPATCH_INIT(flowu, struct flowu_entry, FT_TABLE_VARIANT_FLOWU)

/*===========================================================================
 * Dispatch: cold-path forwarding (always via _gen ops)
 *===========================================================================*/
#define FT_DISPATCH_COLD(prefix)                                               \
                                                                               \
void                                                                           \
ft_##prefix##_table_destroy(struct ft_table *ft)                               \
{                                                                              \
    ft_##prefix##_ops_gen.destroy(ft);                                          \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_flush(struct ft_table *ft)                                 \
{                                                                              \
    ft_##prefix##_ops_gen.flush(ft);                                            \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_nb_entries(const struct ft_table *ft)                      \
{                                                                              \
    return ft_##prefix##_ops_gen.nb_entries(ft);                                \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_nb_bk(const struct ft_table *ft)                           \
{                                                                              \
    return ft_##prefix##_ops_gen.nb_bk(ft);                                    \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_stats(const struct ft_table *ft,                           \
                          struct ft_table_stats *out)                           \
{                                                                              \
    ft_##prefix##_ops_gen.stats(ft, out);                                      \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_status(const struct ft_table *ft,                          \
                           struct flow_status *out)                            \
{                                                                              \
    ft_##prefix##_ops_gen.status(ft, out);                                     \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_walk(struct ft_table *ft,                                  \
                         int (*cb)(u32 entry_idx, void *arg),                  \
                         void *arg)                                            \
{                                                                              \
    return ft_##prefix##_ops_gen.walk(ft, cb, arg);                            \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_migrate(struct ft_table *ft,                               \
                            void *new_buckets,                                 \
                            size_t new_bucket_size)                             \
{                                                                              \
    return ft_##prefix##_ops_gen.migrate(ft, new_buckets, new_bucket_size);    \
}

FT_DISPATCH_COLD(flow4)
FT_DISPATCH_COLD(flow6)
FT_DISPATCH_COLD(flowu)

/*===========================================================================
 * Dispatch: hot-path forwarding (via active ops pointer)
 *===========================================================================*/
#define FT_DISPATCH_HOT(prefix, active_ptr)                                    \
                                                                               \
u32                                                                            \
ft_##prefix##_table_find(struct ft_table *ft,                                  \
                         const struct prefix##_key *key,                       \
                         u64 now)                                              \
{                                                                              \
    struct ft_table_result result = { .entry_idx = 0u };                       \
    active_ptr->find_bulk(ft, key, 1u, now, &result);                          \
    return result.entry_idx;                                                   \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_find_bulk(struct ft_table *ft,                             \
                              const struct prefix##_key *keys,                 \
                              unsigned nb_keys,                                \
                              u64 now,                                         \
                              struct ft_table_result *results)                 \
{                                                                              \
    active_ptr->find_bulk(ft, keys, nb_keys, now, results);                    \
}                                                                              \
                                                                               \
u32                                                                            \
ft_##prefix##_table_add_idx(struct ft_table *ft,                               \
                            u32 entry_idx,                                     \
                            u64 now)                                           \
{                                                                              \
    u32 unused;                                                                \
    active_ptr->add_idx_bulk(ft, &entry_idx, 1u, FT_ADD_IGNORE, now,           \
                             &unused);                                         \
    return entry_idx;                                                          \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_add_idx_bulk(struct ft_table *ft,                          \
                                 u32 *entry_idxv,                              \
                                 unsigned nb_keys,                             \
                                 enum ft_add_policy policy,                    \
                                 u64 now,                                      \
                                 u32 *unused_idxv)                             \
{                                                                              \
    return active_ptr->add_idx_bulk(ft, entry_idxv, nb_keys, policy,          \
                                    now, unused_idxv);                        \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_add_idx_bulk_maint(struct ft_table *ft,                    \
                                       u32 *entry_idxv,                        \
                                       unsigned nb_keys,                       \
                                       enum ft_add_policy policy,              \
                                       u64 now,                                \
                                       u64 timeout,                            \
                                       u32 *unused_idxv,                       \
                                       unsigned max_unused,                    \
                                       unsigned min_bk_used)                   \
{                                                                              \
    return active_ptr->add_idx_bulk_maint(ft, entry_idxv, nb_keys, policy,    \
                                          now, timeout, unused_idxv,           \
                                          max_unused, min_bk_used);            \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_del_key_bulk(struct ft_table *ft,                          \
                                 const struct prefix##_key *keys,              \
                                 unsigned nb_keys,                             \
                                 u32 *unused_idxv)                             \
{                                                                              \
    return active_ptr->del_key_bulk(ft, keys, nb_keys, unused_idxv);           \
}                                                                              \
                                                                               \
u32                                                                            \
ft_##prefix##_table_del_idx(struct ft_table *ft,                         \
                                  u32 entry_idx)                               \
{                                                                              \
    u32 unused;                                                                \
    return active_ptr->del_idx_bulk(ft, &entry_idx, 1u, &unused)              \
        ? entry_idx : 0u;                                                      \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_del_idx_bulk(struct ft_table *ft,                    \
                                       const u32 *entry_idxv,                  \
                                       unsigned nb_keys,                       \
                                       u32 *unused_idxv)                       \
{                                                                              \
    return active_ptr->del_idx_bulk(ft, entry_idxv, nb_keys, unused_idxv);     \
}

FT_DISPATCH_HOT(flow4, ft_flow4_active)
FT_DISPATCH_HOT(flow6, ft_flow6_active)
FT_DISPATCH_HOT(flowu, ft_flowu_active)

int
ft_table_init(struct ft_table *ft,
              enum ft_table_variant variant,
              void *array,
              unsigned max_entries,
              size_t stride,
              size_t entry_offset,
              void *buckets,
              size_t bucket_size,
              const struct ft_table_config *cfg)
{
    switch (variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_init(ft, array, max_entries, stride,
                                   entry_offset, buckets, bucket_size, cfg);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_init(ft, array, max_entries, stride,
                                   entry_offset, buckets, bucket_size, cfg);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_init(ft, array, max_entries, stride,
                                   entry_offset, buckets, bucket_size, cfg);
    default:
        return -1;
    }
}

void
ft_table_destroy(struct ft_table *ft)
{
    if (ft == NULL)
        return;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        ft_flow4_table_destroy(ft);
        break;
    case FT_TABLE_VARIANT_FLOW6:
        ft_flow6_table_destroy(ft);
        break;
    case FT_TABLE_VARIANT_FLOWU:
        ft_flowu_table_destroy(ft);
        break;
    default:
        memset(ft, 0, sizeof(*ft));
        break;
    }
}

void
ft_table_flush(struct ft_table *ft)
{
    if (ft == NULL)
        return;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        ft_flow4_table_flush(ft);
        break;
    case FT_TABLE_VARIANT_FLOW6:
        ft_flow6_table_flush(ft);
        break;
    case FT_TABLE_VARIANT_FLOWU:
        ft_flowu_table_flush(ft);
        break;
    default:
        break;
    }
}

unsigned
ft_table_nb_entries(const struct ft_table *ft)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_nb_entries(ft);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_nb_entries(ft);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_nb_entries(ft);
    default:
        return 0u;
    }
}

unsigned
ft_table_nb_bk(const struct ft_table *ft)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_nb_bk(ft);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_nb_bk(ft);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_nb_bk(ft);
    default:
        return 0u;
    }
}

void
ft_table_stats(const struct ft_table *ft, struct ft_table_stats *out)
{
    if (ft == NULL) {
        if (out != NULL)
            memset(out, 0, sizeof(*out));
        return;
    }
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        ft_flow4_table_stats(ft, out);
        break;
    case FT_TABLE_VARIANT_FLOW6:
        ft_flow6_table_stats(ft, out);
        break;
    case FT_TABLE_VARIANT_FLOWU:
        ft_flowu_table_stats(ft, out);
        break;
    default:
        if (out != NULL)
            memset(out, 0, sizeof(*out));
        break;
    }
}

void
ft_table_status(const struct ft_table *ft, struct flow_status *out)
{
    if (ft == NULL) {
        if (out != NULL)
            memset(out, 0, sizeof(*out));
        return;
    }
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        ft_flow4_table_status(ft, out);
        break;
    case FT_TABLE_VARIANT_FLOW6:
        ft_flow6_table_status(ft, out);
        break;
    case FT_TABLE_VARIANT_FLOWU:
        ft_flowu_table_status(ft, out);
        break;
    default:
        if (out != NULL)
            memset(out, 0, sizeof(*out));
        break;
    }
}

u32
ft_table_add_idx(struct ft_table *ft, u32 entry_idx, u64 now)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_add_idx(ft, entry_idx, now);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_add_idx(ft, entry_idx, now);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_add_idx(ft, entry_idx, now);
    default:
        return 0u;
    }
}

unsigned
ft_table_add_idx_bulk(struct ft_table *ft,
                      u32 *entry_idxv,
                      unsigned nb_keys,
                      enum ft_add_policy policy,
                      u64 now,
                      u32 *unused_idxv)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_add_idx_bulk(ft, entry_idxv, nb_keys,
                                           policy, now, unused_idxv);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_add_idx_bulk(ft, entry_idxv, nb_keys,
                                           policy, now, unused_idxv);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_add_idx_bulk(ft, entry_idxv, nb_keys,
                                           policy, now, unused_idxv);
    default:
        return 0u;
    }
}

unsigned
ft_table_add_idx_bulk_maint(struct ft_table *ft,
                            u32 *entry_idxv,
                            unsigned nb_keys,
                            enum ft_add_policy policy,
                            u64 now,
                            u64 timeout,
                            u32 *unused_idxv,
                            unsigned max_unused,
                            unsigned min_bk_used)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_add_idx_bulk_maint(ft, entry_idxv, nb_keys,
                                                  policy, now, timeout,
                                                  unused_idxv, max_unused,
                                                  min_bk_used);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_add_idx_bulk_maint(ft, entry_idxv, nb_keys,
                                                  policy, now, timeout,
                                                  unused_idxv, max_unused,
                                                  min_bk_used);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_add_idx_bulk_maint(ft, entry_idxv, nb_keys,
                                                  policy, now, timeout,
                                                  unused_idxv, max_unused,
                                                  min_bk_used);
    default:
        return 0u;
    }
}

u32
ft_table_del_idx(struct ft_table *ft, u32 entry_idx)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_del_idx(ft, entry_idx);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_del_idx(ft, entry_idx);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_del_idx(ft, entry_idx);
    default:
        return 0u;
    }
}

unsigned
ft_table_del_idx_bulk(struct ft_table *ft,
                      const u32 *entry_idxv,
                      unsigned nb_keys,
                      u32 *unused_idxv)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_del_idx_bulk(ft, entry_idxv, nb_keys,
                                           unused_idxv);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_del_idx_bulk(ft, entry_idxv, nb_keys,
                                           unused_idxv);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_del_idx_bulk(ft, entry_idxv, nb_keys,
                                           unused_idxv);
    default:
        return 0u;
    }
}

int
ft_table_walk(struct ft_table *ft,
              int (*cb)(u32 entry_idx, void *arg),
              void *arg)
{
    if (ft == NULL)
        return -1;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_walk(ft, cb, arg);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_walk(ft, cb, arg);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_walk(ft, cb, arg);
    default:
        return -1;
    }
}

int
ft_table_migrate(struct ft_table *ft,
                 void *new_buckets,
                 size_t new_bucket_size)
{
    if (ft == NULL)
        return -1;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_table_migrate(ft, new_buckets, new_bucket_size);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_table_migrate(ft, new_buckets, new_bucket_size);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_table_migrate(ft, new_buckets, new_bucket_size);
    default:
        return -1;
    }
}

/*===========================================================================
 * Dispatch: maintain (variant-agnostic, arch-dispatched)
 *===========================================================================*/
unsigned
ft_table_maintain(const struct ft_maint_ctx *ctx,
                  unsigned start_bk,
                  u64 now,
                  u64 expire_tsc,
                  u32 *expired_idxv,
                  unsigned max_expired,
                  unsigned min_bk_entries,
                  unsigned *next_bk)
{
    return ft_maintain_active(ctx, start_bk, now, expire_tsc,
                              expired_idxv, max_expired,
                              min_bk_entries, next_bk);
}

unsigned
ft_table_maintain_idx_bulk(const struct ft_maint_ctx *ctx,
                           const u32 *entry_idxv,
                           unsigned nb_idx,
                           u64 now,
                           u64 expire_tsc,
                           u32 *expired_idxv,
                           unsigned max_expired,
                           unsigned min_bk_entries,
                           int enable_filter)
{
    return ft_maintain_idx_bulk_active(ctx, entry_idxv, nb_idx,
                                       now, expire_tsc,
                                       expired_idxv, max_expired,
                                       min_bk_entries, enable_filter);
}
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
