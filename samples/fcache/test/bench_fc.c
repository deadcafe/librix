/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_fc.c - fcache benchmark: flow4 / flow6 / flowu.
 *
 * Structured as:
 *   bench_fc_common.h   shared utilities
 *   bench_fc_body.h     include-template (one instantiation per variant)
 *   bench_fc.c          key generators, variant dispatch, main
 *
 * Usage:
 *   fc_bench [variant] <mode> [args...]
 *   fc_bench datapath
 *   fc_bench flow4 findadd_closed <desired> <fill%> <hit%> [rounds]
 *   fc_bench flow6 findadd_open <desired> <start_fill%> <hit%> <keys/s> [timeout_ms]
 *   fc_bench flow6 findadd_window <desired> <low_fill%> <high_fill%> <hit%> <keys/s> <ttl_ms> [duration_ms]
 *   fc_bench flow6 trace_open_custom <desired> <start_fill%> <hit%> <keys/s> ...
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "bench_fc_common.h"
#include <errno.h>
#include <sched.h>

unsigned fcb_query = FCB_QUERY_DEFAULT;
unsigned fcb_dp_hit_repeat = 96u;
unsigned fcb_dp_miss_repeat = 32u;
unsigned fcb_dp_mixed_repeat = 32u;
unsigned fcb_findadd_api_mode = FCB_FINDADD_API_BULK;
unsigned fcb_sample_raw_repeat = 1u;
unsigned fcb_sample_keep_n = 1u;
int fcb_pin_core = -1;

enum fcb_bench_profile {
    FCB_BENCH_FULL = 0,
    FCB_BENCH_SHORT,
};

enum fcb_bench_profile fcb_bench_profile = FCB_BENCH_FULL;

#define FCB_SHORT_COLD_RAW_REPEAT 3u
#define FCB_SHORT_COLD_KEEP_N    1u
#define FCB_FULL_DP_HIT_REPEAT    96u
#define FCB_FULL_DP_MISS_REPEAT   32u
#define FCB_FULL_DP_MIXED_REPEAT  32u
#define FCB_SHORT_DP_HIT_REPEAT   48u
#define FCB_SHORT_DP_MISS_REPEAT  16u
#define FCB_SHORT_DP_MIXED_REPEAT 16u

enum {
    FCB_DP_FINDADD_HIT = 0,
    FCB_DP_FIND_HIT,
    FCB_DP_FINDADD_MISS,
    FCB_DP_ADD_ONLY,
    FCB_DP_ADD_DEL,
    FCB_DP_DEL_BULK,
    FCB_DP_MIXED_90_10_RESET,
    FCB_DP_METRIC_COUNT
};

struct fcb_datapath_report {
    unsigned desired;
    double metrics[FCB_DP_METRIC_COUNT][3];
};

static const char *const fcb_datapath_labels[FCB_DP_METRIC_COUNT] = {
    "findadd_hit",
    "find_hit",
    "findadd_miss",
    "add_only",
    "add+del",
    "del_bulk",
    "mixed_90_10_reset",
};

static inline double
fcb_archcmp_gain_pct(double winner, double loser)
{
    return (winner > 0.0) ? ((loser / winner) - 1.0) * 100.0 : 0.0;
}

static int
fcb_cmp_double(const void *a, const void *b)
{
    const double av = *(const double *)a;
    const double bv = *(const double *)b;
    return (av > bv) - (av < bv);
}

static double
fcb_median_double(double *samples, unsigned n)
{
    qsort(samples, n, sizeof(samples[0]), fcb_cmp_double);
    if ((n & 1u) != 0u)
        return samples[n / 2u];
    return (samples[(n / 2u) - 1u] + samples[n / 2u]) * 0.5;
}

static double
fcb_trimmed_window_median_double(double *samples, unsigned raw_n,
                                 unsigned keep_n)
{
    unsigned best_i = 0u;
    double best_span;

    if (raw_n == 0u)
        return 0.0;

    qsort(samples, raw_n, sizeof(samples[0]), fcb_cmp_double);
    if (keep_n == 0u || keep_n >= raw_n)
        return fcb_median_double(samples, raw_n);

    best_span = samples[keep_n - 1u] - samples[0];
    for (unsigned i = 1u; i + keep_n <= raw_n; i++) {
        double span = samples[i + keep_n - 1u] - samples[i];
        if (span < best_span) {
            best_span = span;
            best_i = i;
        }
    }

    if ((keep_n & 1u) != 0u)
        return samples[best_i + (keep_n / 2u)];
    return (samples[best_i + (keep_n / 2u) - 1u]
            + samples[best_i + (keep_n / 2u)]) * 0.5;
}

static int
fcb_cmp_run_summary_cycles(const void *a, const void *b)
{
    const struct fcb_run_summary *av = a;
    const struct fcb_run_summary *bv = b;
    return (av->cycles_per_key > bv->cycles_per_key)
        - (av->cycles_per_key < bv->cycles_per_key);
}

static double
fcb_median_summary_double(const struct fcb_run_summary *samples,
                          unsigned start, unsigned n,
                          double (*field)(const struct fcb_run_summary *))
{
    double *vals = fcb_alloc((size_t)n * sizeof(*vals));
    double med;

    for (unsigned i = 0; i < n; i++)
        vals[i] = field(&samples[start + i]);
    med = fcb_median_double(vals, n);
    free(vals);
    return med;
}

static uint64_t
fcb_median_summary_u64(const struct fcb_run_summary *samples,
                       unsigned start, unsigned n,
                       uint64_t (*field)(const struct fcb_run_summary *))
{
    uint64_t *vals = fcb_alloc((size_t)n * sizeof(*vals));
    double med;

    for (unsigned i = 0; i < n; i++)
        vals[i] = field(&samples[start + i]);
    med = fcb_median_u64(vals, n);
    free(vals);
    return (uint64_t)(med + 0.5);
}

static unsigned
fcb_median_summary_unsigned(const struct fcb_run_summary *samples,
                            unsigned start, unsigned n,
                            unsigned (*field)(const struct fcb_run_summary *))
{
    uint64_t *vals = fcb_alloc((size_t)n * sizeof(*vals));
    double med;

    for (unsigned i = 0; i < n; i++)
        vals[i] = field(&samples[start + i]);
    med = fcb_median_u64(vals, n);
    free(vals);
    return (unsigned)(med + 0.5);
}

static double fcb_field_cycles(const struct fcb_run_summary *s) { return s->cycles_per_key; }
static double fcb_field_hit_pct(const struct fcb_run_summary *s) { return s->hit_pct; }
static uint64_t fcb_field_misses(const struct fcb_run_summary *s) { return s->misses; }
static uint64_t fcb_field_relief(const struct fcb_run_summary *s) { return s->relief_evictions; }
static uint64_t fcb_field_oldest(const struct fcb_run_summary *s) { return s->oldest_reclaim_evictions; }
static uint64_t fcb_field_maint_calls(const struct fcb_run_summary *s) { return s->maint_calls; }
static uint64_t fcb_field_maint_evict(const struct fcb_run_summary *s) { return s->maint_evictions; }
static double fcb_field_fill_start(const struct fcb_run_summary *s) { return s->fill_start_pct; }
static double fcb_field_fill_end(const struct fcb_run_summary *s) { return s->fill_end_pct; }
static double fcb_field_fill_avg(const struct fcb_run_summary *s) { return s->fill_avg_pct; }
static double fcb_field_fill_min(const struct fcb_run_summary *s) { return s->fill_min_pct; }
static double fcb_field_fill_max(const struct fcb_run_summary *s) { return s->fill_max_pct; }
static unsigned fcb_field_over_high(const struct fcb_run_summary *s) { return s->over_high_rounds; }
static unsigned fcb_field_rounds(const struct fcb_run_summary *s) { return s->rounds; }

static struct fcb_run_summary
fcb_aggregate_run_summaries(struct fcb_run_summary *samples,
                            unsigned raw_n, unsigned keep_n)
{
    struct fcb_run_summary out;
    unsigned best_i = 0u;
    double best_span;

    memset(&out, 0, sizeof(out));
    if (raw_n == 0u)
        return out;

    qsort(samples, raw_n, sizeof(samples[0]), fcb_cmp_run_summary_cycles);
    if (keep_n == 0u || keep_n >= raw_n)
        keep_n = raw_n;

    best_span = samples[keep_n - 1u].cycles_per_key - samples[0].cycles_per_key;
    for (unsigned i = 1u; i + keep_n <= raw_n; i++) {
        double span =
            samples[i + keep_n - 1u].cycles_per_key - samples[i].cycles_per_key;
        if (span < best_span) {
            best_span = span;
            best_i = i;
        }
    }

    out.cycles_per_key =
        fcb_median_summary_double(samples, best_i, keep_n, fcb_field_cycles);
    out.hit_pct =
        fcb_median_summary_double(samples, best_i, keep_n, fcb_field_hit_pct);
    out.misses =
        fcb_median_summary_u64(samples, best_i, keep_n, fcb_field_misses);
    out.relief_evictions =
        fcb_median_summary_u64(samples, best_i, keep_n, fcb_field_relief);
    out.oldest_reclaim_evictions =
        fcb_median_summary_u64(samples, best_i, keep_n, fcb_field_oldest);
    out.maint_calls =
        fcb_median_summary_u64(samples, best_i, keep_n, fcb_field_maint_calls);
    out.maint_evictions =
        fcb_median_summary_u64(samples, best_i, keep_n, fcb_field_maint_evict);
    out.fill_start_pct =
        fcb_median_summary_double(samples, best_i, keep_n, fcb_field_fill_start);
    out.fill_end_pct =
        fcb_median_summary_double(samples, best_i, keep_n, fcb_field_fill_end);
    out.fill_avg_pct =
        fcb_median_summary_double(samples, best_i, keep_n, fcb_field_fill_avg);
    out.fill_min_pct =
        fcb_median_summary_double(samples, best_i, keep_n, fcb_field_fill_min);
    out.fill_max_pct =
        fcb_median_summary_double(samples, best_i, keep_n, fcb_field_fill_max);
    out.over_high_rounds =
        fcb_median_summary_unsigned(samples, best_i, keep_n, fcb_field_over_high);
    out.rounds =
        fcb_median_summary_unsigned(samples, best_i, keep_n, fcb_field_rounds);
    return out;
}

