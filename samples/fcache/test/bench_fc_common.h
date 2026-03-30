/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_fc_common.h - Shared utilities for fcache benchmarks.
 */

#ifndef _BENCH_FC_COMMON_H_
#define _BENCH_FC_COMMON_H_

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flow_cache.h"

/*===========================================================================
 * Constants
 *===========================================================================*/
enum {
    FCB_ALIGN           = 64u,
    FCB_QUERY_DEFAULT   = 256u,
    FCB_HIT_REPEAT      = 200u,
    FCB_MISS_REPEAT     = 80u,
    FCB_MIXED_REPEAT    = 80u
};

extern unsigned fcb_query;
enum {
    FCB_FINDADD_API_BULK = 0u,
    FCB_FINDADD_API_BURST32 = 1u
};
extern unsigned fcb_findadd_api_mode;

#define FCB_QUERY (fcb_query)

struct fcb_run_summary {
    double cycles_per_key;
    double hit_pct;
    uint64_t misses;
    uint64_t relief_evictions;
    uint64_t oldest_reclaim_evictions;
    uint64_t maint_calls;
    uint64_t maint_evictions;
    double fill_start_pct;
    double fill_end_pct;
    double fill_avg_pct;
    double fill_min_pct;
    double fill_max_pct;
    unsigned over_high_rounds;
    unsigned rounds;
};

/*===========================================================================
 * TSC helpers (no v1 dependency)
 *===========================================================================*/
static inline uint64_t
fcb_rdtsc(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

static inline uint64_t
fcb_calibrate_tsc_hz(void)
{
    struct timespec t0, t1;
    uint64_t tsc0, tsc1, ns;
    struct timespec req = { .tv_nsec = 1000000 }; /* 1 ms */

    clock_gettime(CLOCK_MONOTONIC, &t0);
    tsc0 = fcb_rdtsc();
    nanosleep(&req, NULL);
    tsc1 = fcb_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u
                + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    return ns ? (tsc1 - tsc0) * 1000000000ULL / ns : 0;
}

/*===========================================================================
 * Sizing helpers
 *
 * Bench contexts intentionally provision one pool entry per hash slot:
 *   pool_entries == total_slots == nb_bk * RIX_HASH_BUCKET_ENTRY_SZ
 * This allows the harness to exercise high-fill regimes such as 75%.
 *===========================================================================*/
static inline unsigned
fcb_pool_count(unsigned desired)
{
    if (desired < 64u)
        desired = 64u;
    desired--;
    desired |= desired >> 1;
    desired |= desired >> 2;
    desired |= desired >> 4;
    desired |= desired >> 8;
    desired |= desired >> 16;
    return desired + 1u;
}

static inline unsigned
fcb_nb_bk_hint(unsigned max_entries)
{
    unsigned n = (max_entries + (RIX_HASH_BUCKET_ENTRY_SZ - 1u))
                 / RIX_HASH_BUCKET_ENTRY_SZ;
    if (n < 2u)
        n = 2u;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1u;
}

/*===========================================================================
 * Allocation
 *===========================================================================*/
static inline size_t
fcb_round_up(size_t n, size_t align)
{
    return (n + align - 1u) & ~(align - 1u);
}

static inline void *
fcb_alloc(size_t size)
{
    size_t alloc_size = fcb_round_up(size, FCB_ALIGN);
    void *p = aligned_alloc(FCB_ALIGN, alloc_size);

    if (p == NULL) {
        perror("aligned_alloc");
        exit(1);
    }
    memset(p, 0, alloc_size);
    return p;
}

/*===========================================================================
 * Output helpers
 *===========================================================================*/
static inline void
fcb_emit3(const char *label, double v0, double v1, double v2)
{
    printf("  %-20s  flow4=%7.2f  flow6=%7.2f  flowu=%7.2f"
           "  (6/4=%+.1f%%  u/4=%+.1f%%)\n",
           label, v0, v1, v2,
           (v1 / v0 - 1.0) * 100.0,
           (v2 / v0 - 1.0) * 100.0);
}

static inline int
fcb_cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static inline double
fcb_median_u64(uint64_t *samples, unsigned n)
{
    if (n == 0u)
        return 0.0;

    qsort(samples, n, sizeof(samples[0]), fcb_cmp_u64);
    if (n & 1u)
        return (double)samples[n / 2u];

    return ((double)samples[(n / 2u) - 1u] + (double)samples[n / 2u]) * 0.5;
}

/*===========================================================================
 * Batch-maint kick calculator
 *===========================================================================*/
static inline unsigned
fcb_batch_maint_kicks(unsigned fill_pct,
                       unsigned fill0, unsigned fill1,
                       unsigned fill2, unsigned fill3,
                       unsigned kicks0, unsigned kicks1,
                       unsigned kicks2, unsigned kicks3,
                       unsigned kick_scale)
{
    unsigned kicks;

    if (fill_pct >= fill3)
        kicks = kicks3;
    else if (fill_pct >= fill2)
        kicks = kicks2;
    else if (fill_pct >= fill1)
        kicks = kicks1;
    else if (fill_pct >= fill0)
        kicks = kicks0;
    else
        kicks = 0u;
    return kicks * kick_scale;
}

/*===========================================================================
 * Arch option parser (shared by fc_bench and fc_test)
 *
 *   --arch gen|sse|avx2|avx512|auto   (default: auto)
 *
 * Returns the consumed argument count (0 or 2).
 *===========================================================================*/
static inline unsigned
fc_parse_arch_flag(const char *name)
{
    if (strcmp(name, "gen") == 0)    return FC_ARCH_GEN;
    if (strcmp(name, "sse") == 0)    return FC_ARCH_SSE;
    if (strcmp(name, "avx2") == 0)   return FC_ARCH_SSE | FC_ARCH_AVX2;
    if (strcmp(name, "avx512") == 0) return FC_ARCH_AUTO;
    if (strcmp(name, "auto") == 0)   return FC_ARCH_AUTO;
    fprintf(stderr, "unknown arch: %s (valid: gen sse avx2 avx512 auto)\n",
            name);
    return FC_ARCH_AUTO;
}

static inline const char *
fc_arch_label(unsigned enable)
{
    if (enable & FC_ARCH_AVX512) return "avx512";
    if (enable & FC_ARCH_AVX2)   return "avx2";
    if (enable & FC_ARCH_SSE)    return "sse";
    return "gen";
}

/**
 * Parse --arch from argv.  Modifies *argc_p / *argv_p to skip consumed args.
 * Returns the arch enable mask.
 */
static inline unsigned
fc_parse_arch_args(int *argc_p, char ***argv_p)
{
    unsigned enable = FC_ARCH_AUTO;

    if (*argc_p >= 3 && strcmp((*argv_p)[1], "--arch") == 0) {
        enable = fc_parse_arch_flag((*argv_p)[2]);
        *argc_p -= 2;
        *argv_p += 2;
    }
    return enable;
}

#endif /* _BENCH_FC_COMMON_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
