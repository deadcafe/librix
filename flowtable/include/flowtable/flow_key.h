/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_KEY_H_
#define _FLOW_KEY_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rix/rix_hash.h>

struct flow_entry_meta {
    u32 cur_hash;
    u16 slot;
    u16 reserved0;
    u32 timestamp;
};

_Static_assert(sizeof(struct flow_entry_meta) == 12u,
               "flow_entry_meta must be 12 bytes");
_Static_assert(_Alignof(struct flow_entry_meta) == _Alignof(u32),
               "flow_entry_meta must keep u32 alignment");

#ifndef FLOW_TIMESTAMP_DEFAULT_SHIFT
#define FLOW_TIMESTAMP_DEFAULT_SHIFT 4u
#endif

#ifndef FLOW_TIMESTAMP_MAX_SHIFT
#define FLOW_TIMESTAMP_MAX_SHIFT 24u
#endif

#define FLOW_TIMESTAMP_MASK       UINT64_C(0x00000000ffffffff)

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

_Static_assert(sizeof(struct flow4_key) == 24u,
               "flow4_key must be 24 bytes");

struct flow4_entry {
    struct flow4_key key;
    struct flow_entry_meta meta;
};

_Static_assert(offsetof(struct flow4_entry, meta) == sizeof(struct flow4_key),
               "flow4_entry.meta must follow key");
_Static_assert(sizeof(struct flow4_entry) == 36u,
               "flow4_entry must be 36 bytes");

/* Packed intentionally: flow6 hash input is the exact 44-byte key image. */
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

_Static_assert(sizeof(struct flow6_key) == 44u,
               "flow6_key must be 44 bytes");

struct flow6_entry {
    struct flow6_key key;
    struct flow_entry_meta meta;
};

_Static_assert(offsetof(struct flow6_entry, meta) == sizeof(struct flow6_key),
               "flow6_entry.meta must follow key");
_Static_assert(sizeof(struct flow6_entry) == 56u,
               "flow6_entry must be 56 bytes");

/* Packed intentionally: flowu keeps IPv4/IPv6 keys in one 44-byte image. */
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

_Static_assert(sizeof(struct flowu_key) == 44u,
               "flowu_key must be 44 bytes");

struct flowu_entry {
    struct flowu_key key;
    struct flow_entry_meta meta;
};

_Static_assert(offsetof(struct flowu_entry, meta) == sizeof(struct flowu_key),
               "flowu_entry.meta must follow key");
_Static_assert(sizeof(struct flowu_entry) == 56u,
               "flowu_entry must be 56 bytes");

#endif /* _FLOW_KEY_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
