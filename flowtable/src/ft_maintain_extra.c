/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * ft_maintain_extra.c - bucket maintenance using bk->extra[] timestamps.
 *
 * EXTRA: no entry-side cacheline loads in ft_table_extra_maintain.
 * maintain_idx_bulk still needs one entry load per input to resolve
 * (bucket, slot) from entry->meta; it then uses the extra scan for
 * the expire decision.
 *
 * Compiled once per arch variant (gen/sse/avx2/avx512) with FT_ARCH_SUFFIX.
 */

#include <string.h>

#if defined(__AVX512F__) || defined(__AVX2__) || defined(__SSE2__)
#include <immintrin.h>
#endif

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>
#include <rix/rix_defs_private.h>

#include "flowtable/flow_extra_common.h"
#include "flowtable/flow_extra_key.h"

/*===========================================================================
 * Arch-suffix name mangling
 *===========================================================================*/
#define _FTM_EXTRA_CAT2(a, b) a##b
#define _FTM_EXTRA_CAT(a, b)  _FTM_EXTRA_CAT2(a, b)

#ifdef FT_ARCH_SUFFIX
#define _FTM_EXTRA_FN(name) _FTM_EXTRA_CAT(name, FT_ARCH_SUFFIX)
#else
#define _FTM_EXTRA_FN(name) name
#endif

/* Forward declarations for -Wmissing-prototypes */
unsigned _FTM_EXTRA_FN(ft_table_extra_maintain)(
    const struct ft_maint_extra_ctx *, unsigned, u64, u64,
    u32 *, unsigned, unsigned, unsigned *);
unsigned _FTM_EXTRA_FN(ft_table_extra_maintain_idx_bulk)(
    const struct ft_maint_extra_ctx *, const u32 *, unsigned,
    u64, u64, u32 *, unsigned, unsigned, int);

/*
 * Get flow_entry_meta_extra pointer for a given entry index.
 * meta_off = pool_entry_offset + offsetof(flowX_extra_entry, meta)
 */
static inline const struct flow_entry_meta_extra *
ft_maint_extra_meta_(const struct ft_maint_extra_ctx *ctx, unsigned entry_idx)
{
    return (const struct flow_entry_meta_extra *)(const void *)(
        ctx->pool_base +
        RIX_IDX_TO_OFF0(entry_idx) * ctx->pool_stride +
        ctx->meta_off);
}

static inline unsigned
ft_maint_extra_bucket_of_meta_(const struct ft_maint_extra_ctx *ctx,
                               const struct flow_entry_meta_extra *meta)
{
    return meta->cur_hash & ctx->rhh_mask;
}

static inline int
ft_maint_extra_live_bucket_of_idx_(const struct ft_maint_extra_ctx *ctx,
                                   unsigned entry_idx,
                                   const struct flow_entry_meta_extra *meta,
                                   unsigned *bk_out)
{
    unsigned slot;
    unsigned bk;

    if (!RIX_IDX_IS_VALID(entry_idx, ctx->max_entries))
        return 0;

    slot = (unsigned)meta->slot;
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)
        return 0;

    bk = ft_maint_extra_bucket_of_meta_(ctx, meta);
    if (ctx->buckets[bk].idx[slot] != entry_idx)
        return 0;

    *bk_out = bk;
    return 1;
}

/*
 * EXTRA scan_bucket: reads hash[] for occupancy, extra[] for TS.
 * Never touches entry memory.
 *
 * Returns the updated expired count. If out_used_count is non-NULL, writes
 * the number of occupied slots observed during the scan.
 */
