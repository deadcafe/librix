/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <inttypes.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flow_table.h"

enum {
    FTB_ALIGN = 64u,
    FTB_QUERY = 256u,
    FTB_FILL_PCT = 40u,
    FTB_FIND_REPEAT = 200u,
    FTB_UPDATE_REPEAT = 80u,
    FTB_GROW_REPEAT = 9u,
    FTB_SAMPLE_MAX = 64u
};

static unsigned ftb_sample_raw_repeat = 11u;
static unsigned ftb_sample_keep_n = 7u;
static int ftb_pin_core = -1;

static void *
ftb_bucket_alloc(size_t size, size_t align, void *arg __attribute__((unused)))
{
    return aligned_alloc(align, size);
}

static void
ftb_bucket_free(void *ptr, size_t size __attribute__((unused)),
                size_t align __attribute__((unused)),
                void *arg __attribute__((unused)))
{
    free(ptr);
}

static void *
ftb_alloc_zero(size_t count, size_t size)
{
    size_t total = count * size;
    size_t rounded = (total + (FTB_ALIGN - 1u)) & ~(size_t)(FTB_ALIGN - 1u);
    void *ptr = aligned_alloc(FTB_ALIGN, rounded);

    if (ptr == NULL) {
        perror("aligned_alloc");
        exit(1);
    }
    memset(ptr, 0, rounded);
    return ptr;
}

static inline uint64_t
ftb_rdtsc(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

static int
ftb_cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static double
ftb_median_u64(uint64_t *samples, unsigned n)
{
    qsort(samples, n, sizeof(samples[0]), ftb_cmp_u64);
    if ((n & 1u) != 0u)
        return (double)samples[n / 2u];
    return ((double)samples[(n / 2u) - 1u] + (double)samples[n / 2u]) * 0.5;
}

static int
ftb_cmp_double(const void *a, const void *b)
{
    double av = *(const double *)a;
    double bv = *(const double *)b;
    return (av > bv) - (av < bv);
}

static double
ftb_median_double(double *samples, unsigned n)
{
    qsort(samples, n, sizeof(samples[0]), ftb_cmp_double);
    if ((n & 1u) != 0u)
        return samples[n / 2u];
    return (samples[(n / 2u) - 1u] + samples[n / 2u]) * 0.5;
}

static double
ftb_aggregate_double(double *samples, unsigned raw_n, unsigned keep_n)
{
    unsigned best_i = 0u;
    double best_span;

    qsort(samples, raw_n, sizeof(samples[0]), ftb_cmp_double);
    if (keep_n == 0u || keep_n >= raw_n)
        keep_n = raw_n;

    best_span = samples[keep_n - 1u] - samples[0];
    for (unsigned i = 1u; i + keep_n <= raw_n; i++) {
        double span = samples[i + keep_n - 1u] - samples[i];
        if (span < best_span) {
            best_span = span;
            best_i = i;
        }
    }
    return ftb_median_double(&samples[best_i], keep_n);
}

static int
ftb_apply_pin_core(void)
{
    cpu_set_t set;

    if (ftb_pin_core < 0)
        return 0;

    CPU_ZERO(&set);
    CPU_SET((unsigned)ftb_pin_core, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "sched_setaffinity(core=%d) failed: %s\n",
                ftb_pin_core, strerror(errno));
        return 2;
    }
    return 0;
}

static void
ftb_print_sampling_config(void)
{
    printf("sampling: raw_repeat=%u  keep_n=%u\n",
           ftb_sample_raw_repeat, ftb_sample_keep_n);
}