static void
fcb_print_sampling_config(void)
{
    printf("sampling: raw_repeat=%u  keep_n=%u\n",
           fcb_sample_raw_repeat, fcb_sample_keep_n);
}

static int
fcb_mode_is_cold(const char *mode)
{
    return strcmp(mode, "datapath") == 0
        || strcmp(mode, "maint") == 0
        || strcmp(mode, "maint_partial") == 0;
}

struct fcb_sampling_guard {
    unsigned raw_repeat;
    unsigned keep_n;
    unsigned dp_hit_repeat;
    unsigned dp_miss_repeat;
    unsigned dp_mixed_repeat;
    int active;
};

static void
fcb_apply_mode_sampling_profile(const char *mode, struct fcb_sampling_guard *guard)
{
    guard->raw_repeat = fcb_sample_raw_repeat;
    guard->keep_n = fcb_sample_keep_n;
    guard->dp_hit_repeat = fcb_dp_hit_repeat;
    guard->dp_miss_repeat = fcb_dp_miss_repeat;
    guard->dp_mixed_repeat = fcb_dp_mixed_repeat;
    guard->active = 0;

    if (fcb_bench_profile != FCB_BENCH_SHORT || !fcb_mode_is_cold(mode))
        return;

    if (fcb_sample_raw_repeat > FCB_SHORT_COLD_RAW_REPEAT)
        fcb_sample_raw_repeat = FCB_SHORT_COLD_RAW_REPEAT;
    if (fcb_sample_keep_n > FCB_SHORT_COLD_KEEP_N)
        fcb_sample_keep_n = FCB_SHORT_COLD_KEEP_N;
    if (fcb_sample_keep_n > fcb_sample_raw_repeat)
        fcb_sample_keep_n = fcb_sample_raw_repeat;
    if (strcmp(mode, "datapath") == 0) {
        fcb_dp_hit_repeat = FCB_SHORT_DP_HIT_REPEAT;
        fcb_dp_miss_repeat = FCB_SHORT_DP_MISS_REPEAT;
        fcb_dp_mixed_repeat = FCB_SHORT_DP_MIXED_REPEAT;
    }
    guard->active = 1;
}

static void
fcb_restore_mode_sampling_profile(const struct fcb_sampling_guard *guard)
{
    if (!guard->active)
        return;
    fcb_sample_raw_repeat = guard->raw_repeat;
    fcb_sample_keep_n = guard->keep_n;
    fcb_dp_hit_repeat = guard->dp_hit_repeat;
    fcb_dp_miss_repeat = guard->dp_miss_repeat;
    fcb_dp_mixed_repeat = guard->dp_mixed_repeat;
}

static int
fcb_apply_pin_core(void)
{
    cpu_set_t set;

    if (fcb_pin_core < 0)
        return 0;

    CPU_ZERO(&set);
    CPU_SET((unsigned)fcb_pin_core, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "sched_setaffinity(core=%d) failed: %s\n",
                fcb_pin_core, strerror(errno));
        return 2;
    }
    return 0;
}

static const char *
fcb_findadd_api_name(void)
{
    return (fcb_findadd_api_mode == FCB_FINDADD_API_BURST32)
        ? "burst32" : "bulk";
}

static void
fcb_print_archcmp_scalar_summary(const char *mode,
                                 const char *lhs_name,
                                 struct fcb_run_summary lhs,
                                 const char *rhs_name,
                                 struct fcb_run_summary rhs)
{
    const char *winner_name;
    double winner;
    double loser;

    if (lhs.cycles_per_key <= rhs.cycles_per_key) {
        winner_name = lhs_name;
        winner = lhs.cycles_per_key;
        loser = rhs.cycles_per_key;
    } else {
        winner_name = rhs_name;
        winner = rhs.cycles_per_key;
        loser = lhs.cycles_per_key;
    }

    printf("[archcmp summary]\n\n");
    printf("%s: %s=%.2f  %s=%.2f  winner=%s  (%s faster by %.1f%%)\n",
           mode,
           lhs_name, lhs.cycles_per_key,
           rhs_name, rhs.cycles_per_key,
           winner_name, winner_name,
           fcb_archcmp_gain_pct(winner, loser));
}

static void
fcb_print_archcmp_datapath_summary(const char *lhs_name,
                                   const struct fcb_datapath_report *lhs,
                                   const char *rhs_name,
                                   const struct fcb_datapath_report *rhs,
                                   unsigned n_reports)
{
    unsigned lhs_total_wins = 0u;
    unsigned rhs_total_wins = 0u;

    printf("[archcmp summary]\n\n");
    for (unsigned r = 0; r < n_reports; r++) {
        unsigned lhs_wins = 0u;
        unsigned rhs_wins = 0u;

        for (unsigned m = 0; m < FCB_DP_METRIC_COUNT; m++) {
            for (unsigned v = 0; v < 3u; v++) {
                if (lhs[r].metrics[m][v] <= rhs[r].metrics[m][v])
                    lhs_wins++;
                else
                    rhs_wins++;
            }
        }
        lhs_total_wins += lhs_wins;
        rhs_total_wins += rhs_wins;
        printf("%uK entries: %s wins=%u  %s wins=%u  winner=%s\n",
               lhs[r].desired / 1024u,
               lhs_name, lhs_wins,
               rhs_name, rhs_wins,
               (lhs_wins >= rhs_wins) ? lhs_name : rhs_name);
    }
    printf("overall: %s wins=%u  %s wins=%u  winner=%s\n",
           lhs_name, lhs_total_wins,
           rhs_name, rhs_total_wins,
           (lhs_total_wins >= rhs_total_wins) ? lhs_name : rhs_name);
}

/*===========================================================================
 * Key generators
 *===========================================================================*/
static inline struct flow4_key
fcb_make_key4(unsigned i)
{
    return fc_flow4_key_make(0x0a000000u | (i & 0x00ffffffu),
                             0x14000000u |
                             ((i * 2654435761u) & 0x00ffffffu),
                             (uint16_t)(1024u + (i & 0x7fffu)),
                             (uint16_t)(2048u + ((i >> 11) & 0x7fffu)),
                             (uint8_t)(6u + (i & 1u)),
                             1u + (i >> 24));
}

static inline struct flow6_key
fcb_make_key6(unsigned i)
{
    uint8_t src[16] = {0x20, 0x01};
    uint8_t dst[16] = {0x20, 0x01};
    uint32_t a, b;

    a = 0x0a000000u | (i & 0x00ffffffu);
    b = 0x14000000u | ((i * 2654435761u) & 0x00ffffffu);
    memcpy(&src[12], &a, 4);
    memcpy(&dst[12], &b, 4);
    return fc_flow6_key_make(src, dst,
                             (uint16_t)(1024u + (i & 0x7fffu)),
                             (uint16_t)(2048u + ((i >> 11) & 0x7fffu)),
                             (uint8_t)(6u + (i & 1u)),
                             1u + (i >> 24));
}

static inline struct flowu_key
fcb_make_keyu(unsigned i)
{
    uint32_t src = 0x0a000000u | (i & 0x00ffffffu);
    uint32_t dst = 0x14000000u | ((i * 2654435761u) & 0x00ffffffu);

    return fc_flowu_key_v4(src, dst,
                            (uint16_t)(1024u + (i & 0x7fffu)),
                            (uint16_t)(2048u + ((i >> 11) & 0x7fffu)),
                            (uint8_t)(6u + (i & 1u)),
                            1u + (i >> 24));
}

static inline void
fcb_flow4_call_findadd(struct fc_flow4_cache *fc,
                       const struct flow4_key *keys,
                       unsigned n, uint64_t now,
                       struct fc_flow4_result *results)
{
    if (fcb_findadd_api_mode == FCB_FINDADD_API_BURST32 && n <= 32u) {
        fc_flow4_cache_findadd_burst32(fc, keys, n, now, results);
    } else {
        fc_flow4_cache_findadd_bulk(fc, keys, n, now, results);
    }
}

/*===========================================================================
 * Instantiate per-variant bench functions via include-template
 *===========================================================================*/

/* --- flow4 --- */
#define FCB_PREFIX    flow4
#define FCB_KEY_T     struct flow4_key
#define FCB_RESULT_T  struct fc_flow4_result
#define FCB_ENTRY_T   struct fc_flow4_entry
#define FCB_CACHE_T   struct fc_flow4_cache
#define FCB_CONFIG_T  struct fc_flow4_config
#define FCB_STATS_T   struct fc_flow4_stats
#define FCB_PRESSURE  FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FCB_MAKE_KEY(i) fcb_make_key4(i)
#define FCB_CALL_FINDADD(fc, keys, n, now, results) \
    fcb_flow4_call_findadd((fc), (keys), (n), (now), (results))
#include "bench_fc_body.h"
#undef FCB_CALL_FINDADD

/* --- flow6 --- */
#define FCB_PREFIX    flow6
#define FCB_KEY_T     struct flow6_key
#define FCB_RESULT_T  struct fc_flow6_result
#define FCB_ENTRY_T   struct fc_flow6_entry
#define FCB_CACHE_T   struct fc_flow6_cache
#define FCB_CONFIG_T  struct fc_flow6_config
#define FCB_STATS_T   struct fc_flow6_stats
#define FCB_PRESSURE  FC_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FCB_MAKE_KEY(i) fcb_make_key6(i)
#include "bench_fc_body.h"

/* --- flowu --- */
#define FCB_PREFIX    flowu
#define FCB_KEY_T     struct flowu_key
#define FCB_RESULT_T  struct fc_flowu_result
#define FCB_ENTRY_T   struct fc_flowu_entry
#define FCB_CACHE_T   struct fc_flowu_cache
#define FCB_CONFIG_T  struct fc_flowu_config
#define FCB_STATS_T   struct fc_flowu_stats
#define FCB_PRESSURE  FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FCB_MAKE_KEY(i) fcb_make_keyu(i)
#include "bench_fc_body.h"

/*===========================================================================
 * Quick 3-variant datapath comparison (no args needed)
 *===========================================================================*/
