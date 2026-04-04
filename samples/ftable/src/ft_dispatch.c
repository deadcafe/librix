/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <rix/rix_hash_arch.h>

#include "ft_ops.h"

#define FT_ENTRY_T_flow4 struct flow4_entry
#define FT_ENTRY_T_flow6 struct flow6_entry
#define FT_ENTRY_T_flowu struct flowu_entry
#define FT_ENTRY_T(prefix) FT_ENTRY_T_##prefix

/*===========================================================================
 * Extern declarations for _gen init/destroy/flush/query/walk/grow
 *===========================================================================*/
#define FT_DISPATCH_DECLARE_GEN(prefix)                                        \
extern int ft_##prefix##_table_init_ex_gen(                                    \
    struct ft_##prefix##_table *ft, void *array, unsigned max_entries,          \
    size_t stride, size_t entry_offset,                                        \
    const struct ft_table_config *cfg);                                   \
extern int ft_##prefix##_table_init_gen(                                       \
    struct ft_##prefix##_table *ft, FT_ENTRY_T(prefix) *pool,                  \
    unsigned max_entries, const struct ft_table_config *cfg);             \
extern void ft_##prefix##_table_destroy_gen(struct ft_##prefix##_table *ft);   \
extern void ft_##prefix##_table_flush_gen(struct ft_##prefix##_table *ft);     \
extern unsigned ft_##prefix##_table_nb_entries_gen(                            \
    const struct ft_##prefix##_table *ft);                                     \
extern unsigned ft_##prefix##_table_nb_bk_gen(                                 \
    const struct ft_##prefix##_table *ft);                                     \
extern void ft_##prefix##_table_stats_gen(                                     \
    const struct ft_##prefix##_table *ft, struct ft_table_stats *out);    \
extern void ft_##prefix##_table_status_gen(                                    \
    const struct ft_##prefix##_table *ft, struct fcore_status *out);      \
extern int ft_##prefix##_table_walk_gen(                                       \
    struct ft_##prefix##_table *ft,                                            \
    int (*cb)(u32 entry_idx, void *arg), void *arg);                     \
extern int ft_##prefix##_table_grow_2x_gen(struct ft_##prefix##_table *ft);    \
extern int ft_##prefix##_table_reserve_gen(                                    \
    struct ft_##prefix##_table *ft, unsigned min_entries)

FT_DISPATCH_DECLARE_GEN(flow4);
FT_DISPATCH_DECLARE_GEN(flow6);
FT_DISPATCH_DECLARE_GEN(flowu);

/*===========================================================================
 * Active ops pointers (default to _gen)
 *===========================================================================*/
static const struct ft_flow4_ops *ft_flow4_active = &ft_flow4_ops_gen;
static const struct ft_flow6_ops *ft_flow6_active = &ft_flow6_ops_gen;
static const struct ft_flowu_ops *ft_flowu_active = &ft_flowu_ops_gen;

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

void
ft_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(ft_rix_hash_arch_enable_(arch_enable));
    FT_OPS_SELECT(flow4, arch_enable, &ft_flow4_active);
    FT_OPS_SELECT(flow6, arch_enable, &ft_flow6_active);
    FT_OPS_SELECT(flowu, arch_enable, &ft_flowu_active);
}

/*===========================================================================
 * Dispatch: per-variant cold-path forwarding (always via _gen)
 *===========================================================================*/
#define FT_DISPATCH_COLD(prefix)                                               \
                                                                               \
