/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_EXTRA_KEY_H_
#define _FLOW_EXTRA_KEY_H_

#include <stdint.h>

#include <rix/rix_hash_slot_extra.h>

#include "flow_key.h"   /* reuse FLOW_TIMESTAMP_* encoding, flowX_key structs */

struct flow_entry_meta_extra {
    u32 cur_hash;   /* hash_field */
    u16 slot;       /* slot_field */
    u16 reserved0;
};

_Static_assert(sizeof(struct flow_entry_meta_extra) == 8u,
               "flow_entry_meta_extra must be 8 bytes");

/* flow4_extra_key uses first 16B of flow4_key layout. */
struct flow4_extra_key {
    u8  family;
    u8  proto;
    u16 src_port;
    u16 dst_port;
    u16 pad;
    u32 src_addr;
    u32 dst_addr;
};

_Static_assert(sizeof(struct flow4_extra_key) == 16u,
               "flow4_extra_key must be 16 bytes");

struct flow4_extra_entry {
    struct flow4_extra_key       key;   /* +0  : 16 B */
    struct flow_entry_meta_extra meta;  /* +16 :  8 B */
};

_Static_assert(offsetof(struct flow4_extra_entry, meta) == 16u,
               "flow4_extra_entry.meta must be at offset 16");

static inline u32
flow_extra_ts_get(const struct rix_hash_bucket_extra_s *bk, unsigned slot)
{
    return bk->extra[slot];
}

static inline void
flow_extra_ts_set(struct rix_hash_bucket_extra_s *bk, unsigned slot,
                  u32 encoded)
{
    bk->extra[slot] = encoded;
}

static inline int
flow_extra_ts_is_permanent(u32 encoded)
{
    return encoded == 0u;
}

/* Encode/decode re-use classic's flow_timestamp_encode /
 * flow_timestamp_is_expired_raw so classic <-> extra comparisons are
 * bit-exact. */

#endif /* _FLOW_EXTRA_KEY_H_ */
