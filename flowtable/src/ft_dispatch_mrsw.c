/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include "flow_mrsw_dispatch.h"

static const struct ft_flow4_mrsw_ops *ft_flow4_mrsw_active =
    &ft_flow4_mrsw_ops_gen;
static const struct ft_flow6_mrsw_ops *ft_flow6_mrsw_active =
    &ft_flow6_mrsw_ops_gen;
static const struct ft_flowu_mrsw_ops *ft_flowu_mrsw_active =
    &ft_flowu_mrsw_ops_gen;

static inline u64
ft_mrsw_load_u64_(const _Atomic u64 *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

static inline u32
ft_mrsw_load_u32_(const _Atomic u32 *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

static void
ft_mrsw_stats_clear_(struct ft_mrsw_table_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
}

static void
ft_mrsw_status_clear_(struct ft_mrsw_flow_status *status)
{
    memset(status, 0, sizeof(*status));
}

void
ft_mrsw_arch_init(unsigned arch_enable)
{
    void (*variant_init)(void) = ft_mrsw_arch_variant_init_gen;

    FT_MRSW_OPS_SELECT(flow4, arch_enable, &ft_flow4_mrsw_active);
    FT_MRSW_OPS_SELECT(flow6, arch_enable, &ft_flow6_mrsw_active);
    FT_MRSW_OPS_SELECT(flowu, arch_enable, &ft_flowu_mrsw_active);

#if defined(__x86_64__)
    __builtin_cpu_init();
    if ((arch_enable & FT_ARCH_AVX512) &&
        __builtin_cpu_supports("avx512f")) {
        variant_init = ft_mrsw_arch_variant_init_avx512;
    } else if ((arch_enable & (FT_ARCH_AVX2 | FT_ARCH_AVX512)) &&
               __builtin_cpu_supports("avx2")) {
        variant_init = ft_mrsw_arch_variant_init_avx2;
    } else if ((arch_enable & (FT_ARCH_SSE | FT_ARCH_AVX2 |
                               FT_ARCH_AVX512)) &&
               __builtin_cpu_supports("sse4.2")) {
        variant_init = ft_mrsw_arch_variant_init_sse;
    }
#else
    (void)arch_enable;
#endif
    variant_init();
}

static int
ft_mrsw_table_init_common_(struct ft_mrsw_table *ft,
                           enum ft_table_variant variant,
                           void *array,
                           unsigned max_entries,
                           size_t stride,
                           size_t entry_offset,
                           void *buckets_raw,
                           size_t bucket_size,
                           const struct ft_table_config *cfg)
{
    struct ft_table_config defcfg;
    struct rix_hash_bucket_s *buckets;
    unsigned nb_bk;

    if (ft == NULL || array == NULL || max_entries == 0u ||
        buckets_raw == NULL || bucket_size == 0u)
        return -1;

    buckets = ft_mrsw_table_bucket_carve(buckets_raw, bucket_size, &nb_bk);
    if (nb_bk < FT_TABLE_MIN_NB_BK)
        return -1;

    memset(&defcfg, 0, sizeof(defcfg));
    if (cfg == NULL)
        cfg = &defcfg;
    (void)cfg;

    memset(ft, 0, sizeof(*ft));
    ft->variant = (u8)variant;
    ft->max_entries = max_entries;
    ft->start_mask = nb_bk - 1u;
    ft->pool_base = (unsigned char *)array;
    ft->pool_stride = stride;
    ft->pool_entry_offset = entry_offset;
    ft->buckets = buckets;
    ft->nb_bk = nb_bk;
    ft->ht_head.rhh_mask = nb_bk - 1u;
    atomic_init(&ft->ht_head.rhh_nb, 0u);
    rix_hash_mrsw_buckets_init(ft->buckets, ft->nb_bk);
    ft_mrsw_stats_clear_(&ft->stats);
    ft_mrsw_status_clear_(&ft->status);
    return 0;
}

#define FT_MRSW_DISPATCH_INIT(prefix, entry_type, table_variant)              \
int                                                                           \
ft_##prefix##_mrsw_table_init(struct ft_mrsw_table *ft,                       \
                              void *array, unsigned max_entries,              \
                              size_t stride, size_t entry_offset,             \
                              void *buckets, size_t bucket_size,              \
                              const struct ft_table_config *cfg)              \
{                                                                             \
    if (stride < sizeof(entry_type) ||                                         \
        entry_offset + sizeof(entry_type) > stride)                            \
        return -1;                                                            \
    return ft_mrsw_table_init_common_(ft, table_variant, array, max_entries,  \
                                      stride, entry_offset, buckets,           \
                                      bucket_size, cfg);                      \
}

FT_MRSW_DISPATCH_INIT(flow4, struct flow4_mrsw_entry, FT_TABLE_VARIANT_FLOW4)
FT_MRSW_DISPATCH_INIT(flow6, struct flow6_mrsw_entry, FT_TABLE_VARIANT_FLOW6)
FT_MRSW_DISPATCH_INIT(flowu, struct flowu_mrsw_entry, FT_TABLE_VARIANT_FLOWU)

int
ft_mrsw_table_init(struct ft_mrsw_table *ft,
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
        return ft_flow4_mrsw_table_init(ft, array, max_entries, stride,
                                        entry_offset, buckets, bucket_size, cfg);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_mrsw_table_init(ft, array, max_entries, stride,
                                        entry_offset, buckets, bucket_size, cfg);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_mrsw_table_init(ft, array, max_entries, stride,
                                        entry_offset, buckets, bucket_size, cfg);
    default:
        return -1;
    }
}

