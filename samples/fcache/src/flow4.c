/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>
#include <string.h>

#include "flow4_cache.h"
#include "fc_cache_generate.h"

/*
 * Direct CRC32C hash for 24B flow4 key -- bypasses rix_hash_arch->hash_bytes
 * function-pointer dispatch.  24B = 3 x crc32q, no remainder handling.
 */
static inline union rix_hash_hash_u
fc_flow4_hash_fn(const struct fc_flow4_key *key, uint32_t mask)
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
fc_flow4_cmp(const struct fc_flow4_key *a, const struct fc_flow4_key *b)
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

FC_CACHE_GENERATE(flow4, FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
                   fc_flow4_hash_fn, fc_flow4_cmp)

static inline void
fc_flow4_findadd_resolve_ctx(struct fc_flow4_cache *fc,
                             const struct fc_flow4_key *keys,
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

    entry = _FCG_HT(flow4, cmp_key_empties)(ctx, fc->pool);
    if (RIX_LIKELY(entry != NULL)) {
        entry->last_ts = now;
        _FCG_INT(flow4, result_set_hit)(&results[idx],
                                        RIX_IDX_FROM_PTR(fc->pool, entry));
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
            if (_FCG_INT(flow4, reclaim_bucket)(fc, bk0i, expire_before)) {
                fc->stats.relief_evictions++;
                fc->stats.relief_bk0_evictions++;
            } else {
                fc->stats.relief_bucket_checks++;
                if ((unsigned)__builtin_popcount(ctx->empties[1]) <= pressure_empty &&
                    bk1i != bk0i &&
                    _FCG_INT(flow4, reclaim_bucket)(fc, bk1i, expire_before)) {
                    fc->stats.relief_evictions++;
                    fc->stats.relief_bk1_evictions++;
                }
            }
        }
    }

    entry = _FCG_INT(flow4, alloc_entry)(fc);
    if (RIX_UNLIKELY(entry == NULL) &&
        _FCG_INT(flow4, reclaim_oldest_global)(fc, now)) {
        entry = _FCG_INT(flow4, alloc_entry)(fc);
    }
    if (RIX_UNLIKELY(entry == NULL)) {
        fc->stats.fill_full++;
        _FCG_INT(flow4, result_set_miss)(&results[idx]);
        return;
    }

    entry->key = keys[idx];
    entry->last_ts = now;
    {
        struct fc_flow4_entry *ret;

        ret = _FCG_HT(flow4, insert_hashed)(&fc->ht_head, fc->buckets,
                                            fc->pool, entry, ctx->hash);
        if (RIX_LIKELY(ret == NULL)) {
            fc->stats.fills++;
            _FCG_INT(flow4, result_set_filled)(
                &results[idx], RIX_IDX_FROM_PTR(fc->pool, entry));
        } else {
            if (ret == entry &&
                _FCG_INT(flow4, reclaim_oldest_global)(fc, now)) {
                ret = _FCG_HT(flow4, insert_hashed)(&fc->ht_head,
                                                    fc->buckets, fc->pool,
                                                    entry, ctx->hash);
            }
            if (ret == NULL) {
                fc->stats.fills++;
                _FCG_INT(flow4, result_set_filled)(
                    &results[idx], RIX_IDX_FROM_PTR(fc->pool, entry));
            } else if (ret != entry) {
                RIX_ASSUME_NONNULL(entry);
                _FCG_INT(flow4, free_entry)(fc, entry);
                RIX_ASSUME_NONNULL(ret);
                ret->last_ts = now;
                _FCG_INT(flow4, result_set_filled)(
                    &results[idx], RIX_IDX_FROM_PTR(fc->pool, ret));
            } else {
                RIX_ASSUME_NONNULL(entry);
                _FCG_INT(flow4, free_entry)(fc, entry);
                fc->stats.fill_full++;
                _FCG_INT(flow4, result_set_miss)(&results[idx]);
            }
        }
    }

    {
        struct fc_flow4_entry *next_free =
            RIX_SLIST_FIRST(&fc->free_head, fc->pool);
        if (next_free != NULL)
            rix_hash_prefetch_entry(next_free);
    }
}

void _FCG_API(flow4, findadd_burst32)(struct fc_flow4_cache *fc,
                                      const struct fc_flow4_key *keys,
                                      unsigned nb_keys,
                                      uint64_t now,
                                      struct fc_flow4_result *results);

void
_FCG_API(flow4, findadd_burst32)(struct fc_flow4_cache *fc,
                                 const struct fc_flow4_key *keys,
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
        struct fc_flow4_entry *free_head =
            RIX_SLIST_FIRST(&fc->free_head, fc->pool);
        if (free_head != NULL)
            rix_hash_prefetch_entry(free_head);
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
                    &ctx[(base + j) & (FC_CACHE_BULK_CTX_COUNT - 1u)], fc->pool);
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
