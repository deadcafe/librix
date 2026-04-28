/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_flow_extra_table.c - slot-extra full-family benchmark.
 */

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "flow_table.h"

#define DEFAULT_ENTRIES (1u << 20)
#define DEFAULT_FILL    60u
#define DEFAULT_QUERY   256u
#define DEFAULT_REPS    7u
#define TS_SHIFT        4u

static RIX_FORCE_INLINE u64
tsc_start(void)
{
    u32 lo, hi;

    __asm__ volatile ("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static RIX_FORCE_INLINE u64
tsc_end(void)
{
    u32 lo, hi;

    __asm__ volatile ("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((u64)hi << 32) | lo;
}

static void *
hugealloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    assert(p != MAP_FAILED);
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

static unsigned
parse_u32(const char *s, unsigned fallback)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL)
        return fallback;
    v = strtoul(s, &end, 10);
    if (end == NULL || *end != '\0' || v == 0ul || v > UINT_MAX)
        return fallback;
    return (unsigned)v;
}

static unsigned
parse_arch(const char *s)
{
    if (s == NULL || strcmp(s, "auto") == 0)
        return FT_ARCH_AUTO;
    if (strcmp(s, "gen") == 0)
        return FT_ARCH_GEN;
    if (strcmp(s, "sse") == 0)
        return FT_ARCH_SSE;
    if (strcmp(s, "avx2") == 0)
        return FT_ARCH_AVX2;
    if (strcmp(s, "avx512") == 0)
        return FT_ARCH_AVX512;
    fprintf(stderr, "unknown arch: %s\n", s);
    exit(2);
}

static struct flow4_extra_key
key4(unsigned i, u8 proto)
{
    struct flow4_extra_key k = { 0 };

    k.family = 4u;
    k.proto = proto;
    k.src_addr = UINT32_C(0x0a000000) | i;
    k.dst_addr = UINT32_C(0x0b000000) | (i + 1u);
    k.src_port = (u16)(i * 7u);
    k.dst_port = (u16)(i * 11u);
    return k;
}

static struct flow6_extra_key
key6(unsigned i, u8 proto)
{
    struct flow6_extra_key k = { 0 };
    u32 w;

    k.family = 10u;
    k.proto = proto;
    k.src_port = (u16)(1000u + i);
    k.dst_port = (u16)(2000u + i);
    k.vrfid = 1u;
    w = i;
    memcpy(&k.src_ip[0], &w, sizeof(w));
    w = i ^ UINT32_C(0x13579bdf);
    memcpy(&k.src_ip[4], &w, sizeof(w));
    w = i * UINT32_C(0x9e3779b1);
    memcpy(&k.src_ip[8], &w, sizeof(w));
    w = i + UINT32_C(0x01020304);
    memcpy(&k.src_ip[12], &w, sizeof(w));
    w = ~i;
    memcpy(&k.dst_ip[0], &w, sizeof(w));
    w = i ^ UINT32_C(0x2468ace0);
    memcpy(&k.dst_ip[4], &w, sizeof(w));
    w = (i << 1) ^ UINT32_C(0xa5a5a5a5);
    memcpy(&k.dst_ip[8], &w, sizeof(w));
    w = i ^ UINT32_C(0x5a5a5a5a);
    memcpy(&k.dst_ip[12], &w, sizeof(w));
    return k;
}

static struct flowu_extra_key
keyu(unsigned i, u8 proto)
{
    struct flowu_extra_key k = { 0 };
    u32 w;

    k.proto = proto;
    k.src_port = (u16)(1000u + i);
    k.dst_port = (u16)(2000u + i);
    k.vrfid = 1u;
    if ((i & 1u) == 0u) {
        k.family = 2u;
        k.addr.v4.src = UINT32_C(0x0a000001) ^ i;
        k.addr.v4.dst = UINT32_C(0x0a000101) ^ (i * UINT32_C(0x9e3779b1));
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

#define DEFINE_EXTRA_BENCH(prefix, make_key)                                  \
static void                                                                   \
fill_##prefix(struct prefix##_extra_entry *pool,                              \
              struct prefix##_extra_key *hit_keys,                            \
              struct prefix##_extra_key *miss_keys, unsigned n)               \
{                                                                             \
    for (unsigned i = 0u; i < n; i++) {                                       \
        pool[i].key = make_key(i, 6u);                                        \
        hit_keys[i] = pool[i].key;                                            \
        miss_keys[i] = make_key(i + n + 17u, 17u);                            \
    }                                                                         \
}                                                                             \
                                                                              \
static void                                                                   \
age_##prefix(struct ft_table_extra *ft,                                       \
             struct prefix##_extra_entry *pool, unsigned live,                \
             unsigned stale_n, u64 now, u64 old)                              \
{                                                                             \
    u32 enc_old = flow_extra_timestamp_encode(old, TS_SHIFT);                 \
    u32 enc_now = flow_extra_timestamp_encode(now, TS_SHIFT);                 \
    unsigned mask = ft->ht_head.rhh_mask;                                     \
                                                                              \
    for (unsigned i = 0u; i < live; i++) {                                    \
        unsigned bk_idx = pool[i].meta.cur_hash & mask;                       \
        unsigned slot = (unsigned)pool[i].meta.slot;                          \
                                                                              \
        flow_extra_ts_set(&ft->buckets[bk_idx], slot,                         \
                          i < stale_n ? enc_old : enc_now);                   \
    }                                                                         \
}                                                                             \
                                                                              \
static void                                                                   \
bench_##prefix(unsigned entries, unsigned fill_pct, unsigned query,           \
               unsigned reps, int run_maint, int run_grow)                   \
{                                                                             \
    unsigned live = (unsigned)(((u64)entries * fill_pct) / 100u);             \
    size_t pool_sz = (size_t)entries * sizeof(struct prefix##_extra_entry);   \
    size_t bk_sz = prefix##_extra_table_bucket_size(entries);                 \
    struct prefix##_extra_entry *pool = hugealloc(pool_sz);                  \
    struct prefix##_extra_key *hit_keys = hugealloc(                          \
        (size_t)entries * sizeof(*hit_keys));                                 \
    struct prefix##_extra_key *miss_keys = hugealloc(                         \
        (size_t)entries * sizeof(*miss_keys));                                \
    struct ft_table_extra_config cfg = { .ts_shift = TS_SHIFT };             \
    u32 *idxv = hugealloc((size_t)entries * sizeof(*idxv));                  \
    u32 *unused = hugealloc((size_t)entries * sizeof(*unused));              \
    u32 *expired = hugealloc((size_t)entries * sizeof(*expired));            \
    u32 *results = hugealloc((size_t)query * sizeof(*results));              \
    u64 add_cy = 0u, find_cy = 0u, miss_cy = 0u, del_cy = 0u;                \
    u64 maint_cy = 0u, grow_alloc_cy = 0u, grow_mig_cy = 0u;                 \
    unsigned maint_units = 0u;                                                \
    unsigned grow_units = 0u;                                                 \
                                                                              \
    fill_##prefix(pool, hit_keys, miss_keys, entries);                        \
    for (unsigned r = 0u; r < reps; r++) {                                    \
        struct ft_table_extra ft;                                             \
        size_t cur_bk_sz = bk_sz;                                             \
        void *bk = hugealloc(cur_bk_sz);                                      \
        int rc;                                                               \
        u64 t0, t1;                                                           \
                                                                              \
        rc = prefix##_extra_table_init(&ft, pool, entries, bk,                \
                                       cur_bk_sz, &cfg);                      \
        if (rc != 0)                                                          \
            abort();                                                          \
        for (unsigned i = 0u; i < entries; i++)                               \
            idxv[i] = i + 1u;                                                 \
        for (unsigned i = 0u; i < live; i += query) {                         \
            unsigned q = live - i;                                            \
            if (q > query)                                                    \
                q = query;                                                    \
            (void)ft_table_extra_add_idx_bulk(&ft, &idxv[i], q,               \
                                              FT_ADD_IGNORE, 1000u, unused);  \
        }                                                                     \
                                                                              \
        if (run_maint) {                                                      \
            struct ft_maint_extra_ctx ctx;                                    \
            unsigned next = 0u;                                               \
                                                                              \
            rc = ft_table_extra_maint_ctx_init(&ft, &ctx);                    \
            if (rc != 0)                                                      \
                abort();                                                      \
            age_##prefix(&ft, pool, live, live / 2u,                          \
                         1000u + (1u << 20), 0u);                             \
            t0 = tsc_start();                                                 \
            (void)ft_table_extra_maintain(&ctx, 0u, 1000u + (1u << 20),       \
                                          1u << 18, expired, entries,         \
                                          0u, &next);                         \
            t1 = tsc_end();                                                   \
            maint_cy += t1 - t0;                                              \
            maint_units += ft_table_extra_nb_bk(&ft);                         \
        } else if (run_grow) {                                                \
            size_t new_sz = ft_table_extra_bucket_mem_size(                   \
                ft_table_extra_nb_bk(&ft) * 2u);                              \
            void *new_bk;                                                     \
                                                                              \
            t0 = tsc_start();                                                 \
            new_bk = hugealloc(new_sz);                                       \
            t1 = tsc_end();                                                   \
            grow_alloc_cy += t1 - t0;                                         \
            t0 = tsc_start();                                                 \
            rc = ft_table_extra_migrate(&ft, new_bk, new_sz);                 \
            if (rc != 0)                                                      \
                abort();                                                      \
            t1 = tsc_end();                                                   \
            grow_mig_cy += t1 - t0;                                           \
            grow_units += live;                                               \
            munmap(bk, cur_bk_sz);                                            \
            bk = new_bk;                                                      \
            cur_bk_sz = new_sz;                                               \
        } else {                                                              \
            t0 = tsc_start();                                                 \
            for (unsigned i = live; i < entries; i += query) {                \
                unsigned q = entries - i;                                     \
                if (q > query)                                                \
                    q = query;                                                \
                (void)ft_table_extra_add_idx_bulk(&ft, &idxv[i], q,           \
                                                  FT_ADD_IGNORE, 2000u,       \
                                                  unused);                    \
            }                                                                 \
            t1 = tsc_end();                                                   \
            add_cy += t1 - t0;                                                \
            t0 = tsc_start();                                                 \
            for (unsigned i = 0u; i < live; i += query) {                     \
                unsigned q = live - i;                                        \
                if (q > query)                                                \
                    q = query;                                                \
                (void)prefix##_extra_table_find_bulk(&ft, &hit_keys[i],       \
                                                     q, 3000u, results);      \
            }                                                                 \
            t1 = tsc_end();                                                   \
            find_cy += t1 - t0;                                               \
            t0 = tsc_start();                                                 \
            for (unsigned i = 0u; i < live; i += query) {                     \
                unsigned q = live - i;                                        \
                if (q > query)                                                \
                    q = query;                                                \
                (void)prefix##_extra_table_find_bulk(&ft, &miss_keys[i],      \
                                                     q, 0u, results);         \
            }                                                                 \
            t1 = tsc_end();                                                   \
            miss_cy += t1 - t0;                                               \
            t0 = tsc_start();                                                 \
            for (unsigned i = 0u; i < live; i += query) {                     \
                unsigned q = live - i;                                        \
                if (q > query)                                                \
                    q = query;                                                \
                (void)prefix##_extra_table_del_bulk(&ft, &hit_keys[i],        \
                                                    q, unused);               \
            }                                                                 \
            t1 = tsc_end();                                                   \
            del_cy += t1 - t0;                                                \
        }                                                                     \
        ft_table_extra_destroy(&ft);                                          \
        munmap(bk, cur_bk_sz);                                                \
    }                                                                         \
    printf("\nextra full bench (%s_extra, entries=%u, fill=%u%%, q=%u, reps=%u)\n", \
           #prefix, entries, fill_pct, query, reps);                          \
    if (run_maint) {                                                          \
        printf("  maintain_50pct %8.2f cy/bucket\n",                         \
               (double)maint_cy / (double)maint_units);                       \
    } else if (run_grow) {                                                    \
        printf("  grow_alloc     %8.2f cy/live-entry\n",                     \
               (double)grow_alloc_cy / (double)grow_units);                  \
        printf("  grow_migrate   %8.2f cy/live-entry\n",                     \
               (double)grow_mig_cy / (double)grow_units);                    \
        printf("  grow_total     %8.2f cy/live-entry\n",                     \
               (double)(grow_alloc_cy + grow_mig_cy) / (double)grow_units);  \
    } else {                                                                  \
        double add_units = (double)(entries - live) * (double)reps;           \
        double live_units = (double)live * (double)reps;                      \
        printf("  add_idx        %8.2f cy/key\n", (double)add_cy / add_units);\
        printf("  find_hit_touch %8.2f cy/key\n",                            \
               (double)find_cy / live_units);                                \
        printf("  find_miss      %8.2f cy/key\n",                            \
               (double)miss_cy / live_units);                                \
        printf("  del_key        %8.2f cy/key\n", (double)del_cy / live_units);\
    }                                                                         \
    munmap(pool, pool_sz);                                                    \
    munmap(hit_keys, (size_t)entries * sizeof(*hit_keys));                    \
    munmap(miss_keys, (size_t)entries * sizeof(*miss_keys));                  \
    munmap(idxv, (size_t)entries * sizeof(*idxv));                            \
    munmap(unused, (size_t)entries * sizeof(*unused));                        \
    munmap(expired, (size_t)entries * sizeof(*expired));                      \
    munmap(results, (size_t)query * sizeof(*results));                        \
}

