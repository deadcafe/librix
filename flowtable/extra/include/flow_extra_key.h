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

/*
 * Bare bucket-side timestamp accessors.  Use these only when (bk, slot) are
 * already fully derived in the local context (e.g. test setup, bench prep,
 * post-find inline writes where scan has just produced the bk pointer).
 *
 * For higher-level code that wants validation against an expected entry index,
 * prefer rix_hash_slot_extra_set() / rix_hash_slot_extra_get() /
 * rix_hash_slot_extra_touch_2bk() in <rix/rix_hash_slot_extra.h>.
 */

/**
 * @brief Read @c bk->extra[slot] without any validation.
 */
static inline u32
flow_extra_ts_get(const struct rix_hash_bucket_extra_s *bk, unsigned slot)
{
    return bk->extra[slot];
}

/**
 * @brief Write @p encoded into @c bk->extra[slot] without any validation.
 */
static inline void
flow_extra_ts_set(struct rix_hash_bucket_extra_s *bk, unsigned slot,
                  u32 encoded)
{
    bk->extra[slot] = encoded;
}

/**
 * @brief Check whether @p encoded is the "permanent" timestamp sentinel
 *        (0).  Permanent entries are skipped by maintain expiry.
 */
static inline int
flow_extra_ts_is_permanent(u32 encoded)
{
    return encoded == 0u;
}

/* Encode/decode re-use classic's flow_timestamp_encode /
 * flow_timestamp_is_expired_raw so classic <-> extra comparisons are
 * bit-exact. */

/**
 * @brief Encode a TSC value @p now with shift @p shift into the 32-bit
 *        bucket extra[] timestamp form used by the slot_extra variant.
 *
 * Narrowing wrapper around flow_timestamp_encode(): bk->extra[] is u32 so
 * a u32 return avoids casts at every store site.  The underlying encode
 * already masks to FLOW_TIMESTAMP_MASK (32 bits) so this is safe.
 */
static inline u32
flow_extra_timestamp_encode(u64 now, unsigned shift)
{
    return (u32)flow_timestamp_encode(now, shift);
}

#endif /* _FLOW_EXTRA_KEY_H_ */
