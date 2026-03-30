/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_fc_body.h - Include-template for per-variant bench functions.
 *
 * Before including, define:
 *   FCB_PREFIX      e.g. flow4
 *   FCB_KEY_T       e.g. struct flow4_key
 *   FCB_RESULT_T    e.g. struct fc_flow4_result
 *   FCB_ENTRY_T     e.g. struct fc_flow4_entry
 *   FCB_CACHE_T     e.g. struct fc_flow4_cache
 *   FCB_CONFIG_T    e.g. struct fc_flow4_config
 *   FCB_STATS_T     e.g. struct fc_flow4_stats
 *   FCB_PRESSURE    e.g. FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
 *   FCB_MAKE_KEY(i) e.g. fcb_make_key4(i)
 */

#ifndef FCB_PREFIX
#error "FCB_PREFIX must be defined before including bench_fc_body.h"
#endif

/* Token-pasting helpers */
#define _FCB_CAT2(a, b) a##b
#define _FCB_CAT(a, b)  _FCB_CAT2(a, b)

/* fcb_flow4_xxx */
#define FCB_FN(name)  _FCB_CAT(fcb_, _FCB_CAT(FCB_PREFIX, _##name))

/* fc_flow4_cache_xxx */
#define FCB_API(name) _FCB_CAT(fc_, _FCB_CAT(FCB_PREFIX, _cache_##name))

#ifndef FCB_CALL_FINDADD
#define FCB_CALL_FINDADD(fc, keys, n, now, results) \
    FCB_API(findadd_bulk)((fc), (keys), (n), (now), (results))
#endif

/* fc_flow4_xxx (non-cache API) */
#define FCB_PUB(name) _FCB_CAT(fc_, _FCB_CAT(FCB_PREFIX, _##name))

/*===========================================================================
 * Cache context
 *===========================================================================*/
struct FCB_FN(ctx) {
    FCB_CACHE_T               fc;
    struct rix_hash_bucket_s  *buckets;
    FCB_ENTRY_T              *pool;
};

struct FCB_FN(stats_delta) {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t fills;
    uint64_t fill_full;
    uint64_t relief_calls;
    uint64_t relief_bucket_checks;
    uint64_t relief_evictions;
    uint64_t relief_bk0_evictions;
    uint64_t relief_bk1_evictions;
    uint64_t oldest_reclaim_calls;
    uint64_t oldest_reclaim_evictions;
    uint64_t maint_calls;
    uint64_t maint_bucket_checks;
    uint64_t maint_evictions;
};

/*===========================================================================
 * Context init / reset / free
 *===========================================================================*/
static void
FCB_FN(ctx_init)(struct FCB_FN(ctx) *ctx, unsigned nb_bk,
                  unsigned max_entries, uint64_t timeout_tsc)
{
    size_t bk_bytes   = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    size_t pool_bytes = (size_t)max_entries * sizeof(FCB_ENTRY_T);
    FCB_CONFIG_T cfg;

    ctx->buckets = fcb_alloc(bk_bytes);
    ctx->pool    = fcb_alloc(pool_bytes);
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_tsc          = timeout_tsc;
    cfg.pressure_empty_slots = FCB_PRESSURE;
    FCB_API(init)(&ctx->fc, ctx->buckets, nb_bk, ctx->pool, max_entries, &cfg);
}

static void
FCB_FN(ctx_init_cfg)(struct FCB_FN(ctx) *ctx, unsigned nb_bk,
                      unsigned max_entries, const FCB_CONFIG_T *cfg)
{
    size_t bk_bytes   = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    size_t pool_bytes = (size_t)max_entries * sizeof(FCB_ENTRY_T);

    ctx->buckets = fcb_alloc(bk_bytes);
    ctx->pool    = fcb_alloc(pool_bytes);
    FCB_API(init)(&ctx->fc, ctx->buckets, nb_bk, ctx->pool, max_entries, cfg);
}

static void
FCB_FN(ctx_reset)(struct FCB_FN(ctx) *ctx)
{
    FCB_API(flush)(&ctx->fc);
}

static void
FCB_FN(ctx_free)(struct FCB_FN(ctx) *ctx)
{
    free(ctx->pool);
    free(ctx->buckets);
}

/*===========================================================================
 * Stats helpers
 *===========================================================================*/
static struct FCB_FN(stats_delta)
FCB_FN(stats_snapshot)(const struct FCB_FN(ctx) *ctx)
{
    FCB_STATS_T raw;
    struct FCB_FN(stats_delta) d;

    FCB_API(stats)(&ctx->fc, &raw);
    d.lookups               = raw.lookups;
    d.hits                  = raw.hits;
    d.misses                = raw.misses;
    d.fills                 = raw.fills;
    d.fill_full             = raw.fill_full;
    d.relief_calls          = raw.relief_calls;
    d.relief_bucket_checks  = raw.relief_bucket_checks;
    d.relief_evictions      = raw.relief_evictions;
    d.relief_bk0_evictions  = raw.relief_bk0_evictions;
    d.relief_bk1_evictions  = raw.relief_bk1_evictions;
    d.oldest_reclaim_calls  = raw.oldest_reclaim_calls;
    d.oldest_reclaim_evictions = raw.oldest_reclaim_evictions;
    d.maint_calls           = raw.maint_calls;
    d.maint_bucket_checks   = raw.maint_bucket_checks;
    d.maint_evictions       = raw.maint_evictions;
    return d;
}

static struct FCB_FN(stats_delta)
FCB_FN(stats_diff)(struct FCB_FN(stats_delta) after,
                    struct FCB_FN(stats_delta) before)
{
    struct FCB_FN(stats_delta) d;

    d.lookups               = after.lookups - before.lookups;
    d.hits                  = after.hits - before.hits;
    d.misses                = after.misses - before.misses;
    d.fills                 = after.fills - before.fills;
    d.fill_full             = after.fill_full - before.fill_full;
    d.relief_calls          = after.relief_calls - before.relief_calls;
    d.relief_bucket_checks  = after.relief_bucket_checks - before.relief_bucket_checks;
    d.relief_evictions      = after.relief_evictions - before.relief_evictions;
    d.relief_bk0_evictions  = after.relief_bk0_evictions - before.relief_bk0_evictions;
    d.relief_bk1_evictions  = after.relief_bk1_evictions - before.relief_bk1_evictions;
    d.oldest_reclaim_calls  = after.oldest_reclaim_calls - before.oldest_reclaim_calls;
    d.oldest_reclaim_evictions = after.oldest_reclaim_evictions - before.oldest_reclaim_evictions;
    d.maint_calls           = after.maint_calls - before.maint_calls;
    d.maint_bucket_checks   = after.maint_bucket_checks - before.maint_bucket_checks;
    d.maint_evictions       = after.maint_evictions - before.maint_evictions;
    return d;
}

