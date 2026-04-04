/* bench_rix_hash32.c
 *  rix_hash32.h lookup performance benchmark (32-bit key variant)
 *  Metric: CPU cycles per 256 lookups
 *
 *  Usage: ./hash32_bench [table_n [nb_bk [repeat]]]
 *    table_n : number of table entries  (default: 10,000,000)
 *    nb_bk   : number of buckets       (default: auto - ~80% fill of table_n)
 *    repeat  : number of iterations    (default: 2000)
 *
 *  Differences from the 128-bit variant:
 *    - Keys are u32 values directly; no pointer indirection to key data.
 *      -> key prefetch stage (rix_hash_prefetch_key) is not needed/used.
 *    - Since hash_field is not stored in the node, bk_0/bk_1 placement is
 *      determined by re-hashing each slot's key.
 *    - Node size is small (8 bytes), so whole-table working set is compact.
 *      cold-cache mode therefore uses explicit cache thrash before each round.
 *    - cmp_key only checks idx != RIX_NIL (no full key comparison) -> fast.
 *    - Hot/cold comparison uses the same fixed 256-key hit set.
 *      hot-cache: repeated on the same warmed lookup set
 *      cold-cache: same lookup set, but LLC-sized thrash before each round
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rix_hash32.h"

/* ================================================================== */
/* Node definition                                                     */
/* ================================================================== */
typedef struct mynode_s {
    u32 key;
    u32 val;
} mynode_t;

#define BENCH_INVALID_KEY  0xFFFFFFFFu   /* sentinel: never used as a real key */

RIX_HASH32_HEAD(myht32);
RIX_HASH32_GENERATE(myht32, mynode_t, key, BENCH_INVALID_KEY)

/* ================================================================== */
/* TSC measurement helper                                              */
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

static double
now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ================================================================== */
/* Benchmark constants and global buffers                              */
/* ================================================================== */
#define BENCH_N  256                   /* lookups per iteration */
#define BENCH_N6 ((BENCH_N / 6) * 6)  /* 252: for x6 batch */
#define BENCH_N8 ((BENCH_N / 8) * 8)  /* 256: for x8 batch */
#define THRASH_BYTES_MIN (64u * 1024u * 1024u)

/*
 * Key prefetch distance (in batches):
 * ~48 cy per hash_key batch (CRC32x8 + prefetchx8x1)
 * DRAM latency ~300 cy -> 300/48 ~ 7 batches needed -> use 8/11 with margin
 */
#define KPD8  8   /* x8 pipeline: 8 batches x 8 keys = 64 keys ahead */
#define KPD6 11   /* x6 pipeline: 11 batches x 6 keys = 66 keys ahead */

static struct rix_hash32_find_ctx_s g_ctx[BENCH_N];
static mynode_t                    *g_res[BENCH_N];
static unsigned                     g_idx[BENCH_N];
static volatile u64            g_thrash_sink;

struct bench_result_s {
    const char *label;
    u64 min_cy;
    u64 sum_cy;
    u64 *samples;
    unsigned sample_count;
    int      ops;
};

/* ================================================================== */
/* xorshift64 PRNG (for key pool generation)                          */
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

