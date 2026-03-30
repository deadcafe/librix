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
    struct fc_flow4_config defcfg = {
        .timeout_tsc = UINT64_C(1000000),
        .pressure_empty_slots = FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
    };

    if (cfg == NULL)
        cfg = &defcfg;

    memset(fc, 0, sizeof(*fc));
    memset(buckets, 0, (size_t)nb_bk * sizeof(*buckets));
    FCG_LAYOUT_INIT_STORAGE(fc, array, stride, entry_offset);
    fc->buckets = buckets;
    fc->nb_bk = nb_bk;
    fc->max_entries = max_entries;
    fc->total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    fc->timeout_tsc = cfg->timeout_tsc;
    fc->eff_timeout_tsc = cfg->timeout_tsc ? cfg->timeout_tsc : 1u;
    fc->pressure_empty_slots = cfg->pressure_empty_slots ?
        cfg->pressure_empty_slots : FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    fc->maint_interval_tsc = cfg->maint_interval_tsc;
    fc->maint_base_bk = cfg->maint_base_bk ? cfg->maint_base_bk : nb_bk;
    fc->maint_fill_threshold = cfg->maint_fill_threshold;
    _FCG_INT(flow4, init_thresholds)(fc);
    FCG_FREE_LIST_INIT(fc);
    _FCG_HT(flow4, init)(&fc->ht_head, nb_bk);
    for (unsigned i = max_entries; i > 0u; i--) {
        struct fc_flow4_entry *entry = FCG_LAYOUT_ENTRY_PTR(fc, i);

        RIX_ASSUME_NONNULL(entry);
        FCG_LAYOUT_ENTRY_CLEAR(fc, entry);
        FCG_FREE_LIST_PUSH_ENTRY(fc, entry, i);
    }
}