/*===========================================================================
 * Prefill / active scan
 *===========================================================================*/
static unsigned
FCB_FN(prefill)(struct FCB_FN(ctx) *ctx, const FCB_KEY_T *keys,
                 unsigned n, uint64_t now)
{
    FCB_RESULT_T *results = fcb_alloc((size_t)FCB_QUERY * sizeof(*results));
    unsigned offset = 0u;

    while (offset < n) {
        unsigned chunk = n - offset;

        if (chunk > FCB_QUERY)
            chunk = FCB_QUERY;
        FCB_CALL_FINDADD(&ctx->fc, keys + offset, chunk, now, results);
        offset += chunk;
    }
    free(results);
    return FCB_API(nb_entries)(&ctx->fc);
}

static unsigned
FCB_FN(active_scan)(const struct FCB_FN(ctx) *ctx)
{
    unsigned count = 0u;

    for (unsigned i = 0; i < ctx->fc.max_entries; i++) {
        if (ctx->pool[i].last_ts != 0u)
            count++;
    }
    return count;
}

/*===========================================================================
 * Pure datapath: hit
 *===========================================================================*/
static double
FCB_FN(bench_hit)(struct FCB_FN(ctx) *ctx,
                  const FCB_KEY_T *bg_keys, unsigned bg_n,
                  const FCB_KEY_T *keys, unsigned n, unsigned repeat)
{
    FCB_RESULT_T *results = fcb_alloc((size_t)n * sizeof(*results));
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));
    uint64_t t0, t1;

    for (unsigned r = 0; r < repeat; r++) {
        const FCB_KEY_T *round_keys = keys + ((size_t)r * n);
        uint64_t now = (uint64_t)r + 100u;

        FCB_FN(ctx_reset)(ctx);
        if (bg_n != 0u)
            (void)FCB_FN(prefill)(ctx, bg_keys, bg_n, now);
        (void)FCB_FN(prefill)(ctx, round_keys, n, now + 1u);
        fcb_cold_touch();
        t0 = fcb_rdtsc();
        FCB_CALL_FINDADD(&ctx->fc, round_keys, n, now + 2u, results);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }

    free(results);
    {
        double med = fcb_median_u64(samples, repeat) / (double)n;
        free(samples);
        return med;
    }
}

/*===========================================================================
 * Pure datapath: find_bulk hit (search only, no insert path)
 *===========================================================================*/
static double
FCB_FN(bench_find_hit)(struct FCB_FN(ctx) *ctx,
                       const FCB_KEY_T *bg_keys, unsigned bg_n,
                       const FCB_KEY_T *keys, unsigned n, unsigned repeat)
{
    FCB_RESULT_T *results = fcb_alloc((size_t)n * sizeof(*results));
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));
    uint64_t t0, t1;

    for (unsigned r = 0; r < repeat; r++) {
        const FCB_KEY_T *round_keys = keys + ((size_t)r * n);
        uint64_t now = (uint64_t)r + 100u;

        FCB_FN(ctx_reset)(ctx);
        if (bg_n != 0u)
            (void)FCB_FN(prefill)(ctx, bg_keys, bg_n, now);
        (void)FCB_FN(prefill)(ctx, round_keys, n, now + 1u);
        fcb_cold_touch();
        t0 = fcb_rdtsc();
        FCB_API(find_bulk)(&ctx->fc, round_keys, n, now + 2u, results);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }

    free(results);
    {
        double med = fcb_median_u64(samples, repeat) / (double)n;
        free(samples);
        return med;
    }
}

/*===========================================================================
 * Pure datapath: add_bulk only
 *===========================================================================*/
static double
FCB_FN(bench_add_only)(struct FCB_FN(ctx) *ctx,
                       const FCB_KEY_T *bg_keys, unsigned bg_n,
                       const FCB_KEY_T *keys, unsigned n, unsigned repeat)
{
    FCB_RESULT_T *results = fcb_alloc((size_t)n * sizeof(*results));
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));

    for (unsigned r = 0; r < repeat; r++) {
        const FCB_KEY_T *round_keys = keys + ((size_t)r * n);
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;

        FCB_FN(ctx_reset)(ctx);
        if (bg_n != 0u)
            (void)FCB_FN(prefill)(ctx, bg_keys, bg_n, now);
        fcb_cold_touch();
        t0 = fcb_rdtsc();
        FCB_API(add_bulk)(&ctx->fc, round_keys, n, now + 1u, results);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }

    free(results);
    {
        double med = fcb_median_u64(samples, repeat) / (double)n;
        free(samples);
        return med;
    }
}

/*===========================================================================
 * Pure datapath: add_bulk + del_bulk cycle
 *===========================================================================*/
static double
FCB_FN(bench_add_del)(struct FCB_FN(ctx) *ctx,
                      const FCB_KEY_T *bg_keys, unsigned bg_n,
                      const FCB_KEY_T *keys, unsigned n, unsigned repeat)
{
    FCB_RESULT_T *results = fcb_alloc((size_t)n * sizeof(*results));
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));

    for (unsigned r = 0; r < repeat; r++) {
        const FCB_KEY_T *round_keys = keys + ((size_t)r * n);
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;

        FCB_FN(ctx_reset)(ctx);
        if (bg_n != 0u)
            (void)FCB_FN(prefill)(ctx, bg_keys, bg_n, now);
        fcb_cold_touch();
        t0 = fcb_rdtsc();
        FCB_API(add_bulk)(&ctx->fc, round_keys, n, now + 1u, results);
        FCB_API(del_bulk)(&ctx->fc, round_keys, n);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }

    free(results);
    {
        double med = fcb_median_u64(samples, repeat) / (double)n;
        free(samples);
        return med;
    }
}

/*===========================================================================
 * Pure datapath: del_bulk (delete pre-filled entries)
 *===========================================================================*/
static double
FCB_FN(bench_del_bulk)(struct FCB_FN(ctx) *ctx,
                       const FCB_KEY_T *bg_keys, unsigned bg_n,
                       const FCB_KEY_T *keys, unsigned n, unsigned repeat)
{
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));
    FCB_RESULT_T *results = fcb_alloc((size_t)n * sizeof(*results));

    for (unsigned r = 0; r < repeat; r++) {
        const FCB_KEY_T *round_keys = keys + ((size_t)r * n);
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;

        FCB_FN(ctx_reset)(ctx);
        if (bg_n != 0u)
            (void)FCB_FN(prefill)(ctx, bg_keys, bg_n, now);
        FCB_API(add_bulk)(&ctx->fc, round_keys, n, now + 1u, results);
        fcb_cold_touch();
        t0 = fcb_rdtsc();
        FCB_API(del_bulk)(&ctx->fc, round_keys, n);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }

    free(results);
    {
        double med = fcb_median_u64(samples, repeat) / (double)n;
        free(samples);
        return med;
    }
}