static int
ftb_parse_sampling_args(int *argc, char ***argv)
{
    while (*argc >= 2) {
        const char *arg = (*argv)[1];
        const char *value = NULL;
        enum { FTB_SAMPLE_RAW, FTB_SAMPLE_KEEP, FTB_SAMPLE_PIN } kind;
        unsigned long parsed;

        if (strcmp(arg, "--raw-repeat") == 0 || strcmp(arg, "--keep-n") == 0
            || strcmp(arg, "--pin-core") == 0) {
            if (*argc < 3) {
                fprintf(stderr, "%s requires an integer value\n", arg);
                return 2;
            }
            value = (*argv)[2];
            kind = (arg[2] == 'r') ? FTB_SAMPLE_RAW
                 : (arg[2] == 'k') ? FTB_SAMPLE_KEEP
                                   : FTB_SAMPLE_PIN;
            memmove(&(*argv)[1], &(*argv)[3],
                    (size_t)(*argc - 3 + 1) * sizeof((*argv)[0]));
            *argc -= 2;
        } else if (strncmp(arg, "--raw-repeat=", 13) == 0) {
            value = arg + 13;
            kind = FTB_SAMPLE_RAW;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else if (strncmp(arg, "--keep-n=", 9) == 0) {
            value = arg + 9;
            kind = FTB_SAMPLE_KEEP;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else if (strncmp(arg, "--pin-core=", 11) == 0) {
            value = arg + 11;
            kind = FTB_SAMPLE_PIN;
            memmove(&(*argv)[1], &(*argv)[2],
                    (size_t)(*argc - 2 + 1) * sizeof((*argv)[0]));
            *argc -= 1;
        } else {
            break;
        }

        parsed = strtoul(value, NULL, 10);
        if (parsed == 0ul && kind != FTB_SAMPLE_PIN) {
            fprintf(stderr, "%s must be greater than 0\n",
                    (kind == FTB_SAMPLE_RAW) ? "--raw-repeat" : "--keep-n");
            return 2;
        }
        if (kind != FTB_SAMPLE_PIN && parsed > FTB_SAMPLE_MAX) {
            fprintf(stderr, "%s must be less than or equal to %u\n",
                    (kind == FTB_SAMPLE_RAW) ? "--raw-repeat" : "--keep-n",
                    FTB_SAMPLE_MAX);
            return 2;
        }

        switch (kind) {
        case FTB_SAMPLE_RAW:
            ftb_sample_raw_repeat = (unsigned)parsed;
            break;
        case FTB_SAMPLE_KEEP:
            ftb_sample_keep_n = (unsigned)parsed;
            break;
        case FTB_SAMPLE_PIN:
            ftb_pin_core = (int)parsed;
            break;
        }
    }

    return 0;
}

static int
ftb_validate_sampling_args(void)
{
    if (ftb_sample_raw_repeat == 0u || ftb_sample_keep_n == 0u) {
        fprintf(stderr, "--raw-repeat and --keep-n must both be greater than 0\n");
        return 2;
    }
    if (ftb_sample_keep_n > ftb_sample_raw_repeat) {
        fprintf(stderr, "--keep-n must be less than or equal to --raw-repeat\n");
        return 2;
    }
    return 0;
}

static struct ft_flow4_config
ftb_cfg(void)
{
    struct ft_flow4_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.bucket_alloc.alloc = ftb_bucket_alloc;
    cfg.bucket_alloc.free = ftb_bucket_free;
    cfg.bucket_alloc.arg = NULL;
    cfg.grow_fill_pct = FT_FLOW4_DEFAULT_GROW_FILL_PCT;
    return cfg;
}

static struct ft_flow4_key
ftb_key(unsigned i)
{
    return ft_flow4_key_make(UINT32_C(0x0a000001) + i,
                             UINT32_C(0x0a100001) + i,
                             (uint16_t)(1000u + i),
                             (uint16_t)(2000u + i),
                             6u,
                             1u);
}

static unsigned
ftb_fill_target(unsigned entries, unsigned pct)
{
    return (unsigned)(((uint64_t)entries * pct) / 100u);
}

static void
ftb_bind_key(struct ft_flow4_table *ft, uint32_t entry_idx,
             const struct ft_flow4_key *key)
{
    struct ft_flow4_entry *entry = ft_flow4_table_entry_ptr(ft, entry_idx);

    if (entry == NULL) {
        fprintf(stderr, "bind_key failed at entry_idx=%u\n", entry_idx);
        exit(1);
    }
    entry->key = *key;
}

static void
ftb_prefill(struct ft_flow4_table *ft, unsigned n, unsigned key_base)
{
    for (unsigned i = 0; i < n; i++) {
        struct ft_flow4_key key = ftb_key(key_base + i);

        ftb_bind_key(ft, i + 1u, &key);
        if (ft_flow4_table_add_idx(ft, i + 1u) != i + 1u) {
            fprintf(stderr, "prefill failed at %u\n", i);
            exit(1);
        }
    }
}

static double
ftb_measure_find(struct ft_flow4_table *ft,
                 const struct ft_flow4_key *keys,
                 unsigned repeat)
{
    struct ft_flow4_result results[FTB_QUERY];
    uint64_t samples[FTB_FIND_REPEAT];

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = ftb_rdtsc();
        ft_flow4_table_find_bulk(ft, keys, FTB_QUERY, results);
        uint64_t t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    return ftb_median_u64(samples, repeat) / (double)FTB_QUERY;
}

static double
ftb_sample_find(struct ft_flow4_table *ft, const struct ft_flow4_key *keys)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++)
        samples[r] = ftb_measure_find(ft, keys, FTB_FIND_REPEAT);
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_add_only(struct ft_flow4_table *ft, unsigned key_base)
{
    struct ft_flow4_key keys[FTB_QUERY];
    uint32_t idxv[FTB_QUERY];
    struct ft_flow4_result results[FTB_QUERY];
    uint64_t samples[FTB_UPDATE_REPEAT];

    for (unsigned i = 0; i < FTB_QUERY; i++) {
        keys[i] = ftb_key(key_base + i);
        idxv[i] = i + 1u;
    }

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        uint64_t t0, t1;

        ft_flow4_table_flush(ft);
        for (unsigned i = 0; i < FTB_QUERY; i++)
            ftb_bind_key(ft, idxv[i], &keys[i]);
        t0 = ftb_rdtsc();
        ft_flow4_table_add_idx_bulk(ft, idxv, FTB_QUERY, results);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)FTB_QUERY;
}

