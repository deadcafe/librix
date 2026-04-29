/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_DISPATCH_H_
#define _FLOW_DISPATCH_H_

#include "flow_table.h"

/*===========================================================================
 * Ops table macro: generates struct ft_<prefix>_ops
 *===========================================================================*/
#define FT_OPS_DEFINE(prefix)                                                 \
struct ft_##prefix##_ops {                                                    \
    /* cold-path */                                                           \
    void (*destroy)(struct ft_table *ft);                                     \
    void (*flush)(struct ft_table *ft);                                       \
    unsigned (*nb_entries)(const struct ft_table *ft);                        \
    unsigned (*nb_bk)(const struct ft_table *ft);                             \
    void (*stats)(const struct ft_table *ft,                                  \
                  struct ft_table_stats *out);                                \
    void (*status)(const struct ft_table *ft,                                 \
                   struct flow_status *out);                                  \
    int (*walk)(struct ft_table *ft,                                          \
                int (*cb)(u32 entry_idx, void *arg), void *arg);              \
    int (*migrate)(struct ft_table *ft,                                       \
                   void *new_buckets, size_t new_bucket_size);                \
    /* hot-path */                                                            \
    void (*find_bulk)(struct ft_table *ft,                                    \
                      const struct prefix##_key *keys,                        \
                      unsigned nb_keys,                                       \
                      u64 now,                                                \
                      struct ft_table_result *results);                       \
    unsigned (*add_idx_bulk)(struct ft_table *ft,                             \
                             u32 *entry_idxv,                                 \
                             unsigned nb_keys,                                \
                             enum ft_add_policy policy,                       \
                             u64 now,                                         \
                             u32 *unused_idxv);                               \
    unsigned (*add_idx_bulk_maint)(struct ft_table *ft,                       \
                                   u32 *entry_idxv,                           \
                                   unsigned nb_keys,                          \
                                   enum ft_add_policy policy,                 \
                                   u64 now,                                   \
                                   u64 timeout,                               \
                                   u32 *unused_idxv,                          \
                                   unsigned max_unused,                       \
                                   unsigned min_bk_used);                     \
    unsigned (*del_key_bulk)(struct ft_table *ft,                             \
                             const struct prefix##_key *keys,                 \
                             unsigned nb_keys,                                \
                             u32 *unused_idxv);                               \
    unsigned (*del_idx_bulk)(struct ft_table *ft,                             \
                             const u32 *entry_idxv,                           \
                             unsigned nb_keys,                                \
                             u32 *unused_idxv);                               \
}

FT_OPS_DEFINE(flow4);
FT_OPS_DEFINE(flow6);
FT_OPS_DEFINE(flowu);

/*===========================================================================
 * Per-arch ops table declarations
 *===========================================================================*/
#define FT_OPS_DECLARE(prefix, suffix)                                        \
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
#define FT_OPS_SELECT(prefix, arch_enable, out_ops)                           \
do {                                                                          \
    *(out_ops) = &ft_##prefix##_ops_gen;                                      \
    _FT_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                         \
} while (0)

#if defined(__x86_64__)
#define _FT_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                     \
    __builtin_cpu_init();                                                     \
    if (((arch_enable) & FT_ARCH_AVX512) &&                                   \
        __builtin_cpu_supports("avx512f")) {                                  \
        *(out_ops) = &ft_##prefix##_ops_avx512;                               \
    } else if (((arch_enable) & (FT_ARCH_AVX2 | FT_ARCH_AVX512)) &&           \
               __builtin_cpu_supports("avx2")) {                              \
        *(out_ops) = &ft_##prefix##_ops_avx2;                                 \
    } else if (((arch_enable) & (FT_ARCH_SSE | FT_ARCH_AVX2 |                 \
                                  FT_ARCH_AVX512)) &&                         \
               __builtin_cpu_supports("sse4.2")) {                            \
        *(out_ops) = &ft_##prefix##_ops_sse;                                  \
    }
#else
#define _FT_OPS_SELECT_BODY(prefix, arch_enable, out_ops) (void)(arch_enable)
#endif

#endif /* _FLOW_DISPATCH_H_ */
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
