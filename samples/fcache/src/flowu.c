/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>
#include <string.h>

#include "flowu_cache.h"

static inline union rix_hash_hash_u
fc_flowu_hash_fn(const struct flowu_key *key, uint32_t mask)
{
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
}

static inline int
fc_flowu_cmp(const struct flowu_key *a, const struct flowu_key *b)
{
    return memcmp(a, b, sizeof(*a));
}

static inline struct fc_flowu_entry *
fc_flowu_layout_entry_ptr_(const struct fc_flowu_cache *fc, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FC_RECORD_MEMBER_PTR(fc->pool_base, fc->pool_stride, idx,
                                fc->pool_entry_offset,
                                struct fc_flowu_entry);
}

static inline unsigned
fc_flowu_layout_entry_idx_(const struct fc_flowu_cache *fc,
                           const struct fc_flowu_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return fc_record_index_from_member_ptr(fc->pool_base, fc->pool_stride,
                                           fc->pool_entry_offset, entry);
}

static inline void
fc_flowu_event_emit_idx_(struct fc_flowu_cache *fc,
                         enum fc_flowu_event event,
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
                                          struct fc_flowu_entry);              \
    } while (0)

#define FCG_LAYOUT_HASH_BASE(fc)                                               \
    ((struct fc_flowu_entry *)(void *)(fc))

#define FCG_LAYOUT_ENTRY_PTR(fc, idx) fc_flowu_layout_entry_ptr_((fc), (idx))
#define FCG_LAYOUT_ENTRY_INDEX(fc, entry)                                      \
    fc_flowu_layout_entry_idx_((fc), (entry))

#define FCG_LAYOUT_ENTRY_AT(fc, off0)                                          \
    fc_flowu_layout_entry_ptr_((fc), (unsigned)(off0) + 1u)

#define FCG_EVENT_EMIT_ALLOC(fc, idx)                                          \
    fc_flowu_event_emit_idx_((fc), FC_FLOWU_EVENT_ALLOC, (idx))

#define FCG_EVENT_EMIT_FREE(fc, idx, reason)                                   \
    fc_flowu_event_emit_idx_((fc), (enum fc_flowu_event)(reason), (idx))

#define FCG_EVENT_REASON_DELETE   FC_FLOWU_EVENT_FREE_DELETE
#define FCG_EVENT_REASON_TIMEOUT  FC_FLOWU_EVENT_FREE_TIMEOUT
#define FCG_EVENT_REASON_PRESSURE FC_FLOWU_EVENT_FREE_PRESSURE
#define FCG_EVENT_REASON_OLDEST   FC_FLOWU_EVENT_FREE_OLDEST
#define FCG_EVENT_REASON_FLUSH    FC_FLOWU_EVENT_FREE_FLUSH
#define FCG_EVENT_REASON_ROLLBACK FC_FLOWU_EVENT_FREE_ROLLBACK

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct fc_flowu_cache *fc =                                         \
        (const struct fc_flowu_cache *)(const void *)base;                    \
    return fc_flowu_layout_entry_idx_(fc, p);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct fc_flowu_cache *fc =                                         \
        (const struct fc_flowu_cache *)(const void *)base;                    \
    return fc_flowu_layout_entry_ptr_(fc, i);                                 \
}

#include "fc_cache_generate.h"

FC_CACHE_GENERATE(flowu, FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS,
                   fc_flowu_hash_fn, fc_flowu_cmp)

void _FCG_API(flowu, init_ex)(struct fc_flowu_cache *fc,
                              struct rix_hash_bucket_s *buckets,
                              unsigned nb_bk,
                              void *array,
                              unsigned max_entries,
                              size_t stride,
                              size_t entry_offset,
                              const struct fc_flowu_config *cfg);

void
_FCG_API(flowu, init_ex)(struct fc_flowu_cache *fc,
                         struct rix_hash_bucket_s *buckets,
                         unsigned nb_bk,
                         void *array,
                         unsigned max_entries,
                         size_t stride,
                         size_t entry_offset,
                         const struct fc_flowu_config *cfg)
{
    _FCG_INIT_EX_BODY(flowu, FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS,
                      fc, buckets, nb_bk, array, max_entries,
                      stride, entry_offset, cfg);
}

void _FCG_API(flowu, findadd_burst32)(struct fc_flowu_cache *fc,
                                      const struct flowu_key *keys,
                                      unsigned nb_keys,
                                      uint64_t now,
                                      struct fc_flowu_result *results);

void
_FCG_API(flowu, findadd_burst32)(struct fc_flowu_cache *fc,
                                 const struct flowu_key *keys,
                                 unsigned nb_keys,
                                 uint64_t now,
                                 struct fc_flowu_result *results)
{
    _FCG_FINDADD_BURST32_BODY(flowu, fc, keys, nb_keys, now, results);
}

#ifdef FC_ARCH_SUFFIX
#include "fc_ops.h"
FC_OPS_TABLE(flowu, FC_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