static double
ftb_sample_add_only(struct ft_flow4_table *ft, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++)
        samples[r] = ftb_measure_add_only(ft, key_base + r * (FTB_QUERY + 1024u));
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_del_bulk(struct ft_flow4_table *ft, unsigned key_base)
{
    struct ft_flow4_key keys[FTB_QUERY];
    uint32_t idxv[FTB_QUERY];
    struct ft_flow4_result results[FTB_QUERY];
    uint64_t samples[FTB_UPDATE_REPEAT];

    for (unsigned i = 0; i < FTB_QUERY; i++) {
        keys[i] = ftb_key(key_base + i);
        idxv[i] = i + 1u;
    }

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        uint64_t t0, t1;

        ft_flow4_table_flush(ft);
        for (unsigned i = 0; i < FTB_QUERY; i++)
            ftb_bind_key(ft, idxv[i], &keys[i]);
        ft_flow4_table_add_idx_bulk(ft, idxv, FTB_QUERY, results);
        t0 = ftb_rdtsc();
        ft_flow4_table_del_bulk(ft, keys, FTB_QUERY, results);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)FTB_QUERY;
}

static double
ftb_sample_del_bulk(struct ft_flow4_table *ft, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++)
        samples[r] = ftb_measure_del_bulk(ft, key_base + r * (FTB_QUERY + 1024u));
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_grow(unsigned entries, unsigned fill_pct)
{
    struct ft_flow4_config cfg = ftb_cfg();
    struct ft_flow4_entry *pool;
    uint64_t samples[FTB_GROW_REPEAT];
    unsigned live = ftb_fill_target(entries, fill_pct);

    pool = ftb_alloc_zero(entries, sizeof(*pool));

    for (unsigned r = 0; r < FTB_GROW_REPEAT; r++) {
        struct ft_flow4_table ft;
        uint64_t t0, t1;

        if (ft_flow4_table_init(&ft, pool, entries, &cfg) != 0) {
            fprintf(stderr, "grow bench init failed\n");
            exit(1);
        }
        ftb_prefill(&ft, live, r * (live + 1024u));
        t0 = ftb_rdtsc();
        if (ft_flow4_table_grow_2x(&ft) != 0) {
            fprintf(stderr, "grow_2x failed in grow bench\n");
            exit(1);
        }
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
        ft_flow4_table_destroy(&ft);
    }

    free(pool);
    return ftb_median_u64(samples, FTB_GROW_REPEAT) / (double)live;
}

static double
ftb_sample_grow(unsigned entries, unsigned fill_pct)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++)
        samples[r] = ftb_measure_grow(entries, fill_pct);
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static void
ftb_run_datapath(void)
{
    const unsigned sizes[] = { 32768u, 1048576u };
    struct ft_flow4_key hit_keys[FTB_QUERY];
    struct ft_flow4_key miss_keys[FTB_QUERY];
    printf("\nftable datapath comparison\n\n");
    ftb_print_sampling_config();

    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        unsigned entries = sizes[s];
        unsigned live = ftb_fill_target(entries, FTB_FILL_PCT);
        struct ft_flow4_config cfg = ftb_cfg();
        struct ft_flow4_entry *pool = ftb_alloc_zero(entries, sizeof(*pool));
        struct ft_flow4_table ft;
        double find_hit, find_miss, add_only, del_bulk;

        if (ft_flow4_table_init(&ft, pool, entries, &cfg) != 0) {
            fprintf(stderr, "datapath init failed\n");
            exit(1);
        }
        if (ft_flow4_table_reserve(&ft, live) != 0) {
            fprintf(stderr, "datapath reserve failed\n");
            exit(1);
        }
        ftb_prefill(&ft, live, 0u);
        for (unsigned i = 0; i < FTB_QUERY; i++) {
            hit_keys[i] = ftb_key(i);
            miss_keys[i] = ftb_key(entries + 100000u + i);
        }

        find_hit = ftb_sample_find(&ft, hit_keys);
        find_miss = ftb_sample_find(&ft, miss_keys);
        add_only = ftb_sample_add_only(&ft, entries + 200000u);
        del_bulk = ftb_sample_del_bulk(&ft, entries + 300000u);

        printf("[entries=%u nb_bk=%u live=%u]\n",
               entries, ft_flow4_table_nb_bk(&ft), live);
        printf("  find_hit   %8.2f cy/key\n", find_hit);
        printf("  find_miss  %8.2f cy/key\n", find_miss);
        printf("  add_only   %8.2f cy/key\n", add_only);
        printf("  del_bulk   %8.2f cy/key\n", del_bulk);
        printf("\n");

        ft_flow4_table_destroy(&ft);
        free(pool);
    }
}