static inline void
fc_flow4_findadd_resolve_ctx(struct fc_flow4_cache *fc,
                             const struct flow4_key *keys,
                             uint64_t now,
                             struct fc_flow4_result *results,
                             struct rix_hash_find_ctx_s *ctx,
                             unsigned idx,
                             uint64_t *hit_count,
                             uint64_t *miss_count)
{
    unsigned bk0i = (unsigned)(ctx->bk[0] - fc->buckets);
    unsigned bk1i = (unsigned)(ctx->bk[1] - fc->buckets);
    struct fc_flow4_entry *entry;

    entry = _FCG_HT(flow4, cmp_key_empties)(ctx, FCG_LAYOUT_HASH_BASE(fc));
    if (RIX_LIKELY(entry != NULL)) {
        entry->last_ts = now;
        _FCG_INT(flow4, result_set_hit)(&results[idx],
                                        FCG_LAYOUT_ENTRY_INDEX(fc, entry));
        (*hit_count)++;
        return;
    }

    (*miss_count)++;
    if (fc->total_slots != 0u) {
        unsigned pressure_empty = _FCG_INT(flow4, relief_empty_slots)(fc);

        fc->stats.relief_calls++;
        fc->stats.relief_bucket_checks++;
        if ((unsigned)__builtin_popcount(ctx->empties[0]) <= pressure_empty) {
            uint64_t expire_before;

            _FCG_INT(flow4, update_eff_timeout)(fc);
            expire_before = (now > fc->eff_timeout_tsc)
                ? (now - fc->eff_timeout_tsc) : 0u;
            if (_FCG_INT(flow4, reclaim_bucket)(fc, bk0i, expire_before,
                                                FCG_EVENT_REASON_PRESSURE)) {
                fc->stats.relief_evictions++;
                fc->stats.relief_bk0_evictions++;
            } else {
                fc->stats.relief_bucket_checks++;
                if ((unsigned)__builtin_popcount(ctx->empties[1]) <= pressure_empty &&
                    bk1i != bk0i &&
                    _FCG_INT(flow4, reclaim_bucket)(fc, bk1i, expire_before,
                                                    FCG_EVENT_REASON_PRESSURE)) {
                    fc->stats.relief_evictions++;
                    fc->stats.relief_bk1_evictions++;
                }
            }
        }
    }

    entry = _FCG_INT(flow4, alloc_entry)(fc);
    if (RIX_UNLIKELY(entry == NULL) &&
        _FCG_INT(flow4, reclaim_oldest_global)(fc, now,
                                               FCG_EVENT_REASON_OLDEST)) {
        entry = _FCG_INT(flow4, alloc_entry)(fc);
    }
    if (RIX_UNLIKELY(entry == NULL)) {
        fc->stats.fill_full++;
        _FCG_INT(flow4, result_set_miss)(&results[idx]);
        return;
    }

    entry->hdr.key = keys[idx];
    entry->last_ts = now;
    {
        struct fc_flow4_entry *ret;

        ret = _FCG_HT(flow4, insert_hashed)(&fc->ht_head, fc->buckets,
                                            FCG_LAYOUT_HASH_BASE(fc),
                                            entry, ctx->hash);
        if (RIX_LIKELY(ret == NULL)) {
            fc->stats.fills++;
            _FCG_INT(flow4, result_set_filled)(
                &results[idx], FCG_LAYOUT_ENTRY_INDEX(fc, entry));
        } else {
            if (ret == entry &&
                _FCG_INT(flow4, reclaim_oldest_global)(fc, now,
                                                       FCG_EVENT_REASON_OLDEST)) {
                ret = _FCG_HT(flow4, insert_hashed)(&fc->ht_head,
                                                    fc->buckets,
                                                    FCG_LAYOUT_HASH_BASE(fc),
                                                    entry, ctx->hash);
            }
            if (ret == NULL) {
                fc->stats.fills++;
                _FCG_INT(flow4, result_set_filled)(
                    &results[idx], FCG_LAYOUT_ENTRY_INDEX(fc, entry));
            } else if (ret != entry) {
                RIX_ASSUME_NONNULL(entry);
                _FCG_INT(flow4, free_entry)(fc, entry,
                                            FCG_EVENT_REASON_ROLLBACK);
                RIX_ASSUME_NONNULL(ret);
                ret->last_ts = now;
                _FCG_INT(flow4, result_set_filled)(
                    &results[idx], FCG_LAYOUT_ENTRY_INDEX(fc, ret));
            } else {
                RIX_ASSUME_NONNULL(entry);
                _FCG_INT(flow4, free_entry)(fc, entry,
                                            FCG_EVENT_REASON_ROLLBACK);
                fc->stats.fill_full++;
                _FCG_INT(flow4, result_set_miss)(&results[idx]);
            }
        }
    }

    {
        struct fc_flow4_entry *next_free = FCG_FREE_LIST_FIRST_PTR(fc);
        if (next_free != NULL)
            rix_hash_prefetch_entry_of(next_free);
    }
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
    struct rix_hash_find_ctx_s stack_ctx[FC_CACHE_BULK_CTX_COUNT];
    struct rix_hash_find_ctx_s *ctx =
        (fc->bulk_ctx != NULL &&
         fc->bulk_ctx_count >= FC_CACHE_BULK_CTX_COUNT) ?
        fc->bulk_ctx : stack_ctx;
    uint64_t hit_count = 0u;
    uint64_t miss_count = 0u;
    unsigned step_keys;
    unsigned ahead_keys;
    unsigned total;

    if (nb_keys == 0u)
        return;

    {
        struct fc_flow4_entry *free_head = FCG_FREE_LIST_FIRST_PTR(fc);
        if (free_head != NULL)
            rix_hash_prefetch_entry_of(free_head);
    }

    if (nb_keys <= 4u) {
        step_keys = 1u;
        ahead_keys = 1u;
    } else if (nb_keys <= 8u) {
        step_keys = 2u;
        ahead_keys = 2u;
    } else if (nb_keys <= 16u) {
        step_keys = 4u;
        ahead_keys = 4u;
    } else {
        step_keys = 8u;
        ahead_keys = 8u;
    }

    total = nb_keys + 3u * ahead_keys;
    for (unsigned i = 0; i < total; i += step_keys) {
        if (i < nb_keys) {
            unsigned n = (i + step_keys <= nb_keys) ? step_keys : (nb_keys - i);

            for (unsigned j = 0; j < n; j++) {
                _FCG_HT(flow4, hash_key_2bk)(
                    &ctx[(i + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)], &fc->ht_head,
                                             fc->buckets, &keys[i + j]);
            }
        }
        if (i >= ahead_keys && i - ahead_keys < nb_keys) {
            unsigned base = i - ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++)
                _FCG_HT(flow4, scan_bk_empties)(
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],
                    &fc->ht_head, fc->buckets);
        }
        if (i >= 2u * ahead_keys && i - 2u * ahead_keys < nb_keys) {
            unsigned base = i - 2u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++)
                _FCG_HT(flow4, prefetch_node)(
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],
                    FCG_LAYOUT_HASH_BASE(fc));
        }
        if (i >= 3u * ahead_keys && i - 3u * ahead_keys < nb_keys) {
            unsigned base = i - 3u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++) {
                fc_flow4_findadd_resolve_ctx(fc, keys, now, results,
                                             &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)],
                                             base + j,
                                             &hit_count, &miss_count);
            }
        }
    }

    fc->stats.lookups += nb_keys;
    fc->stats.hits += hit_count;
    fc->stats.misses += miss_count;
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