static void
bench_datapath_one(unsigned desired, struct fcb_datapath_report *report)
{
    struct fcb_datapath_report local_report;
    struct fcb_datapath_report *out = report ? report : &local_report;
    double *metric_samples = NULL;
    unsigned max_entries = fcb_pool_count(desired);
    unsigned nb_bk = fcb_nb_bk_hint(max_entries);
    unsigned prefill_n = max_entries / 2u;
    unsigned hit_key_n = (unsigned)((size_t)FCB_QUERY * fcb_dp_hit_repeat);
    unsigned miss_key_n = (unsigned)((size_t)FCB_QUERY * fcb_dp_miss_repeat);
    unsigned mixed_key_n = (unsigned)((size_t)FCB_QUERY * fcb_dp_mixed_repeat);
    unsigned mixed_hit_n = (FCB_QUERY * 9u) / 10u;

    struct fcb_flow4_ctx ctx4;
    struct fcb_flow6_ctx ctx6;
    struct fcb_flowu_ctx ctxu;

    fcb_flow4_ctx_init(&ctx4, nb_bk, max_entries, 1000000000ull);
    fcb_flow6_ctx_init(&ctx6, nb_bk, max_entries, 1000000000ull);
    fcb_flowu_ctx_init(&ctxu, nb_bk, max_entries, 1000000000ull);

    /* generate keys */
    struct flow4_key *p4 = fcb_alloc((size_t)prefill_n * sizeof(*p4));
    struct flow6_key *p6 = fcb_alloc((size_t)prefill_n * sizeof(*p6));
    struct flowu_key *pu = fcb_alloc((size_t)prefill_n * sizeof(*pu));
    struct flow4_key *h4 = fcb_alloc((size_t)hit_key_n * sizeof(*h4));
    struct flow6_key *h6 = fcb_alloc((size_t)hit_key_n * sizeof(*h6));
    struct flowu_key *hu = fcb_alloc((size_t)hit_key_n * sizeof(*hu));
    struct flow4_key *m4 = fcb_alloc((size_t)miss_key_n * sizeof(*m4));
    struct flow6_key *m6 = fcb_alloc((size_t)miss_key_n * sizeof(*m6));
    struct flowu_key *mu = fcb_alloc((size_t)miss_key_n * sizeof(*mu));
    struct flow4_key *x4 = fcb_alloc((size_t)mixed_key_n * sizeof(*x4));
    struct flow6_key *x6 = fcb_alloc((size_t)mixed_key_n * sizeof(*x6));
    struct flowu_key *xu = fcb_alloc((size_t)mixed_key_n * sizeof(*xu));

    if (fcb_sample_raw_repeat > 1u)
        metric_samples = fcb_alloc((size_t)fcb_sample_raw_repeat
                                   * sizeof(*metric_samples));

    for (unsigned i = 0; i < prefill_n; i++) {
        p4[i] = fcb_make_key4(i);
        p6[i] = fcb_make_key6(i);
        pu[i] = fcb_make_keyu(i);
    }
    for (unsigned i = 0; i < hit_key_n; i++) {
        h4[i] = fcb_make_key4(i);
        h6[i] = fcb_make_key6(i);
        hu[i] = fcb_make_keyu(i);
    }
    for (unsigned i = 0; i < miss_key_n; i++) {
        m4[i] = fcb_make_key4(max_entries + i);
        m6[i] = fcb_make_key6(max_entries + i);
        mu[i] = fcb_make_keyu(max_entries + i);
    }
    for (unsigned r = 0; r < fcb_dp_mixed_repeat; r++) {
        unsigned base = r * FCB_QUERY;
        for (unsigned i = 0; i < FCB_QUERY; i++) {
            unsigned idx = base + i;
            if (i < mixed_hit_n) {
                x4[idx] = fcb_make_key4(prefill_n + idx);
                x6[idx] = fcb_make_key6(prefill_n + idx);
                xu[idx] = fcb_make_keyu(prefill_n + idx);
            } else {
                unsigned miss_off = idx - mixed_hit_n;
                x4[idx] = fcb_make_key4(max_entries + hit_key_n + miss_key_n
                                        + mixed_key_n + miss_off);
                x6[idx] = fcb_make_key6(max_entries + hit_key_n + miss_key_n
                                        + mixed_key_n + miss_off);
                xu[idx] = fcb_make_keyu(max_entries + hit_key_n + miss_key_n
                                        + mixed_key_n + miss_off);
            }
        }
    }

    printf("  entries=%u nb_bk=%u query=%u\n",
           max_entries, nb_bk, FCB_QUERY);
    fcb_print_sampling_config();
    printf("  cold datapath: unique query keys per timed round\n");

    memset(out, 0, sizeof(*out));
    out->desired = desired;

#define FCB_SAMPLE_METRIC(dst, expr)                                                 \
    do {                                                                             \
        if (fcb_sample_raw_repeat <= 1u) {                                           \
            (dst) = (expr);                                                          \
        } else {                                                                     \
            for (unsigned _rep = 0; _rep < fcb_sample_raw_repeat; _rep++)            \
                metric_samples[_rep] = (expr);                                       \
            (dst) = fcb_trimmed_window_median_double(metric_samples,                  \
                                                     fcb_sample_raw_repeat,          \
                                                     fcb_sample_keep_n);            \
        }                                                                            \
    } while (0)

    {
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FINDADD_HIT][0],
                          fcb_flow4_bench_hit(&ctx4, p4, prefill_n - FCB_QUERY,
                                              h4, FCB_QUERY, fcb_dp_hit_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FINDADD_HIT][1],
                          fcb_flow6_bench_hit(&ctx6, p6, prefill_n - FCB_QUERY,
                                              h6, FCB_QUERY, fcb_dp_hit_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FINDADD_HIT][2],
                          fcb_flowu_bench_hit(&ctxu, pu, prefill_n - FCB_QUERY,
                                              hu, FCB_QUERY, fcb_dp_hit_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FIND_HIT][0],
                          fcb_flow4_bench_find_hit(&ctx4, p4, prefill_n - FCB_QUERY,
                                                   h4, FCB_QUERY, fcb_dp_hit_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FIND_HIT][1],
                          fcb_flow6_bench_find_hit(&ctx6, p6, prefill_n - FCB_QUERY,
                                                   h6, FCB_QUERY, fcb_dp_hit_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FIND_HIT][2],
                          fcb_flowu_bench_find_hit(&ctxu, pu, prefill_n - FCB_QUERY,
                                                   hu, FCB_QUERY, fcb_dp_hit_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FINDADD_MISS][0],
                          fcb_flow4_bench_miss_fill(&ctx4, p4, prefill_n,
                                                    m4, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FINDADD_MISS][1],
                          fcb_flow6_bench_miss_fill(&ctx6, p6, prefill_n,
                                                    m6, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_FINDADD_MISS][2],
                          fcb_flowu_bench_miss_fill(&ctxu, pu, prefill_n,
                                                    mu, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_ADD_ONLY][0],
                          fcb_flow4_bench_add_only(&ctx4, p4, prefill_n,
                                                   m4, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_ADD_ONLY][1],
                          fcb_flow6_bench_add_only(&ctx6, p6, prefill_n,
                                                   m6, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_ADD_ONLY][2],
                          fcb_flowu_bench_add_only(&ctxu, pu, prefill_n,
                                                   mu, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_ADD_DEL][0],
                          fcb_flow4_bench_add_del(&ctx4, p4, prefill_n,
                                                  m4, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_ADD_DEL][1],
                          fcb_flow6_bench_add_del(&ctx6, p6, prefill_n,
                                                  m6, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_ADD_DEL][2],
                          fcb_flowu_bench_add_del(&ctxu, pu, prefill_n,
                                                  mu, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_DEL_BULK][0],
                          fcb_flow4_bench_del_bulk(&ctx4, p4, prefill_n,
                                                   m4, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_DEL_BULK][1],
                          fcb_flow6_bench_del_bulk(&ctx6, p6, prefill_n,
                                                   m6, FCB_QUERY, fcb_dp_miss_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_DEL_BULK][2],
                          fcb_flowu_bench_del_bulk(&ctxu, pu, prefill_n,
                                                   mu, FCB_QUERY, fcb_dp_miss_repeat));
    }

    printf("  pure datapath (median cycles/key across timed rounds, reset/prefill/cold-touch per round):\n");
    for (unsigned m = 0; m < FCB_DP_DEL_BULK + 1u; m++) {
        const double *row = out->metrics[m];
        fcb_emit3(fcb_datapath_labels[m],
                  row[0], row[1], row[2]);
    }

    {
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_MIXED_90_10_RESET][0],
                          fcb_flow4_bench_mixed(&ctx4, p4, prefill_n, x4,
                                                FCB_QUERY, fcb_dp_mixed_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_MIXED_90_10_RESET][1],
                          fcb_flow6_bench_mixed(&ctx6, p6, prefill_n, x6,
                                                FCB_QUERY, fcb_dp_mixed_repeat));
        FCB_SAMPLE_METRIC(out->metrics[FCB_DP_MIXED_90_10_RESET][2],
                          fcb_flowu_bench_mixed(&ctxu, pu, prefill_n, xu,
                                                FCB_QUERY, fcb_dp_mixed_repeat));
    }

#undef FCB_SAMPLE_METRIC

    fcb_emit3(fcb_datapath_labels[FCB_DP_MIXED_90_10_RESET],
              out->metrics[FCB_DP_MIXED_90_10_RESET][0],
              out->metrics[FCB_DP_MIXED_90_10_RESET][1],
              out->metrics[FCB_DP_MIXED_90_10_RESET][2]);

    free(metric_samples);
    free(xu); free(x6); free(x4);
    free(mu); free(m6); free(m4);
    free(hu); free(h6); free(h4);
    free(pu); free(p6); free(p4);
    fcb_flowu_ctx_free(&ctxu);
    fcb_flow6_ctx_free(&ctx6);
    fcb_flow4_ctx_free(&ctx4);
}

static void
bench_datapath(unsigned desired_filter, struct fcb_datapath_report *reports)
{
    unsigned sizes[] = { 32768u, 1048576u, 4194304u };
    unsigned out_idx = 0u;

    printf("fcache variant comparison\n\n");
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        if (desired_filter != 0u && sizes[s] != desired_filter)
            continue;
        printf("[%uK entries]\n", sizes[s] / 1024u);
        bench_datapath_one(sizes[s], reports ? &reports[out_idx] : NULL);
        out_idx++;
        printf("\n");
    }
}

/*===========================================================================
 * maintain_step comparison (no args needed)
 *===========================================================================*/
