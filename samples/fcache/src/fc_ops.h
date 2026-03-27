/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * fc_ops.h - Internal architecture-dispatch ops table for fcache.
 *
 * Private header (not part of the public API).  Each variant has an
 * ops table with function pointers for all generated functions.
 * fc_dispatch.c uses these tables to forward public API calls to
 * the best available SIMD implementation.
 */

#ifndef _FC_OPS_H_
#define _FC_OPS_H_

#include "flow_cache.h"

/*===========================================================================
 * Ops table macro: generates struct fc_<prefix>_ops
 *===========================================================================*/
#define FC_OPS_DEFINE(prefix)                                                  \
struct fc_##prefix##_ops {                                                     \
    /* cold-path */                                                            \
    void (*flush)(struct fc_##prefix##_cache *fc);                             \
    unsigned (*nb_entries)(const struct fc_##prefix##_cache *fc);               \
    int (*remove_idx)(struct fc_##prefix##_cache *fc, uint32_t entry_idx);     \
    void (*stats)(const struct fc_##prefix##_cache *fc,                        \
                  struct fc_##prefix##_stats *out);                            \
    int (*walk)(struct fc_##prefix##_cache *fc,                                \
                int (*cb)(uint32_t entry_idx, void *arg), void *arg);          \
    /* hot-path */                                                             \
    void (*find_bulk)(struct fc_##prefix##_cache *fc,                           \
                      const struct fc_##prefix##_key *keys,                     \
                      unsigned nb_keys, uint64_t now,                           \
                      struct fc_##prefix##_result *results);                    \
    void (*findadd_bulk)(struct fc_##prefix##_cache *fc,                        \
                         const struct fc_##prefix##_key *keys,                  \
                         unsigned nb_keys, uint64_t now,                        \
                         struct fc_##prefix##_result *results);                 \
    void (*add_bulk)(struct fc_##prefix##_cache *fc,                            \
                     const struct fc_##prefix##_key *keys,                      \
                     unsigned nb_keys, uint64_t now,                            \
                     struct fc_##prefix##_result *results);                     \
    void (*del_bulk)(struct fc_##prefix##_cache *fc,                            \
                     const struct fc_##prefix##_key *keys,                      \
                     unsigned nb_keys);                                         \
    void (*del_idx_bulk)(struct fc_##prefix##_cache *fc,                        \
                         const uint32_t *idxs, unsigned nb_idxs);              \
    unsigned (*maintain)(struct fc_##prefix##_cache *fc,                        \
                         unsigned start_bk, unsigned bucket_count,              \
                         uint64_t now);                                         \
    unsigned (*maintain_step_ex)(struct fc_##prefix##_cache *fc,                \
                                 unsigned start_bk, unsigned bucket_count,      \
                                 unsigned skip_threshold, uint64_t now);        \
    unsigned (*maintain_step)(struct fc_##prefix##_cache *fc,                   \
                              uint64_t now, int idle);                          \
}

FC_OPS_DEFINE(flow4);
FC_OPS_DEFINE(flow6);
FC_OPS_DEFINE(flowu);

/*===========================================================================
 * Per-arch ops table declarations (defined in arch-specific .o files)
 *===========================================================================*/
#define FC_OPS_DECLARE(prefix, suffix)                                         \
    extern const struct fc_##prefix##_ops fc_##prefix##_ops##suffix

/* GEN (always available) */
FC_OPS_DECLARE(flow4, _gen);
FC_OPS_DECLARE(flow6, _gen);
FC_OPS_DECLARE(flowu, _gen);

/* SSE4.2 */
FC_OPS_DECLARE(flow4, _sse);
FC_OPS_DECLARE(flow6, _sse);
FC_OPS_DECLARE(flowu, _sse);

/* AVX2 */
FC_OPS_DECLARE(flow4, _avx2);
FC_OPS_DECLARE(flow6, _avx2);
FC_OPS_DECLARE(flowu, _avx2);

/* AVX-512 */
FC_OPS_DECLARE(flow4, _avx512);
FC_OPS_DECLARE(flow6, _avx512);
FC_OPS_DECLARE(flowu, _avx512);

/*===========================================================================
 * Runtime selection helper
 *===========================================================================*/
#define FC_OPS_SELECT(prefix, arch_enable, out_ops)                            \
do {                                                                           \
    *(out_ops) = &fc_##prefix##_ops_gen;                                       \
    _FC_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                          \
} while (0)

#if defined(__x86_64__)
#define _FC_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                      \
    __builtin_cpu_init();                                                      \
    if (((arch_enable) & FC_ARCH_AVX512) &&                                    \
        __builtin_cpu_supports("avx512f")) {                                   \
        *(out_ops) = &fc_##prefix##_ops_avx512;                                \
    } else if (((arch_enable) & (FC_ARCH_AVX2 | FC_ARCH_AVX512)) &&            \
               __builtin_cpu_supports("avx2")) {                               \
        *(out_ops) = &fc_##prefix##_ops_avx2;                                  \
    } else if (((arch_enable) & (FC_ARCH_SSE | FC_ARCH_AVX2 |                  \
                                  FC_ARCH_AVX512)) &&                           \
               __builtin_cpu_supports("sse4.2")) {                             \
        *(out_ops) = &fc_##prefix##_ops_sse;                                   \
    }
#else
#define _FC_OPS_SELECT_BODY(prefix, arch_enable, out_ops)  (void)(arch_enable);
#endif

#endif /* _FC_OPS_H_ */