static int
ftb_parse_u32_arg(const char *s, unsigned *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (s == NULL || *s == '\0' || end == NULL || *end != '\0')
        return -1;
    if (v == 0ul || v > UINT_MAX)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static void
ftb_run_grow(int argc, char **argv)
{
    unsigned entries = 1048576u;
    unsigned fill_pct = FTB_FILL_PCT;
    double grow_cpe;

    if (argc >= 3 && ftb_parse_u32_arg(argv[2], &entries) != 0) {
        fprintf(stderr, "invalid grow entries: %s\n", argv[2]);
        exit(1);
    }
    if (argc >= 4 && ftb_parse_u32_arg(argv[3], &fill_pct) != 0) {
        fprintf(stderr, "invalid grow fill_pct: %s\n", argv[3]);
        exit(1);
    }
    if (fill_pct == 0u || fill_pct >= 100u) {
        fprintf(stderr, "grow fill_pct must be in 1..99\n");
        exit(1);
    }

    printf("ftable grow benchmark\n\n");
    ftb_print_sampling_config();
    grow_cpe = ftb_sample_grow(entries, fill_pct);
    printf("[entries=%u fill=%u%%]\n", entries, fill_pct);
    printf("  grow_2x    %8.2f cy/live-entry\n", grow_cpe);
}

static unsigned
ftb_parse_arch_flag(const char *name)
{
    if (strcmp(name, "gen") == 0)    return FT_ARCH_GEN;
    if (strcmp(name, "sse") == 0)    return FT_ARCH_SSE;
    if (strcmp(name, "avx2") == 0)   return FT_ARCH_AVX2;
    if (strcmp(name, "avx512") == 0) return FT_ARCH_AVX512;
    if (strcmp(name, "auto") == 0)   return FT_ARCH_AUTO;
    fprintf(stderr, "unknown arch: %s (valid: gen sse avx2 avx512 auto)\n",
            name);
    exit(1);
}

static const char *
ftb_arch_label(unsigned enable)
{
    if (enable == FT_ARCH_AUTO)      return "auto";
    if (enable & FT_ARCH_AVX512) return "avx512";
    if (enable & FT_ARCH_AVX2)   return "avx2";
    if (enable & FT_ARCH_SSE)    return "sse";
    return "gen";
}

int
main(int argc, char **argv)
{
    unsigned arch_enable = FT_ARCH_AUTO;
    const char *mode;
    int rc;

    rc = ftb_parse_sampling_args(&argc, &argv);
    if (rc != 0)
        return rc;
    rc = ftb_validate_sampling_args();
    if (rc != 0)
        return rc;

    if (argc >= 3 && strcmp(argv[1], "--arch") == 0) {
        arch_enable = ftb_parse_arch_flag(argv[2]);
        argc -= 2;
        argv += 2;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s [--arch gen|sse|avx2|avx512|auto] datapath|grow [entries] [fill_pct]\n",
                argv[0]);
        return 1;
    }

    rc = ftb_apply_pin_core();
    if (rc != 0)
        return rc;
    ft_arch_init(arch_enable);
    printf("[arch: %s]\n\n", ftb_arch_label(arch_enable));

    mode = argv[1];
    if (strcmp(mode, "datapath") == 0) {
        ftb_run_datapath();
        return 0;
    }
    if (strcmp(mode, "grow") == 0) {
        ftb_run_grow(argc, argv);
        return 0;
    }

    fprintf(stderr, "unknown mode: %s\n", mode);
    return 1;
}