static void
bench_maint(unsigned desired_filter)
{
    unsigned configs[][2] = {
        /* { desired_entries, nb_bk } */
        {    1024u,   128u },
        {    8192u,  1024u },
        {   65536u,  8192u },
        { 1048576u, 65536u },
        { 4194304u, 262144u },
    };

    printf("maintain_step benchmark (median cycles/call, full-table sweep, all expired)\n\n");
    for (unsigned c = 0; c < sizeof(configs) / sizeof(configs[0]); c++) {
        unsigned desired = configs[c][0];
        unsigned nb_bk   = configs[c][1];
        unsigned max_entries = fcb_pool_count(desired);
        double data_mb = ((double)nb_bk * sizeof(struct rix_hash_bucket_s)
                          + (double)max_entries * sizeof(struct fc_flow4_entry))
                         / (1024.0 * 1024.0);

        if (desired_filter != 0u && desired != desired_filter)
            continue;

        printf("  nb_bk=%u  pool=%u  (%.1f MB)\n", nb_bk, max_entries, data_mb);
        printf("  [flow4]\n");
        fcb_flow4_bench_maintain_step_report(desired, nb_bk);
        printf("  [flow6]\n");
        fcb_flow6_bench_maintain_step_report(desired, nb_bk);
        printf("  [flowu]\n");
        fcb_flowu_bench_maintain_step_report(desired, nb_bk);
        printf("\n");
    }
}

/*===========================================================================
 * maintain_step partial-sweep benchmark
 *===========================================================================*/
static void
bench_maint_partial(void)
{
    unsigned configs[][2] = {
        {    8192u,  1024u },
        {   65536u,  8192u },
        { 1048576u, 65536u },
        { 4194304u, 262144u },
    };
    unsigned steps[] = { 64u, 128u, 256u, 512u, 1024u };
    unsigned n_steps = sizeof(steps) / sizeof(steps[0]);

    printf("maintain_step partial-sweep (median cycles/call, fill=75%%, all expired)\n\n");
    for (unsigned c = 0; c < sizeof(configs) / sizeof(configs[0]); c++) {
        unsigned desired = configs[c][0];
        unsigned nb_bk   = configs[c][1];
        unsigned max_entries = fcb_pool_count(desired);
        double data_mb = ((double)nb_bk * sizeof(struct rix_hash_bucket_s)
                          + (double)max_entries * sizeof(struct fc_flow4_entry))
                         / (1024.0 * 1024.0);
        /* skip steps larger than nb_bk */
        unsigned valid_steps = 0u;
        for (unsigned s = 0; s < n_steps; s++) {
            if (steps[s] <= nb_bk)
                valid_steps = s + 1u;
        }
        printf("  nb_bk=%u  pool=%u  (%.1f MB)\n", nb_bk, max_entries, data_mb);
        printf("  [flow4]\n");
        fcb_flow4_bench_maint_step_partial(desired, nb_bk, 75u, steps, valid_steps);
        printf("  [flow6]\n");
        fcb_flow6_bench_maint_step_partial(desired, nb_bk, 75u, steps, valid_steps);
        printf("  [flowu]\n");
        fcb_flowu_bench_maint_step_partial(desired, nb_bk, 75u, steps, valid_steps);
        printf("\n");
    }
}

/*===========================================================================
 * Variant dispatch helpers
 *===========================================================================*/
typedef void (*findadd_closed_fn)(unsigned, unsigned, unsigned, unsigned, unsigned);
typedef void (*findadd_closed_stream_fn)(unsigned, unsigned, unsigned, unsigned,
                                         unsigned);
typedef void (*findadd_open_fn)(unsigned, unsigned, unsigned, unsigned, unsigned,
                                unsigned);
typedef void (*findadd_window_fn)(unsigned, unsigned, unsigned, unsigned,
                                  unsigned, unsigned, unsigned, unsigned);
typedef struct fcb_run_summary
(*findadd_closed_summary_fn)(unsigned, unsigned, unsigned, unsigned, unsigned);
typedef struct fcb_run_summary
(*findadd_closed_stream_summary_fn)(unsigned, unsigned, unsigned, unsigned,
                                    unsigned);
typedef struct fcb_run_summary
(*findadd_open_summary_fn)(unsigned, unsigned, unsigned, unsigned, unsigned,
                           unsigned);
typedef struct fcb_run_summary
(*findadd_window_summary_fn)(unsigned, unsigned, unsigned, unsigned,
                             unsigned, unsigned, unsigned, unsigned);
typedef void (*trace_open_fn)(unsigned, unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned);

static struct fcb_run_summary
fcb_sample_closed_summary(findadd_closed_summary_fn fn,
                          unsigned desired, unsigned nb_bk,
                          unsigned fill_pct, unsigned hit, unsigned rounds,
                          unsigned raw_repeat, unsigned keep_n)
{
    struct fcb_run_summary *samples;

    if (raw_repeat <= 1u)
        return fn(desired, nb_bk, fill_pct, hit, rounds);

    samples = fcb_alloc((size_t)raw_repeat * sizeof(*samples));
    for (unsigned rep = 0; rep < raw_repeat; rep++)
        samples[rep] = fn(desired, nb_bk, fill_pct, hit, rounds);
    {
        struct fcb_run_summary out =
            fcb_aggregate_run_summaries(samples, raw_repeat, keep_n);
        free(samples);
        return out;
    }
}

static struct fcb_run_summary
fcb_sample_closed_stream_summary(findadd_closed_stream_summary_fn fn,
                                 unsigned desired, unsigned nb_bk,
                                 unsigned fill_pct, unsigned hit,
                                 unsigned total_keys,
                                 unsigned raw_repeat, unsigned keep_n)
{
    struct fcb_run_summary *samples;

    if (raw_repeat <= 1u)
        return fn(desired, nb_bk, fill_pct, hit, total_keys);

    samples = fcb_alloc((size_t)raw_repeat * sizeof(*samples));
    for (unsigned rep = 0; rep < raw_repeat; rep++)
        samples[rep] = fn(desired, nb_bk, fill_pct, hit, total_keys);
    {
        struct fcb_run_summary out =
            fcb_aggregate_run_summaries(samples, raw_repeat, keep_n);
        free(samples);
        return out;
    }
}

static struct fcb_run_summary
fcb_sample_open_summary(findadd_open_summary_fn fn,
                        unsigned desired, unsigned nb_bk,
                        unsigned start_fill, unsigned hit,
                        unsigned keys_per_sec, unsigned timeout_ms,
                        unsigned raw_repeat, unsigned keep_n)
{
    struct fcb_run_summary *samples;

    if (raw_repeat <= 1u)
        return fn(desired, nb_bk, start_fill, hit, keys_per_sec, timeout_ms);

    samples = fcb_alloc((size_t)raw_repeat * sizeof(*samples));
    for (unsigned rep = 0; rep < raw_repeat; rep++)
        samples[rep] =
            fn(desired, nb_bk, start_fill, hit, keys_per_sec, timeout_ms);
    {
        struct fcb_run_summary out =
            fcb_aggregate_run_summaries(samples, raw_repeat, keep_n);
        free(samples);
        return out;
    }
}

static struct fcb_run_summary
fcb_sample_window_summary(findadd_window_summary_fn fn,
                          unsigned desired, unsigned nb_bk,
                          unsigned low_fill, unsigned high_fill,
                          unsigned hit, unsigned keys_per_sec,
                          unsigned ttl_ms, unsigned duration_ms,
                          unsigned raw_repeat, unsigned keep_n)
{
    struct fcb_run_summary *samples;

    if (raw_repeat <= 1u)
        return fn(desired, nb_bk, low_fill, high_fill, hit,
                  keys_per_sec, ttl_ms, duration_ms);

    samples = fcb_alloc((size_t)raw_repeat * sizeof(*samples));
    for (unsigned rep = 0; rep < raw_repeat; rep++)
        samples[rep] = fn(desired, nb_bk, low_fill, high_fill, hit,
                          keys_per_sec, ttl_ms, duration_ms);
    {
        struct fcb_run_summary out =
            fcb_aggregate_run_summaries(samples, raw_repeat, keep_n);
        free(samples);
        return out;
    }
}

static void
select_variant(const char *name,
               findadd_closed_fn *out_closed,
               findadd_closed_stream_fn *out_closed_stream,
               findadd_open_fn *out_open,
               findadd_window_fn *out_window,
               trace_open_fn *out_trace)
{
    if (strcmp(name, "flow4") == 0) {
        *out_closed = fcb_flow4_findadd_closed;
        *out_closed_stream = fcb_flow4_findadd_closed_stream;
        *out_open   = fcb_flow4_findadd_open;
        *out_window = fcb_flow4_findadd_window;
        *out_trace  = fcb_flow4_trace_open_custom;
    } else if (strcmp(name, "flow6") == 0) {
        *out_closed = fcb_flow6_findadd_closed;
        *out_closed_stream = fcb_flow6_findadd_closed_stream;
        *out_open   = fcb_flow6_findadd_open;
        *out_window = fcb_flow6_findadd_window;
        *out_trace  = fcb_flow6_trace_open_custom;
    } else if (strcmp(name, "flowu") == 0) {
        *out_closed = fcb_flowu_findadd_closed;
        *out_closed_stream = fcb_flowu_findadd_closed_stream;
        *out_open   = fcb_flowu_findadd_open;
        *out_window = fcb_flowu_findadd_window;
        *out_trace  = fcb_flowu_trace_open_custom;
    } else {
        fprintf(stderr, "unknown variant: %s (use flow4/flow6/flowu)\n", name);
        exit(2);
    }
}

static void
select_variant_summary(const char *name,
                       findadd_closed_summary_fn *out_closed,
                       findadd_closed_stream_summary_fn *out_closed_stream,
                       findadd_open_summary_fn *out_open,
                       findadd_window_summary_fn *out_window)
{
    if (strcmp(name, "flow4") == 0) {
        *out_closed = fcb_flow4_findadd_closed_summary;
        *out_closed_stream = fcb_flow4_findadd_closed_stream_summary;
        *out_open   = fcb_flow4_findadd_open_summary;
        *out_window = fcb_flow4_findadd_window_summary;
    } else if (strcmp(name, "flow6") == 0) {
        *out_closed = fcb_flow6_findadd_closed_summary;
        *out_closed_stream = fcb_flow6_findadd_closed_stream_summary;
        *out_open   = fcb_flow6_findadd_open_summary;
        *out_window = fcb_flow6_findadd_window_summary;
    } else if (strcmp(name, "flowu") == 0) {
        *out_closed = fcb_flowu_findadd_closed_summary;
        *out_closed_stream = fcb_flowu_findadd_closed_stream_summary;
        *out_open   = fcb_flowu_findadd_open_summary;
        *out_window = fcb_flowu_findadd_window_summary;
    } else {
        fprintf(stderr, "unknown variant: %s (use flow4/flow6/flowu)\n", name);
        exit(2);
    }
}

