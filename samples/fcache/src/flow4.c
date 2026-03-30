/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>
#include <string.h>

#include "flow4_cache.h"

/*
 * Direct CRC32C hash for 24B flow4 key -- bypasses rix_hash_arch->hash_bytes
 * function-pointer dispatch.  24B = 3 x crc32q, no remainder handling.
 */
static inline union rix_hash_hash_u
fc_flow4_hash_fn(const struct flow4_key *key, uint32_t mask)
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
    bk0  = h0 & mask;
    seed = ~h0;
    do {
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, w0);
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1,   w1);
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1,   w2);
        seed = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, (uint64_t)h0);
    } while ((h1 & mask) == bk0);
    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
#else
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
#endif
}

/*
 * Inline 24B key comparison -- avoids function-pointer overhead.
 * 24B = 3 x uint64_t XOR-OR.
 */
static inline int
fc_flow4_cmp(const struct flow4_key *a, const struct flow4_key *b)
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

static inline struct fc_flow4_entry *
fc_flow4_layout_entry_ptr_(const struct fc_flow4_cache *fc, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FC_RECORD_MEMBER_PTR(fc->pool_base, fc->pool_stride, idx,
                                fc->pool_entry_offset,
                                struct fc_flow4_entry);
}

static inline unsigned
fc_flow4_layout_entry_idx_(const struct fc_flow4_cache *fc,
                           const struct fc_flow4_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return fc_record_index_from_member_ptr(fc->pool_base, fc->pool_stride,
                                           fc->pool_entry_offset, entry);
}

static inline void
fc_flow4_event_emit_idx_(struct fc_flow4_cache *fc,
                         enum fc_flow4_event event,
                         unsigned idx)
{
    if (RIX_UNLIKELY(fc->event_cb != NULL) && idx != RIX_NIL)
        fc->event_cb(event, idx, fc->event_cb_arg);
}

#define FCG_LAYOUT_INIT_STORAGE(fc, array, stride, entry_offset)               \
    do {                                                                       \
        (fc)->pool_base = (unsigned char *)(array);                            \
        (fc)->pool_stride = (stride);                                          \
        (fc)->pool_entry_offset = (entry_offset);                              \
        (fc)->pool = FC_RECORD_MEMBER_PTR((fc)->pool_base, (fc)->pool_stride, \
                                          1u, (fc)->pool_entry_offset,         \
                                          struct fc_flow4_entry);              \
    } while (0)

#define FCG_LAYOUT_HASH_BASE(fc)                                               \
    ((struct fc_flow4_entry *)(void *)(fc))

#define FCG_LAYOUT_ENTRY_PTR(fc, idx) fc_flow4_layout_entry_ptr_((fc), (idx))
#define FCG_LAYOUT_ENTRY_INDEX(fc, entry)                                      \
    fc_flow4_layout_entry_idx_((fc), (entry))

#define FCG_LAYOUT_ENTRY_AT(fc, off0)                                          \
    fc_flow4_layout_entry_ptr_((fc), (unsigned)(off0) + 1u)

#define FCG_EVENT_EMIT_ALLOC(fc, idx)                                          \
    fc_flow4_event_emit_idx_((fc), FC_FLOW4_EVENT_ALLOC, (idx))

#define FCG_EVENT_EMIT_FREE(fc, idx, reason)                                   \
    fc_flow4_event_emit_idx_((fc), (enum fc_flow4_event)(reason), (idx))

#define FCG_EVENT_REASON_DELETE   FC_FLOW4_EVENT_FREE_DELETE
#define FCG_EVENT_REASON_TIMEOUT  FC_FLOW4_EVENT_FREE_TIMEOUT
#define FCG_EVENT_REASON_PRESSURE FC_FLOW4_EVENT_FREE_PRESSURE
#define FCG_EVENT_REASON_OLDEST   FC_FLOW4_EVENT_FREE_OLDEST
#define FCG_EVENT_REASON_FLUSH    FC_FLOW4_EVENT_FREE_FLUSH
#define FCG_EVENT_REASON_ROLLBACK FC_FLOW4_EVENT_FREE_ROLLBACK

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct fc_flow4_cache *fc =                                         \
        (const struct fc_flow4_cache *)(const void *)base;                    \
    return fc_flow4_layout_entry_idx_(fc, p);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                               \
name##_hptr(struct type *base, unsigned i)                                     \
{                                                                             \
    const struct fc_flow4_cache *fc =                                         \
        (const struct fc_flow4_cache *)(const void *)base;                    \
    return fc_flow4_layout_entry_ptr_(fc, i);                                 \
}

#include "fc_cache_generate.h"

FC_CACHE_GENERATE(flow4, FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
                   fc_flow4_hash_fn, fc_flow4_cmp)

void _FCG_API(flow4, init_ex)(struct fc_flow4_cache *fc,
                              struct rix_hash_bucket_s *buckets,
                              unsigned nb_bk,
                              void *array,
                              unsigned max_entries,
                              size_t stride,
                              size_t entry_offset,
                              const struct fc_flow4_config *cfg);

void
_FCG_API(flow4, init_ex)(struct fc_flow4_cache *fc,
                         struct rix_hash_bucket_s *buckets,
                         unsigned nb_bk,
                         void *array,
                         unsigned max_entries,
                         size_t stride,
                         size_t entry_offset,
                         const struct fc_flow4_config *cfg)
{
    _FCG_INIT_EX_BODY(flow4, FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
                      fc, buckets, nb_bk, array, max_entries,
                      stride, entry_offset, cfg);
}

void _FCG_API(flow4, findadd_burst32)(struct fc_flow4_cache *fc,
                                      const struct flow4_key *keys,
                                      unsigned nb_keys,
                                      uint64_t now,
                                      struct fc_flow4_result *results);

void
_FCG_API(flow4, findadd_burst32)(struct fc_flow4_cache *fc,
                                 const struct flow4_key *keys,
                                 unsigned nb_keys,
                                 uint64_t now,
                                 struct fc_flow4_result *results)
{
    _FCG_FINDADD_BURST32_BODY(flow4, fc, keys, nb_keys, now, results);
}

#ifdef FC_ARCH_SUFFIX
#include "fc_ops.h"
FC_OPS_TABLE(flow4, FC_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