static unsigned
ft_maint_extra_scan_bucket_(const struct ft_maint_extra_ctx *ctx,
                            unsigned bk_idx,
                            u64 now_ts,
                            u64 timeout_ts,
                            u32 *expired_idxv,
                            unsigned out_pos,
                            unsigned max_expired,
                            unsigned min_bk_entries,
                            unsigned *out_used_count)
{
    struct rix_hash_bucket_extra_s *bk = ctx->buckets + bk_idx;
    u32 used_bits;
    u32 expired_bits;
    unsigned used_count;

    RIX_ASSERT(RIX_HASH_BUCKET_ENTRY_SZ == 16u);

    /*
     * Sparse-bucket fast path.  When the caller demands that a bucket carry
     * at least @min_bk_entries occupied slots, count occupancy from hash[]
     * alone (one cache line) and skip loading extra[] / running the SIMD
     * timestamp compare when the bucket is below threshold.
     */
    if (min_bk_entries > 1u) {
        u32 empty_bits = RIX_HASH_FIND_U32X16(bk->hash, 0u);
        u32 early_used_bits = (~empty_bits) & 0xffffu;
        unsigned early_used_count =
            (unsigned)__builtin_popcount(early_used_bits);

        if (early_used_count < min_bk_entries) {
            if (out_used_count != NULL)
                *out_used_count = early_used_count;
            return out_pos;
        }
    }

#if defined(__AVX512F__)
    {
        /*
         * 16-wide u32 lanes: one 512-bit load each for hash[] and
         * extra[].  Native cmpneq_epu32 / cmpgt_epu32 mask
         * intrinsics directly produce the 16-bit used / expired
         * bitsets, no movemask plumbing needed.
         */
        __m512i hash_v = _mm512_loadu_si512((const void *)&bk->hash[0]);
        __m512i ts_v   = _mm512_loadu_si512((const void *)&bk->extra[0]);
        __m512i now_v  = _mm512_set1_epi32((int)(u32)now_ts);
        __m512i tmo_v  = _mm512_set1_epi32((int)(u32)timeout_ts);
        __m512i diff_v = _mm512_sub_epi32(now_v, ts_v);
        __mmask16 used_mask = _mm512_cmpneq_epu32_mask(hash_v,
                                  _mm512_setzero_si512());
        __mmask16 exp_mask  = _mm512_cmpgt_epu32_mask(diff_v, tmo_v);

        used_bits    = (u32)used_mask;
        expired_bits = (u32)(used_mask & exp_mask);
    }
#elif defined(__AVX2__)
    {
        /*
         * Two 256-bit halves.  AVX2 has only signed cmpgt_epi32, so
         * the unsigned (now-ts) > timeout test is rewritten as a
         * signed compare on values biased by 0x80000000.
         */
        const __m256i bias  = _mm256_set1_epi32((int)0x80000000);
        const __m256i zero  = _mm256_setzero_si256();
        __m256i now_v       = _mm256_set1_epi32((int)(u32)now_ts);
        __m256i tmo_b       = _mm256_xor_si256(
                                  _mm256_set1_epi32((int)(u32)timeout_ts),
                                  bias);
        __m256i hash_lo     = _mm256_loadu_si256(
                                  (const __m256i_u *)&bk->hash[0]);
        __m256i hash_hi     = _mm256_loadu_si256(
                                  (const __m256i_u *)&bk->hash[8]);
        __m256i ts_lo       = _mm256_loadu_si256(
                                  (const __m256i_u *)&bk->extra[0]);
        __m256i ts_hi       = _mm256_loadu_si256(
                                  (const __m256i_u *)&bk->extra[8]);
        __m256i diff_lo     = _mm256_sub_epi32(now_v, ts_lo);
        __m256i diff_hi     = _mm256_sub_epi32(now_v, ts_hi);
        __m256i diff_lo_b   = _mm256_xor_si256(diff_lo, bias);
        __m256i diff_hi_b   = _mm256_xor_si256(diff_hi, bias);
        __m256i exp_lo      = _mm256_cmpgt_epi32(diff_lo_b, tmo_b);
        __m256i exp_hi      = _mm256_cmpgt_epi32(diff_hi_b, tmo_b);
        __m256i used_lo     = _mm256_xor_si256(
                                  _mm256_cmpeq_epi32(hash_lo, zero),
                                  _mm256_set1_epi32(-1));
        __m256i used_hi     = _mm256_xor_si256(
                                  _mm256_cmpeq_epi32(hash_hi, zero),
                                  _mm256_set1_epi32(-1));
        unsigned ulo = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
                                                       used_lo));
        unsigned uhi = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
                                                       used_hi));
        unsigned elo = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
                                                       exp_lo));
        unsigned ehi = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
                                                       exp_hi));

        used_bits    = ulo | (uhi << 8);
        expired_bits = used_bits & (elo | (ehi << 8));
    }
