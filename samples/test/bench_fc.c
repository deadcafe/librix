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

#include "bench_fc_common.h"

/*===========================================================================
 * Key generators
 *===========================================================================*/
static inline struct fc_flow4_key
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

static inline struct fc_flow6_key
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

static inline struct fc_flowu_key
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

/*===========================================================================
 * Instantiate per-variant bench functions via include-template
 *===========================================================================*/

/* --- flow4 --- */
#define FCB_PREFIX    flow4
#define FCB_KEY_T     struct fc_flow4_key
#define FCB_RESULT_T  struct fc_flow4_result
#define FCB_ENTRY_T   struct fc_flow4_entry
#define FCB_CACHE_T   struct fc_flow4_cache
#define FCB_CONFIG_T  struct fc_flow4_config
#define FCB_STATS_T   struct fc_flow4_stats
#define FCB_PRESSURE  FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FCB_MAKE_KEY(i) fcb_make_key4(i)
#include "bench_fc_body.h"

/* --- flow6 --- */
#define FCB_PREFIX    flow6
#define FCB_KEY_T     struct fc_flow6_key
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
#define FCB_KEY_T     struct fc_flowu_key
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
bench_datapath_one(unsigned desired)
{
    unsigned max_entries = fcb_pool_count(desired);
    unsigned nb_bk = fcb_nb_bk_hint(max_entries);
    unsigned prefill_n = max_entries / 2u;

    struct fcb_flow4_ctx ctx4;
    struct fcb_flow6_ctx ctx6;
    struct fcb_flowu_ctx ctxu;

    fcb_flow4_ctx_init(&ctx4, nb_bk, max_entries, 1000000000ull);
    fcb_flow6_ctx_init(&ctx6, nb_bk, max_entries, 1000000000ull);
    fcb_flowu_ctx_init(&ctxu, nb_bk, max_entries, 1000000000ull);

    /* generate keys */
    struct fc_flow4_key *p4 = fcb_alloc((size_t)prefill_n * sizeof(*p4));
    struct fc_flow6_key *p6 = fcb_alloc((size_t)prefill_n * sizeof(*p6));
    struct fc_flowu_key *pu = fcb_alloc((size_t)prefill_n * sizeof(*pu));
    struct fc_flow4_key *h4 = fcb_alloc((size_t)FCB_QUERY * sizeof(*h4));
    struct fc_flow6_key *h6 = fcb_alloc((size_t)FCB_QUERY * sizeof(*h6));
    struct fc_flowu_key *hu = fcb_alloc((size_t)FCB_QUERY * sizeof(*hu));
    struct fc_flow4_key *m4 = fcb_alloc((size_t)FCB_QUERY * sizeof(*m4));
    struct fc_flow6_key *m6 = fcb_alloc((size_t)FCB_QUERY * sizeof(*m6));
    struct fc_flowu_key *mu = fcb_alloc((size_t)FCB_QUERY * sizeof(*mu));
    struct fc_flow4_key *x4 = fcb_alloc((size_t)FCB_QUERY * sizeof(*x4));
    struct fc_flow6_key *x6 = fcb_alloc((size_t)FCB_QUERY * sizeof(*x6));
    struct fc_flowu_key *xu = fcb_alloc((size_t)FCB_QUERY * sizeof(*xu));

    for (unsigned i = 0; i < prefill_n; i++) {
        p4[i] = fcb_make_key4(i);
        p6[i] = fcb_make_key6(i);
        pu[i] = fcb_make_keyu(i);
    }
    for (unsigned i = 0; i < FCB_QUERY; i++) {
        h4[i] = fcb_make_key4(i);
        h6[i] = fcb_make_key6(i);
        hu[i] = fcb_make_keyu(i);
        m4[i] = fcb_make_key4(max_entries + i);
        m6[i] = fcb_make_key6(max_entries + i);
        mu[i] = fcb_make_keyu(max_entries + i);
        /* mixed: 90% hit / 10% miss */
        unsigned idx = (i < (FCB_QUERY * 9u / 10u))
                       ? (i % prefill_n)
                       : (prefill_n + i);
        x4[i] = fcb_make_key4(idx);
        x6[i] = fcb_make_key6(idx);
        xu[i] = fcb_make_keyu(idx);
    }

    (void)fcb_flow4_prefill(&ctx4, p4, prefill_n, 1u);
    (void)fcb_flow6_prefill(&ctx6, p6, prefill_n, 1u);
    (void)fcb_flowu_prefill(&ctxu, pu, prefill_n, 1u);

    printf("  entries=%u nb_bk=%u query=%u\n",
           max_entries, nb_bk, FCB_QUERY);
    printf("  entries active: flow4=%u  flow6=%u  flowu=%u\n",
           fc_flow4_cache_nb_entries(&ctx4.fc),
           fc_flow6_cache_nb_entries(&ctx6.fc),
           fc_flowu_cache_nb_entries(&ctxu.fc));

    printf("  pure datapath (median cycles/key across timed rounds, reset per round, no expire):\n");
    fcb_emit3("findadd_hit",
               fcb_flow4_bench_hit(&ctx4, h4, FCB_QUERY, FCB_HIT_REPEAT),
               fcb_flow6_bench_hit(&ctx6, h6, FCB_QUERY, FCB_HIT_REPEAT),
               fcb_flowu_bench_hit(&ctxu, hu, FCB_QUERY, FCB_HIT_REPEAT));
    fcb_emit3("find_hit",
               fcb_flow4_bench_find_hit(&ctx4, h4, FCB_QUERY, FCB_HIT_REPEAT),
               fcb_flow6_bench_find_hit(&ctx6, h6, FCB_QUERY, FCB_HIT_REPEAT),
               fcb_flowu_bench_find_hit(&ctxu, hu, FCB_QUERY, FCB_HIT_REPEAT));
    fcb_emit3("findadd_miss",
               fcb_flow4_bench_miss_fill(&ctx4, m4, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flow6_bench_miss_fill(&ctx6, m6, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flowu_bench_miss_fill(&ctxu, mu, FCB_QUERY, FCB_MISS_REPEAT));
    fcb_emit3("add_only",
               fcb_flow4_bench_add_only(&ctx4, m4, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flow6_bench_add_only(&ctx6, m6, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flowu_bench_add_only(&ctxu, mu, FCB_QUERY, FCB_MISS_REPEAT));
    fcb_emit3("add+del",
               fcb_flow4_bench_add_del(&ctx4, m4, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flow6_bench_add_del(&ctx6, m6, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flowu_bench_add_del(&ctxu, mu, FCB_QUERY, FCB_MISS_REPEAT));
    fcb_emit3("del_bulk",
               fcb_flow4_bench_del_bulk(&ctx4, m4, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flow6_bench_del_bulk(&ctx6, m6, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flowu_bench_del_bulk(&ctxu, mu, FCB_QUERY, FCB_MISS_REPEAT));

    /* re-prefill for mixed */
    fcb_flow4_ctx_reset(&ctx4);
    fcb_flow6_ctx_reset(&ctx6);
    fcb_flowu_ctx_reset(&ctxu);
    (void)fcb_flow4_prefill(&ctx4, p4, prefill_n, 1u);
    (void)fcb_flow6_prefill(&ctx6, p6, prefill_n, 1u);
    (void)fcb_flowu_prefill(&ctxu, pu, prefill_n, 1u);

    fcb_emit3("mixed_90_10_reset",
               fcb_flow4_bench_mixed(&ctx4, p4, prefill_n, x4, FCB_QUERY, FCB_MIXED_REPEAT),
               fcb_flow6_bench_mixed(&ctx6, p6, prefill_n, x6, FCB_QUERY, FCB_MIXED_REPEAT),
               fcb_flowu_bench_mixed(&ctxu, pu, prefill_n, xu, FCB_QUERY, FCB_MIXED_REPEAT));

    free(xu); free(x6); free(x4);
    free(mu); free(m6); free(m4);
    free(hu); free(h6); free(h4);
    free(pu); free(p6); free(p4);
    fcb_flowu_ctx_free(&ctxu);
    fcb_flow6_ctx_free(&ctx6);
    fcb_flow4_ctx_free(&ctx4);
}

static void
bench_datapath(void)
{
    unsigned sizes[] = { 32768u, 1048576u, 4194304u };

    printf("fcache variant comparison\n\n");
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        printf("[%uK entries]\n", sizes[s] / 1024u);
        bench_datapath_one(sizes[s]);
        printf("\n");
    }
}

/*===========================================================================
 * maintain_step comparison (no args needed)
 *===========================================================================*/
static void
bench_maint(void)
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
typedef void (*findadd_open_fn)(unsigned, unsigned, unsigned, unsigned, unsigned,
                                unsigned);
typedef void (*findadd_window_fn)(unsigned, unsigned, unsigned, unsigned,
                                  unsigned, unsigned, unsigned, unsigned);
typedef void (*trace_open_fn)(unsigned, unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned);

static void
select_variant(const char *name,
               findadd_closed_fn *out_closed,
               findadd_open_fn *out_open,
               findadd_window_fn *out_window,
               trace_open_fn *out_trace)
{
    if (strcmp(name, "flow4") == 0) {
        *out_closed = fcb_flow4_findadd_closed;
        *out_open   = fcb_flow4_findadd_open;
        *out_window = fcb_flow4_findadd_window;
        *out_trace  = fcb_flow4_trace_open_custom;
    } else if (strcmp(name, "flow6") == 0) {
        *out_closed = fcb_flow6_findadd_closed;
        *out_open   = fcb_flow6_findadd_open;
        *out_window = fcb_flow6_findadd_window;
        *out_trace  = fcb_flow6_trace_open_custom;
    } else if (strcmp(name, "flowu") == 0) {
        *out_closed = fcb_flowu_findadd_closed;
        *out_open   = fcb_flowu_findadd_open;
        *out_window = fcb_flowu_findadd_window;
        *out_trace  = fcb_flowu_trace_open_custom;
    } else {
        fprintf(stderr, "unknown variant: %s (use flow4/flow6/flowu)\n", name);
        exit(2);
    }
}

/*===========================================================================
 * main
 *===========================================================================*/
static void
usage(const char *prog)
{
    printf("usage:\n");
    printf("  %s [--arch gen|sse|avx2|avx512] datapath\n", prog);
    printf("  %s [--arch ...] maint\n", prog);
    printf("  %s [--arch ...] maint_partial\n", prog);
    printf("  %s [--arch ...] [flow4|flow6|flowu] findadd_closed <desired> <fill%%> <hit%%> [rounds]\n", prog);
    printf("  %s [--arch ...] [flow4|flow6|flowu] findadd_open <desired> <start_fill%%> <hit%%> <keys_per_sec> [timeout_ms]\n", prog);
    printf("  %s [--arch ...] [flow4|flow6|flowu] findadd_window <desired> <low_fill%%> <high_fill%%> <hit%%> <keys_per_sec> <ttl_ms> [duration_ms]\n", prog);
    printf("  %s [--arch ...] [flow4|flow6|flowu] trace_open_custom <desired> <start_fill%%> <hit%%> <keys_per_sec>"
           " <timeout_ms> <soak_mul> <report_ms>"
           " <fill0> <fill1> <fill2> <fill3>"
           " <k0> <k1> <k2> <k3> [kick_scale]\n", prog);
    printf("  %s archcmp [flow4|flow6|flowu] <mode> [args...]\n", prog);
    printf("\n");
    printf("  findadd_closed : fixed hit set + fixed miss set; miss set is deleted after each round.\n");
    printf("  findadd_open   : persistent fresh-miss stream; fill can grow without a window cap.\n");
    printf("  findadd_window : persistent fresh-miss stream with maintenance trying to hold a fill window.\n");
    printf("  archcmp        : run the same command twice, once with auto and once with avx2.\n");
    printf("  keys_per_sec   : key rate. 1 request = %u keys.\n", FCB_QUERY);
}

static int
run_mode(const char *variant, const char *mode, int argc, char **argv, int arg_off)
{
    if (strcmp(mode, "datapath") == 0) {
        bench_datapath();
        return 0;
    }
    if (strcmp(mode, "maint") == 0) {
        bench_maint();
        return 0;
    }
    if (strcmp(mode, "maint_partial") == 0) {
        bench_maint_partial();
        return 0;
    }

    findadd_closed_fn fn_closed;
    findadd_open_fn fn_open;
    findadd_window_fn fn_window;
    trace_open_fn fn_trace;

    select_variant(variant, &fn_closed, &fn_open, &fn_window, &fn_trace);

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
        fn_closed(desired, nb_bk, fill_pct, hit, rounds);
        return 0;
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
        fn_open(desired, nb_bk, sfill, hit, keys_per_sec, timeout_ms);
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
        fn_window(desired, nb_bk, low_fill, high_fill, hit,
                  keys_per_sec, ttl_ms, duration_ms);
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

    arch_enable = fc_parse_arch_args(&argc, &argv);

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
        int rc = 0;

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
            fc_arch_init(arch_runs[i].enable);
            printf("[archcmp: %s]\n\n", arch_runs[i].name);
            rc = run_mode(variant, mode, argc, argv, arg_off);
            if (rc != 0)
                return rc;
            if (i + 1u < sizeof(arch_runs) / sizeof(arch_runs[0]))
                printf("\n");
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

    return run_mode(variant, mode, argc, argv, arg_off);
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