/*===========================================================================
 * Pure datapath: miss + fill
 *===========================================================================*/
static double
FCB_FN(bench_miss_fill)(struct FCB_FN(ctx) *ctx,
                        const FCB_KEY_T *bg_keys, unsigned bg_n,
                        const FCB_KEY_T *keys, unsigned n, unsigned repeat)
{
    FCB_RESULT_T *results = fcb_alloc((size_t)n * sizeof(*results));
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));

    for (unsigned r = 0; r < repeat; r++) {
        const FCB_KEY_T *round_keys = keys + ((size_t)r * n);
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;

        FCB_FN(ctx_reset)(ctx);
        if (bg_n != 0u)
            (void)FCB_FN(prefill)(ctx, bg_keys, bg_n, now);
        fcb_cold_touch();
        t0 = fcb_rdtsc();
        FCB_CALL_FINDADD(&ctx->fc, round_keys, n, now + 1u, results);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }

    free(results);
    {
        double med = fcb_median_u64(samples, repeat) / (double)n;
        free(samples);
        return med;
    }
}

/*===========================================================================
 * Pure datapath: mixed (90% hit / 10% miss, reset per round)
 *===========================================================================*/
static double
FCB_FN(bench_mixed)(struct FCB_FN(ctx) *ctx,
                    const FCB_KEY_T *prefill_keys, unsigned prefill_n,
                    const FCB_KEY_T *query_keys, unsigned query_n,
                     unsigned repeat)
{
    FCB_RESULT_T *results = fcb_alloc((size_t)query_n * sizeof(*results));
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));
    unsigned hit_n = (query_n * 9u) / 10u;
    unsigned bg_n = (prefill_n > hit_n) ? (prefill_n - hit_n) : 0u;

    for (unsigned r = 0; r < repeat; r++) {
        const FCB_KEY_T *round_query = query_keys + ((size_t)r * query_n);
        uint64_t now = (uint64_t)r + 5000u;
        uint64_t t0, t1;

        FCB_FN(ctx_reset)(ctx);
        if (bg_n != 0u)
            (void)FCB_FN(prefill)(ctx, prefill_keys, bg_n, now);
        if (hit_n != 0u)
            (void)FCB_FN(prefill)(ctx, round_query, hit_n, now + 1u);
        fcb_cold_touch();

        t0 = fcb_rdtsc();
        FCB_CALL_FINDADD(&ctx->fc, round_query, query_n, now + 2u, results);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }

    free(results);
    {
        double med = fcb_median_u64(samples, repeat) / (double)query_n;
        free(samples);
        return med;
    }
}

/*===========================================================================
 * Stable closed-set findadd:
 *   - fixed resident hit set
 *   - fixed non-resident miss set
 *   - miss set is deleted after each timed round to restore state
 *===========================================================================*/
static struct fcb_run_summary
FCB_FN(findadd_closed_summary)(unsigned desired_entries, unsigned nb_bk,
                               unsigned fill_pct, unsigned hit_pct,
                               unsigned rounds)
{
    struct FCB_FN(ctx) ctx;
    FCB_KEY_T *prefill_keys;
    FCB_KEY_T *query;
    FCB_RESULT_T *results;
    struct FCB_FN(stats_delta) before, after, delta;
    struct fcb_run_summary out;
    unsigned max_entries = fcb_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned fill_n = (unsigned)(((uint64_t)total_slots * fill_pct) / 100u);
    unsigned miss_n_max = FCB_QUERY - ((FCB_QUERY * hit_pct) / 100u);
    unsigned active;
    uint64_t now = 1u;
    uint64_t t0, t1;
    unsigned hit_accum = 0u;

    memset(&out, 0, sizeof(out));

    if (rounds == 0u)
        rounds = 1u;
    if (fill_n > max_entries)
        fill_n = max_entries;
    if ((uint64_t)fill_n + (uint64_t)miss_n_max > (uint64_t)max_entries) {
        fprintf(stderr,
                "findadd_closed requires headroom: fill=%u plus miss=%u exceeds pool=%u\n",
                fill_n, miss_n_max, max_entries);
        exit(2);
    }

    FCB_FN(ctx_init)(&ctx, nb_bk, max_entries, 1000000000ull);
    prefill_keys = fcb_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query        = fcb_alloc((size_t)FCB_QUERY * sizeof(*query));
    results      = fcb_alloc((size_t)FCB_QUERY * sizeof(*results));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = FCB_MAKE_KEY(i);
    (void)FCB_FN(prefill)(&ctx, prefill_keys, fill_n, now);

    for (unsigned w = 0; w < 20u; w++) {
        unsigned hit_n = 0u;
        unsigned miss_n;

        for (unsigned i = 0; i < FCB_QUERY; i++) {
            hit_accum += hit_pct;
            if (fill_n > 0u && hit_accum >= 100u) {
                hit_accum -= 100u;
                hit_n++;
            }
        }
        miss_n = FCB_QUERY - hit_n;
        for (unsigned i = 0; i < hit_n; i++)
            query[i] = prefill_keys[(w * 17u + i * 13u) % fill_n];
        for (unsigned i = 0; i < miss_n; i++)
            query[hit_n + i] = FCB_MAKE_KEY(max_entries + w * FCB_QUERY + i);
        now += FCB_QUERY;
        FCB_CALL_FINDADD(&ctx.fc, query, FCB_QUERY, now, results);
        if (miss_n > 0u)
            FCB_API(del_bulk)(&ctx.fc, query + hit_n, miss_n);
    }

    before = FCB_FN(stats_snapshot)(&ctx);
    t0 = fcb_rdtsc();
    for (unsigned r = 0; r < rounds; r++) {
        unsigned hit_n = 0u;
        unsigned miss_n;

        for (unsigned i = 0; i < FCB_QUERY; i++) {
            hit_accum += hit_pct;
            if (fill_n > 0u && hit_accum >= 100u) {
                hit_accum -= 100u;
                hit_n++;
            }
        }
        miss_n = FCB_QUERY - hit_n;
        for (unsigned i = 0; i < hit_n; i++)
            query[i] = prefill_keys[(r * 17u + i * 13u) % fill_n];
        for (unsigned i = 0; i < miss_n; i++)
            query[hit_n + i] = FCB_MAKE_KEY(max_entries + (20u + r) * FCB_QUERY + i);
        now += FCB_QUERY;
        FCB_CALL_FINDADD(&ctx.fc, query, FCB_QUERY, now, results);
        if (miss_n > 0u)
            FCB_API(del_bulk)(&ctx.fc, query + hit_n, miss_n);
    }
    t1 = fcb_rdtsc();
    after = FCB_FN(stats_snapshot)(&ctx);
    delta = FCB_FN(stats_diff)(after, before);
    active = FCB_FN(active_scan)(&ctx);
    out.cycles_per_key = (double)(t1 - t0) / (double)(rounds * FCB_QUERY);
    out.hit_pct = delta.lookups
        ? (100.0 * (double)delta.hits / (double)delta.lookups)
        : 0.0;
    out.misses = delta.misses;
    out.relief_evictions = delta.relief_evictions;
    out.oldest_reclaim_evictions = delta.oldest_reclaim_evictions;
    out.fill_end_pct = 100.0 * (double)active / (double)total_slots;
    out.rounds = rounds;

    free(results);
    free(query);
    free(prefill_keys);
    FCB_FN(ctx_free)(&ctx);
    return out;
}

