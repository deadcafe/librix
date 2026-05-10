/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_MRSW_DISPATCH_H_
#define _FLOW_MRSW_DISPATCH_H_

#include "flowtable/flow_mrsw_table.h"
#include "flow_arch_common.h"

#define FT_MRSW_OPS_DEFINE(prefix)                                           \
struct ft_##prefix##_mrsw_ops {                                              \
    u32 (*find)(struct ft_mrsw_table *ft,                                    \
                const struct prefix##_key *key, u64 now);                    \
    void (*find_bulk)(struct ft_mrsw_table *ft,                              \
                      const struct prefix##_key *keys,                       \
                      unsigned nb_keys, u64 now,                             \
                      struct ft_table_result *results);                      \
    unsigned (*add_idx_bulk)(struct ft_mrsw_table *ft,                       \
                             u32 *entry_idxv, unsigned nb_keys,              \
                             enum ft_add_policy policy, u64 now,             \
                             u32 *unused_idxv);                              \
    unsigned (*del_key_bulk)(struct ft_mrsw_table *ft,                       \
                             const struct prefix##_key *keys,                \
                             unsigned nb_keys, u32 *unused_idxv);            \
    unsigned (*del_idx_bulk)(struct ft_mrsw_table *ft,                       \
                             const u32 *entry_idxv, unsigned nb_keys,        \
                             u32 *unused_idxv);                              \
    int (*walk)(struct ft_mrsw_table *ft,                                    \
                int (*cb)(u32 entry_idx, void *arg), void *arg);             \
    int (*migrate)(struct ft_mrsw_table *ft,                                 \
                   void *new_buckets, size_t new_bucket_size);               \
}

FT_MRSW_OPS_DEFINE(flow4);
FT_MRSW_OPS_DEFINE(flow6);
FT_MRSW_OPS_DEFINE(flowu);

#define FT_MRSW_OPS_DECLARE(prefix, suffix)                                  \
    extern const struct ft_##prefix##_mrsw_ops ft_##prefix##_mrsw_ops##suffix

FT_MRSW_OPS_DECLARE(flow4, _gen);
FT_MRSW_OPS_DECLARE(flow4, _sse);
FT_MRSW_OPS_DECLARE(flow4, _avx2);
FT_MRSW_OPS_DECLARE(flow4, _avx512);

FT_MRSW_OPS_DECLARE(flow6, _gen);
FT_MRSW_OPS_DECLARE(flow6, _sse);
FT_MRSW_OPS_DECLARE(flow6, _avx2);
FT_MRSW_OPS_DECLARE(flow6, _avx512);

FT_MRSW_OPS_DECLARE(flowu, _gen);
FT_MRSW_OPS_DECLARE(flowu, _sse);
FT_MRSW_OPS_DECLARE(flowu, _avx2);
FT_MRSW_OPS_DECLARE(flowu, _avx512);

void ft_mrsw_arch_variant_init_gen(void);
void ft_mrsw_arch_variant_init_sse(void);
void ft_mrsw_arch_variant_init_avx2(void);
void ft_mrsw_arch_variant_init_avx512(void);

#define FT_MRSW_ARCH_DECLARE(prefix, key_t, suffix)                          \
    u32 ft_##prefix##_mrsw_table_find##suffix(                               \
        struct ft_mrsw_table *ft, const key_t *key, u64 now);                \
    void ft_##prefix##_mrsw_table_find_bulk##suffix(                         \
        struct ft_mrsw_table *ft, const key_t *keys,                         \
        unsigned nb_keys, u64 now, struct ft_table_result *results);         \
    unsigned ft_##prefix##_mrsw_table_add_idx_bulk##suffix(                  \
        struct ft_mrsw_table *ft, u32 *entry_idxv, unsigned nb_keys,         \
        enum ft_add_policy policy, u64 now, u32 *unused_idxv);               \
    unsigned ft_##prefix##_mrsw_table_del_key_bulk##suffix(                  \
        struct ft_mrsw_table *ft, const key_t *keys,                         \
        unsigned nb_keys, u32 *unused_idxv);                                 \
    unsigned ft_##prefix##_mrsw_table_del_idx_bulk##suffix(                  \
        struct ft_mrsw_table *ft, const u32 *entry_idxv,                     \
        unsigned nb_keys, u32 *unused_idxv);                                 \
    int ft_##prefix##_mrsw_table_walk##suffix(                               \
        struct ft_mrsw_table *ft,                                            \
        int (*cb)(u32 entry_idx, void *arg), void *arg);                     \
    int ft_##prefix##_mrsw_table_migrate##suffix(                            \
        struct ft_mrsw_table *ft, void *new_buckets, size_t new_bucket_size)

#define FT_MRSW_ARCH_DECLARE_ALL(prefix, key_t)                              \
    FT_MRSW_ARCH_DECLARE(prefix, key_t, _gen);                               \
    FT_MRSW_ARCH_DECLARE(prefix, key_t, _sse);                               \
    FT_MRSW_ARCH_DECLARE(prefix, key_t, _avx2);                              \
    FT_MRSW_ARCH_DECLARE(prefix, key_t, _avx512)

FT_MRSW_ARCH_DECLARE_ALL(flow4, struct flow4_key);
FT_MRSW_ARCH_DECLARE_ALL(flow6, struct flow6_key);
FT_MRSW_ARCH_DECLARE_ALL(flowu, struct flowu_key);

#define FT_MRSW_OPS_SELECT(prefix, arch_enable, out_ops)                     \
do {                                                                         \
    *(out_ops) = &ft_##prefix##_mrsw_ops_gen;                                \
    _FT_MRSW_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                   \
} while (0)

#if defined(__x86_64__)
#define _FT_MRSW_OPS_SELECT_BODY(prefix, arch_enable, out_ops)               \
    __builtin_cpu_init();                                                    \
    if (((arch_enable) & FT_ARCH_AVX512) &&                                  \
        __builtin_cpu_supports("avx512f")) {                                 \
        *(out_ops) = &ft_##prefix##_mrsw_ops_avx512;                         \
    } else if (((arch_enable) & (FT_ARCH_AVX2 | FT_ARCH_AVX512)) &&          \
               __builtin_cpu_supports("avx2")) {                             \
        *(out_ops) = &ft_##prefix##_mrsw_ops_avx2;                           \
    } else if (((arch_enable) & (FT_ARCH_SSE | FT_ARCH_AVX2 |                \
                                  FT_ARCH_AVX512)) &&                        \
               __builtin_cpu_supports("sse4.2")) {                           \
        *(out_ops) = &ft_##prefix##_mrsw_ops_sse;                            \
    }
#else
#define _FT_MRSW_OPS_SELECT_BODY(prefix, arch_enable, out_ops)               \
    (void)(arch_enable);
#endif

void ft_mrsw_arch_init(unsigned arch_enable);

#endif /* _FLOW_MRSW_DISPATCH_H_ */
