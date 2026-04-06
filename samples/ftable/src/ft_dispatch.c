/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <rix/rix_hash_arch.h>

#include "ft_ops.h"

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
 * Dispatch: cold-path forwarding (always via _gen ops)
 *===========================================================================*/
#define FT_DISPATCH_COLD(prefix)                                               \
                                                                               \
int                                                                            \
ft_##prefix##_table_init(struct ft_##prefix##_table *ft,                       \
                         struct prefix##_entry *pool,                          \
                         unsigned max_entries,                                  \
                         const struct ft_table_config *cfg)                    \
{                                                                              \
    return ft_##prefix##_ops_gen.init_ex(ft, pool, max_entries,                \
                                         sizeof(*pool), 0u, cfg);              \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_init_ex(struct ft_##prefix##_table *ft,                    \
                            void *array,                                       \
                            unsigned max_entries,                               \
                            size_t stride,                                      \
                            size_t entry_offset,                                \
                            const struct ft_table_config *cfg)                 \
{                                                                              \
    RIX_ASSERT(stride >= sizeof(struct prefix##_entry));                        \
    RIX_ASSERT(entry_offset + sizeof(struct prefix##_entry) <= stride);         \
    RIX_ASSERT(FT_PTR_IS_ALIGNED(FT_BYTE_PTR_ADD(array, entry_offset),        \
                                 _Alignof(struct prefix##_entry)));             \
    return ft_##prefix##_ops_gen.init_ex(ft, array, max_entries,               \
                                          stride, entry_offset, cfg);           \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_destroy(struct ft_##prefix##_table *ft)                    \
{                                                                              \
    ft_##prefix##_ops_gen.destroy(ft);                                          \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_flush(struct ft_##prefix##_table *ft)                      \
{                                                                              \
    ft_##prefix##_ops_gen.flush(ft);                                            \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_nb_entries(const struct ft_##prefix##_table *ft)            \
{                                                                              \
    return ft_##prefix##_ops_gen.nb_entries(ft);                                \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_nb_bk(const struct ft_##prefix##_table *ft)                \
{                                                                              \
    return ft_##prefix##_ops_gen.nb_bk(ft);                                    \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_stats(const struct ft_##prefix##_table *ft,                \
                          struct ft_table_stats *out)                           \
{                                                                              \
    ft_##prefix##_ops_gen.stats(ft, out);                                      \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_status(const struct ft_##prefix##_table *ft,               \
                           struct fcore_status *out)                           \
{                                                                              \
    ft_##prefix##_ops_gen.status(ft, out);                                     \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_walk(struct ft_##prefix##_table *ft,                       \
                         int (*cb)(u32 entry_idx, void *arg),                  \
                         void *arg)                                            \
{                                                                              \
    return ft_##prefix##_ops_gen.walk(ft, cb, arg);                            \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_grow_2x(struct ft_##prefix##_table *ft)                    \
{                                                                              \
    return ft_##prefix##_ops_gen.grow_2x(ft);                                  \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_reserve(struct ft_##prefix##_table *ft,                    \
                            unsigned min_entries)                               \
{                                                                              \
    return ft_##prefix##_ops_gen.reserve(ft, min_entries);                      \
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
ft_##prefix##_table_find(struct ft_##prefix##_table *ft,                       \
                         const struct prefix##_key *key,                       \
                         u64 now)                                              \
{                                                                              \
    struct ft_table_result _r = { .entry_idx = 0u };                           \
    active_ptr->find_bulk(ft, key, 1u, now, &_r);                              \
    return _r.entry_idx;                                                       \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_find_bulk(struct ft_##prefix##_table *ft,                  \
                              const struct prefix##_key *keys,                 \
                              unsigned nb_keys,                                \
                              u64 now,                                         \
                              struct ft_table_result *results)                 \
{                                                                              \
    active_ptr->find_bulk(ft, keys, nb_keys, now, results);                    \
}                                                                              \
                                                                               \
u32                                                                            \
ft_##prefix##_table_add_idx(struct ft_##prefix##_table *ft,                    \
                            u32 entry_idx,                                     \
                            u64 now)                                           \
{                                                                              \
    u32 _unused;                                                               \
    active_ptr->add_idx_bulk(ft, &entry_idx, 1u, FT_ADD_IGNORE, now,           \
                             &_unused);                                        \
    return entry_idx;                                                          \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_add_idx_bulk(struct ft_##prefix##_table *ft,               \
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
ft_##prefix##_table_del_key_bulk(struct ft_##prefix##_table *ft,               \
                                 const struct prefix##_key *keys,              \
                                 unsigned nb_keys,                             \
                                 u32 *unused_idxv)                             \
{                                                                              \
    return active_ptr->del_key_bulk(ft, keys, nb_keys, unused_idxv);           \
}                                                                              \
                                                                               \
u32                                                                            \
ft_##prefix##_table_del_entry_idx(struct ft_##prefix##_table *ft,              \
                                  u32 entry_idx)                               \
{                                                                              \
    u64 _dels = ft->stats.core.dels;                                           \
    active_ptr->del_idx_bulk(ft, &entry_idx, 1u);                              \
    return (ft->stats.core.dels != _dels) ? entry_idx : 0u;                    \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_del_entry_idx_bulk(struct ft_##prefix##_table *ft,         \
                                       const u32 *entry_idxv,                  \
                                       unsigned nb_keys)                       \
{                                                                              \
    active_ptr->del_idx_bulk(ft, entry_idxv, nb_keys);                         \
}

FT_DISPATCH_HOT(flow4, ft_flow4_active)
FT_DISPATCH_HOT(flow6, ft_flow6_active)
FT_DISPATCH_HOT(flowu, ft_flowu_active)

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