static void
FCB_FN(findadd_closed)(unsigned desired_entries, unsigned nb_bk,
                       unsigned fill_pct, unsigned hit_pct,
                       unsigned rounds)
{
    struct fcb_run_summary s =
        FCB_FN(findadd_closed_summary)(desired_entries, nb_bk,
                                       fill_pct, hit_pct, rounds);

    printf("fc : cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  oldest=%" PRIu64
           "  fill=%.1f%%  rounds=%u\n",
           s.cycles_per_key, s.hit_pct, s.misses,
           s.relief_evictions, s.oldest_reclaim_evictions,
           s.fill_end_pct, s.rounds);
}

/*===========================================================================
 * Closed-set fixed-stream path:
 *   - fixed per-key hit/miss stream independent of query width
 *   - query width only changes grouping into bulk calls
 *   - miss keys are deleted after each bulk call
 *===========================================================================*/
static struct fcb_run_summary
FCB_FN(findadd_closed_stream_summary)(unsigned desired_entries, unsigned nb_bk,
                                      unsigned fill_pct, unsigned hit_pct,
                                      unsigned total_keys)
{
    struct FCB_FN(ctx) ctx;
    FCB_KEY_T *prefill_keys;
    FCB_KEY_T *stream_keys;
    uint8_t *stream_is_hit;
    FCB_KEY_T *query;
    FCB_KEY_T *miss_keys;
    FCB_RESULT_T *results;
    struct FCB_FN(stats_delta) before, after, delta;
    struct fcb_run_summary out;
    unsigned max_entries = fcb_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned fill_n = (unsigned)(((uint64_t)total_slots * fill_pct) / 100u);
    unsigned miss_n_max = FCB_QUERY;
    unsigned active;
    uint64_t now = 1u;
    uint64_t t0, t1;
    unsigned hit_accum = 0u;

    memset(&out, 0, sizeof(out));

    if (total_keys == 0u)
        total_keys = 1u;
    if (fill_n > max_entries)
        fill_n = max_entries;
    if ((uint64_t)fill_n + (uint64_t)miss_n_max > (uint64_t)max_entries) {
        fprintf(stderr,
                "findadd_closed_stream requires headroom: fill=%u plus miss=%u exceeds pool=%u\n",
                fill_n, miss_n_max, max_entries);
        exit(2);
    }

    FCB_FN(ctx_init)(&ctx, nb_bk, max_entries, 1000000000ull);
    prefill_keys = fcb_alloc((size_t)max_entries * sizeof(*prefill_keys));
    stream_keys  = fcb_alloc((size_t)total_keys * sizeof(*stream_keys));
    stream_is_hit = fcb_alloc((size_t)total_keys * sizeof(*stream_is_hit));
    query        = fcb_alloc((size_t)FCB_QUERY * sizeof(*query));
    miss_keys    = fcb_alloc((size_t)FCB_QUERY * sizeof(*miss_keys));
    results      = fcb_alloc((size_t)FCB_QUERY * sizeof(*results));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = FCB_MAKE_KEY(i);
    (void)FCB_FN(prefill)(&ctx, prefill_keys, fill_n, now);

    for (unsigned i = 0; i < total_keys; i++) {
        unsigned is_hit = 0u;

        hit_accum += hit_pct;
        if (fill_n > 0u && hit_accum >= 100u) {
            hit_accum -= 100u;
            is_hit = 1u;
        }
        stream_is_hit[i] = (uint8_t)is_hit;
        if (is_hit)
            stream_keys[i] = prefill_keys[(i * 17u + 13u) % fill_n];
        else
            stream_keys[i] = FCB_MAKE_KEY(max_entries + i);
    }

    for (unsigned warm = 0; warm < 4096u; warm += FCB_QUERY) {
        unsigned batch_n = 4096u - warm;
        unsigned miss_n = 0u;

        if (batch_n > FCB_QUERY)
            batch_n = FCB_QUERY;
        if (batch_n > total_keys)
            batch_n = total_keys;
        for (unsigned i = 0; i < batch_n; i++) {
            unsigned idx = (warm + i) % total_keys;
            query[i] = stream_keys[idx];
        }
        now += batch_n;
        FCB_CALL_FINDADD(&ctx.fc, query, batch_n, now, results);
        for (unsigned i = 0; i < batch_n; i++) {
            unsigned idx = (warm + i) % total_keys;
            if (stream_is_hit[idx] == 0u)
                miss_keys[miss_n++] = query[i];
        }
        if (miss_n > 0u)
            FCB_API(del_bulk)(&ctx.fc, miss_keys, miss_n);
    }

    before = FCB_FN(stats_snapshot)(&ctx);
    t0 = fcb_rdtsc();
    for (unsigned pos = 0; pos < total_keys; pos += FCB_QUERY) {
        unsigned batch_n = total_keys - pos;
        unsigned miss_n = 0u;

        if (batch_n > FCB_QUERY)
            batch_n = FCB_QUERY;
        for (unsigned i = 0; i < batch_n; i++)
            query[i] = stream_keys[pos + i];
        now += batch_n;
        FCB_CALL_FINDADD(&ctx.fc, query, batch_n, now, results);
        for (unsigned i = 0; i < batch_n; i++) {
            if (stream_is_hit[pos + i] == 0u)
                miss_keys[miss_n++] = query[i];
        }
        if (miss_n > 0u)
            FCB_API(del_bulk)(&ctx.fc, miss_keys, miss_n);
    }
    t1 = fcb_rdtsc();
    after = FCB_FN(stats_snapshot)(&ctx);
    delta = FCB_FN(stats_diff)(after, before);
    active = FCB_FN(active_scan)(&ctx);
    out.cycles_per_key = (double)(t1 - t0) / (double)total_keys;
    out.hit_pct = delta.lookups
        ? (100.0 * (double)delta.hits / (double)delta.lookups)
        : 0.0;
    out.misses = delta.misses;
    out.relief_evictions = delta.relief_evictions;
    out.oldest_reclaim_evictions = delta.oldest_reclaim_evictions;
    out.fill_end_pct = 100.0 * (double)active / (double)total_slots;
    out.rounds = (total_keys + FCB_QUERY - 1u) / FCB_QUERY;

    free(results);
    free(miss_keys);
    free(query);
    free(stream_is_hit);
    free(stream_keys);
    free(prefill_keys);
    FCB_FN(ctx_free)(&ctx);
    return out;
}

