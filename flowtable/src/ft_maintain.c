/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * ft_maintain.c - Protocol-free bucket maintenance (timeout expiry).
 *
 * Compiled once per arch variant (gen/sse/avx2/avx512) with FT_ARCH_SUFFIX.
 */

#include <rix/rix_hash.h>
#include <rix/rix_defs_private.h>

#include "flow_common.h"
#include "flow_key.h"

/*===========================================================================
 * Arch-suffix name mangling
 *===========================================================================*/
#define _FTM_CAT2(a, b) a##b
#define _FTM_CAT(a, b)  _FTM_CAT2(a, b)

#ifdef FT_ARCH_SUFFIX
#define _FTM_FN(name) _FTM_CAT(name, FT_ARCH_SUFFIX)
#else
#define _FTM_FN(name) name
#endif

/* Forward declarations for -Wmissing-prototypes */
unsigned _FTM_FN(ft_table_maintain)(const struct ft_maint_ctx *,
                                     unsigned, u64, u64,
                                     u32 *, unsigned, unsigned,
                                     unsigned *);
unsigned _FTM_FN(ft_table_maintain_idx_bulk)(const struct ft_maint_ctx *,
                                              const u32 *, unsigned,
                                              u64, u64,
                                              u32 *, unsigned, unsigned,
                                              int);

/*
 * Get flow_entry_meta pointer for a given entry index.
 * meta_off = pool_entry_offset + offsetof(flowX_entry, meta)
 */
static inline const struct flow_entry_meta *
ft_maint_meta_(const struct ft_maint_ctx *ctx, unsigned entry_idx)
{
    return (const struct flow_entry_meta *)(const void *)(
        ctx->pool_base +
        RIX_IDX_TO_OFF0(entry_idx) * ctx->pool_stride +
        ctx->meta_off);
}

static inline unsigned
ft_maint_bucket_of_meta_(const struct ft_maint_ctx *ctx,
                         const struct flow_entry_meta *meta)
{
    return meta->cur_hash & ctx->rhh_mask;
}

static inline int
ft_maint_live_bucket_of_idx_(const struct ft_maint_ctx *ctx,
                             unsigned entry_idx,
                             const struct flow_entry_meta *meta,
                             unsigned *bk_out)
{
    unsigned slot;
    unsigned bk;

    if (!RIX_IDX_IS_VALID(entry_idx, ctx->max_entries))
        return 0;

    slot = (unsigned)meta->slot;
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)
        return 0;

    bk = ft_maint_bucket_of_meta_(ctx, meta);
    if (ctx->buckets[bk].idx[slot] != entry_idx)
        return 0;

    *bk_out = bk;
    return 1;
}

/*
 * Scan one bucket once, derive occupancy via used_bits/popcount, and when the
 * occupancy threshold is met remove expired entries.
 *
 * Returns the updated expired count. If out_used_count is non-NULL, writes the
 * number of occupied slots observed during the scan.
 */
