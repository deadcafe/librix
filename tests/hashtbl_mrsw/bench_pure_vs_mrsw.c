/* bench_pure_vs_mrsw.c
 *  Compare lookup/insert/remove performance of pure cuckoo hash variants
 *  (fp, slot) against the MRSW variants (fp, slot) under identical
 *  workloads.  Single-threaded baseline -- isolates protocol overhead
 *  rather than contention.
 *
 *  Usage:
 *    ./bench_pure_vs_mrsw [table_n [repeat [rand_keys]]]
 *      table_n   : number of resident entries (default: 1,048,576)
 *      repeat    : iterations per pattern    (default: 200)
 *      rand_keys : 0=sequential, 1=random    (default: 1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rix/rix_hash.h"
#include "rix/rix_hash_mrsw.h"

/* ================================================================== */
/* Key / node definitions                                              */
/* ================================================================== */
struct mykey {
    u64 hi;
};

static RIX_FORCE_INLINE int
mykey_cmp(const struct mykey *a, const struct mykey *b)
{
    return (a->hi != b->hi) ? 1 : 0;
}

/* pure fp */
struct n_pure_fp {
    u32     cur_hash;
    u32     pad;
    struct mykey key;
};
RIX_HASH_HEAD(ht_pure_fp);
RIX_HASH_GENERATE(ht_pure_fp, n_pure_fp, key, cur_hash, mykey_cmp)

/* pure slot */
struct n_pure_slot {
    u32     cur_hash;
    u16     slot;
    u16     pad;
    struct mykey key;
};
RIX_HASH_HEAD(ht_pure_slot);
RIX_HASH_GENERATE_SLOT(ht_pure_slot, n_pure_slot, key, cur_hash, slot,
                       mykey_cmp)

/* MRSW fp */
struct n_mrsw_fp {
    u32     cur_hash;
    u32     pad;
    struct mykey key;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_fp);
RIX_HASH_MRSW_GENERATE(ht_mrsw_fp, n_mrsw_fp, key, cur_hash, mykey_cmp)

/* MRSW slot */
struct n_mrsw_slot {
    u32     cur_hash;
    u16     slot;
    u16     pad;
    struct mykey key;
};
RIX_HASH_MRSW_HEAD(ht_mrsw_slot);
RIX_HASH_MRSW_GENERATE_SLOT(ht_mrsw_slot, n_mrsw_slot, key, cur_hash, slot,
                            mykey_cmp)