static void
FCB_FN(findadd_closed_stream)(unsigned desired_entries, unsigned nb_bk,
                              unsigned fill_pct, unsigned hit_pct,
                              unsigned total_keys)
{
    struct fcb_run_summary s =
        FCB_FN(findadd_closed_stream_summary)(desired_entries, nb_bk,
                                              fill_pct, hit_pct, total_keys);

    printf("fc : cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  oldest=%" PRIu64
           "  fill=%.1f%%  rounds=%u\n",
           s.cycles_per_key, s.hit_pct, s.misses,
           s.relief_evictions, s.oldest_reclaim_evictions,
           s.fill_end_pct, s.rounds);
}

/*===========================================================================
 * Persistent open-set path: fresh miss stream, no maintenance
 *===========================================================================*/
static struct fcb_run_summary
FCB_FN(findadd_open_summary)(unsigned desired_entries, unsigned nb_bk,
                             unsigned start_fill_pct, unsigned hit_pct,
                             unsigned keys_per_sec, unsigned timeout_ms)
{
    struct FCB_FN(ctx) ctx;
    FCB_CONFIG_T cfg;
    FCB_KEY_T *prefill_keys;
    FCB_KEY_T *query;
    FCB_RESULT_T *results;
    struct FCB_FN(stats_delta) before, after, delta;
    struct fcb_run_summary out;
    unsigned max_entries = fcb_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned start_active, active, max_seen;
    uint64_t tsc_hz, tsc_per_key, now;
    uint64_t total_cycles = 0u;

    memset(&out, 0, sizeof(out));

    if (start_entries > max_entries)
        start_entries = max_entries;
    tsc_hz = fcb_calibrate_tsc_hz();
    if (tsc_hz == 0u) tsc_hz = UINT64_C(2500000000);
    tsc_per_key = tsc_hz / keys_per_sec;
    if (tsc_per_key == 0u) tsc_per_key = 1u;
    rounds = (unsigned)(((uint64_t)keys_per_sec * (uint64_t)timeout_ms / 1000u
                         + FCB_QUERY - 1u) / FCB_QUERY);
    if (rounds == 0u)
        rounds = 1u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_tsc = tsc_hz;
    cfg.pressure_empty_slots = FCB_PRESSURE;
    FCB_FN(ctx_init_cfg)(&ctx, nb_bk, max_entries, &cfg);

    prefill_keys = fcb_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query        = fcb_alloc((size_t)FCB_QUERY * sizeof(*query));
    results      = fcb_alloc((size_t)FCB_QUERY * sizeof(*results));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = FCB_MAKE_KEY(i);

    now = 1u;
    (void)FCB_FN(prefill)(&ctx, prefill_keys, start_entries, now);
    start_active = FCB_FN(active_scan)(&ctx);
    active = start_active;
    max_seen = start_active;
    before = FCB_FN(stats_snapshot)(&ctx);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FCB_QUERY * hit_pct) / 100u;
        unsigned miss_base = 50000000u + round * FCB_QUERY;
        uint64_t t0, t1;

        now += tsc_per_key * FCB_QUERY;
        for (unsigned i = 0; i < FCB_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query[i] = prefill_keys[idx];
            } else {
                query[i] = FCB_MAKE_KEY(miss_base + i);
            }
        }
        t0 = fcb_rdtsc();
        FCB_CALL_FINDADD(&ctx.fc, query, FCB_QUERY, now, results);
        t1 = fcb_rdtsc();
        total_cycles += t1 - t0;
        active = FCB_FN(active_scan)(&ctx);
        if (active > max_seen) max_seen = active;
    }

    after = FCB_FN(stats_snapshot)(&ctx);
    delta = FCB_FN(stats_diff)(after, before);
    out.cycles_per_key = (double)total_cycles / (double)(rounds * FCB_QUERY);
    out.hit_pct = delta.lookups
        ? (100.0 * (double)delta.hits / (double)delta.lookups)
        : 0.0;
    out.misses = delta.misses;
    out.relief_evictions = delta.relief_evictions;
    out.oldest_reclaim_evictions = delta.oldest_reclaim_evictions;
    out.fill_start_pct = 100.0 * (double)start_active / (double)total_slots;
    out.fill_end_pct = 100.0 * (double)active / (double)total_slots;
    out.fill_max_pct = 100.0 * (double)max_seen / (double)total_slots;
    out.rounds = rounds;

    free(results);
    free(query);
    free(prefill_keys);
    FCB_FN(ctx_free)(&ctx);
    return out;
}

static void
FCB_FN(findadd_open)(unsigned desired_entries, unsigned nb_bk,
                     unsigned start_fill_pct, unsigned hit_pct,
                     unsigned keys_per_sec, unsigned timeout_ms)
{
    struct fcb_run_summary s =
        FCB_FN(findadd_open_summary)(desired_entries, nb_bk,
                                     start_fill_pct, hit_pct,
                                     keys_per_sec, timeout_ms);

    printf("fc : cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  oldest=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           s.cycles_per_key, s.hit_pct, s.misses,
           s.relief_evictions, s.oldest_reclaim_evictions,
           s.fill_start_pct, s.fill_end_pct, s.fill_max_pct);
}

/*===========================================================================
 * Windowed open-set path:
 *   - fresh miss stream
 *   - maintenance runs only when fill exceeds high watermark
 *   - tries to pull fill back toward low watermark
 *===========================================================================*/
