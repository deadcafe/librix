/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_HASH_H_
#define _FLOW_HASH_H_

#include <string.h>

#include "flow_key.h"

/*===========================================================================
 * flow4 key hash / cmp
 *
 * 24B key: 3x CRC32C unrolled on x86+SSE4.2, fallback to
 * rix_hash_bytes_fast.
 *===========================================================================*/
static inline union rix_hash_hash_u
flow4_key_hash(const struct flow4_key *key, u32 mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    union rix_hash_hash_u r;
    u64 w0, w1, w2;
    u32 h0, bk0, h1, inc;

    memcpy(&w0, (const char *)key,       8u);
    memcpy(&w1, (const char *)key + 8u,  8u);
    memcpy(&w2, (const char *)key + 16u, 8u);
    h0  = (u32)__builtin_ia32_crc32di(0ULL, w0);
    h0  = (u32)__builtin_ia32_crc32di((u64)h0, w1);
    h0  = (u32)__builtin_ia32_crc32di((u64)h0, w2);
    h1  = (u32)__builtin_ia32_crc32di(UINT64_C(0x9e3779b97f4a7c15), w0);
    h1  = (u32)__builtin_ia32_crc32di((u64)h1, w1);
    h1  = (u32)__builtin_ia32_crc32di((u64)h1, w2);
    h1  = rix_hash_retry_mix32(h1, UINT32_C(0x85ebca6b));
    bk0  = h0 & mask;
    inc = 1u;
    while ((h1 & mask) == bk0) {
        h1 = rix_hash_retry_mix32(h1, inc);
        inc++;
    }

    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
#else
    return rix_hash_bytes_fast(key, sizeof(*key), mask);
#endif
}

static inline int
flow4_key_cmp(const struct flow4_key *a, const struct flow4_key *b)
{
    u64 a0, a1, a2, b0, b1, b2;

    memcpy(&a0, a,                     8u);
    memcpy(&a1, (const char *)a + 8u,  8u);
    memcpy(&a2, (const char *)a + 16u, 8u);
    memcpy(&b0, b,                     8u);
    memcpy(&b1, (const char *)b + 8u,  8u);
    memcpy(&b2, (const char *)b + 16u, 8u);
    return ((a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2)) ? 1 : 0;
}

/*===========================================================================
 * flow6 key hash / cmp
 *===========================================================================*/
static inline union rix_hash_hash_u
flow6_key_hash(const struct flow6_key *key, u32 mask)
{
    return rix_hash_bytes_fast(key, sizeof(*key), mask);
}

static inline int
flow6_key_cmp(const struct flow6_key *a, const struct flow6_key *b)
{
    return memcmp(a, b, sizeof(*a));
}

/*===========================================================================
 * flowu key hash / cmp
 *===========================================================================*/
static inline union rix_hash_hash_u
flowu_key_hash(const struct flowu_key *key, u32 mask)
{
    return rix_hash_bytes_fast(key, sizeof(*key), mask);
}

static inline int
flowu_key_cmp(const struct flowu_key *a, const struct flowu_key *b)
{
    return memcmp(a, b, sizeof(*a));
}

#endif /* _FLOW_HASH_H_ */