static int
cmp_u64(const void *a, const void *b)
{
    u64 av = *(const u64 *)a;
    u64 bv = *(const u64 *)b;
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
find_idx_from_ctx(struct rix_hash32_find_ctx_s *ctx)
{
    u32 hits = ctx->hits[0];
    if (hits) {
        unsigned bit = (unsigned)__builtin_ctz(hits);
        return ctx->bk[0]->idx[bit];
    }
    hits = rix_hash_arch->find_u32x16(ctx->bk[1]->key, ctx->key);
    if (hits) {
        unsigned bit = (unsigned)__builtin_ctz(hits);
        return ctx->bk[1]->idx[bit];
    }
    return 0u;
}

static RIX_FORCE_INLINE void
find_idx_from_ctx_n(struct rix_hash32_find_ctx_s *ctx, int n, unsigned *results)
{
    for (int j = 0; j < n; j++)
        results[j] = find_idx_from_ctx(&ctx[j]);
}

static RIX_FORCE_INLINE unsigned
find_idx_single(struct myht32 *head, struct rix_hash32_bucket_s *bk, u32 key)
{
    struct rix_hash32_find_ctx_s ctx;
    RIX_HASH32_HASH_KEY(myht32, &ctx, head, bk, key);
    RIX_HASH32_SCAN_BK(myht32, &ctx, head, bk);
    return find_idx_from_ctx(&ctx);
}

static void
thrash_cache(u8 *buf, size_t len)
{
    u64 sum = 0;

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
        result[i].samples = (u64 *)calloc(repeat, sizeof(u64));
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
    result[0].samples = (u64 *)calloc(repeat, sizeof(u64));
    if (!result[0].samples) { perror("calloc samples"); exit(1); }
    result[0].sample_count = repeat;
    result[0].ops = BENCH_N;

    result[1].label = "insert             ";
    result[1].min_cy = UINT64_MAX;
    result[1].sum_cy = 0;
    result[1].samples = (u64 *)calloc(repeat, sizeof(u64));
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
measure_find_patterns(struct myht32 *head,
                      struct rix_hash32_bucket_s *bk,
                      mynode_t *nodes,
                      const u32 keys[BENCH_N],
                      unsigned repeat,
                      u8 *thrash,
                      size_t thrash_len,
                      struct bench_result_s result[17])
{
    init_find_results(result, repeat);

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_idx[i] = find_idx_single(head, bk, keys[i]);
        u64 cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
        result[0].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_HASH_KEY(myht32, &g_ctx[i], head, bk, keys[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_SCAN_BK(myht32, &g_ctx[i], head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_idx[i] = find_idx_from_ctx(&g_ctx[i]);
        u64 cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
        result[1].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], head, bk, keys + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            find_idx_from_ctx_n(&g_ctx[b * 4], 4, &g_idx[b * 4]);
        u64 cy = tsc_end() - t0;
        if (cy < result[2].min_cy) result[2].min_cy = cy;
        result[2].sum_cy += cy;
        result[2].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, head, bk, keys + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            find_idx_from_ctx_n(&g_ctx[b * 6], 6, &g_idx[b * 6]);
        u64 cy = tsc_end() - t0;
        if (cy < result[3].min_cy) result[3].min_cy = cy;
        result[3].sum_cy += cy;
        result[3].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, head, bk, keys + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            find_idx_from_ctx_n(&g_ctx[b * 8], 8, &g_idx[b * 8]);
        u64 cy = tsc_end() - t0;
        if (cy < result[4].min_cy) result[4].min_cy = cy;
        result[4].sum_cy += cy;
        result[4].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6 / 6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, head, bk, keys + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6 / 6)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf * 6], 6, head, bk, keys + pf * 6);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, head, bk);
        }
        for (int b = 0; b < BENCH_N6 / 6; b++)
            find_idx_from_ctx_n(&g_ctx[b * 6], 6, &g_idx[b * 6]);
        u64 cy = tsc_end() - t0;
        if (cy < result[5].min_cy) result[5].min_cy = cy;
        result[5].sum_cy += cy;
        result[5].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8 / 8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, head, bk, keys + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8 / 8)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf * 8], 8, head, bk, keys + pf * 8);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, head, bk);
        }
        for (int b = 0; b < BENCH_N8 / 8; b++)
            find_idx_from_ctx_n(&g_ctx[b * 8], 8, &g_idx[b * 8]);
        u64 cy = tsc_end() - t0;
        if (cy < result[6].min_cy) result[6].min_cy = cy;
        result[6].sum_cy += cy;
        result[6].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_FIND(myht32, head, bk, nodes, keys[i]);
        u64 cy = tsc_end() - t0;
        if (cy < result[7].min_cy) result[7].min_cy = cy;
        result[7].sum_cy += cy;
        result[7].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_HASH_KEY(myht32, &g_ctx[i], head, bk, keys[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_SCAN_BK(myht32, &g_ctx[i], head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        u64 cy = tsc_end() - t0;
        if (cy < result[8].min_cy) result[8].min_cy = cy;
        result[8].sum_cy += cy;
        result[8].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_HASH_KEY(myht32, &g_ctx[i], head, bk, keys[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_SCAN_BK(myht32, &g_ctx[i], head, bk);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_PREFETCH_NODE(myht32, &g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        u64 cy = tsc_end() - t0;
        if (cy < result[9].min_cy) result[9].min_cy = cy;
        result[9].sum_cy += cy;
        result[9].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_HASH_KEY2(myht32, &g_ctx[b * 2], head, bk, keys + b * 2);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_SCAN_BK2(myht32, &g_ctx[b * 2], head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_PREFETCH_NODE2(myht32, &g_ctx[b * 2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_CMP_KEY2(myht32, &g_ctx[b * 2], nodes, &g_res[b * 2]);
        u64 cy = tsc_end() - t0;
        if (cy < result[10].min_cy) result[10].min_cy = cy;
        result[10].sum_cy += cy;
        result[10].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], head, bk, keys + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        u64 cy = tsc_end() - t0;
        if (cy < result[11].min_cy) result[11].min_cy = cy;
        result[11].sum_cy += cy;
        result[11].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], head, bk, keys + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_PREFETCH_NODE4(myht32, &g_ctx[b * 4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        u64 cy = tsc_end() - t0;
        if (cy < result[12].min_cy) result[12].min_cy = cy;
        result[12].sum_cy += cy;
        result[12].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, head, bk, keys + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        u64 cy = tsc_end() - t0;
        if (cy < result[13].min_cy) result[13].min_cy = cy;
        result[13].sum_cy += cy;
        result[13].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, head, bk, keys + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        u64 cy = tsc_end() - t0;
        if (cy < result[14].min_cy) result[14].min_cy = cy;
        result[14].sum_cy += cy;
        result[14].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6 / 6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, head, bk, keys + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6 / 6)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf * 6], 6, head, bk, keys + pf * 6);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, head, bk);
        }
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        u64 cy = tsc_end() - t0;
        if (cy < result[15].min_cy) result[15].min_cy = cy;
        result[15].sum_cy += cy;
        result[15].samples[r] = cy;
    }

    for (unsigned r = 0; r < repeat; r++) {
        if (thrash)
            thrash_cache(thrash, thrash_len);
        u64 t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8 / 8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, head, bk, keys + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8 / 8)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf * 8], 8, head, bk, keys + pf * 8);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, head, bk);
        }
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        u64 cy = tsc_end() - t0;
        if (cy < result[16].min_cy) result[16].min_cy = cy;
        result[16].sum_cy += cy;
        result[16].samples[r] = cy;
    }
}