void
ft_mrsw_table_destroy(struct ft_mrsw_table *ft)
{
    if (ft != NULL)
        memset(ft, 0, sizeof(*ft));
}

void
ft_mrsw_table_flush(struct ft_mrsw_table *ft)
{
    if (ft == NULL || ft->buckets == NULL)
        return;
    rix_hash_mrsw_buckets_init(ft->buckets, ft->nb_bk);
    atomic_store_explicit(&ft->ht_head.rhh_nb, 0u, memory_order_relaxed);
    ft_mrsw_status_clear_(&ft->status);
}

unsigned
ft_mrsw_table_nb_entries(const struct ft_mrsw_table *ft)
{
    if (ft == NULL)
        return 0u;
    return atomic_load_explicit(&ft->ht_head.rhh_nb, memory_order_relaxed);
}

unsigned
ft_mrsw_table_nb_bk(const struct ft_mrsw_table *ft)
{
    return ft == NULL ? 0u : ft->nb_bk;
}

void
ft_mrsw_table_stats(const struct ft_mrsw_table *ft, struct ft_table_stats *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (ft == NULL)
        return;
    /* Reader-side counters (lookups/hits/misses/force_expired) are not
     * tracked in MRSW; the memset above leaves them at zero.  Maintenance
     * counters likewise do not apply. */
    out->core.adds = ft_mrsw_load_u64_(&ft->stats.core.adds);
    out->core.add_existing = ft_mrsw_load_u64_(&ft->stats.core.add_existing);
    out->core.add_failed = ft_mrsw_load_u64_(&ft->stats.core.add_failed);
    out->core.dels = ft_mrsw_load_u64_(&ft->stats.core.dels);
    out->core.del_miss = ft_mrsw_load_u64_(&ft->stats.core.del_miss);
    out->grow_execs = ft_mrsw_load_u64_(&ft->stats.grow_execs);
    out->grow_failures = ft_mrsw_load_u64_(&ft->stats.grow_failures);
}

void
ft_mrsw_table_status(const struct ft_mrsw_table *ft, struct flow_status *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (ft == NULL)
        return;
    out->entries = (u32)atomic_load_explicit(&ft->ht_head.rhh_nb,
                                              memory_order_relaxed);
    out->kickouts = ft_mrsw_load_u32_(&ft->status.kickouts);
    out->add_bk0 = ft_mrsw_load_u32_(&ft->status.add_bk0);
    out->add_bk1 = ft_mrsw_load_u32_(&ft->status.add_bk1);
}

u32
ft_flow4_mrsw_table_find(struct ft_mrsw_table *ft,
                         const struct flow4_key *key,
                         u64 now)
{
    return ft_flow4_mrsw_active->find(ft, key, now);
}

u32
ft_flow6_mrsw_table_find(struct ft_mrsw_table *ft,
                         const struct flow6_key *key,
                         u64 now)
{
    return ft_flow6_mrsw_active->find(ft, key, now);
}

u32
ft_flowu_mrsw_table_find(struct ft_mrsw_table *ft,
                         const struct flowu_key *key,
                         u64 now)
{
    return ft_flowu_mrsw_active->find(ft, key, now);
}

void
ft_flow4_mrsw_table_find_bulk(struct ft_mrsw_table *ft,
                              const struct flow4_key *keys,
                              unsigned nb_keys,
                              u64 now,
                              struct ft_table_result *results)
{
    ft_flow4_mrsw_active->find_bulk(ft, keys, nb_keys, now, results);
}

