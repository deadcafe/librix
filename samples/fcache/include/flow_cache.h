/**
 * @file flow_cache.h
 * @brief Unified public API header for all fcache variants.
 *
 * Include this single header to access all flow cache types
 * (flow4, flow6, flowu) and the architecture dispatch API.
 *
 * @code
 *   #include "flow_cache.h"
 *
 *   fc_arch_init(FC_ARCH_AUTO);              // once at startup
 *
 *   // optional: query packed footprint for hugepage / shared-memory layout
 *   struct fc_cache_size_attr attr;
 *   fc_flow4_cache_size_query(1000000, &attr);
 *   fc_cache_size_bind(mem, &attr);
 *   fc_flow4_cache_init_attr(&attr, NULL);
 *
 *   // or use the classic direct init path
 *   fc_flow4_cache_init(&fc, ...);           // per-cache init
 *   fc_flow4_cache_findadd_bulk(&fc, ...);   // datapath (search + insert)
 *   fc_flow4_cache_find_bulk(&fc, ...);      // datapath (search only)
 *   fc_flow4_cache_maintain_step(&fc, ...);  // periodic GC
 * @endcode
 */

/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_CACHE_H_
#define _FLOW_CACHE_H_
#include "flow4_cache.h"
#include "flow6_cache.h"
#include "flowu_cache.h"

/*===========================================================================
 * Architecture dispatch flags
 *===========================================================================*/
#define FC_ARCH_GEN     0u
#define FC_ARCH_SSE     (1u << 0)
#define FC_ARCH_AVX2    (1u << 1)
#define FC_ARCH_AVX512  (1u << 2)
#define FC_ARCH_AUTO    (FC_ARCH_SSE | FC_ARCH_AVX2 | FC_ARCH_AVX512)

/**
 * @brief One-time CPU detection and SIMD dispatch selection.
 *
 * Call once at startup before any cache operations.  Selects the
 * best available SIMD implementation (Generic/SSE4.2/AVX2/AVX-512)
 * for all flow cache variants.  Without this call, the generic
 * (scalar) implementation is used.
 *
 * @param arch_enable  Bitmask of FC_ARCH_* flags, or FC_ARCH_AUTO.
 */
void fc_arch_init(unsigned arch_enable);

#endif /* _FLOW_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
