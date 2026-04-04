/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _SAMPLES_FLOW_KEY_H_
#define _SAMPLES_FLOW_KEY_H_

#include <stdint.h>
#include <string.h>

#include <rix/rix_hash.h>

struct flow_entry_meta {
    uint32_t cur_hash;
    uint16_t slot;
    uint16_t reserved0;
    uint32_t timestamp;
};

#ifndef FLOW_TIMESTAMP_DEFAULT_SHIFT
#define FLOW_TIMESTAMP_DEFAULT_SHIFT 4u
#endif

#ifndef FLOW_TIMESTAMP_MAX_SHIFT
#define FLOW_TIMESTAMP_MAX_SHIFT 24u
#endif

#define FLOW_TIMESTAMP_MASK       UINT64_C(0x00000000ffffffff)
#define FLOW_TIMESTAMP_HALF_RANGE UINT64_C(0x0000000080000000)

static inline uint8_t
flow_timestamp_shift_clamp(unsigned shift)
{
    return (uint8_t)((shift <= FLOW_TIMESTAMP_MAX_SHIFT)
        ? shift : FLOW_TIMESTAMP_DEFAULT_SHIFT);
}

static inline uint64_t
flow_timestamp_encode(uint64_t now, unsigned shift)
{
    return (now >> flow_timestamp_shift_clamp(shift)) & FLOW_TIMESTAMP_MASK;
}

static inline uint64_t
flow_timestamp_timeout_encode(uint64_t timeout, unsigned shift)
{
    uint64_t encoded = timeout >> flow_timestamp_shift_clamp(shift);

    if (encoded > FLOW_TIMESTAMP_MASK)
        encoded = FLOW_TIMESTAMP_MASK;
    if (timeout != 0u && encoded == 0u)
        encoded = 1u;
    return encoded;
}

static inline uint64_t
flow_timestamp_get(const struct flow_entry_meta *meta)
{
    return meta->timestamp;
}

static inline void
flow_timestamp_set_raw(struct flow_entry_meta *meta, uint64_t encoded)
{
    encoded &= FLOW_TIMESTAMP_MASK;
    meta->timestamp = (uint32_t)encoded;
}

static inline void
flow_timestamp_store(struct flow_entry_meta *meta, uint64_t now,
                     unsigned shift)
{
    flow_timestamp_set_raw(meta, flow_timestamp_encode(now, shift));
}

static inline void
flow_timestamp_touch(struct flow_entry_meta *meta, uint64_t now,
                     unsigned shift)
{
    if (meta->timestamp == 0u)
        return;
    flow_timestamp_store(meta, now, shift);
}

static inline void
flow_timestamp_clear(struct flow_entry_meta *meta)
{
    meta->timestamp = 0u;
}

static inline void
flow_timestamp_set_permanent(struct flow_entry_meta *meta)
{
    meta->timestamp = 0u;
}

static inline int
flow_timestamp_is_zero(const struct flow_entry_meta *meta)
{
    return flow_timestamp_get(meta) == 0u;
}

static inline uint64_t
flow_timestamp_elapsed(uint64_t now_encoded, uint64_t ts_encoded)
{
    return (now_encoded - ts_encoded) & FLOW_TIMESTAMP_MASK;
}

static inline int
flow_timestamp_is_expired_raw(uint64_t ts_encoded, uint64_t now_encoded,
                              uint64_t timeout_encoded)
{
    if (ts_encoded == 0u)
        return 0;
    return flow_timestamp_elapsed(now_encoded, ts_encoded) > timeout_encoded;
}

static inline int
flow_timestamp_is_expired(const struct flow_entry_meta *meta, uint64_t now,
                          uint64_t timeout, unsigned shift)
{
    return flow_timestamp_is_expired_raw(flow_timestamp_get(meta),
        flow_timestamp_encode(now, shift),
        flow_timestamp_timeout_encode(timeout, shift));
}

struct flow4_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t zero;
};

struct flow4_entry {
    struct flow4_key key;
    struct flow_entry_meta meta;
};

struct flow6_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    uint8_t src_ip[16];
    uint8_t dst_ip[16];
} __attribute__((packed));

struct flow6_entry {
    struct flow6_key key;
    struct flow_entry_meta meta;
};

struct flowu_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    union {
        struct {
            uint32_t src;
            uint32_t dst;
            uint8_t  _pad[24];
        } v4;
        struct {
            uint8_t src[16];
            uint8_t dst[16];
        } v6;
    } addr;
} __attribute__((packed));

struct flowu_entry {
    struct flowu_key key;
    struct flow_entry_meta meta;
};

/*===========================================================================
 * flow4 key hash / cmp
 *
 * 24B key: 3x CRC32C unrolled on x86+SSE4.2, fallback to
 * rix_hash_bytes_fast.  Non-zero guaranteed by rix_hash layer.
 *===========================================================================*/

static inline union rix_hash_hash_u
flow4_key_hash(const struct flow4_key *key, uint32_t mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    union rix_hash_hash_u r;
    uint64_t w0, w1, w2;
    uint32_t h0, bk0, seed, h1;

    memcpy(&w0, (const char *)key,      8u);
    memcpy(&w1, (const char *)key + 8u,  8u);
    memcpy(&w2, (const char *)key + 16u, 8u);
    h0  = (uint32_t)__builtin_ia32_crc32di(0ULL,          w0);
    h0  = (uint32_t)__builtin_ia32_crc32di((uint64_t)h0,  w1);
    h0  = (uint32_t)__builtin_ia32_crc32di((uint64_t)h0,  w2);
    h0 |= !h0;
    bk0  = h0 & mask;
    seed = ~h0;
    do {
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, w0);
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1,   w1);
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1,   w2);
        h1 |= !h1;
        seed = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, (uint64_t)h0);
    } while ((h1 & mask) == bk0);

    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
#else
    return rix_hash_bytes_fast(key, sizeof(*key), mask);
#endif
}

/*
 * Inline 24B key comparison -- avoids function-pointer overhead.
 * 24B = 3 x uint64_t XOR-OR.
 */
static inline int
flow4_key_cmp(const struct flow4_key *a, const struct flow4_key *b)
{
    uint64_t a0, a1, a2, b0, b1, b2;

    memcpy(&a0, a,                            8u);
    memcpy(&a1, (const char *)a + 8u,         8u);
    memcpy(&a2, (const char *)a + 16u,        8u);
    memcpy(&b0, b,                            8u);
    memcpy(&b1, (const char *)b + 8u,         8u);
    memcpy(&b2, (const char *)b + 16u,        8u);
    return ((a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2)) ? 1 : 0;
}

/*===========================================================================
 * flow6 key hash / cmp
 *===========================================================================*/

static inline union rix_hash_hash_u
flow6_key_hash(const struct flow6_key *key, uint32_t mask)
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
flowu_key_hash(const struct flowu_key *key, uint32_t mask)
{
    return rix_hash_bytes_fast(key, sizeof(*key), mask);
}

static inline int
flowu_key_cmp(const struct flowu_key *a, const struct flowu_key *b)
{
    return memcmp(a, b, sizeof(*a));
}

#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
