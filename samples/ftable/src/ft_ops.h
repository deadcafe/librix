/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FT_OPS_H_
#define _FT_OPS_H_

#include "flow_table.h"

/*===========================================================================
 * Ops table macro: generates struct ft_<prefix>_ops
 *===========================================================================*/
#define FT_OPS_DEFINE(prefix)                                                  \
struct ft_##prefix##_ops {                                                     \
    uint32_t (*find)(struct ft_##prefix##_table *ft,                           \
                     const struct prefix##_key *key,                           \
                     uint64_t now);                                            \
    void (*find_bulk)(struct ft_##prefix##_table *ft,                          \
                      const struct prefix##_key *keys,                         \
                      unsigned nb_keys,                                        \
                      uint64_t now,                                            \
                      struct ft_table_result *results);                   \
    uint32_t (*add_idx)(struct ft_##prefix##_table *ft,                        \
                        uint32_t entry_idx,                                    \
                        uint64_t now);                                         \
    unsigned (*add_idx_bulk)(struct ft_##prefix##_table *ft,                   \
                             uint32_t *entry_idxv,                             \
                             unsigned nb_keys,                                 \
                             enum ft_add_policy policy,                        \
                             uint64_t now,                                     \
                             uint32_t *unused_idxv);                           \
    uint32_t (*del_key)(struct ft_##prefix##_table *ft,                        \
                        const struct prefix##_key *key);                       \
    uint32_t (*del_entry_idx)(struct ft_##prefix##_table *ft,                  \
                              uint32_t entry_idx);                             \
    void (*del_entry_idx_bulk)(struct ft_##prefix##_table *ft,                 \
                               const uint32_t *entry_idxv,                     \
                               unsigned nb_keys);                              \
}

FT_OPS_DEFINE(flow4);
FT_OPS_DEFINE(flow6);
FT_OPS_DEFINE(flowu);

/*===========================================================================
 * Per-arch ops table declarations
 *===========================================================================*/
#define FT_OPS_DECLARE(prefix, suffix)                                         \
    extern const struct ft_##prefix##_ops ft_##prefix##_ops##suffix

FT_OPS_DECLARE(flow4, _gen);
FT_OPS_DECLARE(flow4, _sse);
FT_OPS_DECLARE(flow4, _avx2);
FT_OPS_DECLARE(flow4, _avx512);

FT_OPS_DECLARE(flow6, _gen);
FT_OPS_DECLARE(flow6, _sse);
FT_OPS_DECLARE(flow6, _avx2);
FT_OPS_DECLARE(flow6, _avx512);

FT_OPS_DECLARE(flowu, _gen);
FT_OPS_DECLARE(flowu, _sse);
FT_OPS_DECLARE(flowu, _avx2);
FT_OPS_DECLARE(flowu, _avx512);

/*===========================================================================
 * Runtime selection helper
 *===========================================================================*/
#define FT_OPS_SELECT(prefix, arch_enable, out_ops)                            \
do {                                                                           \
    *(out_ops) = &ft_##prefix##_ops_gen;                                       \
    _FT_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                          \
} while (0)

#if defined(__x86_64__)
#define _FT_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                      \
    __builtin_cpu_init();                                                      \
    if (((arch_enable) & FT_ARCH_AVX512) &&                                    \
        __builtin_cpu_supports("avx512f")) {                                   \
        *(out_ops) = &ft_##prefix##_ops_avx512;                                \
    } else if (((arch_enable) & (FT_ARCH_AVX2 | FT_ARCH_AVX512)) &&            \
               __builtin_cpu_supports("avx2")) {                               \
        *(out_ops) = &ft_##prefix##_ops_avx2;                                  \
    } else if (((arch_enable) & (FT_ARCH_SSE | FT_ARCH_AVX2 |                  \
                                  FT_ARCH_AVX512)) &&                          \
               __builtin_cpu_supports("sse4.2")) {                             \
        *(out_ops) = &ft_##prefix##_ops_sse;                                   \
    }
#else
#define _FT_OPS_SELECT_BODY(prefix, arch_enable, out_ops) (void)(arch_enable)
#endif

#endif /* _FT_OPS_H_ */
