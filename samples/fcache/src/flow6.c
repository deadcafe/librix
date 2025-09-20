/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>
#include <string.h>

#include "flow6_cache.h"
#include "fc_cache_generate.h"

static inline union rix_hash_hash_u
fc_flow6_hash_fn(const struct fc_flow6_key *key, uint32_t mask)
{
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
}

static inline int
fc_flow6_cmp(const struct fc_flow6_key *a, const struct fc_flow6_key *b)
{
    return memcmp(a, b, sizeof(*a));
}

FC_CACHE_GENERATE(flow6, FC_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS,
                   fc_flow6_hash_fn, fc_flow6_cmp)

#ifdef FC_ARCH_SUFFIX
#include "fc_ops.h"
FC_OPS_TABLE(flow6, FC_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
