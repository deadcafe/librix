/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_ARCH_COMMON_H_
#define _FLOW_ARCH_COMMON_H_

#include <rix/rix_hash_arch.h>

#include "flowtable/flow_common.h"

static inline unsigned
ft_rix_hash_arch_enable(unsigned arch_enable)
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

#define FT_MAINT_DECLARE_VARIANT(prefix, ctx_type, suffix)                   \
    extern unsigned prefix##suffix(                                           \
        const struct ctx_type *, unsigned, u64, u64,                          \
        u32 *, unsigned, unsigned, unsigned *);                               \
    extern unsigned prefix##_idx_bulk##suffix(                                \
        const struct ctx_type *, const u32 *, unsigned,                       \
        u64, u64, u32 *, unsigned, unsigned, int)

#define FT_MAINT_DECLARE_VARIANTS(prefix, ctx_type)                           \
    FT_MAINT_DECLARE_VARIANT(prefix, ctx_type, _gen);                         \
    FT_MAINT_DECLARE_VARIANT(prefix, ctx_type, _sse);                         \
    FT_MAINT_DECLARE_VARIANT(prefix, ctx_type, _avx2);                        \
    FT_MAINT_DECLARE_VARIANT(prefix, ctx_type, _avx512)

#if defined(__x86_64__)
#define FT_MAINT_SELECT(arch_enable, maintain_active, idx_bulk_active, prefix) \
    do {                                                                      \
        (maintain_active) = prefix##_gen;                                     \
        (idx_bulk_active) = prefix##_idx_bulk_gen;                            \
        __builtin_cpu_init();                                                 \
        if (((arch_enable) & FT_ARCH_AVX512) &&                               \
            __builtin_cpu_supports("avx512f")) {                              \
            (maintain_active) = prefix##_avx512;                              \
            (idx_bulk_active) = prefix##_idx_bulk_avx512;                     \
        } else if (((arch_enable) & (FT_ARCH_AVX2 | FT_ARCH_AVX512)) &&       \
                   __builtin_cpu_supports("avx2")) {                          \
            (maintain_active) = prefix##_avx2;                                \
            (idx_bulk_active) = prefix##_idx_bulk_avx2;                       \
        } else if (((arch_enable) & (FT_ARCH_SSE | FT_ARCH_AVX2 |             \
                                      FT_ARCH_AVX512)) &&                     \
                   __builtin_cpu_supports("sse4.2")) {                        \
            (maintain_active) = prefix##_sse;                                 \
            (idx_bulk_active) = prefix##_idx_bulk_sse;                        \
        }                                                                     \
    } while (0)
#else
#define FT_MAINT_SELECT(arch_enable, maintain_active, idx_bulk_active, prefix) \
    do {                                                                      \
        (maintain_active) = prefix##_gen;                                     \
        (idx_bulk_active) = prefix##_idx_bulk_gen;                            \
        (void)(arch_enable);                                                  \
    } while (0)
#endif

#endif /* _FLOW_ARCH_COMMON_H_ */
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