static int
run_findadd_closed_stream_sweep(const char *variant,
                                unsigned desired,
                                unsigned fill_pct,
                                unsigned hit_pct,
                                unsigned total_keys,
                                unsigned keep_n,
                                unsigned raw_repeat)
{
    findadd_closed_summary_fn fn_closed;
    findadd_closed_stream_summary_fn fn_closed_stream;
    findadd_open_summary_fn fn_open;
    findadd_window_summary_fn fn_window;
    unsigned nb_bk = fcb_nb_bk_hint(fcb_pool_count(desired));
    unsigned saved_query = fcb_query;
    unsigned saved_api = fcb_findadd_api_mode;
    double *bulk_samples;
    double *burst_samples;

    select_variant_summary(variant, &fn_closed, &fn_closed_stream,
                           &fn_open, &fn_window);
    (void)fn_closed;
    (void)fn_open;
    (void)fn_window;

    if (keep_n == 0u || raw_repeat == 0u || keep_n > raw_repeat) {
        fprintf(stderr, "findadd_closed_stream_sweep requires 0 < keep_n <= raw_repeat\n");
        return 2;
    }

    bulk_samples = fcb_alloc((size_t)raw_repeat * sizeof(*bulk_samples));
    burst_samples = fcb_alloc((size_t)raw_repeat * sizeof(*burst_samples));

    printf("# %s fixed-stream query sweep\n", variant);
    printf("# fill=%u hit_target=%u total_keys=%u keep_n=%u raw_repeat=%u nb_bk=%u pool=%u\n",
           fill_pct, hit_pct, total_keys, keep_n, raw_repeat,
           nb_bk, fcb_pool_count(desired));
    printf("query\tbulk\tburst32\tgain_pct\n");

    for (unsigned query = 1; query <= 256u; query++) {
        double bulk_med;
        double burst_med;

        fcb_query = query;
        for (unsigned rep = 0; rep < raw_repeat; rep++) {
            struct fcb_run_summary s;

            if ((rep & 1u) == 0u) {
                fcb_findadd_api_mode = FCB_FINDADD_API_BULK;
                s = fn_closed_stream(desired, nb_bk, fill_pct, hit_pct, total_keys);
                bulk_samples[rep] = s.cycles_per_key;
                fcb_findadd_api_mode = FCB_FINDADD_API_BURST32;
                s = fn_closed_stream(desired, nb_bk, fill_pct, hit_pct, total_keys);
                burst_samples[rep] = s.cycles_per_key;
            } else {
                fcb_findadd_api_mode = FCB_FINDADD_API_BURST32;
                s = fn_closed_stream(desired, nb_bk, fill_pct, hit_pct, total_keys);
                burst_samples[rep] = s.cycles_per_key;
                fcb_findadd_api_mode = FCB_FINDADD_API_BULK;
                s = fn_closed_stream(desired, nb_bk, fill_pct, hit_pct, total_keys);
                bulk_samples[rep] = s.cycles_per_key;
            }
        }

        bulk_med = fcb_trimmed_window_median_double(bulk_samples, raw_repeat,
                                                    keep_n);
        burst_med = fcb_trimmed_window_median_double(burst_samples, raw_repeat,
                                                     keep_n);
        printf("%u\t%.6f\t%.6f\t%.2f\n",
               query, bulk_med, burst_med,
               fcb_archcmp_gain_pct(burst_med, bulk_med));
    }

    free(burst_samples);
    free(bulk_samples);
    fcb_findadd_api_mode = saved_api;
    fcb_query = saved_query;
    return 0;
}

static void
fcb_print_closed_summary(const struct fcb_run_summary *s)
{
    printf("fc : cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  oldest=%" PRIu64
           "  fill=%.1f%%  rounds=%u\n",
           s->cycles_per_key, s->hit_pct, s->misses,
           s->relief_evictions, s->oldest_reclaim_evictions,
           s->fill_end_pct, s->rounds);
}

static void
fcb_print_open_summary(const struct fcb_run_summary *s)
{
    printf("fc : cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  oldest=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           s->cycles_per_key, s->hit_pct, s->misses,
           s->relief_evictions, s->oldest_reclaim_evictions,
           s->fill_start_pct, s->fill_end_pct, s->fill_max_pct);
}

static void
fcb_print_window_summary(const struct fcb_run_summary *s)
{
    printf("fc : cycles/key=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  oldest=%" PRIu64
           "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  avg_fill=%.1f%%  min_fill=%.1f%%  max_fill=%.1f%%  over_high=%u/%u\n",
           s->cycles_per_key, s->hit_pct, s->misses,
           s->relief_evictions, s->oldest_reclaim_evictions,
           s->maint_calls, s->maint_evictions,
           s->fill_avg_pct, s->fill_min_pct, s->fill_max_pct,
           s->over_high_rounds, s->rounds);
}

/*===========================================================================
 * main
 *===========================================================================*/
static void
usage(const char *prog)
{
    printf("usage:\n");
    printf("  %s [--arch gen|sse|avx2|avx512] [--bench full|short] [--query 1..256] [--raw-repeat N] [--keep-n N] [--pin-core CPU] datapath\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] maint\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] maint_partial\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] [flow4|flow6|flowu] findadd_closed <desired> <fill%%> <hit%%> [rounds]\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] [flow4|flow6|flowu] findadd_closed_stream <desired> <fill%%> <hit%%> [total_keys]\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--raw-repeat N] [--keep-n N] [--pin-core CPU] [flow4|flow6|flowu] findadd_closed_stream_sweep <desired> <fill%%> <hit%%> [total_keys] [keep_n] [raw_repeat]\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] [flow4|flow6|flowu] findadd_open <desired> <start_fill%%> <hit%%> <keys_per_sec> [timeout_ms]\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] [flow4|flow6|flowu] findadd_window <desired> <low_fill%%> <high_fill%%> <hit%%> <keys_per_sec> <ttl_ms> [duration_ms]\n", prog);
    printf("  %s [--arch ...] [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] [flow4|flow6|flowu] trace_open_custom <desired> <start_fill%%> <hit%%> <keys_per_sec>"
           " <timeout_ms> <soak_mul> <report_ms>"
           " <fill0> <fill1> <fill2> <fill3>"
           " <k0> <k1> <k2> <k3> [kick_scale]\n", prog);
    printf("  %s [--bench full|short] [--query ...] [--raw-repeat N] [--keep-n N] [--pin-core CPU] [--findadd-api bulk|burst32] archcmp [flow4|flow6|flowu] <mode> [args...]\n", prog);
    printf("\n");
    printf("  findadd_closed : fixed hit set + fixed miss set; miss set is deleted after each round.\n");
    printf("  findadd_closed_stream : fixed per-key hit/miss stream; query only changes grouping.\n");
    printf("  findadd_closed_stream_sweep : in-process 1..256 sweep; keeps the tightest N samples per query.\n");
    printf("  findadd_open   : persistent fresh-miss stream; fill can grow without a window cap.\n");
    printf("  findadd_window : persistent fresh-miss stream with maintenance trying to hold a fill window.\n");
    printf("  archcmp        : run the same command twice, once with auto and once with avx2.\n");
    printf("                   winner summary is printed for datapath and findadd_* modes.\n");
    printf("  keys_per_sec   : key rate. 1 request = %u keys.\n", FCB_QUERY);
    printf("  --query        : request width for bulk benchmark calls; 1 through 256.\n");
    printf("  --raw-repeat   : raw sample count for scalar summary aggregation.\n");
    printf("  --keep-n       : keep the tightest N raw samples and report their median.\n");
    printf("  --bench        : full keeps current sampling; short reduces sampling only for cold modes.\n");
    printf("                     full datapath defaults: hit/find=%u miss/add/del=%u mixed=%u.\n",
           FCB_FULL_DP_HIT_REPEAT, FCB_FULL_DP_MISS_REPEAT, FCB_FULL_DP_MIXED_REPEAT);
    printf("                     short datapath defaults: hit/find=%u miss/add/del=%u mixed=%u.\n",
           FCB_SHORT_DP_HIT_REPEAT, FCB_SHORT_DP_MISS_REPEAT, FCB_SHORT_DP_MIXED_REPEAT);
    printf("  --dp-hit-repeat   : override cold datapath hit/find rounds per metric.\n");
    printf("  --dp-miss-repeat  : override cold datapath miss/add/del rounds per metric.\n");
    printf("  --dp-mixed-repeat : override cold datapath mixed rounds per metric.\n");
    printf("  --pin-core     : pin the fc_bench process to one CPU before running.\n");
    printf("  --findadd-api  : bulk (default) or burst32; burst32 is currently flow4-only.\n");
}

static int
fcb_parse_query_args(int *argc, char ***argv)
{
    while (*argc >= 2) {
        if (strcmp((*argv)[1], "--query") == 0) {
            unsigned query;

            if (*argc < 3) {
                fprintf(stderr, "--query requires an integer in the range 1..256\n");
                return 2;
            }
            query = (unsigned)strtoul((*argv)[2], NULL, 10);
            if (query == 0u || query > 256u) {
                fprintf(stderr, "--query must be in the range 1..256\n");
                return 2;
            }
            fcb_query = query;
            memmove(&(*argv)[1], &(*argv)[3],
                    (size_t)(*argc - 3 + 1) * sizeof((*argv)[0]));
            *argc -= 2;
            continue;
        }

        if (strncmp((*argv)[1], "--query=", 8) == 0) {
            unsigned query =
                (unsigned)strtoul((*argv)[1] + 8, NULL, 10);

            if (query == 0u || query > 256u) {
                fprintf(stderr, "--query must be in the range 1..256\n");
                return 2;
            }
            fcb_query = query;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
            continue;
        }

        break;
    }

    return 0;
}