/* ================================================================== */
/* TSC helpers                                                         */
/* ================================================================== */
static inline u64
tsc_start(void)
{
    u32 lo, hi;
    __asm__ volatile ("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static inline u64
tsc_end(void)
{
    u32 lo, hi;
    __asm__ volatile ("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((u64)hi << 32) | lo;
}

/* ================================================================== */
/* PRNG                                                                */
/* ================================================================== */
static u64 xr64 = 0xDEADBEEF1234ABCDULL;

static inline u64
xorshift64(void)
{
    xr64 ^= xr64 >> 12;
    xr64 ^= xr64 << 25;
    xr64 ^= xr64 >> 27;
    return xr64 * 0x2545F4914F6CDD1DULL;
}

/* ================================================================== */
/* Globals and helpers                                                 */
/* ================================================================== */
#define BENCH_N 256

static u64  g_table_n;
static u64  g_repeat;
static int  g_rand_keys;
static volatile u32 g_sink;

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
    if ((n & 1u) == 1u)
        return (double)samples[n / 2u];
    return ((double)samples[n / 2u - 1u] + (double)samples[n / 2u]) / 2.0;
}

static void
report(const char *label, u64 *samples, unsigned n, unsigned ops)
{
    double med = median_cy(samples, n);
    double per_op = med / (double)ops;

    printf("  %-28s median=%8.0f cy/batch  per_op=%6.2f cy\n",
           label, med, per_op);
}

static u64 *
build_probe_idx(void)
{
    u64 *p = (u64 *)xmmap((size_t)g_repeat * BENCH_N * sizeof(u64));
    for (u64 i = 0u; i < g_repeat * BENCH_N; i++)
        p[i] = g_rand_keys ? (xorshift64() % g_table_n)
                           : (i % g_table_n);
    return p;
}

/* ================================================================== */
/* Per-variant bench template                                          */
/*                                                                     */
/*   prefix     : function-prefix used by RIX_HASH(_MRSW)?_GENERATE*   */
/*   node_t     : struct node_type                                     */
/*   ctx_t      : struct find_ctx_type                                 */
/*   init_call  : full call expression that initializes 'head'         */
/*   nb_bk_hint : sizing helper to call (rix_hash_(mrsw_)?nb_bk_hint)  */
/* ================================================================== */
#define DEFINE_BENCH(label_str, prefix, node_t, ctx_t, init_call, nb_bk_hint) \
static void                                                                   \
bench_##prefix(void)                                                          \
{                                                                             \
    unsigned nb_bk = nb_bk_hint((unsigned)g_table_n);                         \
    size_t bk_mem  = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);        \
    size_t nd_mem  = (size_t)g_table_n * sizeof(node_t);                      \
    struct rix_hash_bucket_s *bk    = xmmap(bk_mem);                          \
    node_t                   *nodes = xmmap(nd_mem);                          \
    struct prefix             head;                                           \
    u64                      *probe_idx;                                      \
    u64                      *samples;                                        \
                                                                              \
    init_call;                                                                \
    for (u64 i = 0u; i < g_table_n; i++) {                                    \
        nodes[i].key.hi = (u64)(i + 1u);                                      \
        if (prefix##_insert(&head, bk, nodes, &nodes[i]) != NULL) {           \
            fprintf(stderr, "%s: insert %llu failed\n", label_str,            \
                    (unsigned long long)i);                                   \
            exit(1);                                                          \
        }                                                                     \
    }                                                                         \
    probe_idx = build_probe_idx();                                            \
    samples   = (u64 *)malloc((size_t)g_repeat * sizeof(u64));                \
                                                                              \
    /* find single-shot */                                                    \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            struct mykey key = {                                              \
                (u64)(probe_idx[r * BENCH_N + k] + 1u)                        \
            };                                                                \
            node_t *res = prefix##_find(&head, bk, nodes, &key);              \
            if (res != NULL) g_sink += res->cur_hash;                         \
        }                                                                     \
        u64 t1 = tsc_end();                                                   \
        samples[r] = t1 - t0;                                                 \
    }                                                                         \
    report(label_str " find x256", samples, (unsigned)g_repeat, BENCH_N);     \
                                                                              \
    /* find x4 staged */                                                      \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k += 4u) {                         \
            ctx_t                ctx[4];                                      \
            node_t              *res[4];                                      \
            struct mykey         keys[4];                                     \
            const struct mykey  *kp[4];                                       \
            for (unsigned j = 0u; j < 4u; j++) {                              \
                keys[j].hi = (u64)(probe_idx[r * BENCH_N + k + j] + 1u);      \
                kp[j] = &keys[j];                                             \
            }                                                                 \
            prefix##_hash_key_n(ctx, 4, &head, bk, kp);                       \
            prefix##_scan_bk_n(ctx, 4, &head, bk);                            \
            prefix##_prefetch_node_n(ctx, 4, nodes);                          \
            prefix##_cmp_key_n(ctx, 4, nodes, res);                           \
            for (unsigned j = 0u; j < 4u; j++)                                \
                if (res[j]) g_sink += res[j]->cur_hash;                       \
        }                                                                     \
        u64 t1 = tsc_end();                                                   \
        samples[r] = t1 - t0;                                                 \
    }                                                                         \
    report(label_str " find x4 staged", samples, (unsigned)g_repeat,          \
           BENCH_N);                                                          \
                                                                              \
    /* rm + ins (writer) */                                                   \
    for (u64 r = 0u; r < g_repeat; r++) {                                     \
        u64 idx = probe_idx[r * BENCH_N] % g_table_n;                         \
        u64 t0 = tsc_start();                                                 \
        for (unsigned k = 0u; k < BENCH_N; k++) {                             \
            unsigned i = (unsigned)((idx + k) % g_table_n);                   \
            prefix##_remove(&head, bk, nodes, &nodes[i]);                     \
            prefix##_insert(&head, bk, nodes, &nodes[i]);                     \
        }                                                                     \
        u64 t1 = tsc_end();                                                   \
        samples[r] = t1 - t0;                                                 \
    }                                                                         \
    report(label_str " rm+ins x256", samples, (unsigned)g_repeat, BENCH_N);   \
                                                                              \
    free(samples);                                                            \
    munmap(probe_idx, (size_t)g_repeat * BENCH_N * sizeof(u64));              \
    munmap(nodes, nd_mem);                                                    \
    munmap(bk, bk_mem);                                                       \
}

DEFINE_BENCH("pure FP",   ht_pure_fp,   struct n_pure_fp,
             struct rix_hash_find_ctx_s,
             ht_pure_fp_init(&head, nb_bk),
             rix_hash_nb_bk_hint)
DEFINE_BENCH("pure SLOT", ht_pure_slot, struct n_pure_slot,
             struct rix_hash_find_ctx_s,
             ht_pure_slot_init(&head, nb_bk),
             rix_hash_nb_bk_hint)
DEFINE_BENCH("MRSW FP",   ht_mrsw_fp,   struct n_mrsw_fp,
             struct rix_hash_mrsw_find_ctx_s,
             ht_mrsw_fp_init(&head, bk, nb_bk),
             rix_hash_mrsw_nb_bk_hint)
DEFINE_BENCH("MRSW SLOT", ht_mrsw_slot, struct n_mrsw_slot,
             struct rix_hash_mrsw_find_ctx_s,
             ht_mrsw_slot_init(&head, bk, nb_bk),
             rix_hash_mrsw_nb_bk_hint)

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(int argc, char **argv)
{
    g_table_n   = (argc > 1) ? strtoull(argv[1], NULL, 0) : 1048576ull;
    g_repeat    = (argc > 2) ? strtoull(argv[2], NULL, 0) : 200ull;
    g_rand_keys = (argc > 3) ? (int)atoi(argv[3]) : 1;

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    printf("# bench_pure_vs_mrsw\n");
    printf("#   table_n=%llu repeat=%llu rand_keys=%d batch=%d\n",
           (unsigned long long)g_table_n,
           (unsigned long long)g_repeat,
           g_rand_keys, BENCH_N);
    printf("\n");

    bench_ht_pure_fp();
    bench_ht_pure_slot();
    bench_ht_mrsw_fp();
    bench_ht_mrsw_slot();

    return 0;
}