int                                                                            \
ft_##prefix##_table_init(struct ft_##prefix##_table *ft,                       \
                         FT_ENTRY_T(prefix) *pool,                             \
                         unsigned max_entries,                                  \
                         const struct ft_table_config *cfg)               \
{                                                                              \
    return ft_##prefix##_table_init_gen(ft, pool, max_entries, cfg);            \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_init_ex(struct ft_##prefix##_table *ft,                    \
                            void *array,                                       \
                            unsigned max_entries,                               \
                            size_t stride,                                      \
                            size_t entry_offset,                                \
                            const struct ft_table_config *cfg)            \
{                                                                              \
    RIX_ASSERT(stride >= sizeof(FT_ENTRY_T(prefix)));                          \
    RIX_ASSERT(entry_offset + sizeof(FT_ENTRY_T(prefix)) <= stride);           \
    RIX_ASSERT(FT_PTR_IS_ALIGNED(FT_BYTE_PTR_ADD(array, entry_offset),        \
                                 _Alignof(FT_ENTRY_T(prefix))));               \
    return ft_##prefix##_table_init_ex_gen(ft, array, max_entries,             \
                                           stride, entry_offset, cfg);         \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_destroy(struct ft_##prefix##_table *ft)                    \
{                                                                              \
    ft_##prefix##_table_destroy_gen(ft);                                        \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_flush(struct ft_##prefix##_table *ft)                      \
{                                                                              \
    ft_##prefix##_table_flush_gen(ft);                                          \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_nb_entries(const struct ft_##prefix##_table *ft)            \
{                                                                              \
    return ft_##prefix##_table_nb_entries_gen(ft);                              \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_nb_bk(const struct ft_##prefix##_table *ft)                \
{                                                                              \
    return ft_##prefix##_table_nb_bk_gen(ft);                                  \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_stats(const struct ft_##prefix##_table *ft,                \
                          struct ft_table_stats *out)                     \
{                                                                              \
    ft_##prefix##_table_stats_gen(ft, out);                                     \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_status(const struct ft_##prefix##_table *ft,               \
                           struct fcore_status *out)                           \
{                                                                              \
    ft_##prefix##_table_status_gen(ft, out);                                   \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_walk(struct ft_##prefix##_table *ft,                       \
                         int (*cb)(u32 entry_idx, void *arg),             \
                         void *arg)                                            \
{                                                                              \
    return ft_##prefix##_table_walk_gen(ft, cb, arg);                           \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_grow_2x(struct ft_##prefix##_table *ft)                    \
{                                                                              \
    return ft_##prefix##_table_grow_2x_gen(ft);                                \
}                                                                              \
                                                                               \
int                                                                            \
ft_##prefix##_table_reserve(struct ft_##prefix##_table *ft,                    \
                            unsigned min_entries)                               \
{                                                                              \
    return ft_##prefix##_table_reserve_gen(ft, min_entries);                    \
}

FT_DISPATCH_COLD(flow4)
FT_DISPATCH_COLD(flow6)
FT_DISPATCH_COLD(flowu)

/*===========================================================================
 * Dispatch: per-variant hot-path forwarding (via active ops pointer)
 *===========================================================================*/
#define FT_DISPATCH_HOT(prefix, active_ptr)                                    \
                                                                               \
u32                                                                       \
ft_##prefix##_table_find(struct ft_##prefix##_table *ft,                       \
                         const struct prefix##_key *key,                       \
                         u64 now)                                         \
{                                                                              \
    return active_ptr->find(ft, key, now);                                     \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_find_bulk(struct ft_##prefix##_table *ft,                  \
                              const struct prefix##_key *keys,                 \
                              unsigned nb_keys,                                \
                              u64 now,                                    \
                              struct ft_table_result *results)            \
{                                                                              \
    active_ptr->find_bulk(ft, keys, nb_keys, now, results);                    \
}                                                                              \
                                                                               \
u32                                                                       \
ft_##prefix##_table_add_idx(struct ft_##prefix##_table *ft,                    \
                            u32 entry_idx,                                \
                            u64 now)                                      \
{                                                                              \
    return active_ptr->add_idx(ft, entry_idx, now);                            \
}                                                                              \
                                                                               \
unsigned                                                                       \
ft_##prefix##_table_add_idx_bulk(struct ft_##prefix##_table *ft,               \
                                 u32 *entry_idxv,                         \
                                 unsigned nb_keys,                             \
                                 enum ft_add_policy policy,                    \
                                 u64 now,                                 \
                                 u32 *unused_idxv)                        \
{                                                                              \
    return active_ptr->add_idx_bulk(ft, entry_idxv, nb_keys, policy,          \
                                    now, unused_idxv);                        \
}                                                                              \
                                                                               \
u32                                                                       \
ft_##prefix##_table_del_key(struct ft_##prefix##_table *ft,                    \
                            const struct prefix##_key *key)                    \
{                                                                              \
    return active_ptr->del_key(ft, key);                                       \
}                                                                              \
                                                                               \
u32                                                                       \
ft_##prefix##_table_del_entry_idx(struct ft_##prefix##_table *ft,              \
                                  u32 entry_idx)                          \
{                                                                              \
    return active_ptr->del_entry_idx(ft, entry_idx);                           \
}                                                                              \
                                                                               \
void                                                                           \
ft_##prefix##_table_del_entry_idx_bulk(struct ft_##prefix##_table *ft,         \
                                       const u32 *entry_idxv,             \
                                       unsigned nb_keys)                       \
{                                                                              \
    active_ptr->del_entry_idx_bulk(ft, entry_idxv, nb_keys);                   \
}

FT_DISPATCH_HOT(flow4, ft_flow4_active)
FT_DISPATCH_HOT(flow6, ft_flow6_active)
FT_DISPATCH_HOT(flowu, ft_flowu_active)