static int
fcb_parse_sampling_args(int *argc, char ***argv)
{
    while (*argc >= 2) {
        const char *arg = (*argv)[1];
        const char *value = NULL;
        enum { FCB_SAMPLE_RAW, FCB_SAMPLE_KEEP, FCB_SAMPLE_PIN } kind;
        unsigned long parsed;

        if (strcmp(arg, "--raw-repeat") == 0 || strcmp(arg, "--keep-n") == 0
            || strcmp(arg, "--pin-core") == 0) {
            if (*argc < 3) {
                fprintf(stderr, "%s requires an integer value\n", arg);
                return 2;
            }
            value = (*argv)[2];
            kind = (arg[2] == 'r') ? FCB_SAMPLE_RAW
                 : (arg[2] == 'k') ? FCB_SAMPLE_KEEP
                                   : FCB_SAMPLE_PIN;
            memmove(&(*argv)[1], &(*argv)[3],
                    (size_t)(*argc - 3 + 1) * sizeof((*argv)[0]));
            *argc -= 2;
        } else if (strncmp(arg, "--raw-repeat=", 13) == 0) {
            value = arg + 13;
            kind = FCB_SAMPLE_RAW;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else if (strncmp(arg, "--keep-n=", 9) == 0) {
            value = arg + 9;
            kind = FCB_SAMPLE_KEEP;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else if (strncmp(arg, "--pin-core=", 11) == 0) {
            value = arg + 11;
            kind = FCB_SAMPLE_PIN;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else {
            break;
        }

        parsed = strtoul(value, NULL, 10);
        if (parsed == 0ul && kind != FCB_SAMPLE_PIN) {
            fprintf(stderr, "%s must be greater than 0\n",
                    (kind == FCB_SAMPLE_RAW) ? "--raw-repeat" : "--keep-n");
            return 2;
        }

        switch (kind) {
        case FCB_SAMPLE_RAW:
            fcb_sample_raw_repeat = (unsigned)parsed;
            break;
        case FCB_SAMPLE_KEEP:
            fcb_sample_keep_n = (unsigned)parsed;
            break;
        case FCB_SAMPLE_PIN:
            fcb_pin_core = (int)parsed;
            break;
        }
    }

    return 0;
}

static int
fcb_parse_dp_repeat_args(int *argc, char ***argv)
{
    while (*argc >= 2) {
        const char *arg = (*argv)[1];
        const char *value = NULL;
        unsigned *dst = NULL;
        size_t shift = 0u;

        if (strcmp(arg, "--dp-hit-repeat") == 0 ||
            strcmp(arg, "--dp-miss-repeat") == 0 ||
            strcmp(arg, "--dp-mixed-repeat") == 0) {
            if (*argc < 3) {
                fprintf(stderr, "%s requires an integer value\n", arg);
                return 2;
            }
            value = (*argv)[2];
            memmove(&(*argv)[1], &(*argv)[3],
                    (size_t)(*argc - 3 + 1) * sizeof((*argv)[0]));
            *argc -= 2;
        } else if (strncmp(arg, "--dp-hit-repeat=", 16) == 0) {
            value = arg + 16;
            shift = 1u;
        } else if (strncmp(arg, "--dp-miss-repeat=", 17) == 0) {
            value = arg + 17;
            shift = 1u;
        } else if (strncmp(arg, "--dp-mixed-repeat=", 18) == 0) {
            value = arg + 18;
            shift = 1u;
        } else {
            break;
        }

        if (shift != 0u) {
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        }

        if (strstr(arg, "hit") != NULL)
            dst = &fcb_dp_hit_repeat;
        else if (strstr(arg, "miss") != NULL)
            dst = &fcb_dp_miss_repeat;
        else
            dst = &fcb_dp_mixed_repeat;

        {
            unsigned long parsed = strtoul(value, NULL, 10);
            if (parsed == 0ul) {
                fprintf(stderr, "%s must be greater than 0\n", arg);
                return 2;
            }
            *dst = (unsigned)parsed;
        }
    }

    return 0;
}

static int
fcb_parse_bench_profile_args(int *argc, char ***argv)
{
    while (*argc >= 2) {
        const char *arg = (*argv)[1];
        const char *value = NULL;

        if (strcmp(arg, "--bench") == 0) {
            if (*argc < 3) {
                fprintf(stderr, "--bench requires one of: full, short\n");
                return 2;
            }
            value = (*argv)[2];
            memmove(&(*argv)[1], &(*argv)[3],
                    (size_t)(*argc - 3 + 1) * sizeof((*argv)[0]));
            *argc -= 2;
        } else if (strncmp(arg, "--bench=", 8) == 0) {
            value = arg + 8;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else {
            break;
        }

        if (strcmp(value, "full") == 0) {
            fcb_bench_profile = FCB_BENCH_FULL;
        } else if (strcmp(value, "short") == 0) {
            fcb_bench_profile = FCB_BENCH_SHORT;
        } else {
            fprintf(stderr, "--bench must be one of: full, short\n");
            return 2;
        }
    }

    return 0;
}

static int
fcb_validate_sampling_args(void)
{
    if (fcb_sample_raw_repeat == 0u || fcb_sample_keep_n == 0u) {
        fprintf(stderr, "--raw-repeat and --keep-n must both be greater than 0\n");
        return 2;
    }
    if (fcb_sample_keep_n > fcb_sample_raw_repeat) {
        fprintf(stderr, "--keep-n must be less than or equal to --raw-repeat\n");
        return 2;
    }
    return 0;
}

static int
fcb_parse_findadd_api_args(int *argc, char ***argv)
{
    while (*argc >= 2) {
        const char *arg = (*argv)[1];
        const char *value = NULL;

        if (strcmp(arg, "--findadd-api") == 0) {
            if (*argc < 3) {
                fprintf(stderr, "--findadd-api requires one of: bulk, burst32\n");
                return 2;
            }
            value = (*argv)[2];
            memmove(&(*argv)[1], &(*argv)[3],
                    (size_t)(*argc - 3 + 1) * sizeof((*argv)[0]));
            *argc -= 2;
        } else if (strncmp(arg, "--findadd-api=", 14) == 0) {
            value = arg + 14;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else {
            break;
        }

        if (strcmp(value, "bulk") == 0) {
            fcb_findadd_api_mode = FCB_FINDADD_API_BULK;
        } else if (strcmp(value, "burst32") == 0) {
            fcb_findadd_api_mode = FCB_FINDADD_API_BURST32;
        } else {
            fprintf(stderr, "--findadd-api must be one of: bulk, burst32\n");
            return 2;
        }
    }

    return 0;
}

static int
run_mode(const char *variant, const char *mode, int argc, char **argv, int arg_off)
{
    findadd_closed_summary_fn fn_closed_summary;
    findadd_closed_stream_summary_fn fn_closed_stream_summary;
    findadd_open_summary_fn fn_open_summary;
    findadd_window_summary_fn fn_window_summary;

    if (strcmp(mode, "datapath") == 0) {
        unsigned desired = 0u;

        if (argc - arg_off >= 1)
            desired = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        bench_datapath(desired, NULL);
        return 0;
    }
    if (strcmp(mode, "maint") == 0) {
        unsigned desired = 0u;

        if (argc - arg_off >= 1)
            desired = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        bench_maint(desired);
        return 0;
    }
    if (strcmp(mode, "maint_partial") == 0) {
        bench_maint_partial();
        return 0;
    }

    findadd_closed_fn fn_closed;
    findadd_closed_stream_fn fn_closed_stream;
    findadd_open_fn fn_open;
    findadd_window_fn fn_window;
    trace_open_fn fn_trace;

    select_variant(variant, &fn_closed, &fn_closed_stream, &fn_open, &fn_window, &fn_trace);
    select_variant_summary(variant, &fn_closed_summary, &fn_closed_stream_summary,
                           &fn_open_summary, &fn_window_summary);
    (void)fn_closed;
    (void)fn_closed_stream;
    (void)fn_open;
    (void)fn_window;

    if (strcmp(mode, "findadd_closed") == 0) {
        if (argc - arg_off < 4) {
            fprintf(stderr, "findadd_closed requires: <desired> <fill%%> <hit%%> [rounds]\n");
            return 2;
        }
        unsigned desired  = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        unsigned fill_pct = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        unsigned hit      = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        unsigned rounds   = (argc - arg_off >= 4)
                            ? (unsigned)strtoul(argv[arg_off + 3], NULL, 10)
                            : 200000u;
        unsigned nb_bk    = fcb_nb_bk_hint(fcb_pool_count(desired));

        printf("\n%s closed-set findadd\n", variant);
        printf("fill=%u%%  hit_target=%u%%  rounds=%u  query=%u  nb_bk=%u  pool=%u\n",
               fill_pct, hit, rounds, FCB_QUERY, nb_bk, fcb_pool_count(desired));
        fcb_print_sampling_config();
        {
            struct fcb_run_summary s =
                fcb_sample_closed_summary(fn_closed_summary, desired, nb_bk,
                                          fill_pct, hit, rounds,
                                          fcb_sample_raw_repeat,
                                          fcb_sample_keep_n);
            fcb_print_closed_summary(&s);
        }
        return 0;
    }

    if (strcmp(mode, "findadd_closed_stream") == 0) {
        if (argc - arg_off < 4) {
            fprintf(stderr, "findadd_closed_stream requires: <desired> <fill%%> <hit%%> [total_keys]\n");
            return 2;
        }
        unsigned desired    = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        unsigned fill_pct   = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        unsigned hit        = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        unsigned total_keys = (argc - arg_off >= 4)
                              ? (unsigned)strtoul(argv[arg_off + 3], NULL, 10)
                              : 262144u;
        unsigned nb_bk      = fcb_nb_bk_hint(fcb_pool_count(desired));

        printf("\n%s closed-set fixed-stream findadd\n", variant);
        printf("fill=%u%%  hit_target=%u%%  total_keys=%u  query=%u  api=%s  nb_bk=%u  pool=%u\n",
               fill_pct, hit, total_keys, FCB_QUERY,
               fcb_findadd_api_name(), nb_bk, fcb_pool_count(desired));
        fcb_print_sampling_config();
        {
            struct fcb_run_summary s =
                fcb_sample_closed_stream_summary(fn_closed_stream_summary,
                                                 desired, nb_bk, fill_pct, hit,
                                                 total_keys,
                                                 fcb_sample_raw_repeat,
                                                 fcb_sample_keep_n);
            fcb_print_closed_summary(&s);
        }
        return 0;
    }

    if (strcmp(mode, "findadd_closed_stream_sweep") == 0) {
        if (argc - arg_off < 4) {
            fprintf(stderr, "findadd_closed_stream_sweep requires: <desired> <fill%%> <hit%%> [total_keys] [keep_n] [raw_repeat]\n");
            return 2;
        }
        unsigned desired    = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        unsigned fill_pct   = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        unsigned hit        = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        unsigned total_keys = (argc - arg_off >= 4)
                              ? (unsigned)strtoul(argv[arg_off + 3], NULL, 10)
                              : 262144u;
        unsigned keep_n     = (argc - arg_off >= 5)
                              ? (unsigned)strtoul(argv[arg_off + 4], NULL, 10)
                              : fcb_sample_keep_n;
        unsigned raw_repeat = (argc - arg_off >= 6)
                              ? (unsigned)strtoul(argv[arg_off + 5], NULL, 10)
                              : fcb_sample_raw_repeat;
        return run_findadd_closed_stream_sweep(variant, desired, fill_pct, hit,
                                               total_keys, keep_n, raw_repeat);
    }

    if (strcmp(mode, "findadd_open") == 0) {
        unsigned desired;
        unsigned sfill;
        unsigned hit;
        unsigned keys_per_sec;
        unsigned timeout_ms;
        unsigned nb_bk;

        if (argc - arg_off < 4) {
            fprintf(stderr, "findadd_open requires: <desired> <start_fill%%> <hit%%> <keys_per_sec> [timeout_ms]\n");
            return 2;
        }
        desired      = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        sfill        = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        hit          = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        keys_per_sec = (unsigned)strtoul(argv[arg_off + 3], NULL, 10);
        timeout_ms   = (argc - arg_off >= 5)
                       ? (unsigned)strtoul(argv[arg_off + 4], NULL, 10)
                       : 1000u;
        nb_bk        = fcb_nb_bk_hint(fcb_pool_count(desired));
        if (keys_per_sec == 0u) {
            fprintf(stderr, "findadd_open requires keys_per_sec > 0\n");
            return 2;
        }

        printf("\n%s open-set findadd\n", variant);
        printf("keys/s=%u  req/s=%.1f  hit_target=%u%%  start_fill=%u%%  timeout=%ums"
               "  query=%u  nb_bk=%u  pool=%u\n",
               keys_per_sec, (double)keys_per_sec / (double)FCB_QUERY,
               hit, sfill, timeout_ms, FCB_QUERY, nb_bk, fcb_pool_count(desired));
        fcb_print_sampling_config();
        {
            struct fcb_run_summary s =
                fcb_sample_open_summary(fn_open_summary, desired, nb_bk, sfill,
                                        hit, keys_per_sec, timeout_ms,
                                        fcb_sample_raw_repeat,
                                        fcb_sample_keep_n);
            fcb_print_open_summary(&s);
        }
        return 0;
    }

    if (strcmp(mode, "findadd_window") == 0) {
        unsigned desired;
        unsigned low_fill;
        unsigned high_fill;
        unsigned hit;
        unsigned keys_per_sec;
        unsigned ttl_ms;
        unsigned duration_ms;
        unsigned nb_bk;

        if (argc - arg_off < 6) {
            fprintf(stderr,
                    "findadd_window requires: <desired> <low_fill%%> <high_fill%%> <hit%%> <keys_per_sec> <ttl_ms> [duration_ms]\n");
            return 2;
        }
        desired      = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        low_fill     = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        high_fill    = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        hit          = (unsigned)strtoul(argv[arg_off + 3], NULL, 10);
        keys_per_sec = (unsigned)strtoul(argv[arg_off + 4], NULL, 10);
        ttl_ms       = (unsigned)strtoul(argv[arg_off + 5], NULL, 10);
        duration_ms  = (argc - arg_off >= 7)
                       ? (unsigned)strtoul(argv[arg_off + 6], NULL, 10)
                       : 1000u;
        nb_bk        = fcb_nb_bk_hint(fcb_pool_count(desired));

        if (keys_per_sec == 0u) {
            fprintf(stderr, "findadd_window requires keys_per_sec > 0\n");
            return 2;
        }
        printf("\n%s windowed open-set findadd\n", variant);
        printf("window=%u-%u%%  hit_target=%u%%  keys/s=%u  req/s=%.1f  ttl=%ums  duration=%ums"
               "  query=%u  nb_bk=%u  pool=%u\n",
               low_fill, high_fill, hit, keys_per_sec,
               (double)keys_per_sec / (double)FCB_QUERY,
               ttl_ms, duration_ms, FCB_QUERY, nb_bk, fcb_pool_count(desired));
        fcb_print_sampling_config();
        {
            struct fcb_run_summary s =
                fcb_sample_window_summary(fn_window_summary, desired, nb_bk,
                                          low_fill, high_fill, hit,
                                          keys_per_sec, ttl_ms, duration_ms,
                                          fcb_sample_raw_repeat,
                                          fcb_sample_keep_n);
            fcb_print_window_summary(&s);
        }
        return 0;
    }

    if (strcmp(mode, "trace_open_custom") == 0) {
        if (argc - arg_off < 15) {
            fprintf(stderr, "trace_open_custom requires: <desired> <start_fill%%>"
                    " <hit%%> <keys_per_sec> <timeout_ms> <soak_mul> <report_ms>"
                    " <fill0> <fill1> <fill2> <fill3>"
                    " <k0> <k1> <k2> <k3> [kick_scale]\n");
            return 2;
        }
        unsigned desired  = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        unsigned sfill    = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        unsigned hit      = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        unsigned keys_per_sec = (unsigned)strtoul(argv[arg_off + 3], NULL, 10);
        unsigned tmo      = (unsigned)strtoul(argv[arg_off + 4], NULL, 10);
        unsigned soak     = (unsigned)strtoul(argv[arg_off + 5], NULL, 10);
        unsigned rpt      = (unsigned)strtoul(argv[arg_off + 6], NULL, 10);
        unsigned f0       = (unsigned)strtoul(argv[arg_off + 7], NULL, 10);
        unsigned f1       = (unsigned)strtoul(argv[arg_off + 8], NULL, 10);
        unsigned f2       = (unsigned)strtoul(argv[arg_off + 9], NULL, 10);
        unsigned f3       = (unsigned)strtoul(argv[arg_off + 10], NULL, 10);
        unsigned k0       = (unsigned)strtoul(argv[arg_off + 11], NULL, 10);
        unsigned k1       = (unsigned)strtoul(argv[arg_off + 12], NULL, 10);
        unsigned k2       = (unsigned)strtoul(argv[arg_off + 13], NULL, 10);
        unsigned k3       = (unsigned)strtoul(argv[arg_off + 14], NULL, 10);
        unsigned kscale   = (argc - arg_off >= 16)
                            ? (unsigned)strtoul(argv[arg_off + 15], NULL, 10)
                            : 1u;
        unsigned nb_bk    = fcb_nb_bk_hint(fcb_pool_count(desired));
        if (keys_per_sec == 0u) {
            fprintf(stderr, "trace_open_custom requires keys_per_sec > 0\n");
            return 2;
        }
        fn_trace(desired, nb_bk, sfill, hit, keys_per_sec, tmo, kscale, soak, rpt,
                 f0, f1, f2, f3, k0, k1, k2, k3);
        return 0;
    }

    fprintf(stderr, "unknown mode: %s\n", mode);
    usage(argv[0]);
    return 2;
}

int
main(int argc, char **argv)
{
    const char *variant = "flow4";
    const char *mode;
    int arg_off;
    unsigned arch_enable;
    int parse_rc;

    arch_enable = FC_ARCH_AUTO;
    for (;;) {
        int prev_argc = argc;

        parse_rc = fcb_parse_bench_profile_args(&argc, &argv);
        if (parse_rc != 0)
            return parse_rc;
        {
            int arch_argc = argc;
            unsigned parsed_arch = fc_parse_arch_args(&argc, &argv);

            if (argc != arch_argc)
                arch_enable = parsed_arch;
        }
        parse_rc = fcb_parse_query_args(&argc, &argv);
        if (parse_rc != 0)
            return parse_rc;
        parse_rc = fcb_parse_sampling_args(&argc, &argv);
        if (parse_rc != 0)
            return parse_rc;
        parse_rc = fcb_parse_dp_repeat_args(&argc, &argv);
        if (parse_rc != 0)
            return parse_rc;
        parse_rc = fcb_parse_findadd_api_args(&argc, &argv);
        if (parse_rc != 0)
            return parse_rc;
        if (argc == prev_argc)
            break;
    }

    parse_rc = fcb_validate_sampling_args();
    if (parse_rc != 0)
        return parse_rc;
    parse_rc = fcb_apply_pin_core();
    if (parse_rc != 0)
        return parse_rc;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "archcmp") == 0) {
        static const struct {
            const char *name;
            unsigned enable;
        } arch_runs[] = {
            { "auto", FC_ARCH_AUTO },
            { "avx2", FC_ARCH_SSE | FC_ARCH_AVX2 },
        };
        struct fcb_datapath_report datapath_reports[2][3];
        struct fcb_run_summary scalar_reports[2];
        int rc = 0;
        int have_scalar_summary = 0;
        int have_datapath_summary = 0;

        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }

        if (strcmp(argv[2], "flow4") == 0 ||
            strcmp(argv[2], "flow6") == 0 ||
            strcmp(argv[2], "flowu") == 0) {
            variant = argv[2];
            if (argc < 4) {
                usage(argv[0]);
                return 1;
            }
            mode = argv[3];
            arg_off = 4;
        } else {
            mode = argv[2];
            arg_off = 3;
        }

        for (unsigned i = 0; i < sizeof(arch_runs) / sizeof(arch_runs[0]); i++) {
            struct fcb_sampling_guard sampling_guard;

            fc_arch_init(arch_runs[i].enable);
            fcb_apply_mode_sampling_profile(mode, &sampling_guard);
            printf("[archcmp: %s]\n\n", arch_runs[i].name);

            if (strcmp(mode, "datapath") == 0) {
                bench_datapath(0u, datapath_reports[i]);
                have_datapath_summary = 1;
                rc = 0;
            } else if (strcmp(mode, "findadd_closed") == 0) {
                findadd_closed_summary_fn fn_closed;
                findadd_closed_stream_summary_fn fn_closed_stream;
                findadd_open_summary_fn fn_open;
                findadd_window_summary_fn fn_window;
                unsigned desired;
                unsigned fill_pct;
                unsigned hit;
                unsigned rounds;
                unsigned nb_bk;

                select_variant_summary(variant, &fn_closed, &fn_closed_stream,
                                       &fn_open, &fn_window);
                (void)fn_closed_stream;
                (void)fn_open;
                (void)fn_window;
                if (argc - arg_off < 4) {
                    fprintf(stderr, "findadd_closed requires: <desired> <fill%%> <hit%%> [rounds]\n");
                    return 2;
                }
                desired  = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
                fill_pct = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
                hit      = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
                rounds   = (argc - arg_off >= 4)
                           ? (unsigned)strtoul(argv[arg_off + 3], NULL, 10)
                           : 200000u;
                nb_bk    = fcb_nb_bk_hint(fcb_pool_count(desired));
        printf("\n%s closed-set findadd\n", variant);
        printf("fill=%u%%  hit_target=%u%%  rounds=%u  query=%u  api=%s  nb_bk=%u  pool=%u\n",
               fill_pct, hit, rounds, FCB_QUERY, fcb_findadd_api_name(),
               nb_bk, fcb_pool_count(desired));
                fcb_print_sampling_config();
                scalar_reports[i] =
                    fcb_sample_closed_summary(fn_closed, desired, nb_bk,
                                              fill_pct, hit, rounds,
                                              fcb_sample_raw_repeat,
                                              fcb_sample_keep_n);
                fcb_print_closed_summary(&scalar_reports[i]);
                have_scalar_summary = 1;
                rc = 0;
            } else if (strcmp(mode, "findadd_closed_stream") == 0) {
                findadd_closed_summary_fn fn_closed;
                findadd_closed_stream_summary_fn fn_closed_stream;
                findadd_open_summary_fn fn_open;
                findadd_window_summary_fn fn_window;
                unsigned desired;
                unsigned fill_pct;
                unsigned hit;
                unsigned total_keys;
                unsigned nb_bk;

                select_variant_summary(variant, &fn_closed, &fn_closed_stream,
                                       &fn_open, &fn_window);
                (void)fn_closed;
                (void)fn_open;
                (void)fn_window;
                if (argc - arg_off < 4) {
                    fprintf(stderr, "findadd_closed_stream requires: <desired> <fill%%> <hit%%> [total_keys]\n");
                    return 2;
                }
                desired    = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
                fill_pct   = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
                hit        = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
                total_keys = (argc - arg_off >= 4)
                             ? (unsigned)strtoul(argv[arg_off + 3], NULL, 10)
                             : 262144u;
                nb_bk      = fcb_nb_bk_hint(fcb_pool_count(desired));
        printf("\n%s closed-set fixed-stream findadd\n", variant);
        printf("fill=%u%%  hit_target=%u%%  total_keys=%u  query=%u  api=%s  nb_bk=%u  pool=%u\n",
               fill_pct, hit, total_keys, FCB_QUERY, fcb_findadd_api_name(),
               nb_bk, fcb_pool_count(desired));
                fcb_print_sampling_config();
                scalar_reports[i] =
                    fcb_sample_closed_stream_summary(fn_closed_stream, desired,
                                                     nb_bk, fill_pct, hit,
                                                     total_keys,
                                                     fcb_sample_raw_repeat,
                                                     fcb_sample_keep_n);
                fcb_print_closed_summary(&scalar_reports[i]);
                have_scalar_summary = 1;
                rc = 0;
            } else if (strcmp(mode, "findadd_open") == 0) {
                findadd_closed_summary_fn fn_closed;
                findadd_closed_stream_summary_fn fn_closed_stream;
                findadd_open_summary_fn fn_open;
                findadd_window_summary_fn fn_window;
                unsigned desired;
                unsigned sfill;
                unsigned hit;
                unsigned keys_per_sec;
                unsigned timeout_ms;
                unsigned nb_bk;

                select_variant_summary(variant, &fn_closed, &fn_closed_stream,
                                       &fn_open, &fn_window);
                (void)fn_closed;
                (void)fn_closed_stream;
                (void)fn_window;
                if (argc - arg_off < 4) {
                    fprintf(stderr, "findadd_open requires: <desired> <start_fill%%> <hit%%> <keys_per_sec> [timeout_ms]\n");
                    return 2;
                }
                desired      = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
                sfill        = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
                hit          = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
                keys_per_sec = (unsigned)strtoul(argv[arg_off + 3], NULL, 10);
                timeout_ms   = (argc - arg_off >= 5)
                               ? (unsigned)strtoul(argv[arg_off + 4], NULL, 10)
                               : 1000u;
                nb_bk        = fcb_nb_bk_hint(fcb_pool_count(desired));
                if (keys_per_sec == 0u) {
                    fprintf(stderr, "findadd_open requires keys_per_sec > 0\n");
                    return 2;
                }
        printf("\n%s open-set findadd\n", variant);
        printf("keys/s=%u  req/s=%.1f  hit_target=%u%%  start_fill=%u%%  timeout=%ums"
               "  query=%u  api=%s  nb_bk=%u  pool=%u\n",
               keys_per_sec, (double)keys_per_sec / (double)FCB_QUERY,
               hit, sfill, timeout_ms, FCB_QUERY, fcb_findadd_api_name(),
               nb_bk, fcb_pool_count(desired));
                fcb_print_sampling_config();
                scalar_reports[i] =
                    fcb_sample_open_summary(fn_open, desired, nb_bk, sfill,
                                            hit, keys_per_sec, timeout_ms,
                                            fcb_sample_raw_repeat,
                                            fcb_sample_keep_n);
                fcb_print_open_summary(&scalar_reports[i]);
                have_scalar_summary = 1;
                rc = 0;
            } else if (strcmp(mode, "findadd_window") == 0) {
                findadd_closed_summary_fn fn_closed;
                findadd_closed_stream_summary_fn fn_closed_stream;
                findadd_open_summary_fn fn_open;
                findadd_window_summary_fn fn_window;
                unsigned desired;
                unsigned low_fill;
                unsigned high_fill;
                unsigned hit;
                unsigned keys_per_sec;
                unsigned ttl_ms;
                unsigned duration_ms;
                unsigned nb_bk;

                select_variant_summary(variant, &fn_closed, &fn_closed_stream,
                                       &fn_open, &fn_window);
                (void)fn_closed;
                (void)fn_closed_stream;
                (void)fn_open;
                if (argc - arg_off < 6) {
                    fprintf(stderr,
                            "findadd_window requires: <desired> <low_fill%%> <high_fill%%> <hit%%> <keys_per_sec> <ttl_ms> [duration_ms]\n");
                    return 2;
                }
                desired      = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
                low_fill     = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
                high_fill    = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
                hit          = (unsigned)strtoul(argv[arg_off + 3], NULL, 10);
                keys_per_sec = (unsigned)strtoul(argv[arg_off + 4], NULL, 10);
                ttl_ms       = (unsigned)strtoul(argv[arg_off + 5], NULL, 10);
                duration_ms  = (argc - arg_off >= 7)
                               ? (unsigned)strtoul(argv[arg_off + 6], NULL, 10)
                               : 1000u;
                nb_bk        = fcb_nb_bk_hint(fcb_pool_count(desired));
                if (keys_per_sec == 0u) {
                    fprintf(stderr, "findadd_window requires keys_per_sec > 0\n");
                    return 2;
                }
        printf("\n%s windowed open-set findadd\n", variant);
        printf("window=%u-%u%%  hit_target=%u%%  keys/s=%u  req/s=%.1f  ttl=%ums  duration=%ums"
               "  query=%u  api=%s  nb_bk=%u  pool=%u\n",
               low_fill, high_fill, hit, keys_per_sec,
               (double)keys_per_sec / (double)FCB_QUERY,
               ttl_ms, duration_ms, FCB_QUERY, fcb_findadd_api_name(),
               nb_bk, fcb_pool_count(desired));
                fcb_print_sampling_config();
                scalar_reports[i] =
                    fcb_sample_window_summary(fn_window, desired, nb_bk,
                                              low_fill, high_fill, hit,
                                              keys_per_sec, ttl_ms, duration_ms,
                                              fcb_sample_raw_repeat,
                                              fcb_sample_keep_n);
                fcb_print_window_summary(&scalar_reports[i]);
                have_scalar_summary = 1;
                rc = 0;
            } else {
                rc = run_mode(variant, mode, argc, argv, arg_off);
            }
            fcb_restore_mode_sampling_profile(&sampling_guard);
            if (rc != 0)
                return rc;
            if (i + 1u < sizeof(arch_runs) / sizeof(arch_runs[0]))
                printf("\n");
        }
        if (have_datapath_summary) {
            printf("\n");
            fcb_print_archcmp_datapath_summary(arch_runs[0].name,
                                              datapath_reports[0],
                                              arch_runs[1].name,
                                              datapath_reports[1],
                                              3u);
        } else if (have_scalar_summary) {
            printf("\n");
            fcb_print_archcmp_scalar_summary(mode,
                                             arch_runs[0].name,
                                             scalar_reports[0],
                                             arch_runs[1].name,
                                             scalar_reports[1]);
        }
        return rc;
    }

    fc_arch_init(arch_enable);
    printf("[arch: %s]\n\n", fc_arch_label(arch_enable));

    if (strcmp(argv[1], "flow4") == 0 ||
        strcmp(argv[1], "flow6") == 0 ||
        strcmp(argv[1], "flowu") == 0) {
        variant = argv[1];
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        mode = argv[2];
        arg_off = 3;
    } else {
        mode = argv[1];
        arg_off = 2;
    }

    {
        struct fcb_sampling_guard sampling_guard;
        int rc;

        fcb_apply_mode_sampling_profile(mode, &sampling_guard);
        rc = run_mode(variant, mode, argc, argv, arg_off);
        fcb_restore_mode_sampling_profile(&sampling_guard);
        return rc;
    }
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
