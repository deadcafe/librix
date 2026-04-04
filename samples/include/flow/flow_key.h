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
    u32 cur_hash;
    u16 slot;
    u16 reserved0;
    u32 timestamp;
};

#ifndef FLOW_TIMESTAMP_DEFAULT_SHIFT
#define FLOW_TIMESTAMP_DEFAULT_SHIFT 4u
#endif

#ifndef FLOW_TIMESTAMP_MAX_SHIFT
#define FLOW_TIMESTAMP_MAX_SHIFT 24u
#endif

#define FLOW_TIMESTAMP_MASK       UINT64_C(0x00000000ffffffff)
#define FLOW_TIMESTAMP_HALF_RANGE UINT64_C(0x0000000080000000)

static inline u8
flow_timestamp_shift_clamp(unsigned shift)
{
    return (u8)((shift <= FLOW_TIMESTAMP_MAX_SHIFT)
        ? shift : FLOW_TIMESTAMP_DEFAULT_SHIFT);
}

static inline u64
flow_timestamp_encode(u64 now, unsigned shift)
{
    return (now >> flow_timestamp_shift_clamp(shift)) & FLOW_TIMESTAMP_MASK;
}

static inline u64
flow_timestamp_timeout_encode(u64 timeout, unsigned shift)
{
    u64 encoded = timeout >> flow_timestamp_shift_clamp(shift);

    if (encoded > FLOW_TIMESTAMP_MASK)
        encoded = FLOW_TIMESTAMP_MASK;
    if (timeout != 0u && encoded == 0u)
        encoded = 1u;
    return encoded;
}

static inline u64
flow_timestamp_get(const struct flow_entry_meta *meta)
{
    return meta->timestamp;
}

static inline void
flow_timestamp_set_raw(struct flow_entry_meta *meta, u64 encoded)
{
    encoded &= FLOW_TIMESTAMP_MASK;
    meta->timestamp = (u32)encoded;
}

static inline void
flow_timestamp_store(struct flow_entry_meta *meta, u64 now,
                     unsigned shift)
{
    flow_timestamp_set_raw(meta, flow_timestamp_encode(now, shift));
}

static inline void
flow_timestamp_touch(struct flow_entry_meta *meta, u64 now,
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

static inline u64
flow_timestamp_elapsed(u64 now_encoded, u64 ts_encoded)
{
    return (now_encoded - ts_encoded) & FLOW_TIMESTAMP_MASK;
}

static inline int
flow_timestamp_is_expired_raw(u64 ts_encoded, u64 now_encoded,
                              u64 timeout_encoded)
{
    if (ts_encoded == 0u)
        return 0;
    return flow_timestamp_elapsed(now_encoded, ts_encoded) > timeout_encoded;
}

static inline int
flow_timestamp_is_expired(const struct flow_entry_meta *meta, u64 now,
                          u64 timeout, unsigned shift)
{
    return flow_timestamp_is_expired_raw(flow_timestamp_get(meta),
        flow_timestamp_encode(now, shift),
        flow_timestamp_timeout_encode(timeout, shift));
}

struct flow4_key {
    u8  family;
    u8  proto;
    u16 src_port;
    u16 dst_port;
    u16 pad;
    u32 vrfid;
    u32 src_ip;
    u32 dst_ip;
    u32 zero;
};

struct flow4_entry {
    struct flow4_key key;
    struct flow_entry_meta meta;
};

struct flow6_key {
    u8  family;
    u8  proto;
    u16 src_port;
    u16 dst_port;
    u16 pad;
    u32 vrfid;
    u8 src_ip[16];
    u8 dst_ip[16];
} __attribute__((packed));

struct flow6_entry {
    struct flow6_key key;
    struct flow_entry_meta meta;
};

struct flowu_key {
    u8  family;
    u8  proto;
    u16 src_port;
    u16 dst_port;
    u16 pad;
    u32 vrfid;
    union {
        struct {
            u32 src;
            u32 dst;
            u8  _pad[24];
        } v4;
        struct {
            u8 src[16];
            u8 dst[16];
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
flow4_key_hash(const struct flow4_key *key, u32 mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    union rix_hash_hash_u r;
    u64 w0, w1, w2;
    u32 h0, bk0, seed, h1;

    memcpy(&w0, (const char *)key,      8u);
    memcpy(&w1, (const char *)key + 8u,  8u);
    memcpy(&w2, (const char *)key + 16u, 8u);
    h0  = (u32)__builtin_ia32_crc32di(0ULL,          w0);
    h0  = (u32)__builtin_ia32_crc32di((u64)h0,  w1);
    h0  = (u32)__builtin_ia32_crc32di((u64)h0,  w2);
    h0 |= !h0;
    bk0  = h0 & mask;
    seed = ~h0;
    do {
        h1   = (u32)__builtin_ia32_crc32di((u64)seed, w0);
        h1   = (u32)__builtin_ia32_crc32di((u64)h1,   w1);
        h1   = (u32)__builtin_ia32_crc32di((u64)h1,   w2);
        h1 |= !h1;
        seed = (u32)__builtin_ia32_crc32di((u64)seed, (u64)h0);
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
 * 24B = 3 x u64 XOR-OR.
 */
static inline int
flow4_key_cmp(const struct flow4_key *a, const struct flow4_key *b)
{
    u64 a0, a1, a2, b0, b1, b2;

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

#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