#else
    {
        u32 ub = 0u;
        u32 eb = 0u;

        for (unsigned slot = 0; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {
            if (bk->hash[slot] != 0u)
                ub |= (u32)(1u << slot);
        }
        {
            u32 u = ub;

            while (u != 0u) {
                unsigned slot = (unsigned)__builtin_ctz(u);
                u32 ts = bk->extra[slot];

                u &= u - 1u;
                if (flow_timestamp_is_expired_raw(ts, now_ts,
                                                  timeout_ts))
                    eb |= (u32)(1u << slot);
            }
        }
        used_bits    = ub;
        expired_bits = eb;
    }
#endif

    used_count = (unsigned)__builtin_popcount(used_bits);
    if (out_used_count != NULL)
        *out_used_count = used_count;
    if (min_bk_entries > 1u && used_count < min_bk_entries)
        return out_pos;

    ctx->stats->maint_extras_loaded++;

    while (expired_bits != 0u && out_pos < max_expired) {
        unsigned slot = (unsigned)__builtin_ctz(expired_bits);
        unsigned idx = bk->idx[slot];

        expired_bits &= expired_bits - 1u;
        RIX_ASSERT(idx != (unsigned)RIX_NIL);

        bk->hash[slot]  = 0u;
        bk->idx[slot]   = (u32)RIX_NIL;
        bk->extra[slot] = 0u;
        (*ctx->rhh_nb)--;

        expired_idxv[out_pos++] = (u32)idx;
    }
    return out_pos;
}

unsigned
_FTM_EXTRA_FN(ft_table_extra_maintain)(const struct ft_maint_extra_ctx *ctx,
                                       unsigned start_bk,
                                       u64 now,
                                       u64 expire_tsc,
                                       u32 *expired_idxv,
                                       unsigned max_expired,
                                       unsigned min_bk_entries,
                                       unsigned *next_bk)
{
    unsigned mask;
    unsigned cur_bk;
    unsigned nb_bk;
    unsigned count = 0u;
    u64 now_ts;
    u64 timeout_ts;

    if (ctx == NULL || max_expired == 0u || expired_idxv == NULL) {
        if (next_bk != NULL)
            *next_bk = start_bk;
        return 0u;
    }
    if (expire_tsc == 0u) {
        if (next_bk != NULL)
            *next_bk = start_bk;
        return 0u;
    }

    ctx->stats->maint_calls++;

    mask = ctx->rhh_mask;
    nb_bk = mask + 1u;
    cur_bk = start_bk & mask;

    now_ts = flow_timestamp_encode(now, ctx->ts_shift);
    timeout_ts = flow_timestamp_timeout_encode(expire_tsc, ctx->ts_shift);

    for (unsigned i = 0u; i < nb_bk; i++) {
        unsigned next = (cur_bk + 1u) & mask;

        /* Prefetch next bucket: hashes + indices + extras. */
        rix_hash_prefetch_extra_bucket_full_of(&ctx->buckets[next]);

        ctx->stats->maint_bucket_checks++;

        count = ft_maint_extra_scan_bucket_(ctx, cur_bk, now_ts, timeout_ts,
                                            expired_idxv, count, max_expired,
                                            min_bk_entries, NULL);
        cur_bk = next;

        if (count >= max_expired)
            break;
    }

    ctx->stats->maint_evictions += count;

    if (next_bk != NULL)
        *next_bk = cur_bk;
    return count;
}

/*
 * Core pipeline loop for idx_bulk maintenance.
 * The use_filter parameter is a compile-time constant when inlined,
 * allowing the compiler to eliminate the filter branch entirely.
 *
 * EXTRA: the scan itself does not load entry cachelines, so the per-slot
 * metas[] staging inside ft_maint_scan_bucket_ is gone. We still stage
 * one entry meta read per input (to resolve bk, slot from entry->meta)
 * and prefetch the bucket indices of each resolved bucket.
 */