void
ft_flow6_mrsw_table_find_bulk(struct ft_mrsw_table *ft,
                              const struct flow6_key *keys,
                              unsigned nb_keys,
                              u64 now,
                              struct ft_table_result *results)
{
    ft_flow6_mrsw_active->find_bulk(ft, keys, nb_keys, now, results);
}

void
ft_flowu_mrsw_table_find_bulk(struct ft_mrsw_table *ft,
                              const struct flowu_key *keys,
                              unsigned nb_keys,
                              u64 now,
                              struct ft_table_result *results)
{
    ft_flowu_mrsw_active->find_bulk(ft, keys, nb_keys, now, results);
}

u32
ft_mrsw_table_add_idx(struct ft_mrsw_table *ft, u32 entry_idx, u64 now)
{
    u32 unused;

    return ft_mrsw_table_add_idx_bulk(ft, &entry_idx, 1u, FT_ADD_IGNORE,
                                      now, &unused) == 0u ? entry_idx : RIX_NIL;
}

unsigned
ft_mrsw_table_add_idx_bulk(struct ft_mrsw_table *ft,
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
        return ft_flow4_mrsw_active->add_idx_bulk(
            ft, entry_idxv, nb_keys, policy, now, unused_idxv);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_mrsw_active->add_idx_bulk(
            ft, entry_idxv, nb_keys, policy, now, unused_idxv);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_mrsw_active->add_idx_bulk(
            ft, entry_idxv, nb_keys, policy, now, unused_idxv);
    default:
        return 0u;
    }
}

u32
ft_mrsw_table_del_idx(struct ft_mrsw_table *ft, u32 entry_idx)
{
    u32 unused;

    return ft_mrsw_table_del_idx_bulk(ft, &entry_idx, 1u, &unused) != 0u
        ? entry_idx : RIX_NIL;
}

unsigned
ft_mrsw_table_del_idx_bulk(struct ft_mrsw_table *ft,
                           const u32 *entry_idxv,
                           unsigned nb_keys,
                           u32 *unused_idxv)
{
    if (ft == NULL)
        return 0u;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_mrsw_active->del_idx_bulk(
            ft, entry_idxv, nb_keys, unused_idxv);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_mrsw_active->del_idx_bulk(
            ft, entry_idxv, nb_keys, unused_idxv);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_mrsw_active->del_idx_bulk(
            ft, entry_idxv, nb_keys, unused_idxv);
    default:
        return 0u;
    }
}

unsigned
ft_flow4_mrsw_table_del_key_bulk(struct ft_mrsw_table *ft,
                                 const struct flow4_key *keys,
                                 unsigned nb_keys,
                                 u32 *unused_idxv)
{
    return ft_flow4_mrsw_active->del_key_bulk(ft, keys, nb_keys, unused_idxv);
}

unsigned
ft_flow6_mrsw_table_del_key_bulk(struct ft_mrsw_table *ft,
                                 const struct flow6_key *keys,
                                 unsigned nb_keys,
                                 u32 *unused_idxv)
{
    return ft_flow6_mrsw_active->del_key_bulk(ft, keys, nb_keys, unused_idxv);
}

unsigned
ft_flowu_mrsw_table_del_key_bulk(struct ft_mrsw_table *ft,
                                 const struct flowu_key *keys,
                                 unsigned nb_keys,
                                 u32 *unused_idxv)
{
    return ft_flowu_mrsw_active->del_key_bulk(ft, keys, nb_keys, unused_idxv);
}

int
ft_mrsw_table_walk(struct ft_mrsw_table *ft,
                   int (*cb)(u32 entry_idx, void *arg),
                   void *arg)
{
    if (ft == NULL)
        return -1;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_mrsw_active->walk(ft, cb, arg);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_mrsw_active->walk(ft, cb, arg);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_mrsw_active->walk(ft, cb, arg);
    default:
        return -1;
    }
}

int
ft_mrsw_table_migrate(struct ft_mrsw_table *ft,
                      void *new_buckets,
                      size_t new_bucket_size)
{
    if (ft == NULL)
        return -1;
    switch (ft->variant) {
    case FT_TABLE_VARIANT_FLOW4:
        return ft_flow4_mrsw_active->migrate(ft, new_buckets,
                                             new_bucket_size);
    case FT_TABLE_VARIANT_FLOW6:
        return ft_flow6_mrsw_active->migrate(ft, new_buckets,
                                             new_bucket_size);
    case FT_TABLE_VARIANT_FLOWU:
        return ft_flowu_mrsw_active->migrate(ft, new_buckets,
                                             new_bucket_size);
    default:
        return -1;
    }
}
