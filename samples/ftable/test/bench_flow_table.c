/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <getopt.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include <bench/bench_scope.h>

#include "flow_table.h"

enum {
    FTB_ALIGN = 64u,
    FTB_DEFAULT_QUERY = 256u,
    FTB_QUERY_MAX = 4096u,
    FTB_FILL_PCT = 50u,
    FTB_FIND_REPEAT = 200u,
    FTB_UPDATE_REPEAT = 80u,
    FTB_GROW_REPEAT = 9u,
    FTB_COLD_REPEAT = 7u,
    FTB_SAMPLE_MAX = 64u,
    FTB_DEFAULT_ENTRIES = 1048576u,
    FTB_MAINT_MAX_EXPIRED = 512u,
    FTB_COLD_BYTES = 64u * 1024u * 1024u
};

static unsigned ftb_sample_raw_repeat = 11u;
static unsigned ftb_sample_keep_n = 7u;
static unsigned ftb_query_n = FTB_DEFAULT_QUERY;
static int ftb_pin_core = -1;
static int ftb_use_hugepage = 1;
static const char *ftb_op_filter;   /* NULL = run all */
static unsigned char *ftb_cold_buf;
static volatile u64 ftb_cold_sink;

#define FTB_NOW_ADD  UINT64_C(0x101)
#define FTB_NOW_FIND UINT64_C(0x202)

