/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_TABLE_H_
#define _FLOW_TABLE_H_

#define FT_ARCH_GEN     0u
#define FT_ARCH_SSE     (1u << 0)
#define FT_ARCH_AVX2    (1u << 1)
#define FT_ARCH_AVX512  (1u << 2)
#define FT_ARCH_AUTO    (FT_ARCH_SSE | FT_ARCH_AVX2 | FT_ARCH_AVX512)

#include "flow4_table.h"

void ft_arch_init(unsigned arch_enable);

#endif /* _FLOW_TABLE_H_ */