static unsigned
ft_maint_scan_bucket_(const struct ft_maint_ctx *ctx,
                      unsigned bk_idx,
                      u64 now_ts,
                      u64 timeout_ts,
                      u32 *expired_idxv,
                      unsigned out_pos,
                      unsigned max_expired,
                      unsigned min_bk_entries,
                      unsigned *out_used_count)
{
    struct rix_hash_bucket_s *bk = ctx->buckets + bk_idx;
    u32 used_bits = 0u;
    u32 expired_bits = 0u;
    const struct flow_entry_meta *metas[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned slots[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned count = out_pos;
    unsigned used_count = 0u;
    unsigned cur_base = 0u;
    unsigned cur_count;
    unsigned next_base;
    unsigned next_count;

    RIX_ASSERT(RIX_HASH_BUCKET_ENTRY_SZ <= 32u);

    for (unsigned slot = 0; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {
        unsigned idx = bk->idx[slot];

        if (idx == (unsigned)RIX_NIL)
            continue;

        used_bits |= (u32)(1u << slot);
        slots[used_count] = slot;
        metas[used_count] = ft_maint_meta_(ctx, idx);
        used_count++;
    }

    RIX_ASSERT(used_count == (unsigned)__builtin_popcount(used_bits));
    if (out_used_count != NULL)
        *out_used_count = used_count;
    if (min_bk_entries > 1u && used_count < min_bk_entries)
        return count;

    enum { FT_MAINT_STAGE_SLOTS = 4u };
    cur_count = (used_count < FT_MAINT_STAGE_SLOTS)
        ? used_count : FT_MAINT_STAGE_SLOTS;
    for (next_count = 0u; next_count < cur_count; next_count++)
        __builtin_prefetch(metas[next_count], 0, 3);
    next_base = cur_count;
    while (cur_count != 0u) {
        next_count = 0u;
        while (next_count < FT_MAINT_STAGE_SLOTS && next_base < used_count) {
            __builtin_prefetch(metas[next_base], 0, 3);
            next_count++;
            next_base++;
        }
        for (unsigned i = 0u; i < cur_count; i++) {
            unsigned pos = cur_base + i;
            u64 ts = flow_timestamp_get(metas[pos]);

            if (!flow_timestamp_is_expired_raw(ts, now_ts, timeout_ts))
                continue;
            expired_bits |= (u32)(1u << slots[pos]);
        }
        cur_base += cur_count;
        cur_count = next_count;
    }

    while (expired_bits != 0u && count < max_expired) {
        unsigned slot = (unsigned)__builtin_ctz(expired_bits);
        unsigned idx = bk->idx[slot];

        expired_bits &= expired_bits - 1u;
        RIX_ASSERT(idx != (unsigned)RIX_NIL);

        /* Remove from bucket */
        bk->hash[slot] = 0u;
        bk->idx[slot] = (u32)RIX_NIL;
        (*ctx->rhh_nb)--;

        expired_idxv[count++] = (u32)idx;
    }
    return count;
}

unsigned
_FTM_FN(ft_table_maintain)(const struct ft_maint_ctx *ctx,
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

        /* Prefetch next bucket indices */
        rix_hash_prefetch_bucket_indices_of_idx(ctx->buckets, next);

        ctx->stats->maint_bucket_checks++;

        count = ft_maint_scan_bucket_(ctx, cur_bk, now_ts, timeout_ts,
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
 */
static inline unsigned
ft_maint_idx_bulk_loop_(const struct ft_maint_ctx *ctx,
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
    const struct flow_entry_meta *meta_ring[16];
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
                    __builtin_prefetch(ft_maint_meta_(ctx, entry_idx), 0, 3);
            }
        }

        if (i >= FT_MAINT_HIT_AHEAD && i - FT_MAINT_HIT_AHEAD < nb_idx) {
            unsigned base = i - FT_MAINT_HIT_AHEAD;
            unsigned n = (base + FT_MAINT_HIT_STEP <= nb_idx)
                       ? FT_MAINT_HIT_STEP : (nb_idx - base);

            for (unsigned j = 0u; j < n; j++) {
                unsigned entry_idx = entry_idxv[base + j];
                unsigned pos = (base + j) & 15u;
                const struct flow_entry_meta *meta;

                if (!RIX_IDX_IS_VALID(entry_idx, ctx->max_entries)) {
                    meta_ring[pos] = NULL;
                    valid_ring[pos] = 0u;
                    continue;
                }

                meta = ft_maint_meta_(ctx, entry_idx);
                meta_ring[pos] = meta;
                if (!ft_maint_live_bucket_of_idx_(ctx, entry_idx, meta,
                                                  &bk_ring[pos])) {
                    valid_ring[pos] = 0u;
                    continue;
                }
                valid_ring[pos] = 1u;
                rix_hash_prefetch_bucket_indices_of_idx(ctx->buckets,
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
                count = ft_maint_scan_bucket_(ctx, bk_idx, now_ts,
                                              timeout_ts, expired_idxv, count,
                                              max_expired, min_bk_entries,
                                              NULL);
                if (count >= max_expired)
                    return count;
            }
        }
    }

    return count;
}

unsigned
_FTM_FN(ft_table_maintain_idx_bulk)(const struct ft_maint_ctx *ctx,
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
        count = ft_maint_idx_bulk_loop_(ctx, entry_idxv, nb_idx,
                                        now_ts, timeout_ts, expired_idxv,
                                        max_expired, min_bk_entries, 1);
    else
        count = ft_maint_idx_bulk_loop_(ctx, entry_idxv, nb_idx,
                                        now_ts, timeout_ts, expired_idxv,
                                        max_expired, min_bk_entries, 0);

    ctx->stats->maint_evictions += count;
    return count;
}