struct ftb_user_record4 {
    struct flow4_entry entry;
    u32 cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct ftb_user_record6 {
    struct flow6_entry entry;
    u32 cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct ftb_user_recordu {
    struct flowu_entry entry;
    u32 cookie;
    unsigned char pad[64];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

union ftb_any_table {
    struct ft_flow4_table flow4;
    struct ft_flow6_table flow6;
    struct ft_flowu_table flowu;
};

union ftb_any_key {
    struct flow4_key flow4;
    struct flow6_key flow6;
    struct flowu_key flowu;
};

struct ftb_variant_ops {
    const char *name;
    size_t table_size;
    size_t key_size;
    size_t record_size;
    int (*init)(void *ft, void *records, unsigned max_entries,
                const struct ft_table_config *cfg);
    void (*destroy)(void *ft);
    void (*flush)(void *ft);
    unsigned (*nb_bk)(const void *ft);
    void *(*entry_ptr)(void *ft, u32 entry_idx);
    u32 (*add_idx)(void *ft, u32 entry_idx, u64 now);
    void (*find_bulk)(void *ft, const void *keys, unsigned nb_keys,
                      u64 now,
                      struct ft_table_result *results);
    unsigned (*add_idx_bulk)(void *ft, u32 *entry_idxv,
                             unsigned nb_keys,
                             enum ft_add_policy policy,
                             u64 now,
                             u32 *unused_idxv);
    unsigned (*del_idx_bulk)(void *ft, const u32 *entry_idxv,
                               unsigned nb_keys, u32 *unused_idxv);
    unsigned (*del_key_bulk)(void *ft, const void *keys, unsigned nb_keys,
                             u32 *unused_idxv);
    int (*migrate)(void *ft, void *new_buckets, size_t new_bucket_size);
    unsigned (*maintain)(void *ft, unsigned start_bk, u64 now,
                         u64 expire_tsc, u32 *expired_idxv,
                         unsigned max_expired, unsigned min_bk_entries,
                         unsigned *next_bk);
    unsigned (*maintain_idx_bulk)(void *ft, const u32 *entry_idxv,
                                  unsigned nb_idx, u64 now,
                                  u64 expire_tsc,
                                  u32 *expired_idxv,
                                  unsigned max_expired,
                                  unsigned min_bk_entries,
                                  int enable_filter);
    struct flow_entry_meta *(*entry_meta)(void *entry);
    void (*make_key)(void *out, unsigned i);
};

static unsigned ftb_parse_arch_flag(const char *name);

static size_t
ftb_hugepage_size(void)
{
    return (size_t)2u * 1024u * 1024u; /* 2 MiB */
}

static size_t
ftb_round_up(size_t n, size_t align)
{
    return (n + align - 1u) & ~(align - 1u);
}

static void *
ftb_mmap_huge(size_t size)
{
    size_t hp = ftb_hugepage_size();
    size_t rounded = ftb_round_up(size, hp);
    void *ptr = mmap(NULL, rounded,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                     -1, 0);

    if (ptr == MAP_FAILED) {
        /* Fall back to THP via madvise */
        ptr = mmap(NULL, rounded,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
        if (ptr == MAP_FAILED) {
            fprintf(stderr, "mmap(%zu): %s\n", rounded, strerror(errno));
            return NULL;
        }
        if (madvise(ptr, rounded, MADV_HUGEPAGE) != 0)
            fprintf(stderr, "madvise(MADV_HUGEPAGE, %zu): %s\n",
                    rounded, strerror(errno));
        /* Touch pages to trigger THP promotion */
        for (size_t off = 0; off < rounded; off += 4096u)
            ((volatile char *)ptr)[off] = 0;
    }
    return ptr;
}

static void
ftb_munmap_huge(void *ptr, size_t size)
{
    size_t hp = ftb_hugepage_size();
    size_t rounded = ftb_round_up(size, hp);

    munmap(ptr, rounded);
}

static void *
ftb_alloc_buckets(size_t size)
{
    if (ftb_use_hugepage)
        return ftb_mmap_huge(size);
    return aligned_alloc(FT_TABLE_BUCKET_ALIGN, size);
}

static void
ftb_free_buckets(void *ptr, size_t size)
{
    if (ftb_use_hugepage)
        ftb_munmap_huge(ptr, size);
    else
        free(ptr);
}

static void *
ftb_alloc_zero(size_t count, size_t size)
{
    size_t total = count * size;

    if (ftb_use_hugepage) {
        void *ptr = ftb_mmap_huge(total);

        if (ptr == NULL)
            exit(1);
        return ptr; /* mmap returns zeroed memory */
    }
    {
        size_t rounded = ftb_round_up(total, FTB_ALIGN);
        void *ptr = aligned_alloc(FTB_ALIGN, rounded);

        if (ptr == NULL) {
            perror("aligned_alloc");
            exit(1);
        }
        memset(ptr, 0, rounded);
        return ptr;
    }
}

static void
ftb_free_zero(void *ptr, size_t count, size_t size)
{
    if (ftb_use_hugepage)
        ftb_munmap_huge(ptr, count * size);
    else
        free(ptr);
}

static void *
ftb_xcalloc(size_t count, size_t size)
{
    void *ptr = calloc(count, size);

    if (ptr == NULL) {
        perror("calloc");
        exit(1);
    }
    return ptr;
}

static void
ftb_cold_touch(void)
{
    u64 sum = 0u;

    if (ftb_cold_buf == NULL) {
        ftb_cold_buf = ftb_alloc_zero(1u, FTB_COLD_BYTES);
    }
    for (size_t off = 0; off < FTB_COLD_BYTES; off += 64u)
        sum += ftb_cold_buf[off];
    ftb_cold_sink += sum;
}

static inline u64
ftb_rdtsc(void)
{
#if defined(__x86_64__)
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
#endif
}

static int
ftb_cmp_u64(const void *a, const void *b)
{
    u64 av = *(const u64 *)a;
    u64 bv = *(const u64 *)b;
    return (av > bv) - (av < bv);
}

static double
ftb_median_u64(u64 *samples, unsigned n)
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

static unsigned
ftb_pool_count(unsigned desired)
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
    printf("sampling: raw_repeat=%u  keep_n=%u  query=%u\n",
           ftb_sample_raw_repeat, ftb_sample_keep_n, ftb_query_n);
}

static int
ftb_parse_u32_text(const char *s, unsigned *out, int allow_zero)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return -1;
    v = strtoul(s, &end, 10);
    if (end == NULL || *end != '\0')
        return -1;
    if ((!allow_zero && v == 0ul) || v > UINT_MAX)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static void
ftb_print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "usage: %s [options] [flow4|flow6|flowu] [entries] [fill_pct]\n"
            "options:\n"
            "  -a, --arch ARCH          gen|sse|avx2|avx512|auto\n"
            "  -r, --raw-repeat N       sampling raw repeat count (1..%u)\n"
            "  -k, --keep-n N           kept sample count (1..%u)\n"
            "  -q, --query N            query batch size (1..%u)\n"
            "  -g, --grow               run grow benchmark instead of datapath\n"
            "  -m, --maint              run maintain benchmark instead of datapath\n"
            "  -p, --pin-core CORE      pin benchmark to a CPU core\n"
            "  -o, --op OP              run only OP (find_hit,find_miss,add_idx,\n"
            "                           add_ignore,add_update,del_idx,del_key,\n"
            "                           find_del_idx)\n"
            "  -H, --no-hugepage        disable 2 MiB hugepages (on by default)\n"
            "  -h, --help               show this help\n",
            prog, FTB_SAMPLE_MAX, FTB_SAMPLE_MAX, FTB_QUERY_MAX);
}

static int
ftb_parse_options(int argc, char **argv, unsigned *arch_enable,
                  int *run_grow, int *run_maint)
{
    static const struct option long_opts[] = {
        { "arch",       required_argument, NULL, 'a' },
        { "raw-repeat", required_argument, NULL, 'r' },
        { "keep-n",     required_argument, NULL, 'k' },
        { "query",      required_argument, NULL, 'q' },
        { "grow",       no_argument,       NULL, 'g' },
        { "maint",      no_argument,       NULL, 'm' },
        { "pin-core",   required_argument, NULL, 'p' },
        { "op",         required_argument, NULL, 'o' },
        { "no-hugepage", no_argument,       NULL, 'H' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL,         0,                 NULL,  0  }
    };

    int opt;

    opterr = 0;
    while ((opt = getopt_long(argc, argv, "a:r:k:q:gmo:p:Hh", long_opts, NULL)) != -1) {
        unsigned parsed = 0u;

        switch (opt) {
        case 'a':
            *arch_enable = ftb_parse_arch_flag(optarg);
            break;
        case 'r':
            if (ftb_parse_u32_text(optarg, &parsed, 0) != 0 || parsed > FTB_SAMPLE_MAX) {
                fprintf(stderr, "--raw-repeat must be in 1..%u\n", FTB_SAMPLE_MAX);
                return 2;
            }
            ftb_sample_raw_repeat = parsed;
            break;
        case 'k':
            if (ftb_parse_u32_text(optarg, &parsed, 0) != 0 || parsed > FTB_SAMPLE_MAX) {
                fprintf(stderr, "--keep-n must be in 1..%u\n", FTB_SAMPLE_MAX);
                return 2;
            }
            ftb_sample_keep_n = parsed;
            break;
        case 'q':
            if (ftb_parse_u32_text(optarg, &parsed, 0) != 0 || parsed > FTB_QUERY_MAX) {
                fprintf(stderr, "--query must be in 1..%u\n", FTB_QUERY_MAX);
                return 2;
            }
            ftb_query_n = parsed;
            break;
        case 'g':
            *run_grow = 1;
            break;
        case 'm':
            *run_maint = 1;
            break;
        case 'o':
            ftb_op_filter = optarg;
            break;
        case 'H':
            ftb_use_hugepage = 0;
            break;
        case 'p':
            if (ftb_parse_u32_text(optarg, &parsed, 1) != 0 || parsed > (unsigned)INT_MAX) {
                fprintf(stderr, "--pin-core must be in 0..%d\n", INT_MAX);
                return 2;
            }
            ftb_pin_core = (int)parsed;
            break;
        case 'h':
            ftb_print_usage(stdout, argv[0]);
            return 1;
        case '?':
        default:
            if (optopt != 0)
                fprintf(stderr, "unknown or malformed option: -%c\n", optopt);
            else
                fprintf(stderr, "unknown option: %s\n", argv[optind - 1]);
            ftb_print_usage(stderr, argv[0]);
            return 2;
        }
    }

    if (ftb_sample_keep_n > ftb_sample_raw_repeat) {
        fprintf(stderr, "--keep-n must be less than or equal to --raw-repeat\n");
        return 2;
    }
    if (*run_grow && *run_maint) {
        fprintf(stderr, "--grow and --maint are mutually exclusive\n");
        return 2;
    }
    return 0;
}

static struct ft_table_config
ftb_cfg(void)
{
    struct ft_table_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    return cfg;
}

static struct flow4_key
ftb_key4(unsigned i)
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

static struct flow6_key
ftb_key6(unsigned i)
{
    struct flow6_key k;
    u32 w;

    memset(&k, 0, sizeof(k));
    k.family   = 10u;
    k.proto    = 6u;
    k.src_port = (u16)(1000u + i);
    k.dst_port = (u16)(2000u + i);
    k.vrfid    = 1u;
    w = i;
    memcpy(&k.src_ip[0], &w, sizeof(w));
    w = i ^ UINT32_C(0x13579bdf);
    memcpy(&k.src_ip[4], &w, sizeof(w));
    w = i ^ UINT32_C(0x2468ace0);
    memcpy(&k.dst_ip[0], &w, sizeof(w));
    w = ~i;
    memcpy(&k.dst_ip[4], &w, sizeof(w));
    w = i * UINT32_C(0x9e3779b1);
    memcpy(&k.src_ip[8], &w, sizeof(w));
    w = (i << 1) ^ UINT32_C(0xa5a5a5a5);
    memcpy(&k.dst_ip[8], &w, sizeof(w));
    w = i + UINT32_C(0x01020304);
    memcpy(&k.src_ip[12], &w, sizeof(w));
    w = i ^ UINT32_C(0x5a5a5a5a);
    memcpy(&k.dst_ip[12], &w, sizeof(w));
    return k;
}

static struct flowu_key
ftb_keyu(unsigned i)
{
    struct flowu_key k;
    u32 w;

    memset(&k, 0, sizeof(k));
    k.proto    = 6u;
    k.src_port = (u16)(1000u + i);
    k.dst_port = (u16)(2000u + i);
    k.vrfid    = 1u;
    if ((i & 1u) == 0u) {
        k.family = 2u;
        k.addr.v4.src = UINT32_C(0x0a000001) ^ i;
        k.addr.v4.dst = UINT32_C(0x0a100001) ^ (i * UINT32_C(0x9e3779b1));
    } else {
        k.family = 10u;
        w = i;
        memcpy(&k.addr.v6.src[0], &w, sizeof(w));
        w = i ^ UINT32_C(0x13579bdf);
        memcpy(&k.addr.v6.src[4], &w, sizeof(w));
        w = i * UINT32_C(0x9e3779b1);
        memcpy(&k.addr.v6.src[8], &w, sizeof(w));
        w = i + UINT32_C(0x01020304);
        memcpy(&k.addr.v6.src[12], &w, sizeof(w));
        w = ~i;
        memcpy(&k.addr.v6.dst[0], &w, sizeof(w));
        w = i ^ UINT32_C(0x2468ace0);
        memcpy(&k.addr.v6.dst[4], &w, sizeof(w));
        w = (i << 1) ^ UINT32_C(0xa5a5a5a5);
        memcpy(&k.addr.v6.dst[8], &w, sizeof(w));
        w = i ^ UINT32_C(0x5a5a5a5a);
        memcpy(&k.addr.v6.dst[12], &w, sizeof(w));
    }
    return k;
}

#define FTB_VARIANT_WRAPPERS(tag, prefix, key_t, record_t, init_fn,           \
                             destroy_fn, flush_fn, nb_bk_fn,                  \
                             entry_ptr_fn, add_idx_fn, find_bulk_fn,          \
                             add_idx_bulk_fn,                                 \
                             del_idx_bulk_fn,                                 \
                             del_key_bulk_fn,                                 \
                             migrate_fn, maintain_fn,                         \
                             maintain_idx_bulk_fn,                            \
                             make_key_fn)                                     \