DEFINE_EXTRA_BENCH(flow4, key4)
DEFINE_EXTRA_BENCH(flow6, key6)
DEFINE_EXTRA_BENCH(flowu, keyu)

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--arch ARCH] [--query N] [--reps N] [--maint|--grow] "
            "[flow4_extra|flow6_extra|flowu_extra|all] [entries] [fill_pct]\n",
            prog);
}

int
main(int argc, char **argv)
{
    const char *family = "all";
    unsigned entries = DEFAULT_ENTRIES;
    unsigned fill = DEFAULT_FILL;
    unsigned query = DEFAULT_QUERY;
    unsigned reps = DEFAULT_REPS;
    unsigned arch = FT_ARCH_AUTO;
    int run_maint = 0;
    int run_grow = 0;
    int pos = 1;

    while (pos < argc && strncmp(argv[pos], "--", 2) == 0) {
        if (strcmp(argv[pos], "--maint") == 0) {
            run_maint = 1;
            pos++;
        } else if (strcmp(argv[pos], "--grow") == 0) {
            run_grow = 1;
            pos++;
        } else if (strcmp(argv[pos], "--arch") == 0 && pos + 1 < argc) {
            arch = parse_arch(argv[pos + 1]);
            pos += 2;
        } else if (strcmp(argv[pos], "--query") == 0 && pos + 1 < argc) {
            query = parse_u32(argv[pos + 1], query);
            pos += 2;
        } else if (strcmp(argv[pos], "--reps") == 0 && pos + 1 < argc) {
            reps = parse_u32(argv[pos + 1], reps);
            pos += 2;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (run_maint && run_grow) {
        fprintf(stderr, "--maint and --grow are mutually exclusive\n");
        return 2;
    }
    if (pos < argc)
        family = argv[pos++];
    if (pos < argc)
        entries = parse_u32(argv[pos++], entries);
    if (pos < argc)
        fill = parse_u32(argv[pos++], fill);
    if (pos != argc || fill > 95u || query == 0u || query > entries) {
        usage(argv[0]);
        return 2;
    }

    ft_arch_init(arch);
    ft_arch_extra_init(arch);

    if (strcmp(family, "all") == 0 || strcmp(family, "flow4_extra") == 0)
        bench_flow4(entries, fill, query, reps, run_maint, run_grow);
    if (strcmp(family, "all") == 0 || strcmp(family, "flow6_extra") == 0)
        bench_flow6(entries, fill, query, reps, run_maint, run_grow);
    if (strcmp(family, "all") == 0 || strcmp(family, "flowu_extra") == 0)
        bench_flowu(entries, fill, query, reps, run_maint, run_grow);
    if (strcmp(family, "all") != 0
        && strcmp(family, "flow4_extra") != 0
        && strcmp(family, "flow6_extra") != 0
        && strcmp(family, "flowu_extra") != 0) {
        usage(argv[0]);
        return 2;
    }
    return 0;
}