static struct fcb_run_summary
FCB_FN(findadd_window_summary)(unsigned desired_entries, unsigned nb_bk,
                               unsigned low_fill_pct, unsigned high_fill_pct,
                               unsigned hit_pct, unsigned keys_per_sec,
                               unsigned ttl_ms, unsigned duration_ms)
{
    struct FCB_FN(ctx) ctx;
    FCB_CONFIG_T cfg;
    FCB_KEY_T *prefill_keys;
    FCB_KEY_T *query;
    FCB_RESULT_T *results;
    struct FCB_FN(stats_delta) before, after, delta;
    struct fcb_run_summary out;
    unsigned max_entries = fcb_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_fill_pct = (low_fill_pct + high_fill_pct) / 2u;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned warmup_rounds;
    unsigned hit_n;
    unsigned maint_cursor = 0u;
    unsigned active, min_seen, max_seen;
    unsigned over_high_rounds = 0u;
    uint64_t fill_sum = 0u;
    uint64_t tsc_hz, tsc_per_key, now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls_sum = 0u;
    uint64_t maint_evictions_sum = 0u;

    memset(&out, 0, sizeof(out));

    if (low_fill_pct > high_fill_pct || high_fill_pct > 100u) {
        fprintf(stderr, "findadd_window requires 0 <= low_fill <= high_fill <= 100\n");
        exit(2);
    }
    if (start_entries > max_entries)
        start_entries = max_entries;
    tsc_hz = fcb_calibrate_tsc_hz();
    if (tsc_hz == 0u) tsc_hz = UINT64_C(2500000000);
    tsc_per_key = tsc_hz / keys_per_sec;
    if (tsc_per_key == 0u) tsc_per_key = 1u;
    rounds = (unsigned)(((uint64_t)keys_per_sec * (uint64_t)duration_ms / 1000u
                         + FCB_QUERY - 1u) / FCB_QUERY);
    if (rounds == 0u)
        rounds = 1u;
    warmup_rounds = (unsigned)(((uint64_t)keys_per_sec * (uint64_t)ttl_ms / 1000u
                                + FCB_QUERY - 1u) / FCB_QUERY);
    if (warmup_rounds == 0u)
        warmup_rounds = 1u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_tsc = ((tsc_hz / 1000u) * ttl_ms);
    if (cfg.timeout_tsc == 0u)
        cfg.timeout_tsc = 1u;
    cfg.pressure_empty_slots = FCB_PRESSURE;
    FCB_FN(ctx_init_cfg)(&ctx, nb_bk, max_entries, &cfg);

    prefill_keys = fcb_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query        = fcb_alloc((size_t)FCB_QUERY * sizeof(*query));
    results      = fcb_alloc((size_t)FCB_QUERY * sizeof(*results));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = FCB_MAKE_KEY(i);

    now = 1u;
    (void)FCB_FN(prefill)(&ctx, prefill_keys, start_entries, now);
    hit_n = (FCB_QUERY * hit_pct) / 100u;

    for (unsigned round = 0; round < warmup_rounds; round++) {
        unsigned miss_base = 90000000u + round * FCB_QUERY;

        now += tsc_per_key * FCB_QUERY;
        for (unsigned i = 0; i < FCB_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query[i] = prefill_keys[idx];
            } else {
                query[i] = FCB_MAKE_KEY(miss_base + i);
            }
        }

        FCB_CALL_FINDADD(&ctx.fc, query, FCB_QUERY, now, results);
        active = FCB_FN(active_scan)(&ctx);
        while (ctx.fc.total_slots != 0u &&
               (active * 100u) > (high_fill_pct * total_slots)) {
            unsigned sweep_budget = (nb_bk + 15u) / 16u;
            unsigned evicted_this_sweep = 0u;
            if (sweep_budget == 0u)
                sweep_budget = 1u;
            while ((active * 100u) > (low_fill_pct * total_slots) &&
                   sweep_budget-- > 0u) {
                unsigned evicted = FCB_API(maintain)(&ctx.fc, maint_cursor,
                                                     16u, now);
                maint_cursor += 16u;
                if (maint_cursor >= nb_bk)
                    maint_cursor -= nb_bk;
                maint_calls_sum++;
                maint_evictions_sum += evicted;
                evicted_this_sweep += evicted;
                active = FCB_FN(active_scan)(&ctx);
            }
            if ((active * 100u) <= (high_fill_pct * total_slots) ||
                evicted_this_sweep == 0u)
                break;
        }
    }

    active = FCB_FN(active_scan)(&ctx);
    min_seen = active;
    max_seen = active;
    fill_sum = active;
    before = FCB_FN(stats_snapshot)(&ctx);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 90000000u + (warmup_rounds + round) * FCB_QUERY;
        uint64_t t0, t1;

        now += tsc_per_key * FCB_QUERY;
        for (unsigned i = 0; i < FCB_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query[i] = prefill_keys[idx];
            } else {
                query[i] = FCB_MAKE_KEY(miss_base + i);
            }
        }

        t0 = fcb_rdtsc();
        FCB_CALL_FINDADD(&ctx.fc, query, FCB_QUERY, now, results);
        t1 = fcb_rdtsc();
        total_cycles += t1 - t0;

        active = FCB_FN(active_scan)(&ctx);
        while (ctx.fc.total_slots != 0u &&
               (active * 100u) > (high_fill_pct * total_slots)) {
            unsigned sweep_budget = (nb_bk + 15u) / 16u;
            unsigned evicted_this_sweep = 0u;
            if (sweep_budget == 0u)
                sweep_budget = 1u;
            while ((active * 100u) > (low_fill_pct * total_slots) &&
                   sweep_budget-- > 0u) {
                unsigned evicted = FCB_API(maintain)(&ctx.fc, maint_cursor,
                                                     16u, now);
                maint_cursor += 16u;
                if (maint_cursor >= nb_bk)
                    maint_cursor -= nb_bk;
                maint_calls_sum++;
                maint_evictions_sum += evicted;
                evicted_this_sweep += evicted;
                active = FCB_FN(active_scan)(&ctx);
            }
            if ((active * 100u) <= (high_fill_pct * total_slots) ||
                evicted_this_sweep == 0u)
                break;
        }

        if ((active * 100u) > (high_fill_pct * total_slots))
            over_high_rounds++;
        if (active < min_seen)
            min_seen = active;
        if (active > max_seen)
            max_seen = active;
        fill_sum += active;
    }

    after = FCB_FN(stats_snapshot)(&ctx);
    delta = FCB_FN(stats_diff)(after, before);
    out.cycles_per_key = (double)total_cycles / (double)(rounds * FCB_QUERY);
    out.hit_pct = delta.lookups
        ? (100.0 * (double)delta.hits / (double)delta.lookups)
        : 0.0;
    out.misses = delta.misses;
    out.relief_evictions = delta.relief_evictions;
    out.oldest_reclaim_evictions = delta.oldest_reclaim_evictions;
    out.maint_calls = maint_calls_sum;
    out.maint_evictions = maint_evictions_sum;
    out.fill_avg_pct =
        100.0 * (double)fill_sum / (double)((uint64_t)(rounds + 1u) * total_slots);
    out.fill_min_pct = 100.0 * (double)min_seen / (double)total_slots;
    out.fill_max_pct = 100.0 * (double)max_seen / (double)total_slots;
    out.over_high_rounds = over_high_rounds;
    out.rounds = rounds;

    free(results);
    free(query);
    free(prefill_keys);
    FCB_FN(ctx_free)(&ctx);
    return out;
}