static int                                                                    \
ftb_init_##tag(void *ft, void *records, unsigned max_entries,                 \
               const struct ft_table_config *cfg)                             \
{                                                                             \
    size_t bsz = ft_table_bucket_size(max_entries);                           \
    void *bk = ftb_alloc_buckets(bsz);                                       \
    if (bk == NULL) return -1;                                                \
    return init_fn((struct ft_##prefix##_table *)ft, records,                 \
                   max_entries, sizeof(record_t),                              \
                   offsetof(record_t, entry), bk, bsz, cfg);                  \
}                                                                             \
static void                                                                   \
ftb_destroy_##tag(void *ft)                                                   \
{                                                                             \
    struct ft_##prefix##_table *t = (struct ft_##prefix##_table *)ft;         \
    void *bk = t->buckets;                                                    \
    size_t bsz = (size_t)t->nb_bk * FT_TABLE_BUCKET_SIZE;                    \
    destroy_fn(t);                                                            \
    ftb_free_buckets(bk, bsz);                                               \
}                                                                             \
static void                                                                   \
ftb_flush_##tag(void *ft)                                                     \
{                                                                             \
    flush_fn((struct ft_##prefix##_table *)ft);                               \
}                                                                             \
static unsigned                                                               \
ftb_nb_bk_##tag(const void *ft)                                               \
{                                                                             \
    return nb_bk_fn((const struct ft_##prefix##_table *)ft);                  \
}                                                                             \
static void *                                                                 \
ftb_entry_ptr_##tag(void *ft, u32 entry_idx)                             \
{                                                                             \
    return entry_ptr_fn((struct ft_##prefix##_table *)ft, entry_idx);         \
}                                                                             \
static struct flow_entry_meta *                                                \
ftb_entry_meta_##tag(void *entry)                                             \
{                                                                             \
    return &((struct prefix##_entry *)entry)->meta;                           \
}                                                                             \
static u32                                                               \
ftb_add_idx_##tag(void *ft, u32 entry_idx, u64 now)                 \
{                                                                             \
    return add_idx_fn((struct ft_##prefix##_table *)ft, entry_idx, now);      \
}                                                                             \
static void                                                                   \
ftb_find_bulk_##tag(void *ft, const void *keys, unsigned nb_keys,             \
                    u64 now,                                             \
                    struct ft_table_result *results)                          \
{                                                                             \
    find_bulk_fn((struct ft_##prefix##_table *)ft,                            \
                 (const key_t *)keys, nb_keys, now, results);                 \
}                                                                             \
static unsigned                                                               \
ftb_add_idx_bulk_##tag(void *ft, u32 *entry_idxv,                        \
                       unsigned nb_keys,                                      \
                       enum ft_add_policy policy,                             \
                       u64 now,                                          \
                       u32 *unused_idxv)                                 \
{                                                                             \
    return add_idx_bulk_fn((struct ft_##prefix##_table *)ft,                  \
                           entry_idxv, nb_keys, policy, now, unused_idxv);    \
}                                                                             \
static unsigned                                                               \
ftb_del_idx_bulk_##tag(void *ft, const u32 *entry_idxv,            \
                             unsigned nb_keys, u32 *unused_idxv)             \
{                                                                             \
    return del_idx_bulk_fn((struct ft_##prefix##_table *)ft,                   \
                          entry_idxv, nb_keys, unused_idxv);                  \
}                                                                             \
static unsigned                                                               \
ftb_del_key_bulk_##tag(void *ft, const void *keys, unsigned nb_keys,          \
                       u32 *unused_idxv)                                 \
{                                                                             \
    return del_key_bulk_fn((struct ft_##prefix##_table *)ft,                  \
                           (const key_t *)keys, nb_keys, unused_idxv);        \
}                                                                             \
static int                                                                    \
ftb_migrate_##tag(void *ft, void *new_buckets, size_t new_bucket_size)        \
{                                                                             \
    return migrate_fn((struct ft_##prefix##_table *)ft,                       \
                      new_buckets, new_bucket_size);                           \
}                                                                             \
static unsigned                                                               \
ftb_maintain_##tag(void *ft, unsigned start_bk, u64 now,                 \
                   u64 expire_tsc, u32 *expired_idxv,               \
                   unsigned max_expired, unsigned min_bk_entries,             \
                   unsigned *next_bk)                                         \
{                                                                             \
    return maintain_fn((struct ft_##prefix##_table *)ft, start_bk, now,       \
                       expire_tsc, expired_idxv, max_expired,                 \
                       min_bk_entries, next_bk);                              \
}                                                                             \
static unsigned                                                               \
ftb_maintain_idx_bulk_##tag(void *ft, const u32 *entry_idxv,             \
                            unsigned nb_idx, u64 now,                    \
                            u64 expire_tsc,                              \
                            u32 *expired_idxv,                           \
                            unsigned max_expired,                             \
                            unsigned min_bk_entries,                          \
                            int enable_filter)                                \
{                                                                             \
    return maintain_idx_bulk_fn((struct ft_##prefix##_table *)ft,             \
                                entry_idxv, nb_idx, now, expire_tsc,          \
                                expired_idxv, max_expired,                    \
                                min_bk_entries, enable_filter);               \
}                                                                             \
static void                                                                   \
ftb_make_key_##tag(void *out, unsigned i)                                     \
{                                                                             \
    *(key_t *)out = make_key_fn(i);                                           \
}                                                                             \
static const struct ftb_variant_ops ftb_ops_##tag = {                         \
    .name = #prefix,                                                          \
    .table_size = sizeof(struct ft_##prefix##_table),                         \
    .key_size = sizeof(key_t),                                                \
    .record_size = sizeof(record_t),                                          \
    .init = ftb_init_##tag,                                                   \
    .destroy = ftb_destroy_##tag,                                             \
    .flush = ftb_flush_##tag,                                                 \
    .nb_bk = ftb_nb_bk_##tag,                                                 \
    .entry_ptr = ftb_entry_ptr_##tag,                                         \
    .add_idx = ftb_add_idx_##tag,                                             \
    .find_bulk = ftb_find_bulk_##tag,                                         \
    .add_idx_bulk = ftb_add_idx_bulk_##tag,                                   \
    .del_idx_bulk = ftb_del_idx_bulk_##tag,                       \
    .del_key_bulk = ftb_del_key_bulk_##tag,                                   \
    .migrate = ftb_migrate_##tag,                                              \
    .maintain = ftb_maintain_##tag,                                           \
    .maintain_idx_bulk = ftb_maintain_idx_bulk_##tag,                         \
    .entry_meta = ftb_entry_meta_##tag,                                       \
    .make_key = ftb_make_key_##tag                                            \
}

FTB_VARIANT_WRAPPERS(flow4, flow4, struct flow4_key, struct ftb_user_record4,
                     ft_flow4_table_init, ft_flow4_table_destroy,
                     ft_flow4_table_flush, ft_flow4_table_nb_bk,
                     ft_flow4_table_entry_ptr,
                     ft_flow4_table_add_idx, ft_flow4_table_find_bulk,
                     ft_flow4_table_add_idx_bulk,
                     ft_flow4_table_del_idx_bulk,
                     ft_flow4_table_del_key_bulk,
                     ft_flow4_table_migrate, ft_flow4_table_maintain,
                     ft_flow4_table_maintain_idx_bulk,
                     ftb_key4);

FTB_VARIANT_WRAPPERS(flow6, flow6, struct flow6_key, struct ftb_user_record6,
                     ft_flow6_table_init, ft_flow6_table_destroy,
                     ft_flow6_table_flush, ft_flow6_table_nb_bk,
                     ft_flow6_table_entry_ptr,
                     ft_flow6_table_add_idx, ft_flow6_table_find_bulk,
                     ft_flow6_table_add_idx_bulk,
                     ft_flow6_table_del_idx_bulk,
                     ft_flow6_table_del_key_bulk,
                     ft_flow6_table_migrate, ft_flow6_table_maintain,
                     ft_flow6_table_maintain_idx_bulk,
                     ftb_key6);

FTB_VARIANT_WRAPPERS(flowu, flowu, struct flowu_key, struct ftb_user_recordu,
                     ft_flowu_table_init, ft_flowu_table_destroy,
                     ft_flowu_table_flush, ft_flowu_table_nb_bk,
                     ft_flowu_table_entry_ptr,
                     ft_flowu_table_add_idx, ft_flowu_table_find_bulk,
                     ft_flowu_table_add_idx_bulk,
                     ft_flowu_table_del_idx_bulk,
                     ft_flowu_table_del_key_bulk,
                     ft_flowu_table_migrate, ft_flowu_table_maintain,
                     ft_flowu_table_maintain_idx_bulk,
                     ftb_keyu);

#define FTB_KEY_AT(base, ops, i) \
    ((void *)((unsigned char *)(base) + (size_t)(i) * (ops)->key_size))

static unsigned
ftb_fill_target(unsigned entries, unsigned pct)
{
    return (unsigned)(((u64)entries * pct) / 100u);
}

static void
ftb_bind_key(const struct ftb_variant_ops *ops, void *ft, u32 entry_idx,
             const void *key)
{
    void *entry = ops->entry_ptr(ft, entry_idx);

    if (entry == NULL) {
        fprintf(stderr, "bind_key failed for %s at entry_idx=%u\n",
                ops->name, entry_idx);
        exit(1);
    }
    memcpy(entry, key, ops->key_size);
}

static void
ftb_prefill(const struct ftb_variant_ops *ops, void *ft, unsigned n,
            unsigned key_base)
{
    union ftb_any_key key;

    for (unsigned i = 0; i < n; i++) {
        ops->make_key(&key, key_base + i);
        ftb_bind_key(ops, ft, i + 1u, &key);
        if (ops->add_idx(ft, i + 1u, FTB_NOW_ADD) != i + 1u) {
            fprintf(stderr, "prefill failed for %s at %u\n", ops->name, i);
            exit(1);
        }
    }
}

enum ftb_find_mode {
    FTB_FIND_HIT,
    FTB_FIND_MISS,
    FTB_FIND_MIX_50_50
};

static void
ftb_build_find_keys(const struct ftb_variant_ops *ops, void *keys,
                    unsigned entries, unsigned live, unsigned key_base,
                    enum ftb_find_mode mode)
{
    unsigned hit_n = (mode == FTB_FIND_MIX_50_50)
                   ? ((ftb_query_n + 1u) / 2u) : ftb_query_n;

    for (unsigned i = 0; i < ftb_query_n; i++) {
        unsigned seq;

        if (mode == FTB_FIND_HIT) {
            seq = key_base + i;
        } else if (mode == FTB_FIND_MISS) {
            seq = entries + key_base + 100000u + i;
        } else {
            unsigned alt = i / 2u;

            if ((i & 1u) == 0u && alt < hit_n)
                seq = key_base + alt;
            else
                seq = entries + key_base + 100000u + (i - alt);
        }
        ops->make_key(FTB_KEY_AT(keys, ops, i), seq);
    }
    (void)live;
}

static double
ftb_measure_find_lookup(const struct ftb_variant_ops *ops,
                        unsigned entries, unsigned fill_pct,
                        unsigned key_base, enum ftb_find_mode mode)
{
    unsigned max_entries = ftb_pool_count(entries);
    struct ft_table_config cfg = ftb_cfg();
    union ftb_any_table ft;
    void *pool = ftb_alloc_zero(max_entries, ops->record_size);
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    struct ft_table_result *results =
        ftb_xcalloc(ftb_query_n, sizeof(*results));
    u64 samples[FTB_COLD_REPEAT];
    unsigned live = ftb_fill_target(max_entries, fill_pct);

    if (ops->init(&ft, pool, max_entries, &cfg) != 0) {
        fprintf(stderr, "find bench init failed for %s\n", ops->name);
        exit(1);
    }
    ftb_prefill(ops, &ft, live, key_base);
    ftb_build_find_keys(ops, keys, max_entries, live, key_base, mode);
    for (unsigned r = 0; r < FTB_COLD_REPEAT; r++) {
        u64 t0, t1;

        ftb_cold_touch();
        t0 = ftb_rdtsc();
        ops->find_bulk(&ft, keys, ftb_query_n, FTB_NOW_FIND, results);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    ops->destroy(&ft);
    free(results);
    free(keys);
    ftb_free_zero(pool, max_entries, ops->record_size);
    return ftb_median_u64(samples, FTB_COLD_REPEAT) / (double)ftb_query_n;
}

static double __attribute__((unused))
ftb_sample_find_lookup(const struct ftb_variant_ops *ops,
                       unsigned entries, unsigned fill_pct,
                       enum ftb_find_mode mode)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_find_lookup(ops, entries, fill_pct,
                                             r * (ftb_query_n + 1024u), mode);
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

/*
 * Common cold-state setup for all datapath benchmarks.
 *
 * flush → prefill(live) → make_key/bind_key(query) → add_idx_bulk → cold_touch
 *
 * After this call the table has live+query entries inserted,
 * keys[] holds the query keys, idxv[] holds the query entry indices,
 * and caches are cold.
 */
static void
ftb_setup_cold_round(const struct ftb_variant_ops *ops, void *ft,
                     unsigned live, unsigned prefill_base,
                     void *keys, u32 *idxv, u32 *unused_idxv)
{
    ops->flush(ft);
    ftb_prefill(ops, ft, live, prefill_base);
    for (unsigned i = 0; i < ftb_query_n; i++) {
        ops->make_key(FTB_KEY_AT(keys, ops, i), prefill_base + live + i);
        idxv[i] = live + i + 1u;
        ftb_bind_key(ops, ft, idxv[i], FTB_KEY_AT(keys, ops, i));
    }
    (void)ops->add_idx_bulk(ft, idxv, ftb_query_n, FT_ADD_IGNORE,
                            FTB_NOW_ADD, unused_idxv);
    ftb_cold_touch();
}

static double
ftb_measure_add_idx(const struct ftb_variant_ops *ops, void *ft,
                    unsigned live, unsigned key_base)
{
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    u32 *idxv = ftb_xcalloc(ftb_query_n, sizeof(*idxv));
    u32 *unused_idxv = ftb_xcalloc(ftb_query_n, sizeof(*unused_idxv));
    u64 samples[FTB_UPDATE_REPEAT];

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        unsigned prefill_base = key_base + r * (live + ftb_query_n * 2u + 1024u);
        u64 t0, t1;

        /* del first, then cold_touch, then measure add */
        ftb_setup_cold_round(ops, ft, live, prefill_base,
                             keys, idxv, unused_idxv);
        ops->del_idx_bulk(ft, idxv, ftb_query_n, unused_idxv);
        ftb_cold_touch();
        t0 = ftb_rdtsc();
        (void)ops->add_idx_bulk(ft, idxv, ftb_query_n, FT_ADD_IGNORE,
                                FTB_NOW_ADD, unused_idxv);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    free(unused_idxv);
    free(idxv);
    free(keys);
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)ftb_query_n;
}

static double
ftb_sample_add_idx(const struct ftb_variant_ops *ops, void *ft,
                   unsigned live, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_add_idx(ops, ft, live,
                                          key_base + r * (live + ftb_query_n * 2u + 1024u));
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_add_dup_policy(const struct ftb_variant_ops *ops, void *ft,
                           unsigned live, unsigned key_base,
                           enum ft_add_policy policy)
{
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    u32 *idxv = ftb_xcalloc(ftb_query_n, sizeof(*idxv));
    u32 *dup_idxv = ftb_xcalloc(ftb_query_n, sizeof(*dup_idxv));
    u32 *unused_idxv = ftb_xcalloc(ftb_query_n, sizeof(*unused_idxv));
    u64 samples[FTB_UPDATE_REPEAT];

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        unsigned prefill_base = key_base + r * (live + ftb_query_n * 2u + 1024u);
        u64 t0, t1;

        ftb_setup_cold_round(ops, ft, live, prefill_base,
                             keys, idxv, unused_idxv);
        /* use different idx with same key for key-duplicate */
        for (unsigned i = 0; i < ftb_query_n; i++) {
            dup_idxv[i] = live + ftb_query_n + i + 1u;
            ftb_bind_key(ops, ft, dup_idxv[i],
                         FTB_KEY_AT(keys, ops, i));
        }
        ftb_cold_touch();
        t0 = ftb_rdtsc();
        (void)ops->add_idx_bulk(ft, dup_idxv, ftb_query_n, policy,
                                FTB_NOW_ADD, unused_idxv);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    free(unused_idxv);
    free(dup_idxv);
    free(idxv);
    free(keys);
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)ftb_query_n;
}

static double
ftb_sample_add_dup_policy(const struct ftb_variant_ops *ops, void *ft,
                          unsigned live, unsigned key_base,
                          enum ft_add_policy policy)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_add_dup_policy(
            ops, ft, live,
            key_base + r * (live + ftb_query_n * 2u + 1024u), policy);
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_find_hit(const struct ftb_variant_ops *ops, void *ft,
                     unsigned live, unsigned key_base)
{
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    u32 *idxv = ftb_xcalloc(ftb_query_n, sizeof(*idxv));
    u32 *unused_idxv = ftb_xcalloc(ftb_query_n, sizeof(*unused_idxv));
    struct ft_table_result *results =
        ftb_xcalloc(ftb_query_n, sizeof(*results));
    u64 samples[FTB_UPDATE_REPEAT];

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        unsigned prefill_base = key_base + r * (live + ftb_query_n * 2u + 1024u);
        u64 t0, t1;

        ftb_setup_cold_round(ops, ft, live, prefill_base,
                             keys, idxv, unused_idxv);
        t0 = ftb_rdtsc();
        ops->find_bulk(ft, keys, ftb_query_n, FTB_NOW_FIND, results);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    free(results);
    free(unused_idxv);
    free(idxv);
    free(keys);
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)ftb_query_n;
}

static double
ftb_sample_find_hit(const struct ftb_variant_ops *ops, void *ft,
                    unsigned live, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_find_hit(ops, ft, live,
                                          key_base + r * (live + ftb_query_n * 2u + 1024u));
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_find_miss(const struct ftb_variant_ops *ops, void *ft,
                      unsigned live, unsigned key_base)
{
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    void *miss_keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    u32 *idxv = ftb_xcalloc(ftb_query_n, sizeof(*idxv));
    u32 *unused_idxv = ftb_xcalloc(ftb_query_n, sizeof(*unused_idxv));
    struct ft_table_result *results =
        ftb_xcalloc(ftb_query_n, sizeof(*results));
    u64 samples[FTB_UPDATE_REPEAT];

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        unsigned prefill_base = key_base + r * (live + ftb_query_n * 2u + 1024u);
        u64 t0, t1;

        ftb_setup_cold_round(ops, ft, live, prefill_base,
                             keys, idxv, unused_idxv);
        for (unsigned i = 0; i < ftb_query_n; i++)
            ops->make_key(FTB_KEY_AT(miss_keys, ops, i),
                          prefill_base + live + ftb_query_n + i);
        ftb_cold_touch();
        t0 = ftb_rdtsc();
        ops->find_bulk(ft, miss_keys, ftb_query_n, FTB_NOW_FIND, results);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    free(results);
    free(unused_idxv);
    free(idxv);
    free(miss_keys);
    free(keys);
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)ftb_query_n;
}

static double
ftb_sample_find_miss(const struct ftb_variant_ops *ops, void *ft,
                     unsigned live, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_find_miss(ops, ft, live,
                                           key_base + r * (live + ftb_query_n * 2u + 1024u));
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_del_idx(const struct ftb_variant_ops *ops, void *ft,
                    unsigned live, unsigned key_base)
{
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    u32 *idxv = ftb_xcalloc(ftb_query_n, sizeof(*idxv));
    u32 *unused_idxv = ftb_xcalloc(ftb_query_n, sizeof(*unused_idxv));
    u64 samples[FTB_UPDATE_REPEAT];

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        unsigned prefill_base = key_base + r * (live + ftb_query_n * 2u + 1024u);
        u64 t0, t1;

        ftb_setup_cold_round(ops, ft, live, prefill_base,
                             keys, idxv, unused_idxv);
        t0 = ftb_rdtsc();
        ops->del_idx_bulk(ft, idxv, ftb_query_n, unused_idxv);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    free(unused_idxv);
    free(idxv);
    free(keys);
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)ftb_query_n;
}

static double
ftb_sample_del_idx(const struct ftb_variant_ops *ops, void *ft,
                   unsigned live, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_del_idx(ops, ft, live,
                                         key_base + r * (live + ftb_query_n * 2u + 1024u));
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_del_key(const struct ftb_variant_ops *ops, void *ft,
                    unsigned live, unsigned key_base)
{
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    u32 *idxv = ftb_xcalloc(ftb_query_n, sizeof(*idxv));
    u32 *unused_idxv = ftb_xcalloc(ftb_query_n, sizeof(*unused_idxv));
    u64 samples[FTB_UPDATE_REPEAT];

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        unsigned prefill_base = key_base + r * (live + ftb_query_n * 2u + 1024u);
        u64 t0, t1;

        ftb_setup_cold_round(ops, ft, live, prefill_base,
                             keys, idxv, unused_idxv);
        t0 = ftb_rdtsc();
        ops->del_key_bulk(ft, keys, ftb_query_n, unused_idxv);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    free(unused_idxv);
    free(idxv);
    free(keys);
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)ftb_query_n;
}

static double
ftb_sample_del_key(const struct ftb_variant_ops *ops, void *ft,
                   unsigned live, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_del_key(ops, ft, live,
                                         key_base + r * (live + ftb_query_n * 2u + 1024u));
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

static double
ftb_measure_find_del_idx(const struct ftb_variant_ops *ops, void *ft,
                         unsigned live, unsigned key_base)
{
    void *keys = ftb_xcalloc(ftb_query_n, ops->key_size);
    u32 *idxv = ftb_xcalloc(ftb_query_n, sizeof(*idxv));
    u32 *unused_idxv = ftb_xcalloc(ftb_query_n, sizeof(*unused_idxv));
    struct ft_table_result *results =
        ftb_xcalloc(ftb_query_n, sizeof(*results));
    u64 samples[FTB_UPDATE_REPEAT];

    for (unsigned r = 0; r < FTB_UPDATE_REPEAT; r++) {
        unsigned prefill_base = key_base + r * (live + ftb_query_n * 2u + 1024u);
        u64 t0, t1;

        ftb_setup_cold_round(ops, ft, live, prefill_base,
                             keys, idxv, unused_idxv);
        t0 = ftb_rdtsc();
        ops->find_bulk(ft, keys, ftb_query_n, FTB_NOW_FIND, results);
        for (unsigned i = 0; i < ftb_query_n; i++)
            idxv[i] = results[i].entry_idx;
        ops->del_idx_bulk(ft, idxv, ftb_query_n, unused_idxv);
        t1 = ftb_rdtsc();
        samples[r] = t1 - t0;
    }
    free(results);
    free(unused_idxv);
    free(idxv);
    free(keys);
    return ftb_median_u64(samples, FTB_UPDATE_REPEAT) / (double)ftb_query_n;
}

static double
ftb_sample_find_del_idx(const struct ftb_variant_ops *ops, void *ft,
                        unsigned live, unsigned key_base)
{
    double samples[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        samples[r] = ftb_measure_find_del_idx(ops, ft, live,
                                              key_base + r * (live + ftb_query_n * 2u + 1024u));
    }
    return ftb_aggregate_double(samples, ftb_sample_raw_repeat,
                                ftb_sample_keep_n);
}

struct ftb_grow_result {
    double alloc_cpe;
    double migrate_cpe;
    double total_cpe;
};

static struct ftb_grow_result
ftb_measure_grow(const struct ftb_variant_ops *ops,
                 unsigned entries, unsigned fill_pct)
{
    unsigned max_entries = ftb_pool_count(entries);
    struct ft_table_config cfg = ftb_cfg();
    void *pool = ftb_alloc_zero(max_entries, ops->record_size);
    u64 alloc_samples[FTB_GROW_REPEAT];
    u64 migrate_samples[FTB_GROW_REPEAT];
    unsigned live = ftb_fill_target(max_entries, fill_pct);
    struct ftb_grow_result res;

    for (unsigned r = 0; r < FTB_GROW_REPEAT; r++) {
        union ftb_any_table ft;
        u64 t0, t1, t2;

        memset(pool, 0, (size_t)max_entries * ops->record_size);
        if (ops->init(&ft, pool, max_entries, &cfg) != 0) {
            fprintf(stderr, "grow bench init failed for %s\n", ops->name);
            exit(1);
        }
        ftb_prefill(ops, &ft, live, r * (live + 1024u));
        {
            unsigned cur_nb_bk = ops->nb_bk(&ft);
            size_t old_bsz = (size_t)cur_nb_bk * FT_TABLE_BUCKET_SIZE;
            size_t new_bsz = (size_t)cur_nb_bk * 2u * FT_TABLE_BUCKET_SIZE;
            void *old_bk;
            void *new_bk;

            t0 = ftb_rdtsc();
            new_bk = ftb_alloc_buckets(new_bsz);
            t1 = ftb_rdtsc();
            if (new_bk == NULL) {
                fprintf(stderr, "bucket alloc failed for %s\n", ops->name);
                exit(1);
            }
            old_bk = ft.flow4.buckets;  /* buckets at same offset in all variants */
            if (ops->migrate(&ft, new_bk, new_bsz) != 0) {
                fprintf(stderr, "migrate failed for %s\n", ops->name);
                ftb_free_buckets(new_bk, new_bsz);
                exit(1);
            }
            ftb_free_buckets(old_bk, old_bsz);
            t2 = ftb_rdtsc();
        }
        alloc_samples[r] = t1 - t0;
        migrate_samples[r] = t2 - t1;
        ops->destroy(&ft);
    }

    ftb_free_zero(pool, max_entries, ops->record_size);
    res.alloc_cpe = ftb_median_u64(alloc_samples, FTB_GROW_REPEAT)
                    / (double)live;
    res.migrate_cpe = ftb_median_u64(migrate_samples, FTB_GROW_REPEAT)
                      / (double)live;
    res.total_cpe = res.alloc_cpe + res.migrate_cpe;
    return res;
}

static struct ftb_grow_result
ftb_sample_grow(const struct ftb_variant_ops *ops,
                unsigned entries, unsigned fill_pct)
{
    double alloc_s[FTB_SAMPLE_MAX], mig_s[FTB_SAMPLE_MAX];

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        struct ftb_grow_result g = ftb_measure_grow(ops, entries, fill_pct);
        alloc_s[r] = g.alloc_cpe;
        mig_s[r] = g.migrate_cpe;
    }
    struct ftb_grow_result out;
    out.alloc_cpe = ftb_aggregate_double(alloc_s, ftb_sample_raw_repeat,
                                         ftb_sample_keep_n);
    out.migrate_cpe = ftb_aggregate_double(mig_s, ftb_sample_raw_repeat,
                                           ftb_sample_keep_n);
    out.total_cpe = out.alloc_cpe + out.migrate_cpe;
    return out;
}

struct ftb_maint_metrics {
    double cycles_per_entry;
    double ipc;
    double cache_hit_rate;
};

enum ftb_maint_mode {
    FTB_MAINT_EXPIRE_DENSE = 0,
    FTB_MAINT_NOHIT_DENSE,
    FTB_MAINT_HIT_IDX_EXPIRE,
    FTB_MAINT_HIT_IDX_EXPIRE_FILTERED,
};

static u64
ftb_perf_value(const struct bench_scope_sample *sample,
               enum bench_scope_event_kind kind)
{
    for (unsigned i = 0; i < sample->count; i++) {
        if (sample->values[i].kind == kind)
            return sample->values[i].value;
    }
    return 0u;
}

static void
ftb_prepare_maint_table(const struct ftb_variant_ops *ops, void *ft,
                        unsigned entries, unsigned fill_pct, unsigned key_base,
                        enum ftb_maint_mode mode)
{
    unsigned live = ftb_fill_target(entries, fill_pct);

    ftb_prefill(ops, ft, live, key_base);
    if (mode == FTB_MAINT_NOHIT_DENSE) {
        for (unsigned i = 0; i < live; i++) {
            void *entry = ops->entry_ptr(ft, i + 1u);
            struct flow_entry_meta *meta;

            if (entry == NULL) {
                fprintf(stderr, "maint setup entry_ptr failed for %s at %u\n",
                        ops->name, i);
                exit(1);
            }
            meta = ops->entry_meta(entry);
            flow_timestamp_store(meta, UINT64_C(150000), FLOW_TIMESTAMP_DEFAULT_SHIFT);
        }
    }
}

static unsigned
ftb_prepare_maint_hit_idx(const struct ftb_variant_ops *ops, void *ft,
                          unsigned entries, unsigned fill_pct, unsigned key_base,
                          void *keys, u32 *hit_idxv, u64 *used_out)
{
    unsigned live = ftb_fill_target(entries, fill_pct);
    unsigned nb_bk;
    unsigned *bucket_of_idx;
    unsigned *bucket_counts;
    unsigned *keeper_by_bk;
    unsigned hit_n = 0u;
    u64 used_sum = 0u;

    ftb_prefill(ops, ft, live, key_base);
    nb_bk = ops->nb_bk(ft);
    bucket_of_idx = ftb_xcalloc(live + 1u, sizeof(*bucket_of_idx));
    bucket_counts = ftb_xcalloc(nb_bk, sizeof(*bucket_counts));
    keeper_by_bk = ftb_xcalloc(nb_bk, sizeof(*keeper_by_bk));

    for (unsigned idx = 1u; idx <= live; idx++) {
        void *entry = ops->entry_ptr(ft, idx);
        struct flow_entry_meta *meta = ops->entry_meta(entry);
        unsigned bk = meta->cur_hash & (nb_bk - 1u);

        bucket_of_idx[idx] = bk;
        bucket_counts[bk]++;
    }

    for (unsigned idx = 1u; idx <= live && hit_n < ftb_query_n; idx++) {
        unsigned bk = bucket_of_idx[idx];

        if (bucket_counts[bk] < 2u || keeper_by_bk[bk] != 0u)
            continue;
        keeper_by_bk[bk] = idx;
        hit_idxv[hit_n] = idx;
        memcpy(FTB_KEY_AT(keys, ops, hit_n), ops->entry_ptr(ft, idx),
               ops->key_size);
        used_sum += bucket_counts[bk];
        hit_n++;
    }

    for (unsigned idx = 1u; idx <= live; idx++) {
        void *entry = ops->entry_ptr(ft, idx);
        struct flow_entry_meta *meta = ops->entry_meta(entry);
        unsigned bk = bucket_of_idx[idx];

        if (keeper_by_bk[bk] == 0u)
            continue;
        if (keeper_by_bk[bk] == idx)
            flow_timestamp_store(meta, UINT64_C(200000), FLOW_TIMESTAMP_DEFAULT_SHIFT);
        else
            flow_timestamp_store(meta, UINT64_C(16), FLOW_TIMESTAMP_DEFAULT_SHIFT);
    }

    free(keeper_by_bk);
    free(bucket_counts);
    free(bucket_of_idx);
    if (used_out != NULL)
        *used_out = used_sum;
    return hit_n;
}

static int
ftb_measure_maint_perf_once(const struct ftb_variant_ops *ops,
                            unsigned entries, unsigned fill_pct,
                            enum ftb_maint_mode mode,
                            const enum bench_scope_event_kind *kinds,
                            unsigned kind_count,
                            struct bench_scope_sample *out,
                            u64 *evicted_out,
                            u64 *units_out)
{
    unsigned max_entries = ftb_pool_count(entries);
    struct ft_table_config cfg = ftb_cfg();
    union ftb_any_table ft;
    void *pool = ftb_alloc_zero(max_entries, ops->record_size);
    u32 *expired_idxv =
        ftb_xcalloc(FTB_MAINT_MAX_EXPIRED, sizeof(*expired_idxv));
    void *keys = NULL;
    struct ft_table_result *results = NULL;
    u32 *hit_idxv = NULL;
    struct bench_scope_group group;
    u64 now = (mode == FTB_MAINT_EXPIRE_DENSE)
                 ? UINT64_C(200000) : UINT64_C(1000);
    unsigned start_bk = 0u;
    unsigned next_bk = 0u;
    unsigned prev_bk;
    unsigned evicted;
    u64 total_evicted = 0u;
    int rc;

    if (ops->init(&ft, pool, max_entries, &cfg) != 0) {
        fprintf(stderr, "maint bench init failed for %s\n", ops->name);
        exit(1);
    }
    if (mode == FTB_MAINT_HIT_IDX_EXPIRE
        || mode == FTB_MAINT_HIT_IDX_EXPIRE_FILTERED) {
        unsigned hit_n;
        u64 used_n = 0u;

        keys = ftb_xcalloc(ftb_query_n, ops->key_size);
        results = ftb_xcalloc(ftb_query_n, sizeof(*results));
        hit_idxv = ftb_xcalloc(ftb_query_n, sizeof(*hit_idxv));
        hit_n = ftb_prepare_maint_hit_idx(ops, &ft, max_entries, fill_pct,
                                          700000u, keys, hit_idxv, &used_n);
        if (hit_n == 0u) {
            fprintf(stderr, "maint_hit_idx setup failed for %s\n", ops->name);
            exit(1);
        }
        ops->find_bulk(&ft, keys, hit_n, FTB_NOW_FIND, results);
        for (unsigned i = 0u; i < hit_n; i++) {
            if (results[i].entry_idx == 0u) {
                fprintf(stderr, "maint_hit_idx find setup miss for %s\n",
                        ops->name);
                exit(1);
            }
            hit_idxv[i] = results[i].entry_idx;
        }
        *units_out = used_n;
    } else {
        ftb_prepare_maint_table(ops, &ft, max_entries, fill_pct, 700000u, mode);
        ftb_cold_touch();
        *units_out = ftb_fill_target(max_entries, fill_pct);
    }

    rc = bench_scope_open(&group, kinds, kind_count);
    if (rc != 0) {
        ops->destroy(&ft);
        free(hit_idxv);
        free(results);
        free(keys);
        free(expired_idxv);
        ftb_free_zero(pool, max_entries, ops->record_size);
        return -1;
    }
    if (bench_scope_begin(&group) != 0) {
        bench_scope_close(&group);
        ops->destroy(&ft);
        free(hit_idxv);
        free(results);
        free(keys);
        free(expired_idxv);
        ftb_free_zero(pool, max_entries, ops->record_size);
        return -1;
    }
    if (mode == FTB_MAINT_HIT_IDX_EXPIRE) {
        evicted = ops->maintain_idx_bulk(&ft, hit_idxv, (unsigned)*units_out,
                                         UINT64_C(200000), UINT64_C(100000),
                                         expired_idxv,
                                         FTB_MAINT_MAX_EXPIRED, 2u, 0);
        total_evicted = evicted;
    } else if (mode == FTB_MAINT_HIT_IDX_EXPIRE_FILTERED) {
        evicted = ops->maintain_idx_bulk(&ft, hit_idxv, (unsigned)*units_out,
                                         UINT64_C(200000), UINT64_C(100000),
                                         expired_idxv,
                                         FTB_MAINT_MAX_EXPIRED, 2u, 1);
        total_evicted = evicted;
    } else {
        for (;;) {
            prev_bk = start_bk;
            evicted = ops->maintain(&ft, start_bk, now, UINT64_C(100000),
                                    expired_idxv,
                                    FTB_MAINT_MAX_EXPIRED, 8u, &next_bk);
            total_evicted += evicted;
            start_bk = next_bk;
            if (evicted == 0u && start_bk == prev_bk)
                break;
        }
    }
    if (bench_scope_end(&group, out) != 0) {
        bench_scope_close(&group);
        ops->destroy(&ft);
        free(hit_idxv);
        free(results);
        free(keys);
        free(expired_idxv);
        ftb_free_zero(pool, max_entries, ops->record_size);
        return -1;
    }
    bench_scope_close(&group);
    ops->destroy(&ft);
    free(hit_idxv);
    free(results);
    free(keys);
    free(expired_idxv);
    ftb_free_zero(pool, max_entries, ops->record_size);
    *evicted_out = total_evicted;
    return 0;
}

static struct ftb_maint_metrics
ftb_measure_maint_metrics(const struct ftb_variant_ops *ops,
                          unsigned entries, unsigned fill_pct,
                          enum ftb_maint_mode mode)
{
    static const enum bench_scope_event_kind ipc_kinds[] = {
        BENCH_SCOPE_EVT_CYCLES,
        BENCH_SCOPE_EVT_INSTRUCTIONS
    };
    static const enum bench_scope_event_kind cache_kinds[] = {
        BENCH_SCOPE_EVT_CACHE_REFERENCES,
        BENCH_SCOPE_EVT_CACHE_MISSES
    };
    double cycles_samples[FTB_SAMPLE_MAX];
    double ipc_samples[FTB_SAMPLE_MAX];
    double hit_rate_samples[FTB_SAMPLE_MAX];
    struct ftb_maint_metrics out = { 0.0, 0.0, 0.0 };
    unsigned max_entries = ftb_pool_count(entries);

    for (unsigned r = 0; r < ftb_sample_raw_repeat; r++) {
        struct bench_scope_sample ipc_sample;
        struct bench_scope_sample cache_sample;
        u64 evicted_ipc = 0u, evicted_cache = 0u;
        u64 units_ipc = 0u, units_cache = 0u;
        u64 cycles, instructions, cache_refs, cache_misses;
        double denom;

        if (ftb_measure_maint_perf_once(ops, max_entries, fill_pct, mode,
                                        ipc_kinds, 2u, &ipc_sample,
                                        &evicted_ipc, &units_ipc) != 0) {
            fprintf(stderr, "perf open failed for maintain ipc metrics\n");
            exit(1);
        }
        if (ftb_measure_maint_perf_once(ops, max_entries, fill_pct, mode,
                                        cache_kinds, 2u, &cache_sample,
                                        &evicted_cache, &units_cache) != 0) {
            fprintf(stderr, "perf open failed for maintain cache metrics\n");
            exit(1);
        }
        (void)evicted_ipc;
        (void)evicted_cache;
        cycles = ftb_perf_value(&ipc_sample, BENCH_SCOPE_EVT_CYCLES);
        instructions = ftb_perf_value(&ipc_sample, BENCH_SCOPE_EVT_INSTRUCTIONS);
        cache_refs = ftb_perf_value(&cache_sample, BENCH_SCOPE_EVT_CACHE_REFERENCES);
        cache_misses = ftb_perf_value(&cache_sample, BENCH_SCOPE_EVT_CACHE_MISSES);
        denom = (double)units_ipc;
        cycles_samples[r] = (denom > 0.0) ? ((double)cycles / denom) : 0.0;
        ipc_samples[r] = (cycles != 0u)
                       ? ((double)instructions / (double)cycles) : 0.0;
        hit_rate_samples[r] = (cache_refs != 0u)
                            ? (100.0 * (double)(cache_refs - cache_misses)
                               / (double)cache_refs)
                            : 100.0;
    }
    out.cycles_per_entry = ftb_aggregate_double(
        cycles_samples, ftb_sample_raw_repeat, ftb_sample_keep_n);
    out.ipc = ftb_aggregate_double(
        ipc_samples, ftb_sample_raw_repeat, ftb_sample_keep_n);
    out.cache_hit_rate = ftb_aggregate_double(
        hit_rate_samples, ftb_sample_raw_repeat, ftb_sample_keep_n);
    return out;
}

static void
ftb_parse_entries_fill(int argc, char **argv, unsigned base_arg,
                       unsigned *entries, unsigned *fill_pct);

static void
ftb_run_maint(const struct ftb_variant_ops *ops,
              int argc, char **argv, unsigned base_arg)
{
    unsigned entries, fill_pct;
    struct ftb_maint_metrics expire_dense, nohit_dense;
    struct ftb_maint_metrics hit_idx_expire, hit_idx_filtered;

    ftb_parse_entries_fill(argc, argv, base_arg, &entries, &fill_pct);
    printf("\nftable maintain benchmark (%s)\n\n", ops->name);
    ftb_print_sampling_config();
    printf("  maintain metrics: cycles/entry  IPC  cache-hit-rate\n");

    expire_dense = ftb_measure_maint_metrics(
        ops, entries, fill_pct, FTB_MAINT_EXPIRE_DENSE);
    nohit_dense = ftb_measure_maint_metrics(
        ops, entries, fill_pct, FTB_MAINT_NOHIT_DENSE);
    hit_idx_expire = ftb_measure_maint_metrics(
        ops, entries, fill_pct, FTB_MAINT_HIT_IDX_EXPIRE);
    hit_idx_filtered = ftb_measure_maint_metrics(
        ops, entries, fill_pct, FTB_MAINT_HIT_IDX_EXPIRE_FILTERED);

    printf("[entries=%u fill=%u%% min_bk_entries=8 max_expired=%u expire_tsc=100000]\n",
           ftb_pool_count(entries), fill_pct, FTB_MAINT_MAX_EXPIRED);
    printf("  maint_expire_dense  %8.2f cy/entry  IPC %5.2f  cache-hit %6.2f%%\n",
           expire_dense.cycles_per_entry, expire_dense.ipc,
           expire_dense.cache_hit_rate);
    printf("  maint_nohit_dense   %8.2f cy/entry  IPC %5.2f  cache-hit %6.2f%%\n",
           nohit_dense.cycles_per_entry, nohit_dense.ipc,
           nohit_dense.cache_hit_rate);
    printf("  maint_idx_expire     %7.2f cy/entry  IPC %5.2f  cache-hit %6.2f%%\n",
           hit_idx_expire.cycles_per_entry, hit_idx_expire.ipc,
           hit_idx_expire.cache_hit_rate);
    printf("  maint_idx_filtered   %7.2f cy/entry  IPC %5.2f  cache-hit %6.2f%%\n",
           hit_idx_filtered.cycles_per_entry, hit_idx_filtered.ipc,
           hit_idx_filtered.cache_hit_rate);
}

static int
ftb_parse_u32_arg(const char *s, unsigned *out)
{
    return ftb_parse_u32_text(s, out, 0);
}

static void
ftb_parse_entries_fill(int argc, char **argv, unsigned base_arg,
                       unsigned *entries, unsigned *fill_pct)
{
    *entries = FTB_DEFAULT_ENTRIES;
    *fill_pct = FTB_FILL_PCT;
    if (argc > (int)base_arg && ftb_parse_u32_arg(argv[base_arg], entries) != 0) {
        fprintf(stderr, "invalid entries: %s\n", argv[base_arg]);
        exit(1);
    }
    if (argc > (int)(base_arg + 1u)
        && ftb_parse_u32_arg(argv[base_arg + 1u], fill_pct) != 0) {
        fprintf(stderr, "invalid fill_pct: %s\n", argv[base_arg + 1u]);
        exit(1);
    }
    if (*fill_pct == 0u || *fill_pct >= 100u) {
        fprintf(stderr, "fill_pct must be in 1..99\n");
        exit(1);
    }
}

static void
ftb_run_datapath(const struct ftb_variant_ops *ops,
                 int argc, char **argv, unsigned base_arg)
{
    unsigned desired, max_entries, fill_pct, live;
    struct ft_table_config cfg;
    union ftb_any_table ft;
    void *pool;


    ftb_parse_entries_fill(argc, argv, base_arg, &desired, &fill_pct);
    max_entries = ftb_pool_count(desired);
    live = ftb_fill_target(max_entries, fill_pct);
    if (live < ftb_query_n || live + ftb_query_n > max_entries) {
        fprintf(stderr, "live entries (%u) must be >= query (%u)\n",
                live, ftb_query_n);
        exit(1);
    }
    cfg = ftb_cfg();
    pool = ftb_alloc_zero(max_entries, ops->record_size);

    if (ops->init(&ft, pool, max_entries, &cfg) != 0) {
        fprintf(stderr, "datapath init failed for %s\n", ops->name);
        exit(1);
    }

    printf("\nftable cold bulk datapath (%s)\n\n", ops->name);
    ftb_print_sampling_config();
    printf("  cold datapath: reset/prefill/cold-touch per round\n");
    printf("[desired=%u entries=%u nb_bk=%u live=%u fill=%u%%]\n",
           desired, max_entries, ops->nb_bk(&ft), live, fill_pct);

#define FTB_RUN_OP(tag, expr) do {                              \
    if (!ftb_op_filter || strcmp(ftb_op_filter, tag) == 0) {    \
        double _v = (expr);                                     \
        printf("  %-12s %8.2f cy/key\n", tag, _v);             \
    }                                                           \
} while (0)

    FTB_RUN_OP("find_hit",    ftb_sample_find_hit(ops, &ft, live, max_entries + 100000u));
    FTB_RUN_OP("find_miss",   ftb_sample_find_miss(ops, &ft, live, max_entries + 150000u));
    FTB_RUN_OP("add_idx",     ftb_sample_add_idx(ops, &ft, live, max_entries + 200000u));
    FTB_RUN_OP("add_ignore",  ftb_sample_add_dup_policy(ops, &ft, live,
                               max_entries + 300000u, FT_ADD_IGNORE));
    FTB_RUN_OP("add_update",  ftb_sample_add_dup_policy(ops, &ft, live,
                               max_entries + 400000u, FT_ADD_UPDATE));
    FTB_RUN_OP("del_idx",     ftb_sample_del_idx(ops, &ft, live, max_entries + 500000u));
    FTB_RUN_OP("del_key",     ftb_sample_del_key(ops, &ft, live, max_entries + 600000u));
    FTB_RUN_OP("find_del_idx", ftb_sample_find_del_idx(ops, &ft, live,
                               max_entries + 700000u));
#undef FTB_RUN_OP

    ops->destroy(&ft);
    ftb_free_zero(pool, max_entries, ops->record_size);
}

static void
ftb_run_grow(const struct ftb_variant_ops *ops,
             int argc, char **argv, unsigned base_arg)
{
    unsigned entries, fill_pct;


    ftb_parse_entries_fill(argc, argv, base_arg, &entries, &fill_pct);
    if (ftb_fill_target(entries, fill_pct) < ftb_query_n) {
        fprintf(stderr, "live entries (%u) must be >= query (%u)\n",
                ftb_fill_target(entries, fill_pct), ftb_query_n);
        exit(1);
    }
    printf("\nftable grow benchmark (%s)\n\n", ops->name);
    ftb_print_sampling_config();
    struct ftb_grow_result grow = ftb_sample_grow(ops, entries, fill_pct);
    printf("[entries=%u fill=%u%%]\n", entries, fill_pct);
    printf("  grow_alloc   %8.2f cy/live-entry\n", grow.alloc_cpe);
    printf("  grow_migrate %8.2f cy/live-entry\n", grow.migrate_cpe);
    printf("  grow_total   %8.2f cy/live-entry\n", grow.total_cpe);
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
    if (enable == FT_ARCH_AUTO) return "auto";
    if (enable & FT_ARCH_AVX512) return "avx512";
    if (enable & FT_ARCH_AVX2) return "avx2";
    if (enable & FT_ARCH_SSE) return "sse";
    return "gen";
}

static const struct ftb_variant_ops *
ftb_parse_variant(const char *name)
{
    if (strcmp(name, "flow4") == 0)
        return &ftb_ops_flow4;
    if (strcmp(name, "flow6") == 0)
        return &ftb_ops_flow6;
    if (strcmp(name, "flowu") == 0)
        return &ftb_ops_flowu;
    return NULL;
}

int
main(int argc, char **argv)
{
    unsigned arch_enable = FT_ARCH_AUTO;
    const struct ftb_variant_ops *ops = &ftb_ops_flow4;
    int run_grow = 0;
    int run_maint = 0;
    int pos_argc;
    char **pos_argv;
    int rc;

    rc = ftb_parse_options(argc, argv, &arch_enable, &run_grow, &run_maint);
    if (rc == 1)
        return 0;
    if (rc != 0)
        return rc;

    pos_argc = argc - optind;
    pos_argv = argv + optind;
    if (pos_argc < 1) {
        ftb_print_usage(stderr, argv[0]);
        return 1;
    }

    if (strcmp(pos_argv[0], "flow4") == 0
        || strcmp(pos_argv[0], "flow6") == 0
        || strcmp(pos_argv[0], "flowu") == 0) {
        ops = ftb_parse_variant(pos_argv[0]);
        if (ops == NULL) {
            fprintf(stderr, "unknown variant: %s (valid: flow4 flow6 flowu)\n",
                    pos_argv[0]);
            ftb_print_usage(stderr, argv[0]);
            return 1;
        }
        pos_argc--;
        pos_argv++;
    }

    rc = ftb_apply_pin_core();
    if (rc != 0)
        return rc;
    ft_arch_init(arch_enable);
    printf("[arch: %s%s]\n\n", ftb_arch_label(arch_enable),
           ftb_use_hugepage ? ", hugepage" : "");

    if (run_grow) {
        ftb_run_grow(ops, pos_argc, pos_argv, 0u);
        return 0;
    }
    if (run_maint) {
        ftb_run_maint(ops, pos_argc, pos_argv, 0u);
        return 0;
    }
    ftb_run_datapath(ops, pos_argc, pos_argv, 0u);
    return 0;
}
