/* bench_pure_vs_mrsw.c
 *  Compare pure cuckoo hash variants against their MRSW counterparts and,
 *  for all multi-reader families, MRMW counterparts
 *  under identical single-threaded workloads.  This isolates the per-bucket
 *  ctrl protocol and MRMW writer-lock overhead rather than contention.
 *
 *  Usage:
 *    ./bench_pure_vs_mrsw [table_n [repeat [rand_keys]]]
 *      table_n   : number of resident entries (default: 1,048,576)
 *      repeat    : iterations per pattern    (default: 200)
 *      rand_keys : 0=sequential, 1=random    (default: 1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/mman.h>

#include "rix/rix_hash.h"
#include "rix/rix_hash_mr.h"
#include "rix/rix_hash_slot_extra.h"

#define BENCH_N 256u
#define U32_INVALID_KEY UINT32_MAX
#define U64_INVALID_KEY UINT64_MAX

struct mykey {
    u64 hi;
};

static RIX_FORCE_INLINE int
mykey_cmp(const struct mykey *a, const struct mykey *b)
{
    return (a->hi != b->hi) ? 1 : 0;
}

struct n_pure_fp {
    u32 cur_hash;
    u32 pad;
    struct mykey key;
};
RIX_HASH_HEAD(ht_pure_fp);
RIX_HASH_GENERATE(ht_pure_fp, n_pure_fp, key, cur_hash, mykey_cmp)

struct n_pure_slot {
    u32 cur_hash;
    u16 slot;
    u16 pad;
    struct mykey key;
};
RIX_HASH_HEAD(ht_pure_slot);
RIX_HASH_GENERATE_SLOT(ht_pure_slot, n_pure_slot, key, cur_hash, slot,
                       mykey_cmp)

struct n_pure_keyonly {
    struct mykey key;
    u32          value;
};
RIX_HASH_HEAD(ht_pure_keyonly);
RIX_HASH_GENERATE_KEYONLY(ht_pure_keyonly, n_pure_keyonly, key, mykey_cmp)

struct n_pure_extra {
    u32 cur_hash;
    u16 slot;
    u16 pad;
    struct mykey key;
};
RIX_HASH_HEAD(ht_pure_extra);
RIX_HASH_GENERATE_SLOT_EXTRA(ht_pure_extra, n_pure_extra, key, cur_hash, slot,
                             mykey_cmp)

struct n_pure_u32 {
    u32 key;
    u32 value;
};
RIX_HASH_U32_HEAD(ht_pure_u32);
RIX_HASH_U32_GENERATE(ht_pure_u32, struct n_pure_u32, key, U32_INVALID_KEY)

struct n_pure_u64 {
    u64 key;
    u64 value;
};
RIX_HASH_U64_HEAD(ht_pure_u64);
RIX_HASH_U64_GENERATE(ht_pure_u64, struct n_pure_u64, key, U64_INVALID_KEY)

struct n_mrsw_fp {
    u32 cur_hash;
    u32 pad;
    struct mykey key;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_fp);
RIX_HASH_MRSW_GENERATE(ht_mrsw_fp, n_mrsw_fp, key, cur_hash, mykey_cmp)

struct n_mrsw_slot {
    u32 cur_hash;
    u16 slot;
    u16 pad;
    struct mykey key;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_slot);
RIX_HASH_MRSW_GENERATE_SLOT(ht_mrsw_slot, n_mrsw_slot, key, cur_hash, slot,
                            mykey_cmp)

struct n_mrsw_keyonly {
    struct mykey key;
    u32          value;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_keyonly);
RIX_HASH_MRSW_GENERATE_KEYONLY(ht_mrsw_keyonly, n_mrsw_keyonly, key,
                               mykey_cmp)

struct n_mrmw_fp {
    u32 cur_hash;
    u32 pad;
    struct mykey key;
};
RIX_HASH_MRMW_HEAD(ht_mrmw_fp);
RIX_HASH_MRMW_GENERATE(ht_mrmw_fp, n_mrmw_fp, key, cur_hash, mykey_cmp)

struct n_mrmw_slot {
    u32 cur_hash;
    u16 slot;
    u16 pad;
    struct mykey key;
};
RIX_HASH_MRMW_HEAD(ht_mrmw_slot);
RIX_HASH_MRMW_GENERATE_SLOT(ht_mrmw_slot, n_mrmw_slot, key, cur_hash, slot,
                            mykey_cmp)

struct n_mrmw_keyonly {
    struct mykey key;
    u32          value;
};
RIX_HASH_MRMW_HEAD(ht_mrmw_keyonly);
RIX_HASH_MRMW_GENERATE_KEYONLY(ht_mrmw_keyonly, n_mrmw_keyonly, key,
                               mykey_cmp)

struct n_mrsw_extra {
    u32 cur_hash;
    u16 slot;
    u16 pad;
    struct mykey key;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_extra);
RIX_HASH_MRSW_GENERATE_SLOT_EXTRA(ht_mrsw_extra, n_mrsw_extra, key, cur_hash,
                                  slot, mykey_cmp)

struct n_mrmw_extra {
    u32 cur_hash;
    u16 slot;
    u16 pad;
    struct mykey key;
};
RIX_HASH_MRMW_HEAD(ht_mrmw_extra);
RIX_HASH_MRMW_GENERATE_SLOT_EXTRA(ht_mrmw_extra, n_mrmw_extra, key, cur_hash,
                                  slot, mykey_cmp)

struct n_mrsw_u32 {
    u32 key;
    u32 value;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_u32);
RIX_HASH_MRSW_GENERATE_U32(ht_mrsw_u32, struct n_mrsw_u32, key)

struct n_mrmw_u32 {
    u32 key;
    u32 value;
};
RIX_HASH_MRMW_HEAD(ht_mrmw_u32);
RIX_HASH_MRMW_GENERATE_U32(ht_mrmw_u32, struct n_mrmw_u32, key)

struct n_mrsw_u64 {
    u64 key;
    u64 value;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_u64);
RIX_HASH_MRSW_GENERATE_U64(ht_mrsw_u64, struct n_mrsw_u64, key)

struct n_mrmw_u64 {
    u64 key;
    u64 value;
};
RIX_HASH_MRMW_HEAD(ht_mrmw_u64);
RIX_HASH_MRMW_GENERATE_U64(ht_mrmw_u64, struct n_mrmw_u64, key)

static u64  g_table_n;
static u64  g_repeat;
static int  g_rand_keys;
static u64 *g_probe_idx;
static u64 *g_probe_miss_idx;
static volatile uintptr_t g_sink;
static u64 xr64 = 0xDEADBEEF1234ABCDULL;

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

static RIX_FORCE_INLINE u64
xorshift64(void)
{
    xr64 ^= xr64 >> 12;
    xr64 ^= xr64 << 25;
    xr64 ^= xr64 >> 27;
    return xr64 * UINT64_C(0x2545F4914F6CDD1D);
}

static void *
xmmap(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    madvise(p, bytes, MADV_HUGEPAGE);
    return p;
}

static int
cmp_u64(const void *a, const void *b)
{
    u64 av = *(const u64 *)a;
    u64 bv = *(const u64 *)b;
    return (av > bv) - (av < bv);
}

static double
median_cy(u64 *samples, unsigned n)
{
    qsort(samples, n, sizeof(samples[0]), cmp_u64);
    if ((n & 1u) != 0u)
        return (double)samples[n / 2u];
    return ((double)samples[n / 2u - 1u] + (double)samples[n / 2u]) / 2.0;
}

static void
report(const char *label, u64 *samples, unsigned n, unsigned ops)
{
    double med = median_cy(samples, n);
    printf("  %-30s median=%8.0f cy/batch  per_op=%6.2f cy\n",
           label, med, med / (double)ops);
}

static void
print_fill(const char *label, unsigned nb_bk, unsigned slots, size_t bk_sz)
{
    double fill = 100.0 * (double)g_table_n / ((double)nb_bk * (double)slots);
    printf("\n# %-14s nb_bk=%u slots/bk=%u bucket=%zuB fill=%.2f%%\n",
           label, nb_bk, slots, bk_sz, fill);
}

static u64 *
build_probe_idx(unsigned salt)
{
    u64 *p = xmmap((size_t)g_repeat * BENCH_N * sizeof(u64));

    for (u64 i = 0u; i < g_repeat * BENCH_N; i++) {
        if (g_rand_keys)
            p[i] = (xorshift64() + salt) % g_table_n;
        else
            p[i] = (i + salt) % g_table_n;
    }
    return p;
}

#define DEFINE_PTR_BENCH(label_str, prefix, node_t, ctx_t, bucket_t,          \
                         init_call, insert_call, remove_call, slots_per_bk)   \
static void                                                                   \
bench_##prefix(void)                                                          \
{                                                                             \
    unsigned nb_bk = rix_hash_nb_bk_hint((unsigned)g_table_n);                \
    size_t bk_mem = (size_t)nb_bk * sizeof(bucket_t);                         \
    size_t nd_mem = (size_t)g_table_n * sizeof(node_t);                       \
    bucket_t *bk = xmmap(bk_mem);                                             \
    node_t *nodes = xmmap(nd_mem);                                            \
    struct prefix head;                                                       \
    u64 *samples = malloc((size_t)g_repeat * sizeof(u64));                    \
                                                                              \
    print_fill(label_str, nb_bk, slots_per_bk, sizeof(bucket_t));             \
    init_call;                                                                \
    for (u64 i = 0u; i < g_table_n; i++) {                                    \
        nodes[i].key.hi = i + 1u;                                             \
        insert_call;                                                          \
    }                                                                         \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            struct mykey key = { g_probe_idx[r * BENCH_N + k] + 1u };         \
            node_t *res = prefix##_find(&head, bk, nodes, &key);              \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x256", samples, (unsigned)g_repeat, BENCH_N);      \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            struct mykey key = { g_table_n + g_probe_miss_idx[r * BENCH_N + k] + 1u }; \
            node_t *res = prefix##_find(&head, bk, nodes, &key);              \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x256", samples, (unsigned)g_repeat, BENCH_N);     \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            struct mykey keys[4];                                             \
            const struct mykey *kp[4];                                        \
            for (unsigned j = 0u; j < 4u; j++) {                              \
                keys[j].hi = g_probe_idx[r * BENCH_N + k + j] + 1u;           \
                kp[j] = &keys[j];                                             \
            }                                                                 \
            prefix##_hash_key_n(ctx, 4u, &head, bk, kp);                      \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            struct mykey keys[4];                                             \
            const struct mykey *kp[4];                                        \
            for (unsigned j = 0u; j < 4u; j++) {                              \
                keys[j].hi = g_table_n + g_probe_miss_idx[r * BENCH_N + k + j] + 1u; \
                kp[j] = &keys[j];                                             \
            }                                                                 \
            prefix##_hash_key_n(ctx, 4u, &head, bk, kp);                      \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 idx = g_probe_idx[r * BENCH_N] % g_table_n;                       \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            unsigned i = (unsigned)((idx + k) % g_table_n);                   \
            remove_call;                                                      \
            insert_call;                                                      \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " rm+ins x256", samples, (unsigned)g_repeat, BENCH_N);   \
                                                                              \
    free(samples);                                                            \
    munmap(nodes, nd_mem);                                                    \
    munmap(bk, bk_mem);                                                       \
}

#define DEFINE_MRSW_PTR_BENCH(label_str, prefix, node_t, ctx_t, bucket_t,     \
                              init_call, insert_call, remove_call, slots_per_bk) \
static void                                                                   \
bench_##prefix(void)                                                          \
{                                                                             \
    unsigned nb_bk = rix_hash_mrsw_nb_bk_hint((unsigned)g_table_n);           \
    size_t bk_mem = (size_t)nb_bk * sizeof(bucket_t);                         \
    size_t nd_mem = (size_t)g_table_n * sizeof(node_t);                       \
    bucket_t *bk = xmmap(bk_mem);                                             \
    node_t *nodes = xmmap(nd_mem);                                            \
    struct prefix head;                                                       \
    u64 *samples = malloc((size_t)g_repeat * sizeof(u64));                    \
                                                                              \
    print_fill(label_str, nb_bk, slots_per_bk, sizeof(bucket_t));             \
    init_call;                                                                \
    for (u64 i = 0u; i < g_table_n; i++) {                                    \
        nodes[i].key.hi = i + 1u;                                             \
        insert_call;                                                          \
    }                                                                         \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            struct mykey key = { g_probe_idx[r * BENCH_N + k] + 1u };         \
            node_t *res = prefix##_find(&head, bk, nodes, &key);              \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x256", samples, (unsigned)g_repeat, BENCH_N);      \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            struct mykey key = { g_table_n + g_probe_miss_idx[r * BENCH_N + k] + 1u }; \
            node_t *res = prefix##_find(&head, bk, nodes, &key);              \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x256", samples, (unsigned)g_repeat, BENCH_N);     \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            struct mykey keys[4];                                             \
            const struct mykey *kp[4];                                        \
            for (unsigned j = 0u; j < 4u; j++) {                              \
                keys[j].hi = g_probe_idx[r * BENCH_N + k + j] + 1u;           \
                kp[j] = &keys[j];                                             \
            }                                                                 \
            prefix##_hash_key_n(ctx, 4u, &head, bk, kp);                      \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            struct mykey keys[4];                                             \
            const struct mykey *kp[4];                                        \
            for (unsigned j = 0u; j < 4u; j++) {                              \
                keys[j].hi = g_table_n + g_probe_miss_idx[r * BENCH_N + k + j] + 1u; \
                kp[j] = &keys[j];                                             \
            }                                                                 \
            prefix##_hash_key_n(ctx, 4u, &head, bk, kp);                      \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 idx = g_probe_idx[r * BENCH_N] % g_table_n;                       \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            unsigned i = (unsigned)((idx + k) % g_table_n);                   \
            remove_call;                                                      \
            insert_call;                                                      \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " rm+ins x256", samples, (unsigned)g_repeat, BENCH_N);   \
                                                                              \
    free(samples);                                                            \
    munmap(nodes, nd_mem);                                                    \
    munmap(bk, bk_mem);                                                       \
}

#define DEFINE_U32_BENCH(label_str, prefix, node_t, ctx_t, bucket_t,          \
                         init_call, insert_call, remove_call, nb_bk_hint, slots_per_bk) \
static void                                                                   \
bench_##prefix(void)                                                          \
{                                                                             \
    unsigned nb_bk = nb_bk_hint((unsigned)g_table_n);                         \
    size_t bk_mem = (size_t)nb_bk * sizeof(bucket_t);                         \
    size_t nd_mem = (size_t)g_table_n * sizeof(node_t);                       \
    bucket_t *bk = xmmap(bk_mem);                                             \
    node_t *nodes = xmmap(nd_mem);                                            \
    struct prefix head;                                                       \
    u64 *samples = malloc((size_t)g_repeat * sizeof(u64));                    \
                                                                              \
    print_fill(label_str, nb_bk, slots_per_bk, sizeof(bucket_t));             \
    init_call;                                                                \
    for (u64 i = 0u; i < g_table_n; i++) {                                    \
        nodes[i].key = (u32)(i + 1u);                                         \
        insert_call;                                                          \
    }                                                                         \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            u32 key = (u32)(g_probe_idx[r * BENCH_N + k] + 1u);               \
            node_t *res = prefix##_find(&head, bk, nodes, key);               \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x256", samples, (unsigned)g_repeat, BENCH_N);      \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            u32 key = (u32)(g_table_n + g_probe_miss_idx[r * BENCH_N + k] + 1u); \
            node_t *res = prefix##_find(&head, bk, nodes, key);               \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x256", samples, (unsigned)g_repeat, BENCH_N);     \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            u32 keys[4];                                                      \
            for (unsigned j = 0u; j < 4u; j++)                                \
                keys[j] = (u32)(g_probe_idx[r * BENCH_N + k + j] + 1u);       \
            prefix##_hash_key_n(ctx, 4u, &head, bk, keys);                    \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            u32 keys[4];                                                      \
            for (unsigned j = 0u; j < 4u; j++)                                \
                keys[j] = (u32)(g_table_n + g_probe_miss_idx[r * BENCH_N + k + j] + 1u); \
            prefix##_hash_key_n(ctx, 4u, &head, bk, keys);                    \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 idx = g_probe_idx[r * BENCH_N] % g_table_n;                       \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            unsigned i = (unsigned)((idx + k) % g_table_n);                   \
            remove_call;                                                      \
            insert_call;                                                      \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " rm+ins x256", samples, (unsigned)g_repeat, BENCH_N);   \
                                                                              \
    free(samples);                                                            \
    munmap(nodes, nd_mem);                                                    \
    munmap(bk, bk_mem);                                                       \
}

#define DEFINE_U64_BENCH(label_str, prefix, node_t, ctx_t, bucket_t,          \
                         init_call, insert_call, remove_call, nb_bk_hint, slots_per_bk) \
static void                                                                   \
bench_##prefix(void)                                                          \
{                                                                             \
    unsigned nb_bk = nb_bk_hint((unsigned)g_table_n);                         \
    size_t bk_mem = (size_t)nb_bk * sizeof(bucket_t);                         \
    size_t nd_mem = (size_t)g_table_n * sizeof(node_t);                       \
    bucket_t *bk = xmmap(bk_mem);                                             \
    node_t *nodes = xmmap(nd_mem);                                            \
    struct prefix head;                                                       \
    u64 *samples = malloc((size_t)g_repeat * sizeof(u64));                    \
                                                                              \
    print_fill(label_str, nb_bk, slots_per_bk, sizeof(bucket_t));             \
    init_call;                                                                \
    for (u64 i = 0u; i < g_table_n; i++) {                                    \
        nodes[i].key = i + 1u;                                                \
        insert_call;                                                          \
    }                                                                         \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            u64 key = g_probe_idx[r * BENCH_N + k] + 1u;                      \
            node_t *res = prefix##_find(&head, bk, nodes, key);               \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x256", samples, (unsigned)g_repeat, BENCH_N);      \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            u64 key = g_table_n + g_probe_miss_idx[r * BENCH_N + k] + 1u;     \
            node_t *res = prefix##_find(&head, bk, nodes, key);               \
            if (res != NULL) g_sink += (uintptr_t)res;                        \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x256", samples, (unsigned)g_repeat, BENCH_N);     \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            u64 keys[4];                                                      \
            for (unsigned j = 0u; j < 4u; j++)                                \
                keys[j] = g_probe_idx[r * BENCH_N + k + j] + 1u;              \
            prefix##_hash_key_n(ctx, 4u, &head, bk, keys);                    \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " hit x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t ctx[4];                                                     \
            node_t *res[4];                                                   \
            u64 keys[4];                                                      \
            for (unsigned j = 0u; j < 4u; j++)                                \
                keys[j] = g_table_n + g_probe_miss_idx[r * BENCH_N + k + j] + 1u; \
            prefix##_hash_key_n(ctx, 4u, &head, bk, keys);                    \
            prefix##_scan_bk_n(ctx, 4u, &head, bk);                           \
            prefix##_prefetch_node_n(ctx, 4u, nodes);                         \
            prefix##_cmp_key_n(ctx, 4u, nodes, res);                          \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j] != NULL) g_sink += (uintptr_t)res[j];              \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " miss x4 staged", samples, (unsigned)g_repeat, BENCH_N); \
                                                                              \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 idx = g_probe_idx[r * BENCH_N] % g_table_n;                       \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            unsigned i = (unsigned)((idx + k) % g_table_n);                   \
            remove_call;                                                      \
            insert_call;                                                      \
        }                                                                     \
        samples[r] = tsc_end() - t0;                                          \
    }                                                                         \
    report(label_str " rm+ins x256", samples, (unsigned)g_repeat, BENCH_N);   \
                                                                              \
    free(samples);                                                            \
    munmap(nodes, nd_mem);                                                    \
    munmap(bk, bk_mem);                                                       \
}

DEFINE_PTR_BENCH("pure FP", ht_pure_fp, struct n_pure_fp,
                 struct rix_hash_find_ctx_s, struct rix_hash_bucket_s,
                 ht_pure_fp_init(&head, nb_bk),
                 if (ht_pure_fp_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_pure_fp_remove(&head, bk, nodes, &nodes[i]),
                 RIX_HASH_BUCKET_ENTRY_SZ)

DEFINE_PTR_BENCH("pure SLOT", ht_pure_slot, struct n_pure_slot,
                 struct rix_hash_find_ctx_s, struct rix_hash_bucket_s,
                 ht_pure_slot_init(&head, nb_bk),
                 if (ht_pure_slot_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_pure_slot_remove(&head, bk, nodes, &nodes[i]),
                 RIX_HASH_BUCKET_ENTRY_SZ)

DEFINE_PTR_BENCH("pure KEYONLY", ht_pure_keyonly, struct n_pure_keyonly,
                 struct rix_hash_find_ctx_s, struct rix_hash_bucket_s,
                 ht_pure_keyonly_init(&head, nb_bk),
                 if (ht_pure_keyonly_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_pure_keyonly_remove(&head, bk, nodes, &nodes[i]),
                 RIX_HASH_BUCKET_ENTRY_SZ)

DEFINE_PTR_BENCH("pure SLOT_EXTRA", ht_pure_extra, struct n_pure_extra,
                 struct rix_hash_find_ctx_extra_s,
                 struct rix_hash_bucket_extra_s,
                 ht_pure_extra_init(&head, nb_bk),
                 if (ht_pure_extra_insert(&head, bk, nodes, &nodes[i], (u32)i) != NULL) exit(2),
                 ht_pure_extra_remove(&head, bk, nodes, &nodes[i]),
                 RIX_HASH_BUCKET_ENTRY_SZ)

DEFINE_U32_BENCH("pure U32", ht_pure_u32, struct n_pure_u32,
                 struct rix_hash32_find_ctx_s, struct rix_hash_bucket_s,
                 ht_pure_u32_init(&head, bk, nb_bk),
                 if (ht_pure_u32_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_pure_u32_remove(&head, bk, nodes, &nodes[i]),
                 rix_hash_nb_bk_hint, RIX_HASH_BUCKET_ENTRY_SZ)

DEFINE_U64_BENCH("pure U64", ht_pure_u64, struct n_pure_u64,
                 struct rix_hash64_find_ctx_s, struct rix_hash64_bucket_s,
                 ht_pure_u64_init(&head, bk, nb_bk),
                 if (ht_pure_u64_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_pure_u64_remove(&head, bk, nodes, &nodes[i]),
                 rix_hash_nb_bk_hint, RIX_HASH_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRSW FP", ht_mrsw_fp, struct n_mrsw_fp,
                      struct rix_hash_mrsw_find_ctx_s,
                      struct rix_hash_bucket_s,
                      ht_mrsw_fp_init(&head, bk, nb_bk),
                      if (ht_mrsw_fp_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                      ht_mrsw_fp_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRSW SLOT", ht_mrsw_slot, struct n_mrsw_slot,
                      struct rix_hash_mrsw_find_ctx_s,
                      struct rix_hash_bucket_s,
                      ht_mrsw_slot_init(&head, bk, nb_bk),
                      if (ht_mrsw_slot_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                      ht_mrsw_slot_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRSW KEYONLY", ht_mrsw_keyonly,
                      struct n_mrsw_keyonly,
                      struct rix_hash_mrsw_find_ctx_s,
                      struct rix_hash_bucket_s,
                      ht_mrsw_keyonly_init(&head, bk, nb_bk),
                      if (ht_mrsw_keyonly_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                      ht_mrsw_keyonly_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRMW FP", ht_mrmw_fp, struct n_mrmw_fp,
                      struct rix_hash_mrsw_find_ctx_s,
                      struct rix_hash_bucket_s,
                      (ht_mrmw_fp_init(&head, bk, nb_bk),
                       ht_mrmw_fp_attach_kickout_scratch(&head,
                           xmmap(RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(nb_bk)
                                 * sizeof(unsigned)))),
                      if (ht_mrmw_fp_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                      ht_mrmw_fp_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRMW SLOT", ht_mrmw_slot, struct n_mrmw_slot,
                      struct rix_hash_mrsw_find_ctx_s,
                      struct rix_hash_bucket_s,
                      (ht_mrmw_slot_init(&head, bk, nb_bk),
                       ht_mrmw_slot_attach_kickout_scratch(&head,
                           xmmap(RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(nb_bk)
                                 * sizeof(unsigned)))),
                      if (ht_mrmw_slot_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                      ht_mrmw_slot_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRMW KEYONLY", ht_mrmw_keyonly,
                      struct n_mrmw_keyonly,
                      struct rix_hash_mrsw_find_ctx_s,
                      struct rix_hash_bucket_s,
                      (ht_mrmw_keyonly_init(&head, bk, nb_bk),
                       ht_mrmw_keyonly_attach_kickout_scratch(&head,
                           xmmap(RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(nb_bk)
                                 * sizeof(unsigned)))),
                      if (ht_mrmw_keyonly_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                      ht_mrmw_keyonly_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRSW SLOT_EXTRA", ht_mrsw_extra, struct n_mrsw_extra,
                      struct rix_hash_mrsw_extra_find_ctx_s,
                      struct rix_hash_bucket_extra_s,
                      ht_mrsw_extra_init(&head, bk, nb_bk),
                      if (ht_mrsw_extra_insert(&head, bk, nodes, &nodes[i], (u32)i) != NULL) exit(2),
                      ht_mrsw_extra_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_MRSW_PTR_BENCH("MRMW SLOT_EXTRA", ht_mrmw_extra, struct n_mrmw_extra,
                      struct rix_hash_mrsw_extra_find_ctx_s,
                      struct rix_hash_bucket_extra_s,
                      (ht_mrmw_extra_init(&head, bk, nb_bk),
                       ht_mrmw_extra_attach_kickout_scratch(&head,
                           xmmap(RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(nb_bk)
                                 * sizeof(unsigned)))),
                      if (ht_mrmw_extra_insert(&head, bk, nodes, &nodes[i], (u32)i) != NULL) exit(2),
                      ht_mrmw_extra_remove(&head, bk, nodes, &nodes[i]),
                      RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_U32_BENCH("MRSW U32", ht_mrsw_u32, struct n_mrsw_u32,
                 struct rix_hash_mrsw_u32_find_ctx_s,
                 struct rix_hash_bucket_s,
                 ht_mrsw_u32_init(&head, bk, nb_bk),
                 if (ht_mrsw_u32_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_mrsw_u32_remove(&head, bk, nodes, &nodes[i]),
                 rix_hash_mrsw_nb_bk_hint, RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_U32_BENCH("MRMW U32", ht_mrmw_u32, struct n_mrmw_u32,
                 struct rix_hash_mrsw_u32_find_ctx_s,
                 struct rix_hash_bucket_s,
                 (ht_mrmw_u32_init(&head, bk, nb_bk),
                  ht_mrmw_u32_attach_kickout_scratch(&head,
                      xmmap(RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(nb_bk)
                            * sizeof(unsigned)))),
                 if (ht_mrmw_u32_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_mrmw_u32_remove(&head, bk, nodes, &nodes[i]),
                 rix_hash_mrsw_nb_bk_hint, RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_U64_BENCH("MRSW U64", ht_mrsw_u64, struct n_mrsw_u64,
                 struct rix_hash_mrsw_u64_find_ctx_s,
                 struct rix_hash64_bucket_s,
                 ht_mrsw_u64_init(&head, bk, nb_bk),
                 if (ht_mrsw_u64_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_mrsw_u64_remove(&head, bk, nodes, &nodes[i]),
                 rix_hash_mrsw_nb_bk_hint, RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

DEFINE_U64_BENCH("MRMW U64", ht_mrmw_u64, struct n_mrmw_u64,
                 struct rix_hash_mrsw_u64_find_ctx_s,
                 struct rix_hash64_bucket_s,
                 (ht_mrmw_u64_init(&head, bk, nb_bk),
                  ht_mrmw_u64_attach_kickout_scratch(&head,
                      xmmap(RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(nb_bk)
                            * sizeof(unsigned)))),
                 if (ht_mrmw_u64_insert(&head, bk, nodes, &nodes[i]) != NULL) exit(2),
                 ht_mrmw_u64_remove(&head, bk, nodes, &nodes[i]),
                 rix_hash_mrsw_nb_bk_hint, RIX_HASH_MRSW_BUCKET_ENTRY_SZ)

int
main(int argc, char **argv)
{
    g_table_n   = (argc > 1) ? strtoull(argv[1], NULL, 0) : 1048576ull;
    g_repeat    = (argc > 2) ? strtoull(argv[2], NULL, 0) : 200ull;
    g_rand_keys = (argc > 3) ? (int)atoi(argv[3]) : 1;

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    g_probe_idx = build_probe_idx(0u);
    g_probe_miss_idx = build_probe_idx(0x9e3779b9u);

    printf("# bench_pure_vs_mrsw\n");
    printf("#   table_n=%llu repeat=%llu rand_keys=%d batch=%u\n",
           (unsigned long long)g_table_n,
           (unsigned long long)g_repeat,
           g_rand_keys, BENCH_N);

    bench_ht_pure_fp();
    bench_ht_mrsw_fp();
    bench_ht_mrmw_fp();
    bench_ht_pure_slot();
    bench_ht_mrsw_slot();
    bench_ht_mrmw_slot();
    bench_ht_pure_keyonly();
    bench_ht_mrsw_keyonly();
    bench_ht_mrmw_keyonly();
    bench_ht_pure_u32();
    bench_ht_mrsw_u32();
    bench_ht_mrmw_u32();
    bench_ht_pure_u64();
    bench_ht_mrsw_u64();
    bench_ht_mrmw_u64();
    bench_ht_pure_extra();
    bench_ht_mrsw_extra();
    bench_ht_mrmw_extra();

    printf("\n# sink=%" PRIuPTR "\n", g_sink);
    munmap(g_probe_idx, (size_t)g_repeat * BENCH_N * sizeof(u64));
    munmap(g_probe_miss_idx, (size_t)g_repeat * BENCH_N * sizeof(u64));
    return 0;
}
