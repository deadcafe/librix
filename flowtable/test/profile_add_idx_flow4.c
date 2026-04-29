/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_scope.h"

#include "flowtable/flow4_table.h"

enum {
    PAD_ALIGN = 64u,
    DEFAULT_ENTRIES = 1048576u,
    DEFAULT_FILL_PCT = 60u,
    DEFAULT_QUERY = 1024u,
    DEFAULT_REPEAT = 5u,
    COLD_BYTES = 64u * 1024u * 1024u
};

struct profile_record4 {
    struct flow4_entry entry;
    u32 cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

static int opt_pin_core = -1;
static unsigned char *cold_buf;
static volatile u64 cold_sink;

static void *
xaligned_zero(size_t count, size_t size)
{
    size_t total = count * size;
    size_t rounded = (total + (PAD_ALIGN - 1u)) & ~(size_t)(PAD_ALIGN - 1u);
    void *ptr = aligned_alloc(PAD_ALIGN, rounded);

    if (ptr == NULL) {
        perror("aligned_alloc");
        exit(1);
    }
    memset(ptr, 0, rounded);
    return ptr;
}

static void *
bucket_alloc(size_t size, size_t align, void *arg __attribute__((unused)))
{
    return aligned_alloc(align, size);
}

static void
bucket_free(void *ptr, size_t size __attribute__((unused)),
            size_t align __attribute__((unused)),
            void *arg __attribute__((unused)))
{
    free(ptr);
}

static struct ft_table_config
profile_cfg(void)
{
    struct ft_table_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    (void)bucket_alloc;
    (void)bucket_free;
    return cfg;
}

static struct flow4_key
profile_key4(unsigned i)
{
    struct flow4_key k;

    memset(&k, 0, sizeof(k));
    k.family   = 2u;
    k.proto    = (u8)(6u + (i & 1u));
    k.src_port = (u16)(1024u + (i & 0x7fffu));
    k.dst_port = (u16)(2048u + ((i >> 11) & 0x7fffu));
    k.vrfid    = 1u + (i >> 24);
    k.src_ip   = UINT32_C(0x0a000000) | (i & 0x00ffffffu);
    k.dst_ip   = UINT32_C(0x14000000)
               | ((i * UINT32_C(2654435761)) & 0x00ffffffu);
    return k;
}

static void
cold_touch(void)
{
    u64 sum = 0u;

    if (cold_buf == NULL)
        cold_buf = xaligned_zero(1u, COLD_BYTES);
    for (size_t off = 0; off < COLD_BYTES; off += 64u)
        sum += cold_buf[off];
    cold_sink += sum;
}

static int
apply_pin_core(void)
{
    cpu_set_t set;

    if (opt_pin_core < 0)
        return 0;
    CPU_ZERO(&set);
    CPU_SET((unsigned)opt_pin_core, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "sched_setaffinity(core=%d) failed: %s\n",
                opt_pin_core, strerror(errno));
        return -1;
    }
    return 0;
}