static void
measure_update_patterns(struct myht32 *head,
                        struct rix_hash32_bucket_s *bk,
                        mynode_t *nodes,
                        unsigned repeat,
                        struct bench_result_s result[2])
{
    init_update_results(result, repeat);

    for (unsigned r = 0; r < repeat; r++) {
        u64 t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_REMOVE(myht32, head, bk, nodes, &nodes[i]);
        u64 cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
        result[0].samples[r] = cy;
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_INSERT(myht32, head, bk, nodes, &nodes[i]);
    }

    for (int i = 0; i < BENCH_N; i++)
        RIX_HASH32_REMOVE(myht32, head, bk, nodes, &nodes[i]);

    for (unsigned r = 0; r < repeat; r++) {
        u64 t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_INSERT(myht32, head, bk, nodes, &nodes[i]);
        u64 cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
        result[1].samples[r] = cy;
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_REMOVE(myht32, head, bk, nodes, &nodes[i]);
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
/* bench_find                                                          */
/* ================================================================== */
static void
bench_find(unsigned table_n, unsigned nb_bk, unsigned repeat)
{
    u64 bk0_hits = 0, bk1_hits = 0;
    /* ---- Memory estimate --------------------------------------------- */
    size_t node_mem = (size_t)table_n * sizeof(mynode_t);
    size_t bk_mem   = (size_t)nb_bk   * sizeof(struct rix_hash32_bucket_s);
    size_t lookup_mem = (size_t)BENCH_N * sizeof(u32);
    printf("[BENCH] table_n=%u  nb_bk=%u  slots=%u\n",
           table_n, nb_bk, nb_bk * RIX_HASH_BUCKET_ENTRY_SZ);
    printf("  memory : nodes=%.1f MB  buckets=%.1f MB"
           "  lookup_set=%.3f MB  total=%.1f MB\n",
           node_mem / 1e6, bk_mem / 1e6, lookup_mem / 1e6,
           (node_mem + bk_mem + lookup_mem) / 1e6);

    /* ---- Node allocation and key setup ------------------------------- */
    /*
     * mmap + MADV_HUGEPAGE: reduces TLB misses.
     * Even though nodes are small (8 bytes), 2MB hugepages suppress
     * TLB misses during random access.
     */
    mynode_t *nodes = (mynode_t *)mmap(NULL, node_mem,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes == MAP_FAILED) { perror("mmap nodes"); exit(1); }
    madvise(nodes, node_mem, MADV_HUGEPAGE);

    /*
     * Keys are 1-origin sequential: no duplicates, avoids 0 (RIX_NIL).
     * Random keys via xorshift64 are also valid, but in 32-bit key space
     * the collision probability is table_n^2 / 2^33, ~1.2% for table_n=10M.
     * Sequential keys guarantee zero collisions and a known insert rate.
     */
    for (unsigned i = 0; i < table_n; i++) {
        nodes[i].key = i + 1u;   /* 1-origin: key=0 is avoided though not strictly necessary */
        nodes[i].val = i;
    }

    /* ---- Bucket allocation ------------------------------------------- */
    struct rix_hash32_bucket_s *bk =
        (struct rix_hash32_bucket_s *)mmap(NULL, bk_mem,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk == MAP_FAILED) { perror("mmap bk"); exit(1); }
    madvise(bk, bk_mem, MADV_HUGEPAGE);
    struct myht32 head;
    RIX_HASH32_INIT(myht32, &head, bk, nb_bk);

    /* ---- Insertion --------------------------------------------------- */
    printf("  inserting...\n"); fflush(stdout);
    unsigned n_hit = 0;
    unsigned report_step = table_n / 10;
    if (report_step == 0) report_step = 1;

    /*
     * Insert path measurement (32-bit variant):
     *   bk0_fast: bk_0 has a free slot -> fast path to bk_0
     *   bk1_fast: bk_0 full & bk_1 has a free slot -> fast path to bk_1
     *   kickout : both full -> cuckoo kickout
     */
    u64 ins_bk0_fast = 0, ins_bk1_fast = 0, ins_kickout = 0;

    double t_ins_start = now_sec();
    for (unsigned i = 0; i < table_n; i++) {
        /* Determine insert path before inserting */
        union rix_hash_hash_u _h =
            rix_hash_arch->hash_u32((u32)nodes[i].key, head.rhh_mask);
        unsigned _b0 = _h.val32[0] & head.rhh_mask;
        unsigned _b1 = _h.val32[1] & head.rhh_mask;
        u32 _nilm0 = rix_hash_arch->find_u32x16(bk[_b0].key,
                                                      BENCH_INVALID_KEY);
        u32 _nilm1 = rix_hash_arch->find_u32x16(bk[_b1].key,
                                                      BENCH_INVALID_KEY);
        if      (_nilm0) ins_bk0_fast++;
        else if (_nilm1) ins_bk1_fast++;
        else             ins_kickout++;

        if (RIX_HASH32_INSERT(myht32, &head, bk, nodes, &nodes[i]) == NULL)
            n_hit++;
        if ((i + 1) % report_step == 0) {
            printf("    %3u%%\r",
                   (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    double t_ins = now_sec() - t_ins_start;
    double fill = 100.0 * n_hit / ((double)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ);
    printf("  inserted : %u/%u (%.1f%% fill)  %.2f s  %.1f ns/insert\n",
           n_hit, table_n, fill, t_ins, t_ins * 1e9 / table_n);
    printf("  insert paths: bk0_fast=%" PRIu64 "  bk1_fast=%" PRIu64
           "  kickout=%" PRIu64 "\n",
           ins_bk0_fast, ins_bk1_fast, ins_kickout);

    /* ---- bk_0 hit rate measurement ----------------------------------- */
    /*
     * Scan all bucket slots to verify bk_0/bk_1 placement.
     * Since hash_field is not stored in the node, the primary bucket
     * (bk_0 = h.val32[0] & mask) is determined by re-hashing each slot's key.
     */
    {
        unsigned mask = head.rhh_mask;

        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
                u32 nidx = bk[b].idx[s];
                if (nidx == (u32)RIX_NIL)
                    continue;
                u32 key = bk[b].key[s];
                union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);
                if (b == (h.val32[0] & mask))
                    bk0_hits++;
                else
                    bk1_hits++;
            }
        }

        u64 total = bk0_hits + bk1_hits;
        printf("  bk_0 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (no bk_1 access needed during find)\n",
               bk0_hits, total, 100.0 * bk0_hits / total);
        printf("  bk_1 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (bk_1 access required after bk_0 miss)\n",
               bk1_hits, total, 100.0 * bk1_hits / total);
    }

    /* ---- Fixed hot/cold lookup set ----------------------------------- */
    u32 hot_keys[BENCH_N];
    for (int i = 0; i < BENCH_N; i++) {
        unsigned idx = (unsigned)(xorshift64() % n_hit);
        hot_keys[i] = nodes[idx].key;
    }

    size_t thrash_len = THRASH_BYTES_MIN;
    u8 *thrash = (u8 *)mmap(NULL, thrash_len,
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
               "hash32", fill, bk0_rate, "hot",
               result_median_op(&hot_result[0]),
               result_median_op(&hot_result[2]),
               result_median_op(&hot_result[5]),
               result_median_op(&hot_result[6]),
               result_median_op(&hot_result[7]),
               result_median_op(&hot_result[12]));
        printf("  %-10s %8.1f %8.2f %8s %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f\n",
               "hash32", fill, bk0_rate, "cold",
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
    unsigned table_n = 10000000u; /* 10M: realistic flow-table size */
    unsigned nb_bk   = 0;
    unsigned repeat  = 2000;

    if (argc >= 2) table_n = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) nb_bk   = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) repeat  = (unsigned)strtoul(argv[3], NULL, 10);

    if (nb_bk == 0) {
        /* Default: target ~80% fill (slots = table_n / 0.8) */
        nb_bk = 1;
        while ((u64)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ * 4 <
               (u64)table_n * 5)
            nb_bk <<= 1;
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
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