static void
FCB_FN(findadd_window)(unsigned desired_entries, unsigned nb_bk,
                       unsigned low_fill_pct, unsigned high_fill_pct,
                       unsigned hit_pct, unsigned keys_per_sec,
                       unsigned ttl_ms, unsigned duration_ms)
{
    struct fcb_run_summary s =
        FCB_FN(findadd_window_summary)(desired_entries, nb_bk,
                                       low_fill_pct, high_fill_pct,
                                       hit_pct, keys_per_sec,
                                       ttl_ms, duration_ms);

    printf("fc : cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  oldest=%" PRIu64
           "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  avg_fill=%.1f%%  min_fill=%.1f%%  max_fill=%.1f%%  over_high=%u/%u\n",
           s.cycles_per_key, s.hit_pct, s.misses,
           s.relief_evictions, s.oldest_reclaim_evictions,
           s.maint_calls, s.maint_evictions,
           s.fill_avg_pct, s.fill_min_pct, s.fill_max_pct,
           s.over_high_rounds, s.rounds);
}

/*===========================================================================
 * Persistent open-set path: batch-maint trace (custom kick policy)
 *===========================================================================*/
static void
FCB_FN(trace_open_custom)(unsigned desired_entries, unsigned nb_bk,
                          unsigned start_fill_pct, unsigned hit_pct,
                          unsigned keys_per_sec, unsigned timeout_ms,
                          unsigned kick_scale, unsigned soak_mul,
                          unsigned report_ms,
                          unsigned fill0, unsigned fill1,
                          unsigned fill2, unsigned fill3,
                          unsigned kicks0, unsigned kicks1,
                          unsigned kicks2, unsigned kicks3)
{
    struct FCB_FN(ctx) ctx;
    FCB_CONFIG_T cfg;
    FCB_KEY_T *prefill_keys;
    FCB_KEY_T *query;
    FCB_RESULT_T *results;
    struct FCB_FN(stats_delta) before;
    unsigned max_entries = fcb_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds, report_rounds;
    unsigned hit_n;
    unsigned maint_cursor = 0u;
    unsigned active, max_seen;
    uint64_t tsc_hz, tsc_per_key, now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls_sum = 0u;
    uint64_t maint_evictions_sum = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;
    tsc_hz = fcb_calibrate_tsc_hz();
    if (tsc_hz == 0u) tsc_hz = UINT64_C(2500000000);
    tsc_per_key = tsc_hz / keys_per_sec;
    if (tsc_per_key == 0u) tsc_per_key = 1u;
    rounds = (unsigned)(((uint64_t)keys_per_sec * ((uint64_t)timeout_ms / 1000u)
                         + FCB_QUERY - 1u) / FCB_QUERY);
    if (rounds < 1u) rounds = 1u;
    rounds *= soak_mul;
    report_rounds = (unsigned)(((uint64_t)keys_per_sec * ((uint64_t)report_ms / 1000u)
                                + FCB_QUERY - 1u) / FCB_QUERY);
    if (report_rounds == 0u) report_rounds = 1u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FCB_PRESSURE;
    FCB_FN(ctx_init_cfg)(&ctx, nb_bk, max_entries, &cfg);

    prefill_keys = fcb_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query        = fcb_alloc((size_t)FCB_QUERY * sizeof(*query));
    results      = fcb_alloc((size_t)FCB_QUERY * sizeof(*results));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = FCB_MAKE_KEY(i);

    now = 1u;
    (void)FCB_FN(prefill)(&ctx, prefill_keys, start_entries, now);
    active = FCB_FN(active_scan)(&ctx);
    max_seen = active;
    before = FCB_FN(stats_snapshot)(&ctx);
    hit_n = (FCB_QUERY * hit_pct) / 100u;

    printf("\nfc open-set batch-maint trace\n");
    printf("keys/s=%u  req/s=%.1f  hit_target=%u%%  start_fill=%u%%  timeout=%ums  soak=%ux"
           "  nb_bk=%u  pool=%u  kicks[%u/%u/%u/%u]=%u/%u/%u/%u * %u\n",
           keys_per_sec, (double)keys_per_sec / (double)FCB_QUERY,
           hit_pct, start_fill_pct, timeout_ms, soak_mul,
           nb_bk, max_entries,
           fill0, fill1, fill2, fill3,
           kicks0, kicks1, kicks2, kicks3, kick_scale);
    printf(" time_s  fill%%  max%%  maint_calls  maint_evict  relief_evict  oldest_evict\n");

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 80000000u + round * FCB_QUERY;
        unsigned fill_pct;
        unsigned kicks;
        uint64_t t0, t1;

        now += tsc_per_key * FCB_QUERY;
        for (unsigned i = 0; i < FCB_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query[i] = prefill_keys[idx];
            } else {
                query[i] = FCB_MAKE_KEY(miss_base + i);
            }
        }

        t0 = fcb_rdtsc();
        FCB_API(findadd_bulk)(&ctx.fc, query, FCB_QUERY, now, results);

        if (ctx.fc.total_slots != 0u)
            fill_pct = (unsigned)((ctx.fc.ht_head.rhh_nb * 100u) / ctx.fc.total_slots);
        else
            fill_pct = 0u;
        kicks = fcb_batch_maint_kicks(fill_pct,
                                       fill0, fill1, fill2, fill3,
                                       kicks0, kicks1, kicks2, kicks3,
                                       kick_scale);
        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = FCB_API(maintain)(&ctx.fc, maint_cursor,
                                                  16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls_sum++;
            maint_evictions_sum += evicted;
        }
        t1 = fcb_rdtsc();
        total_cycles += t1 - t0;

        active = FCB_FN(active_scan)(&ctx);
        if (active > max_seen) max_seen = active;
        if (((round + 1u) % report_rounds) == 0u || round + 1u == rounds) {
            struct FCB_FN(stats_delta) mid =
                FCB_FN(stats_diff)(FCB_FN(stats_snapshot)(&ctx), before);
            double sim_sec = ((double)(round + 1u) * (double)FCB_QUERY)
                             / (double)keys_per_sec;
            printf("%7.2f  %5.1f  %4.1f  %11" PRIu64 "  %11" PRIu64
                   "  %12" PRIu64 "  %12" PRIu64 "\n",
                   sim_sec,
                   100.0 * (double)active / (double)total_slots,
                   100.0 * (double)max_seen / (double)total_slots,
                   maint_calls_sum, maint_evictions_sum,
                   mid.relief_evictions,
                   mid.oldest_reclaim_evictions);
        }
    }

    {
        struct FCB_FN(stats_delta) delta =
            FCB_FN(stats_diff)(FCB_FN(stats_snapshot)(&ctx), before);
        printf("summary: cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
               "  relief=%" PRIu64 "  maint_calls=%" PRIu64
               "  maint_evict=%" PRIu64 "  oldest=%" PRIu64
               "  end_fill=%.1f%%  max_fill=%.1f%%\n",
               (double)total_cycles / (double)(rounds * FCB_QUERY),
               delta.lookups
                   ? (100.0 * (double)delta.hits / (double)delta.lookups)
                   : 0.0,
               delta.misses, delta.relief_evictions,
               maint_calls_sum, maint_evictions_sum,
               delta.oldest_reclaim_evictions,
               100.0 * (double)active / (double)total_slots,
               100.0 * (double)max_seen / (double)total_slots);
    }

    free(results);
    free(query);
    free(prefill_keys);
    FCB_FN(ctx_free)(&ctx);
}