static int
parse_u32(const char *s, unsigned *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return -1;
    v = strtoul(s, &end, 10);
    if (end == NULL || *end != '\0' || v == 0ul || v > UINT_MAX)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static int
cmp_u64(const void *a, const void *b)
{
    u64 av = *(const u64 *)a;
    u64 bv = *(const u64 *)b;
    return (av > bv) - (av < bv);
}

static u64
median_u64(u64 *samples, unsigned n)
{
    qsort(samples, n, sizeof(samples[0]), cmp_u64);
    if ((n & 1u) != 0u)
        return samples[n / 2u];
    return (samples[(n / 2u) - 1u] + samples[n / 2u]) / 2u;
}

static void
prefill(struct ft_table *ft, unsigned live, unsigned key_base)
{
    for (unsigned i = 0; i < live; i++) {
        struct profile_record4 *rec =
            FT_FLOW4_TABLE_RECORD_PTR_AS(ft, struct profile_record4, i + 1u);
        u32 idx = i + 1u;

        RIX_ASSUME_NONNULL(rec);
        rec->entry.key = profile_key4(key_base + i);
        rec->cookie = idx;
        if (ft_flow4_table_add_idx(ft, idx) != idx) {
            fprintf(stderr, "prefill add failed at idx=%u\n", idx);
            exit(1);
        }
    }
}

static void
print_sample(const char *title,
             const struct bench_scope_group *group,
             u64 per_event[][DEFAULT_REPEAT],
             unsigned repeat,
             unsigned query_n)
{
    printf("%s\n", title);
    for (unsigned i = 0; i < group->count; i++) {
        u64 med = median_u64(per_event[i], repeat);
        double per_key = (double)med / (double)query_n;
        printf("  %-18s %" PRIu64 " total  %.2f /key\n",
               bench_scope_event_name(group->kinds[i]), med, per_key);
    }
}

static int
run_group(const char *title,
          const enum bench_scope_event_kind *kinds,
          unsigned kind_count,
          unsigned max_entries,
          unsigned fill_pct,
          unsigned query_n,
          unsigned repeat)
{
    struct ft_table ft;
    struct ft_table_config cfg = profile_cfg();
    struct profile_record4 *records;
    struct ft_table_result *results;
    u32 *idxv;
    struct bench_scope_group group;
    u64 per_event[BENCH_SCOPE_MAX_EVENTS][DEFAULT_REPEAT];
    unsigned live;

    memset(per_event, 0, sizeof(per_event));
    records = xaligned_zero(max_entries, sizeof(*records));
    results = xaligned_zero(query_n, sizeof(*results));
    idxv = xaligned_zero(query_n, sizeof(*idxv));

    if (FT_FLOW4_TABLE_INIT_TYPED(&ft, records, max_entries,
                                  struct profile_record4, entry, &cfg) != 0) {
        fprintf(stderr, "ft_flow4_table_init_ex failed\n");
        return 1;
    }

    if (bench_scope_open(&group, kinds, kind_count) != 0) {
        fprintf(stderr, "bench_scope_open failed: %s\n", strerror(errno));
        ft_flow4_table_destroy(&ft);
        free(idxv);
        free(results);
        free(records);
        return 1;
    }

    live = (unsigned)(((u64)max_entries * fill_pct) / 100u);
    if (live + query_n > max_entries) {
        fprintf(stderr, "fill/query exceeds capacity\n");
        bench_scope_close(&group);
        ft_flow4_table_destroy(&ft);
        free(idxv);
        free(results);
        free(records);
        return 1;
    }

    for (unsigned r = 0; r < repeat; r++) {
        unsigned prefill_base = r * (live + query_n * 2u + 1024u);
        struct bench_scope_sample sample;

        ft_flow4_table_flush(&ft);
        memset(records, 0, (size_t)max_entries * sizeof(*records));
        prefill(&ft, live, prefill_base);
        for (unsigned i = 0; i < query_n; i++) {
            u32 idx = live + i + 1u;
            struct profile_record4 *rec =
                FT_FLOW4_TABLE_RECORD_PTR_AS(&ft, struct profile_record4, idx);
            RIX_ASSUME_NONNULL(rec);
            rec->entry.key = profile_key4(prefill_base + live + i);
            rec->cookie = idx;
            idxv[i] = idx;
        }
        cold_touch();
        if (bench_scope_begin(&group) != 0) {
            fprintf(stderr, "bench_scope_begin failed: %s\n", strerror(errno));
            break;
        }
        ft_flow4_table_add_idx_bulk(&ft, idxv, query_n, results);
        if (bench_scope_end(&group, &sample) != 0) {
            fprintf(stderr, "bench_scope_end failed: %s\n", strerror(errno));
            break;
        }
        for (unsigned i = 0; i < sample.count; i++)
            per_event[i][r] = sample.values[i].value;
    }

    printf("[entries=%u live=%u query=%u fill=%u%%]\n",
           max_entries, live, query_n, fill_pct);
    print_sample(title, &group, per_event, repeat, query_n);

    bench_scope_close(&group);
    ft_flow4_table_destroy(&ft);
    free(idxv);
    free(results);
    free(records);
    return 0;
}

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--entries N] [--fill PCT] [--query N] [--repeat N] [--pin-core CORE]\n",
            prog);
}

int
main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "entries",  required_argument, NULL, 'e' },
        { "fill",     required_argument, NULL, 'f' },
        { "query",    required_argument, NULL, 'q' },
        { "repeat",   required_argument, NULL, 'r' },
        { "pin-core", required_argument, NULL, 'p' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    enum bench_scope_event_kind group_core[] = {
        BENCH_SCOPE_EVT_CYCLES,
        BENCH_SCOPE_EVT_INSTRUCTIONS,
        BENCH_SCOPE_EVT_BRANCHES,
        BENCH_SCOPE_EVT_BRANCH_MISSES
    };
    enum bench_scope_event_kind group_cache[] = {
        BENCH_SCOPE_EVT_CACHE_REFERENCES,
        BENCH_SCOPE_EVT_CACHE_MISSES
    };
    enum bench_scope_event_kind group_l1[] = {
        BENCH_SCOPE_EVT_L1D_LOADS,
        BENCH_SCOPE_EVT_L1D_LOAD_MISSES
    };
    unsigned entries = DEFAULT_ENTRIES;
    unsigned fill_pct = DEFAULT_FILL_PCT;
    unsigned query_n = DEFAULT_QUERY;
    unsigned repeat = DEFAULT_REPEAT;
    int opt;

    while ((opt = getopt_long(argc, argv, "e:f:q:r:p:h", opts, NULL)) != -1) {
        unsigned parsed = 0u;

        switch (opt) {
        case 'e':
            if (parse_u32(optarg, &parsed) != 0) {
                print_usage(argv[0]);
                return 2;
            }
            entries = parsed;
            break;
        case 'f':
            if (parse_u32(optarg, &parsed) != 0 || parsed > 99u) {
                print_usage(argv[0]);
                return 2;
            }
            fill_pct = parsed;
            break;
        case 'q':
            if (parse_u32(optarg, &parsed) != 0) {
                print_usage(argv[0]);
                return 2;
            }
            query_n = parsed;
            break;
        case 'r':
            if (parse_u32(optarg, &parsed) != 0 || parsed > DEFAULT_REPEAT) {
                print_usage(argv[0]);
                return 2;
            }
            repeat = parsed;
            break;
        case 'p':
            if (parse_u32(optarg, &parsed) != 0 || parsed > (unsigned)INT_MAX) {
                print_usage(argv[0]);
                return 2;
            }
            opt_pin_core = (int)parsed;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 2;
        }
    }

    if (apply_pin_core() != 0)
        return 2;

    if (run_group("add_idx_bulk scope: core",
                  group_core, 4u, entries, fill_pct, query_n, repeat) != 0)
        return 1;
    if (run_group("add_idx_bulk scope: cache",
                  group_cache, 2u, entries, fill_pct, query_n, repeat) != 0)
        return 1;
    if (run_group("add_idx_bulk scope: l1d",
                  group_l1, 2u, entries, fill_pct, query_n, repeat) != 0)
        return 1;
    return 0;
}
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
