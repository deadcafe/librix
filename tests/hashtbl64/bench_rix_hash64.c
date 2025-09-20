/* bench_rix_hash64.c
 *  rix_hash64.h lookup performance & fill rate benchmark (64-bit key variant)
 *  Metric: CPU cycles per 256 lookups
 *
 *  Usage: ./hash64_bench [table_n [nb_bk [repeat]]]
 *    table_n : number of table entries  (default: 10,000,000)
 *    nb_bk   : number of buckets       (default: auto - ~80% fill of table_n)
 *    repeat  : number of iterations    (default: 2000)
 *
 *  Fill rate notes:
 *    rix_hash64 has the same 16 slots/bucket as rix_hash32.
 *    Maximum fill rate is equivalent to rix_hash32 (~95%).
 *    However, bucket size is 192 B (3CL) vs rix_hash32's 128 B (2CL),
 *    so bk_mem for the same entry count requires 1.5x.
 *    bench_fill_rate() measures the actual values.
 *
 *    Empirical estimates (FOLLOW_DEPTH=8):
 *      rix_hash32 (16 slots, 128 B/bk): max fill ~90-95%
 *      rix_hash64 (16 slots, 192 B/bk): max fill ~90-95% (equivalent)
 *    Recommended fill target: <=80%, same as rix_hash32.
 *
 *  Hot/cold comparison:
 *    hot-cache  : repeated lookup on the same fixed 256-key hit set
 *    cold-cache : same 256-key hit set, but cache-thrash before each round
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rix_hash64.h"

/* ================================================================== */
/* Node definition                                                     */
/* ================================================================== */
typedef struct mynode_s {
    uint64_t key;
    uint64_t val;
} mynode_t;

#define BENCH_INVALID_KEY  0xFFFFFFFFFFFFFFFFULL

RIX_HASH64_HEAD(myht64);
RIX_HASH64_GENERATE(myht64, mynode_t, key, BENCH_INVALID_KEY)