/*===========================================================================
 * maintain_step benchmark
 *
 * Uses idle=1 to force full sweep, bypassing throttle logic.
 *===========================================================================*/
static double
FCB_FN(bench_maint_step)(struct FCB_FN(ctx) *ctx,
                          const FCB_KEY_T *fill_keys, unsigned fill_n,
                          unsigned repeat)
{
    uint64_t *samples = fcb_alloc((size_t)repeat * sizeof(*samples));

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t fill_ts = (uint64_t)r * 10000u + 1u;
        uint64_t expire_ts = fill_ts + ctx->fc.timeout_tsc + 1u;
        uint64_t t0, t1;

        FCB_FN(ctx_reset)(ctx);
        (void)FCB_FN(prefill)(ctx, fill_keys, fill_n, fill_ts);

        t0 = fcb_rdtsc();
        (void)FCB_API(maintain_step)(&ctx->fc, expire_ts, 1);
        t1 = fcb_rdtsc();
        samples[r] = t1 - t0;
    }
    {
        double med = fcb_median_u64(samples, repeat);
        free(samples);
        return med;
    }
}

static void
FCB_FN(bench_maintain_step_report)(unsigned desired, unsigned nb_bk)
{
    struct FCB_FN(ctx) ctx;
    unsigned max_entries = fcb_pool_count(desired);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    FCB_KEY_T *keys;
    enum { REPEAT = 50u };
    unsigned fill_pcts[] = { 0u, 25u, 50u, 75u, 100u };

    FCB_FN(ctx_init)(&ctx, nb_bk, max_entries, 1000u);
    keys = fcb_alloc((size_t)max_entries * sizeof(*keys));
    for (unsigned i = 0; i < max_entries; i++)
        keys[i] = FCB_MAKE_KEY(i);

    for (unsigned p = 0; p < sizeof(fill_pcts) / sizeof(fill_pcts[0]); p++) {
        unsigned fill_n = (unsigned)(((uint64_t)total_slots * fill_pcts[p]) / 100u);
        double cy;

        if (fill_n > max_entries) fill_n = max_entries;
        cy = FCB_FN(bench_maint_step)(&ctx, keys, fill_n, REPEAT);
        printf("    fill=%3u%%  entries=%-6u  cy/call=%8.0f  cy/bk=%6.1f",
               fill_pcts[p], fill_n, cy, cy / (double)nb_bk);
        if (fill_n > 0u)
            printf("  cy/entry=%6.1f", cy / (double)fill_n);
        printf("\n");
    }

    free(keys);
    FCB_FN(ctx_free)(&ctx);
}

/*===========================================================================
 * maintain_step partial-sweep benchmark
 *
 * Configures maint_base_bk=step and maint_interval_tsc=0 (run every call),
 * then calls maintain_step(idle=0) repeatedly.
 *===========================================================================*/
static void
FCB_FN(bench_maint_step_partial)(unsigned desired, unsigned nb_bk,
                                   unsigned fill_pct,
                                   const unsigned *step_bks, unsigned n_steps)
{
    unsigned max_entries = fcb_pool_count(desired);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned fill_n = (unsigned)(((uint64_t)total_slots * fill_pct) / 100u);
    FCB_KEY_T *keys;
    enum { REPEAT = 20u };

    if (fill_n > max_entries) fill_n = max_entries;

    keys = fcb_alloc((size_t)max_entries * sizeof(*keys));
    for (unsigned i = 0; i < max_entries; i++)
        keys[i] = FCB_MAKE_KEY(i);

    for (unsigned s = 0; s < n_steps; s++) {
        unsigned step = step_bks[s];
        unsigned calls_per_sweep = (nb_bk + step - 1u) / step;
        uint64_t *samples = fcb_alloc((size_t)REPEAT * sizeof(*samples));
        struct FCB_FN(ctx) ctx;
        FCB_CONFIG_T cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.timeout_tsc = 1000u;
        cfg.pressure_empty_slots = FCB_PRESSURE;
        cfg.maint_base_bk = step;
        /* maint_interval_tsc=0 -> run every call */
        FCB_FN(ctx_init_cfg)(&ctx, nb_bk, max_entries, &cfg);

        for (unsigned r = 0; r < REPEAT; r++) {
            uint64_t fill_ts = (uint64_t)r * 10000u + 1u;
            uint64_t expire_ts = fill_ts + ctx.fc.timeout_tsc + 1u;
            uint64_t sample_total = 0u;

            FCB_FN(ctx_reset)(&ctx);
            (void)FCB_FN(prefill)(&ctx, keys, fill_n, fill_ts);

            for (unsigned c = 0; c < calls_per_sweep; c++) {
                uint64_t t0 = fcb_rdtsc();
                (void)FCB_API(maintain_step)(&ctx.fc, expire_ts, 0);
                uint64_t t1 = fcb_rdtsc();
                sample_total += t1 - t0;
            }
            samples[r] = sample_total;
        }

        double cy_per_call = fcb_median_u64(samples, REPEAT) / (double)calls_per_sweep;
        double cy_per_bk = cy_per_call / (double)step;
        unsigned evict_per_call = fill_n / calls_per_sweep;

        printf("    step=%5u  calls/sweep=%3u  cy/call=%8.0f  cy/bk=%6.1f",
               step, calls_per_sweep, cy_per_call, cy_per_bk);
        if (evict_per_call > 0u)
            printf("  cy/entry=%6.1f", cy_per_call / (double)evict_per_call);
        printf("\n");

        FCB_FN(ctx_free)(&ctx);
        free(samples);
    }

    free(keys);
}

/* Clean up macros for next inclusion */
#undef FCB_PREFIX
#undef FCB_KEY_T
#undef FCB_RESULT_T
#undef FCB_ENTRY_T
#undef FCB_CACHE_T
#undef FCB_CONFIG_T
#undef FCB_STATS_T
#undef FCB_PRESSURE
#undef FCB_MAKE_KEY
#undef FCB_FN
#undef FCB_API
#undef FCB_PUB
#undef _FCB_CAT
#undef _FCB_CAT2

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