static inline unsigned
ft_maint_extra_idx_bulk_loop_(const struct ft_maint_extra_ctx *ctx,
                              const u32 *entry_idxv,
                              unsigned nb_idx,
                              u64 now_ts,
                              u64 timeout_ts,
                              u32 *expired_idxv,
                              unsigned max_expired,
                              unsigned min_bk_entries,
                              int use_filter)
{
    enum { FT_MAINT_HIT_STEP = 4u, FT_MAINT_HIT_AHEAD = 8u };
    enum { FT_MAINT_FILTER_SZ = 64u, FT_MAINT_FILTER_MASK = 63u };
    const struct flow_entry_meta_extra *meta_ring[16];
    unsigned bk_ring[16];
    u8 valid_ring[16];
    unsigned scanned[FT_MAINT_FILTER_SZ];
    unsigned count = 0u;
    unsigned total;

    RIX_ASSERT((FT_MAINT_HIT_AHEAD * 2u) <= 16u);

    memset(meta_ring, 0, sizeof(meta_ring));
    memset(bk_ring, 0, sizeof(bk_ring));
    memset(valid_ring, 0, sizeof(valid_ring));
    if (use_filter)
        memset(scanned, 0xff, sizeof(scanned));
    total = nb_idx + (FT_MAINT_HIT_AHEAD * 2u);

    for (unsigned i = 0u; i < total; i += FT_MAINT_HIT_STEP) {
        if (i < nb_idx) {
            unsigned n = (i + FT_MAINT_HIT_STEP <= nb_idx)
                       ? FT_MAINT_HIT_STEP : (nb_idx - i);

            for (unsigned j = 0u; j < n; j++) {
                unsigned entry_idx = entry_idxv[i + j];

                if (RIX_IDX_IS_VALID(entry_idx, ctx->max_entries))
                    __builtin_prefetch(ft_maint_extra_meta_(ctx, entry_idx),
                                       0, 3);
            }
        }

        if (i >= FT_MAINT_HIT_AHEAD && i - FT_MAINT_HIT_AHEAD < nb_idx) {
            unsigned base = i - FT_MAINT_HIT_AHEAD;
            unsigned n = (base + FT_MAINT_HIT_STEP <= nb_idx)
                       ? FT_MAINT_HIT_STEP : (nb_idx - base);

            for (unsigned j = 0u; j < n; j++) {
                unsigned entry_idx = entry_idxv[base + j];
                unsigned pos = (base + j) & 15u;
                const struct flow_entry_meta_extra *meta;

                if (!RIX_IDX_IS_VALID(entry_idx, ctx->max_entries)) {
                    meta_ring[pos] = NULL;
                    valid_ring[pos] = 0u;
                    continue;
                }

                meta = ft_maint_extra_meta_(ctx, entry_idx);
                meta_ring[pos] = meta;
                if (!ft_maint_extra_live_bucket_of_idx_(ctx, entry_idx, meta,
                                                        &bk_ring[pos])) {
                    valid_ring[pos] = 0u;
                    continue;
                }
                valid_ring[pos] = 1u;
                rix_hash_prefetch_extra_bucket_indices_of_idx(ctx->buckets,
                                                              bk_ring[pos]);
                rix_hash_prefetch_extra_bucket_extras_of_idx(ctx->buckets,
                                                             bk_ring[pos]);
            }
        }

        if (i >= FT_MAINT_HIT_AHEAD * 2u
            && i - FT_MAINT_HIT_AHEAD * 2u < nb_idx) {
            unsigned base = i - FT_MAINT_HIT_AHEAD * 2u;
            unsigned n = (base + FT_MAINT_HIT_STEP <= nb_idx)
                       ? FT_MAINT_HIT_STEP : (nb_idx - base);

            for (unsigned j = 0u; j < n; j++) {
                unsigned pos = (base + j) & 15u;
                unsigned bk_idx;

                if (!valid_ring[pos])
                    continue;

                bk_idx = bk_ring[pos];
                if (use_filter) {
                    unsigned fslot = bk_idx & FT_MAINT_FILTER_MASK;

                    if (scanned[fslot] == bk_idx)
                        continue;
                    scanned[fslot] = bk_idx;
                }

                ctx->stats->maint_bucket_checks++;
                count = ft_maint_extra_scan_bucket_(ctx, bk_idx, now_ts,
                                                    timeout_ts, expired_idxv,
                                                    count, max_expired,
                                                    min_bk_entries, NULL);
                if (count >= max_expired)
                    return count;
            }
        }
    }

    return count;
}

unsigned
_FTM_EXTRA_FN(ft_table_extra_maintain_idx_bulk)(
    const struct ft_maint_extra_ctx *ctx,
    const u32 *entry_idxv,
    unsigned nb_idx,
    u64 now,
    u64 expire_tsc,
    u32 *expired_idxv,
    unsigned max_expired,
    unsigned min_bk_entries,
    int enable_filter)
{
    u64 now_ts;
    u64 timeout_ts;
    unsigned count;

    if (ctx == NULL || entry_idxv == NULL || nb_idx == 0u
        || max_expired == 0u || expired_idxv == NULL)
        return 0u;
    if (expire_tsc == 0u)
        return 0u;

    ctx->stats->maint_calls++;
    now_ts = flow_timestamp_encode(now, ctx->ts_shift);
    timeout_ts = flow_timestamp_timeout_encode(expire_tsc, ctx->ts_shift);

    if (enable_filter)
        count = ft_maint_extra_idx_bulk_loop_(ctx, entry_idxv, nb_idx,
                                              now_ts, timeout_ts, expired_idxv,
                                              max_expired, min_bk_entries, 1);
    else
        count = ft_maint_extra_idx_bulk_loop_(ctx, entry_idxv, nb_idx,
                                              now_ts, timeout_ts, expired_idxv,
                                              max_expired, min_bk_entries, 0);

    ctx->stats->maint_evictions += count;
    return count;
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