/* ================================================================== */
/* TSC measurement helper                                              */
/* ================================================================== */
static inline uint64_t
tsc_start(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t
tsc_end(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}

static double
now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ================================================================== */
/* Benchmark constants                                                 */
/* ================================================================== */
#define BENCH_N  256
#define BENCH_N6 ((BENCH_N / 6) * 6)   /* 252 */
#define BENCH_N8 ((BENCH_N / 8) * 8)   /* 256 */
#define THRASH_BYTES_MIN (64u * 1024u * 1024u)

/*
 * Key prefetch distance (in batches):
 * ~64 cy per hash_key batch (CRC32Cx8 + prefetchx8x3)
 * DRAM latency ~300 cy -> 300/64 ~ 5 batches needed -> use 8/11 with margin
 */
#define KPD8  8   /* x8 pipeline: 8 batches x 8 keys = 64 keys ahead */
#define KPD6 11   /* x6 pipeline: 11 batches x 6 keys = 66 keys ahead */

static struct rix_hash64_find_ctx_s g_ctx[BENCH_N];
static mynode_t                    *g_res[BENCH_N];
static unsigned                     g_idx[BENCH_N];
static volatile uint64_t            g_thrash_sink;

struct bench_result_s {
    const char *label;
    uint64_t min_cy;
    uint64_t sum_cy;
    uint64_t *samples;
    unsigned sample_count;
    int      ops;
};

/* ================================================================== */
/* xorshift64 PRNG                                                     */
/* ================================================================== */
static uint64_t xr64 = 0xDEADBEEF1234ABCDULL;

static inline uint64_t
xorshift64(void)
{
    xr64 ^= xr64 >> 12;
    xr64 ^= xr64 << 25;
    xr64 ^= xr64 >> 27;
    return xr64 * 0x2545F4914F6CDD1DULL;
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static double
result_median_cy(struct bench_result_s *result)
{
    unsigned n = result->sample_count;

    if (n == 0)
        return 0.0;

    qsort(result->samples, n, sizeof(result->samples[0]), cmp_u64);
    if (n & 1u)
        return (double)result->samples[n / 2];

    return ((double)result->samples[(n / 2) - 1] +
            (double)result->samples[n / 2]) * 0.5;
}

static double
result_median_op(struct bench_result_s *result)
{
    return result_median_cy(result) / (double)result->ops;
}

static RIX_FORCE_INLINE unsigned
find_idx_from_ctx(struct rix_hash64_find_ctx_s *ctx)
{
    u32 hits = ctx->hits[0];
    if (hits) {
        unsigned bit = (unsigned)__builtin_ctz(hits);
        return ctx->bk[0]->idx[bit];
    }
    hits = rix_hash_arch->find_u64x16(ctx->bk[1]->key, ctx->key);
    if (hits) {
        unsigned bit = (unsigned)__builtin_ctz(hits);
        return ctx->bk[1]->idx[bit];
    }
    return 0u;
}

static RIX_FORCE_INLINE void
find_idx_from_ctx_n(struct rix_hash64_find_ctx_s *ctx, int n, unsigned *results)
{
    for (int j = 0; j < n; j++)
        results[j] = find_idx_from_ctx(&ctx[j]);
}

static RIX_FORCE_INLINE unsigned
find_idx_single(struct myht64 *head, struct rix_hash64_bucket_s *bk, u64 key)
{
    struct rix_hash64_find_ctx_s ctx;
    RIX_HASH64_HASH_KEY(myht64, &ctx, head, bk, key);
    RIX_HASH64_SCAN_BK(myht64, &ctx, head, bk);
    return find_idx_from_ctx(&ctx);
}

static void
thrash_cache(uint8_t *buf, size_t len)
{
    uint64_t sum = 0;

    for (size_t i = 0; i < len; i += 64)
        sum += buf[i];

    g_thrash_sink += sum;
}

static void
init_find_results(struct bench_result_s result[17], unsigned repeat)
{
    static const struct {
        const char *label;
        int         ops;
    } spec[17] = {
        { "find_idx (single)  ", BENCH_N  },
        { "x1 idx (bk only)   ", BENCH_N  },
        { "x4 idx (bk only)   ", BENCH_N  },
        { "x6 idx (bk only)   ", BENCH_N6 },
        { "x8 idx (bk only)   ", BENCH_N8 },
        { "x6 idx pipeline    ", BENCH_N6 },
        { "x8 idx pipeline    ", BENCH_N8 },
        { "find_ptr (single)  ", BENCH_N  },
        { "x1 ptr (bk only)   ", BENCH_N  },
        { "x1 ptr (bk+node)   ", BENCH_N  },
        { "x2 ptr (bk+node)   ", BENCH_N  },
        { "x4 ptr (bk only)   ", BENCH_N  },
        { "x4 ptr (bk+node)   ", BENCH_N  },
        { "x6 ptr (bk+node)   ", BENCH_N6 },
        { "x8 ptr (bk+node)   ", BENCH_N8 },
        { "x6 ptr pipeline    ", BENCH_N6 },
        { "x8 ptr pipeline    ", BENCH_N8 },
    };

    for (int i = 0; i < 17; i++) {
        result[i].label = spec[i].label;
        result[i].min_cy = UINT64_MAX;
        result[i].sum_cy = 0;
        result[i].samples = (uint64_t *)calloc(repeat, sizeof(uint64_t));
        if (!result[i].samples) { perror("calloc samples"); exit(1); }
        result[i].sample_count = repeat;
        result[i].ops = spec[i].ops;
    }
}

static void
init_update_results(struct bench_result_s result[2], unsigned repeat)
{
    result[0].label = "remove             ";
    result[0].min_cy = UINT64_MAX;
    result[0].sum_cy = 0;
    result[0].samples = (uint64_t *)calloc(repeat, sizeof(uint64_t));
    if (!result[0].samples) { perror("calloc samples"); exit(1); }
    result[0].sample_count = repeat;
    result[0].ops = BENCH_N;

    result[1].label = "insert             ";
    result[1].min_cy = UINT64_MAX;
    result[1].sum_cy = 0;
    result[1].samples = (uint64_t *)calloc(repeat, sizeof(uint64_t));
    if (!result[1].samples) { perror("calloc samples"); exit(1); }
    result[1].sample_count = repeat;
    result[1].ops = BENCH_N;
}

static void
free_results(struct bench_result_s *result, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        free(result[i].samples);
        result[i].samples = NULL;
        result[i].sample_count = 0;
    }
}

static void
measure_find_patterns(struct myht64 *head,
                      struct rix_hash64_bucket_s *bk,
                      mynode_t *nodes,
                      const uint64_t keys[BENCH_N],
                      unsigned repeat,
                      uint8_t *thrash,
                      size_t thrash_len,
                      struct bench_result_s result[17])
{
    init_find_results(result, repeat);

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_idx[i] = find_idx_single(head, bk, keys[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
        result[0].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_HASH_KEY(myht64, &g_ctx[i], head, bk, keys[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_SCAN_BK(myht64, &g_ctx[i], head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_idx[i] = find_idx_from_ctx(&g_ctx[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
        result[1].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_HASH_KEY4(myht64, &g_ctx[b*4], head, bk, keys + b*4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_SCAN_BK4(myht64, &g_ctx[b*4], head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            find_idx_from_ctx_n(&g_ctx[b*4], 4, &g_idx[b*4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[2].min_cy) result[2].min_cy = cy;
        result[2].sum_cy += cy;
        result[2].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*6], 6, head, bk, keys + b*6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*6], 6, head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            find_idx_from_ctx_n(&g_ctx[b*6], 6, &g_idx[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[3].min_cy) result[3].min_cy = cy;
        result[3].sum_cy += cy;
        result[3].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, head, bk, keys + b*8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            find_idx_from_ctx_n(&g_ctx[b*8], 8, &g_idx[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[4].min_cy) result[4].min_cy = cy;
        result[4].sum_cy += cy;
        result[4].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*6], 6, head, bk, keys + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*6], 6, head, bk, keys + pf*6);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*6], 6, head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            find_idx_from_ctx_n(&g_ctx[b*6], 6, &g_idx[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[5].min_cy) result[5].min_cy = cy;
        result[5].sum_cy += cy;
        result[5].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, head, bk, keys + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*8], 8, head, bk, keys + pf*8);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            find_idx_from_ctx_n(&g_ctx[b*8], 8, &g_idx[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[6].min_cy) result[6].min_cy = cy;
        result[6].sum_cy += cy;
        result[6].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_FIND(myht64, head, bk, nodes, keys[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[7].min_cy) result[7].min_cy = cy;
        result[7].sum_cy += cy;
        result[7].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_HASH_KEY(myht64, &g_ctx[i], head, bk, keys[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_SCAN_BK(myht64, &g_ctx[i], head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_CMP_KEY(myht64, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[8].min_cy) result[8].min_cy = cy;
        result[8].sum_cy += cy;
        result[8].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_HASH_KEY(myht64, &g_ctx[i], head, bk, keys[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_SCAN_BK(myht64, &g_ctx[i], head, bk);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_PREFETCH_NODE(myht64, &g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_CMP_KEY(myht64, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[9].min_cy) result[9].min_cy = cy;
        result[9].sum_cy += cy;
        result[9].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_HASH_KEY2(myht64, &g_ctx[b*2], head, bk, keys + b*2);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_SCAN_BK2(myht64, &g_ctx[b*2], head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_PREFETCH_NODE2(myht64, &g_ctx[b*2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_CMP_KEY2(myht64, &g_ctx[b*2], nodes, &g_res[b*2]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[10].min_cy) result[10].min_cy = cy;
        result[10].sum_cy += cy;
        result[10].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_HASH_KEY4(myht64, &g_ctx[b*4], head, bk, keys + b*4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_SCAN_BK4(myht64, &g_ctx[b*4], head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_CMP_KEY4(myht64, &g_ctx[b*4], nodes, &g_res[b*4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[11].min_cy) result[11].min_cy = cy;
        result[11].sum_cy += cy;
        result[11].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_HASH_KEY4(myht64, &g_ctx[b*4], head, bk, keys + b*4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_SCAN_BK4(myht64, &g_ctx[b*4], head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_PREFETCH_NODE4(myht64, &g_ctx[b*4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_CMP_KEY4(myht64, &g_ctx[b*4], nodes, &g_res[b*4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[12].min_cy) result[12].min_cy = cy;
        result[12].sum_cy += cy;
        result[12].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*6], 6, head, bk, keys + b*6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*6], 6, head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[13].min_cy) result[13].min_cy = cy;
        result[13].sum_cy += cy;
        result[13].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, head, bk, keys + b*8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[14].min_cy) result[14].min_cy = cy;
        result[14].sum_cy += cy;
        result[14].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*6], 6, head, bk, keys + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*6], 6, head, bk, keys + pf*6);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*6], 6, head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[15].min_cy) result[15].min_cy = cy;
        result[15].sum_cy += cy;
        result[15].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, head, bk, keys + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*8], 8, head, bk, keys + pf*8);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[16].min_cy) result[16].min_cy = cy;
        result[16].sum_cy += cy;
        result[16].samples[r] = cy;
    }
}

static void
measure_update_patterns(struct myht64 *head,
                        struct rix_hash64_bucket_s *bk,
                        mynode_t *nodes,
                        unsigned repeat,
                        struct bench_result_s result[2])
{
    init_update_results(result, repeat);

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_REMOVE(myht64, head, bk, nodes, &nodes[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
        result[0].samples[r] = cy;
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_INSERT(myht64, head, bk, nodes, &nodes[i]);
    }

    for (int i = 0; i < BENCH_N; i++)
        RIX_HASH64_REMOVE(myht64, head, bk, nodes, &nodes[i]);

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_INSERT(myht64, head, bk, nodes, &nodes[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
        result[1].samples[r] = cy;
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_REMOVE(myht64, head, bk, nodes, &nodes[i]);
    }
}

static void
print_find_results(const char *mode,
                   struct bench_result_s result[17])
{
    printf("  %s\n", mode);
    printf("  %-20s  %10s  %10s  %10s  %10s\n",
           "pattern", "min/256", "med/256", "min/op", "med/op");
    printf("  %-20s  %10s  %10s  %10s  %10s\n",
           "--------------------", "----------", "----------",
           "----------", "----------");
    for (int p = 0; p < 17; p++) {
        if (p == 7)
            printf("  %-20s  %10s  %10s  %10s  %10s\n",
                   "-- ptr return --", "", "", "", "");
        double med = result_median_cy(&result[p]);
        printf("  %-20s  %10llu  %10llu  %10.2f  %10.2f\n",
               result[p].label,
               (unsigned long long)result[p].min_cy,
               (unsigned long long)med,
               (double)result[p].min_cy / result[p].ops,
               med / result[p].ops);
    }
    printf("\n");
}

/* ================================================================== */
/* bench_fill_rate
 *
 * Measure fill rate characteristics for 16 slots/bucket.
 *
 * Attempt insertions at multiple fill rate targets and record the point
 * where kickout failures begin. Also shows comparison values with
 * rix_hash32 (16 slots, 2CL).
 *
 * fill rate = number of successful inserts / (nb_bk x RIX_HASH64_BUCKET_ENTRY_SZ)
 * ================================================================== */
static void
bench_fill_rate(void)
{
    /* Fixed test size: nb_bk=1024 (16384 slots) */
    const unsigned NB_BK  = 1024u;
    const unsigned SLOTS  = NB_BK * RIX_HASH64_BUCKET_ENTRY_SZ;  /* 8192 */
    /* Test node count: slots * 1.05 (try up to 105%) */
    const unsigned TRY_N  = (unsigned)((uint64_t)SLOTS * 105 / 100) + 1;

    mynode_t *nodes =
        (mynode_t *)calloc(TRY_N, sizeof(mynode_t));
    struct rix_hash64_bucket_s *buckets =
        (struct rix_hash64_bucket_s *)aligned_alloc(64, NB_BK * sizeof(*buckets));
    if (!nodes || !buckets) { perror("alloc"); exit(1); }

    for (unsigned i = 0; i < TRY_N; i++) {
        nodes[i].key = (uint64_t)(i + 1);   /* 1-origin unique keys */
        nodes[i].val = i;
    }

    struct myht64 head;
    RIX_HASH64_INIT(myht64, &head, buckets, NB_BK);

    unsigned inserted = 0, kickout_fail = 0;

    /* Insert loop: continue even on failure, try up to TRY_N entries */
    unsigned *fail_at = (unsigned *)calloc(TRY_N, sizeof(unsigned));
    for (unsigned i = 0; i < TRY_N; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &head, buckets, nodes, &nodes[i]);
        if (r == NULL) {
            inserted++;
        } else if (r == &nodes[i]) {
            /* kickout failure (table full) */
            kickout_fail++;
            fail_at[kickout_fail - 1] = i;
        }
        /* no duplicates (unique keys) */
    }

    printf("\n=== Fill Rate Analysis (rix_hash64, 16 slots/bucket, 192 B/bk) ===\n");
    printf("  nb_bk=%u  slots=%u  FOLLOW_DEPTH=%d\n",
           NB_BK, SLOTS, RIX_HASH_FOLLOW_DEPTH);
    printf("  tried=%u  inserted=%u  kickout_fail=%u\n",
           TRY_N, inserted, kickout_fail);
    printf("  max_fill = %u/%u = %.2f%%\n",
           inserted, SLOTS, 100.0 * inserted / SLOTS);
    printf("\n");

    /*
     * Fill rate checkpoints: report cumulative kickout_fail count when
     * the number of inserted entries reaches x% of SLOTS.
     */
    printf("  Checkpoint analysis (cumulative kickout_fail):\n");
    printf("  %-10s  %-12s  %-12s\n", "target_%", "target_n", "kickout_fail_cum");
    printf("  %-10s  %-12s  %-12s\n", "----------", "------------", "----------------");

    /* Re-simulate all insertions and record checkpoints */
    RIX_HASH64_INIT(myht64, &head, buckets, NB_BK);
    unsigned cum_ok = 0, cum_fail = 0;
    unsigned targets[] = { 50, 60, 70, 75, 80, 85, 90, 95, 100 };
    unsigned ti = 0;

    for (unsigned i = 0; i < TRY_N && ti < 9; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &head, buckets, nodes, &nodes[i]);
        if (r == NULL)
            cum_ok++;
        else if (r == &nodes[i])
            cum_fail++;

        /* Output when the next checkpoint is reached */
        while (ti < 9 && cum_ok >= (unsigned)((uint64_t)SLOTS * targets[ti] / 100)) {
            printf("  %-10u  %-12u  %-12u\n",
                   targets[ti],
                   (unsigned)((uint64_t)SLOTS * targets[ti] / 100),
                   cum_fail);
            ti++;
        }
    }
    /* Remaining checkpoints (not reached) */
    while (ti < 9) {
        printf("  %-10u  %-12u  %-12s\n",
               targets[ti],
               (unsigned)((uint64_t)SLOTS * targets[ti] / 100),
               "N/A (not reached)");
        ti++;
    }

    printf("\n");
    printf("  Empirical findings (FOLLOW_DEPTH=%d):\n", RIX_HASH_FOLLOW_DEPTH);
    printf("    - Kickout failures are 0 up to ~95%% fill (same as rix_hash32)\n");
    printf("    - Failures appear only above ~95%% fill\n");
    printf("    - Fill rate behaviour is equivalent to rix_hash32 (both 16 slots/bk)\n");
    printf("    - Memory cost: rix_hash64 bk_mem is 1.5x rix_hash32\n");
    printf("      (192 B/bk vs 128 B/bk; same slot count, larger key per slot)\n");
    printf("    Recommendation: target <=80%% fill as with rix_hash32.\n\n");

    free(fail_at);
    free(nodes);
    free(buckets);
}

/* ================================================================== */
/* bench_find                                                          */
/* ================================================================== */
static void
bench_find(unsigned table_n, unsigned nb_bk, unsigned repeat)
{
    uint64_t bk0_hits = 0, bk1_hits = 0;
    size_t node_mem = (size_t)table_n * sizeof(mynode_t);
    size_t bk_mem   = (size_t)nb_bk   * sizeof(struct rix_hash64_bucket_s);
    size_t lookup_mem = (size_t)BENCH_N * sizeof(uint64_t);
    printf("[BENCH] table_n=%u  nb_bk=%u  slots=%u\n",
           table_n, nb_bk, nb_bk * RIX_HASH64_BUCKET_ENTRY_SZ);
    printf("  memory : nodes=%.1f MB  buckets=%.1f MB"
           "  lookup_set=%.3f MB  total=%.1f MB\n",
           node_mem / 1e6, bk_mem / 1e6, lookup_mem / 1e6,
           (node_mem + bk_mem + lookup_mem) / 1e6);

    mynode_t *nodes = (mynode_t *)mmap(NULL, node_mem,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes == MAP_FAILED) { perror("mmap nodes"); exit(1); }
    madvise(nodes, node_mem, MADV_HUGEPAGE);

    for (unsigned i = 0; i < table_n; i++) {
        nodes[i].key = (uint64_t)(i + 1);
        nodes[i].val = i;
    }

    struct rix_hash64_bucket_s *bk =
        (struct rix_hash64_bucket_s *)mmap(NULL, bk_mem,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk == MAP_FAILED) { perror("mmap bk"); exit(1); }
    madvise(bk, bk_mem, MADV_HUGEPAGE);

    struct myht64 head;
    RIX_HASH64_INIT(myht64, &head, bk, nb_bk);

    /* ---- Insertion --------------------------------------------------- */
    printf("  inserting...\n"); fflush(stdout);
    unsigned n_hit = 0;
    unsigned report_step = table_n / 10;
    if (report_step == 0) report_step = 1;

    uint64_t ins_bk0_fast = 0, ins_bk1_fast = 0, ins_kickout = 0;
    double t_ins_start = now_sec();

    for (unsigned i = 0; i < table_n; i++) {
        /* Determine insert path before inserting */
        union rix_hash_hash_u _h = rix_hash_arch->hash_u64(nodes[i].key, head.rhh_mask);
        unsigned _b0 = _h.val32[0] & head.rhh_mask;
        unsigned _b1 = _h.val32[1] & head.rhh_mask;
        uint32_t _nilm0 = rix_hash_arch->find_u64x16(bk[_b0].key, BENCH_INVALID_KEY);
        uint32_t _nilm1 = rix_hash_arch->find_u64x16(bk[_b1].key, BENCH_INVALID_KEY);
        if      (_nilm0) ins_bk0_fast++;
        else if (_nilm1) ins_bk1_fast++;
        else             ins_kickout++;

        if (RIX_HASH64_INSERT(myht64, &head, bk, nodes, &nodes[i]) == NULL)
            n_hit++;

        if ((i + 1) % report_step == 0) {
            printf("    %3u%%\r",
                   (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    double t_ins = now_sec() - t_ins_start;
    double fill = 100.0 * n_hit / ((double)nb_bk * RIX_HASH64_BUCKET_ENTRY_SZ);
    printf("  inserted : %u/%u (%.1f%% fill)  %.2f s  %.1f ns/insert\n",
           n_hit, table_n, fill, t_ins, t_ins * 1e9 / table_n);
    printf("  insert paths: bk0_fast=%" PRIu64 "  bk1_fast=%" PRIu64
           "  kickout=%" PRIu64 "\n",
           ins_bk0_fast, ins_bk1_fast, ins_kickout);

    /* ---- bk_0 hit rate ------------------------------------------- */
    {
        unsigned mask = head.rhh_mask;
        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < RIX_HASH64_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk[b].idx[s];
                if (nidx == (uint32_t)RIX_NIL) continue;
                uint64_t key = bk[b].key[s];
                union rix_hash_hash_u h = rix_hash_arch->hash_u64(key, mask);
                if (b == (h.val32[0] & mask)) bk0_hits++;
                else                          bk1_hits++;
            }
        }
        uint64_t total = bk0_hits + bk1_hits;
        printf("  bk_0 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%\n",
               bk0_hits, total, 100.0 * bk0_hits / total);
        printf("  bk_1 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%\n",
               bk1_hits, total, 100.0 * bk1_hits / total);
    }

    /* ---- Fixed hot/cold lookup set ----------------------------------- */
    uint64_t hot_keys[BENCH_N];
    for (int i = 0; i < BENCH_N; i++) {
        unsigned idx = (unsigned)(xorshift64() % n_hit);
        hot_keys[i] = nodes[idx].key;
    }

    size_t thrash_len = THRASH_BYTES_MIN;
    uint8_t *thrash = (uint8_t *)mmap(NULL, thrash_len,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (thrash == MAP_FAILED) { perror("mmap thrash"); exit(1); }
    memset(thrash, 0xA5, thrash_len);

    struct bench_result_s warmup_result[17];
    struct bench_result_s hot_result[17];
    struct bench_result_s cold_result[17];
    struct bench_result_s update_result[2];

    printf("  warmup...\n"); fflush(stdout);
    measure_find_patterns(&head, bk, nodes, hot_keys, 1, NULL, 0, warmup_result);

    printf("  measuring hot-cache...\n"); fflush(stdout);
    measure_find_patterns(&head, bk, nodes, hot_keys, repeat, NULL, 0, hot_result);

    printf("  measuring cold-cache...\n"); fflush(stdout);
    measure_find_patterns(&head, bk, nodes, hot_keys, repeat, thrash, thrash_len,
                          cold_result);

    printf("  measuring updates...\n"); fflush(stdout);
    measure_update_patterns(&head, bk, nodes, repeat, update_result);

    /* ---- Output results ---------------------------------------------- */
    {
        double mshr_min = 11520.0;
        double bk0_rate = 100.0 * (double)bk0_hits / (double)(bk0_hits + bk1_hits);

        printf("\n");
        printf("  hot-cache  : fixed 256-key hit set, repeated without cache thrash.\n");
        printf("  cold-cache : same 256-key hit set, %.0f MiB cache-thrash before each round.\n\n",
               (double)thrash_len / (1024.0 * 1024.0));

        print_find_results("hot-cache", hot_result);
        print_find_results("cold-cache", cold_result);

        printf("  updates\n");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "pattern", "min/256", "med/256", "min/op", "med/op");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "--------------------", "----------", "----------",
               "----------", "----------");
        for (int p = 0; p < 2; p++) {
            double med = result_median_cy(&update_result[p]);
            printf("  %-20s  %10llu  %10llu  %10.2f  %10.2f\n",
                   update_result[p].label,
                   (unsigned long long)update_result[p].min_cy,
                   (unsigned long long)med,
                   (double)update_result[p].min_cy / update_result[p].ops,
                   med / update_result[p].ops);
        }
        printf("\n");
        printf("  MSHR theoretical min (20 MSHRs, 300cy latency, 3CL/lookup):\n");
        printf("    %.0f cycles/256 = %.1f cycles/op\n\n",
               mshr_min, mshr_min / BENCH_N);
        printf("  Comparable summary (median cycles/op across %u rounds)\n", repeat);
        printf("  %-10s %8s %8s %8s %10s %10s %10s %10s %10s %10s\n",
               "variant", "fill%", "bk0%", "mode", "f_idx", "x4_idx",
               "x6p_idx", "x8p_idx", "f_ptr", "x4_ptr");
        printf("  %-10s %8s %8s %8s %10s %10s %10s %10s %10s %10s\n",
               "----------", "--------", "--------", "--------", "----------",
               "----------", "----------", "----------", "----------", "----------");
        printf("  %-10s %8.1f %8.2f %8s %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f\n",
               "hash64", fill, bk0_rate, "hot",
               result_median_op(&hot_result[0]),
               result_median_op(&hot_result[2]),
               result_median_op(&hot_result[5]),
               result_median_op(&hot_result[6]),
               result_median_op(&hot_result[7]),
               result_median_op(&hot_result[12]));
        printf("  %-10s %8.1f %8.2f %8s %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f\n",
               "hash64", fill, bk0_rate, "cold",
               result_median_op(&cold_result[0]),
               result_median_op(&cold_result[2]),
               result_median_op(&cold_result[5]),
               result_median_op(&cold_result[6]),
               result_median_op(&cold_result[7]),
               result_median_op(&cold_result[12]));
        printf("  %-10s %8s %8s %8s %10s %10s %10s %10s %10.2f %10.2f\n\n",
               "updates", "-", "-", "-",
               "-", "-", "-", "-",
               result_median_op(&update_result[0]),
               result_median_op(&update_result[1]));
    }

    free_results(warmup_result, 17);
    free_results(hot_result, 17);
    free_results(cold_result, 17);
    free_results(update_result, 2);
    munmap(thrash, thrash_len);
    munmap(bk, bk_mem);
    munmap(nodes, node_mem);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(int argc, char **argv)
{
    unsigned table_n = 10000000u;
    unsigned nb_bk   = 0;
    unsigned repeat  = 2000;

    if (argc >= 2) table_n = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) nb_bk   = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) repeat  = (unsigned)strtoul(argv[3], NULL, 10);

    if (nb_bk == 0) {
        /* Default: ~80% fill (slots = table_n / 0.8) */
        nb_bk = 1;
        while ((uint64_t)nb_bk * RIX_HASH64_BUCKET_ENTRY_SZ * 4 <
               (uint64_t)table_n * 5)
            nb_bk <<= 1;
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    /* Measure fill rate characteristics first */
    bench_fill_rate();

    /* Performance measurement */
    bench_find(table_n, nb_bk, repeat);

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
