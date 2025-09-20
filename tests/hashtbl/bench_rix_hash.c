/* bench_rix_hash.c
 *  rix_hash.h lookup performance benchmark
 *  Metric: CPU cycles per 256 lookups
 *
 *  Usage: ./hash_bench [table_n [nb_bk [repeat]]]
 *    table_n : number of table entries  (default: 100,000,000)
 *    nb_bk   : number of buckets       (default: auto - ~50% fill of table_n)
 *    repeat  : number of iterations    (default: 2000)
 *
 *  Cache-cold measurement:
 *    repeat x 256 random keys are pre-generated; each iteration accesses
 *    different buckets. Total bucket memory (~2GB) >> L3 cache, so most
 *    accesses result in DRAM misses.
 *    Each pattern runs in an independent loop, starting from the thrashed
 *    cache state left by the previous pattern.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rix_hash.h"

/* ================================================================== */
/* Node definition and hash function                                   */
/* ================================================================== */
struct mykey {
    uint64_t hi;
};

struct mynode {
    uint32_t     cur_hash; /* hash_field: hash of the current bucket (updated on kickout) */
    uint32_t     _pad;     /* alignment padding */
    struct mykey key;      /* key_field: 8-byte key */
};

/* 8-byte key comparison */
static RIX_FORCE_INLINE int
mykey_cmp(const struct mykey *a, const struct mykey *b)
{
    return (a->hi != b->hi) ? 1 : 0;
}

RIX_HASH_HEAD(myht);
RIX_HASH_GENERATE(myht, mynode, key, cur_hash, mykey_cmp)

/* ================================================================== */
/* keyonly variant: no hash_field in node (8-byte node)                  */
/* ================================================================== */
struct mynode_keyonly {
    struct mykey key;        /* key at offset 0, no hash_field        */
};

RIX_HASH_HEAD(myht_keyonly);
RIX_HASH_GENERATE_KEYONLY(myht_keyonly, mynode_keyonly, key, mykey_cmp)

/* ================================================================== */
/* slot variant: hash_field + slot_field in node                       */
/* ================================================================== */
struct mynode_slot {
    uint32_t     cur_hash; /* hash_field */
    uint16_t     slot;     /* slot_field: slot in current bucket */
    uint16_t     _pad;
    struct mykey key;
};

RIX_HASH_HEAD(myht_slot);
RIX_HASH_GENERATE_SLOT(myht_slot, mynode_slot, key, cur_hash, slot, mykey_cmp)

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
/* Benchmark constants and global buffers                              */
/* ================================================================== */
#define BENCH_N  256                   /* lookups per iteration */
#define BENCH_N6 ((BENCH_N / 6) * 6)  /* 252: for x6 batch (floor of 256/6) */
#define BENCH_N8 ((BENCH_N / 8) * 8)  /* 256: for x8 batch */

/* Key prefetch distance (in batches): ~64 keys ahead ~ 1 DRAM latency */
#define KPD4  16  /* 16 batches x 4 = 64 keys */
#define KPD6  11  /* 11 batches x 6 = 66 keys */
#define KPD8   8  /*  8 batches x 8 = 64 keys */

static struct rix_hash_find_ctx_s g_ctx[BENCH_N];
static struct mynode             *g_res[BENCH_N];
static struct mynode_keyonly        *g_res_keyonly[BENCH_N];
static struct mynode_slot        *g_res_slot[BENCH_N];

/* ================================================================== */
/* xorshift64 PRNG (for key pool generation)                          */
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

struct bench_result_s {
    const char *label;
    uint64_t min_cy;
    uint64_t sum_cy;
    uint64_t *samples;
    unsigned sample_count;
    int      ops;
};

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

    if (n == 0u)
        return 0.0;

    qsort(result->samples, n, sizeof(result->samples[0]), cmp_u64);
    if (n & 1u)
        return (double)result->samples[n / 2u];

    return ((double)result->samples[(n / 2u) - 1u]
            + (double)result->samples[n / 2u]) * 0.5;
}

static double
result_median_op(struct bench_result_s *result)
{
    return result_median_cy(result) / (double)result->ops;
}

static void
record_result(struct bench_result_s *result, unsigned run, uint64_t cy)
{
    if (cy < result->min_cy)
        result->min_cy = cy;
    result->sum_cy += cy;
    result->samples[run] = cy;
}

static void
free_results(struct bench_result_s *result, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        free(result[i].samples);
        result[i].samples = NULL;
        result[i].sample_count = 0u;
    }
}

/* ================================================================== */
/* bench_find                                                          */
/* ================================================================== */
static void
bench_find(unsigned table_n, unsigned nb_bk, unsigned repeat, int rand_keys)
{
    uint64_t fp_bk0 = 0, fp_bk1 = 0;
    uint64_t keyonly_bk0 = 0, keyonly_bk1 = 0;
    uint64_t slot_bk0 = 0, slot_bk1 = 0;
    /* ---- Memory estimate --------------------------------------------- */
    size_t node_mem      = (size_t)table_n * sizeof(struct mynode);
    size_t node_keyonly_mem = (size_t)table_n * sizeof(struct mynode_keyonly);
    size_t node_slot_mem = (size_t)table_n * sizeof(struct mynode_slot);
    size_t bk_mem        = (size_t)nb_bk   * sizeof(struct rix_hash_bucket_s);
    size_t pool_mem      = (size_t)repeat * BENCH_N * sizeof(void *);
    printf("[BENCH] table_n=%u  nb_bk=%u  slots=%u  keys=%s\n",
           table_n, nb_bk, nb_bk * RIX_HASH_BUCKET_ENTRY_SZ,
           rand_keys ? "random" : "sequential");
    printf("  memory : nodes=%.1f MB  buckets=%.1f MB"
           "  lookup_set=%.1f MB  total=%.1f MB\n",
           node_mem / 1e6, bk_mem / 1e6, pool_mem / 1e6,
           (node_mem + bk_mem + pool_mem) / 1e6);
    printf("  keyonly   : nodes=%.1f MB  (node: %zu B)\n",
           node_keyonly_mem / 1e6, sizeof(struct mynode_keyonly));
    printf("  slot   : nodes=%.1f MB  (node: %zu B)\n",
           node_slot_mem / 1e6, sizeof(struct mynode_slot));

    /* ---- Node allocation and key setup ------------------------------- */
    /*
     * mmap + MADV_HUGEPAGE: reduces TLB misses.
     * With 4KB pages, random access across 1.6GB far exceeds L2 TLB capacity
     * (1536 entries), adding a page walk (~20-100 cycles) per access.
     * 2MB hugepages reduce the required TLB entries by 1/512, nearly
     * eliminating TLB misses.
     */
    struct mynode *nodes = (struct mynode *)mmap(NULL, node_mem,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes == MAP_FAILED) { perror("mmap nodes"); exit(1); }
    madvise(nodes, node_mem, MADV_HUGEPAGE);
    if (rand_keys) {
        /* Random keys: generate 128-bit key using xorshift64 */
        for (unsigned i = 0; i < table_n; i++) {
            nodes[i].key.hi = xorshift64();
        }
    } else {
        /* Sequential keys: reference to demonstrate CRC32C uniform distribution */
        for (unsigned i = 0; i < table_n; i++) {
            nodes[i].key.hi = (uint64_t)(i + 1);
        }
    }

    /* ---- Bucket allocation ------------------------------------------- */
    struct rix_hash_bucket_s *bk =
        (struct rix_hash_bucket_s *)mmap(NULL, bk_mem,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk == MAP_FAILED) { perror("mmap bk"); exit(1); }
    madvise(bk, bk_mem, MADV_HUGEPAGE);
    memset(bk, 0, bk_mem);

    struct myht head;
    RIX_HASH_INIT(myht, &head, nb_bk);

    /* ---- keyonly node and bucket allocation ----------------------------- */
    struct mynode_keyonly *nodes_keyonly =
        (struct mynode_keyonly *)mmap(NULL, node_keyonly_mem,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes_keyonly == MAP_FAILED) { perror("mmap nodes_keyonly"); exit(1); }
    madvise(nodes_keyonly, node_keyonly_mem, MADV_HUGEPAGE);
    if (rand_keys) {
        for (unsigned i = 0; i < table_n; i++)
            nodes_keyonly[i].key.hi = xorshift64();
    } else {
        for (unsigned i = 0; i < table_n; i++)
            nodes_keyonly[i].key.hi = (uint64_t)(i + 1);
    }

    struct rix_hash_bucket_s *bk_keyonly =
        (struct rix_hash_bucket_s *)mmap(NULL, bk_mem,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk_keyonly == MAP_FAILED) { perror("mmap bk_keyonly"); exit(1); }
    madvise(bk_keyonly, bk_mem, MADV_HUGEPAGE);
    memset(bk_keyonly, 0, bk_mem);

    struct myht_keyonly head_keyonly;
    RIX_HASH_INIT(myht_keyonly, &head_keyonly, nb_bk);

    /* ---- slot node and bucket allocation ----------------------------- */
    struct mynode_slot *nodes_slot =
        (struct mynode_slot *)mmap(NULL, node_slot_mem,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes_slot == MAP_FAILED) { perror("mmap nodes_slot"); exit(1); }
    madvise(nodes_slot, node_slot_mem, MADV_HUGEPAGE);
    if (rand_keys) {
        for (unsigned i = 0; i < table_n; i++)
            nodes_slot[i].key.hi = xorshift64();
    } else {
        for (unsigned i = 0; i < table_n; i++)
            nodes_slot[i].key.hi = (uint64_t)(i + 1);
    }

    struct rix_hash_bucket_s *bk_slot =
        (struct rix_hash_bucket_s *)mmap(NULL, bk_mem,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk_slot == MAP_FAILED) { perror("mmap bk_slot"); exit(1); }
    madvise(bk_slot, bk_mem, MADV_HUGEPAGE);
    memset(bk_slot, 0, bk_mem);

    struct myht_slot head_slot;
    RIX_HASH_INIT(myht_slot, &head_slot, nb_bk);

    /* ---- Insertion --------------------------------------------------- */
    printf("  inserting (keyonly)...\n"); fflush(stdout);
    for (unsigned i = 0; i < table_n; i++) {
        myht_keyonly_insert(&head_keyonly, bk_keyonly, nodes_keyonly, &nodes_keyonly[i]);
        if ((i + 1) % (table_n / 10) == 0) {
            printf("    %3u%%\r", (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    printf("  keyonly inserted : %u entries (%.1f%% fill)\n",
           head_keyonly.rhh_nb,
           100.0 * (double)head_keyonly.rhh_nb /
           ((double)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ));

    printf("  inserting (slot)...\n"); fflush(stdout);
    for (unsigned i = 0; i < table_n; i++) {
        myht_slot_insert(&head_slot, bk_slot, nodes_slot, &nodes_slot[i]);
        if ((i + 1) % (table_n / 10) == 0) {
            printf("    %3u%%\r", (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    printf("  slot inserted : %u entries (%.1f%% fill)\n",
           head_slot.rhh_nb,
           100.0 * (double)head_slot.rhh_nb /
           ((double)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ));

    printf("  inserting...\n"); fflush(stdout);
    unsigned n_hit = 0;
    unsigned report_step = table_n / 10;
    if (report_step == 0) report_step = 1;

    /*
     * Insert path measurement:
     *   bk0_fast : bk_0 has a free slot -> fast path to bk_0
     *   bk1_fast : bk_0 full & bk_1 has a free slot -> fast path to bk_1
     *   kickout  : both full -> cuckoo kickout (new entry always goes to bk_0)
     * Note: duplicate check occurs first, but this bench has no duplicates
     */
    uint64_t ins_bk0_fast = 0, ins_bk1_fast = 0, ins_kickout = 0;

    double t_ins_start = now_sec();
    for (unsigned i = 0; i < table_n; i++) {
        /* Determine insert path before inserting */
        union rix_hash_hash_u _h =
            rix_hash_arch->hash_bytes(&nodes[i].key, sizeof(nodes[i].key),
                                      head.rhh_mask);
        unsigned _b0 = _h.val32[0] & head.rhh_mask;
        unsigned _b1 = _h.val32[1] & head.rhh_mask;
        uint32_t _nilm0 = rix_hash_arch->find_u32x16(bk[_b0].idx,
                                                      (uint32_t)RIX_NIL);
        uint32_t _nilm1 = rix_hash_arch->find_u32x16(bk[_b1].idx,
                                                      (uint32_t)RIX_NIL);
        if      (_nilm0) ins_bk0_fast++;
        else if (_nilm1) ins_bk1_fast++;
        else             ins_kickout++;

        if (myht_insert(&head, bk, nodes, &nodes[i]) == NULL)
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
     * Scan all bucket slots to verify slot occupancy and bk_0/bk_1 placement.
     * Validate the invariant node->hash_field & mask == current_bucket
     * for each slot, and measure the distribution between bk_0 (primary)
     * and bk_1 (secondary).
     *
     * Since hash_field is written even for failed inserts, scan bucket slots
     * directly rather than the node array.
     */
    {
        unsigned mask = head.rhh_mask;

        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk[b].idx[s];
                if (nidx == (uint32_t)RIX_NIL)
                    continue;
                struct mynode *node = &nodes[nidx - 1u]; /* 1-origin */
                /* Verify invariant: node->cur_hash & mask == b */
                if ((node->cur_hash & mask) != b) {
                    printf("  [BUG] slot b=%u s=%u: cur_hash&mask=%u\n",
                           b, s, node->cur_hash & mask);
                }
                /* Determine primary bucket via re-hash */
                union rix_hash_hash_u h =
                    rix_hash_arch->hash_bytes(&node->key, sizeof(node->key),
                                              mask);
                if (b == (h.val32[0] & mask))
                    fp_bk0++;
                else
                    fp_bk1++;
            }
        }

        uint64_t total = fp_bk0 + fp_bk1;
        printf("  bk_0 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (no bk_1 access needed during find)\n",
               fp_bk0, total, 100.0 * fp_bk0 / total);
        printf("  bk_1 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (bk_1 access required after bk_0 miss)\n",
               fp_bk1, total, 100.0 * fp_bk1 / total);
    }

    {
        unsigned mask = head_keyonly.rhh_mask;

        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk_keyonly[b].idx[s];
                if (nidx == (uint32_t)RIX_NIL)
                    continue;
                struct mynode_keyonly *node = &nodes_keyonly[nidx - 1u];
                union rix_hash_hash_u h =
                    rix_hash_arch->hash_bytes(&node->key, sizeof(node->key),
                                              mask);
                if (b == (h.val32[0] & mask))
                    keyonly_bk0++;
                else
                    keyonly_bk1++;
            }
        }
    }

    {
        unsigned mask = head_slot.rhh_mask;

        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk_slot[b].idx[s];
                if (nidx == (uint32_t)RIX_NIL)
                    continue;
                struct mynode_slot *node = &nodes_slot[nidx - 1u];
                union rix_hash_hash_u h =
                    rix_hash_arch->hash_bytes(&node->key, sizeof(node->key),
                                              mask);
                if (b == (h.val32[0] & mask))
                    slot_bk0++;
                else
                    slot_bk1++;
            }
        }
    }

    /* ---- Key pool generation ----------------------------------------- */
    /*
     * Generate repeat x BENCH_N random key pointers.
     * Each iteration accesses BENCH_N distinct (random) buckets, so when
     * total bucket memory (bk_mem) >> L3 cache, most accesses result in
     * DRAM-level cache misses.
     */
    size_t pool_len = (size_t)repeat * BENCH_N;
    const struct mykey **key_pool = (const struct mykey **)malloc(pool_mem);
    if (!key_pool) { perror("malloc key_pool"); exit(1); }

    for (size_t k = 0; k < pool_len; k++) {
        unsigned idx = (unsigned)(xorshift64() % table_n);
        key_pool[k] = &nodes[idx].key;
    }

    /* ---- keyonly key pool generation ------------------------------------ */
    const struct mykey **key_pool_keyonly = (const struct mykey **)malloc(pool_mem);
    if (!key_pool_keyonly) { perror("malloc key_pool_keyonly"); exit(1); }
    for (size_t k = 0; k < pool_len; k++) {
        unsigned idx = (unsigned)(xorshift64() % table_n);
        key_pool_keyonly[k] = &nodes_keyonly[idx].key;
    }

    /* ---- slot key pool generation ------------------------------------ */
    const struct mykey **key_pool_slot = (const struct mykey **)malloc(pool_mem);
    if (!key_pool_slot) { perror("malloc key_pool_slot"); exit(1); }
    for (size_t k = 0; k < pool_len; k++) {
        unsigned idx = (unsigned)(xorshift64() % table_n);
        key_pool_slot[k] = &nodes_slot[idx].key;
    }

    /* ---- Warmup (instruction cache and branch predictor only) --------- */
    /*
     * Pre-execute inner loop instructions for the first 20 iterations
     * (key_pool[0..20*BENCH_N-1]). Bucket data is too large to remain
     * in L3 after this warmup.
     */
    printf("  warmup...\n"); fflush(stdout);
    for (unsigned w = 0; w < 20 && w < repeat; w++) {
        const struct mykey **wk = key_pool + (size_t)w * BENCH_N;
        for (int i = 0; i < BENCH_N; i++) g_res[i] = myht_find(&head, bk, nodes, wk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_hash_key(&g_ctx[i], &head, bk, wk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_scan_bk (&g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++) g_res[i] = myht_cmp_key(&g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++) rix_hash_prefetch_key(wk[i]);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, wk + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_HASH_KEY_N(myht, &g_ctx[b * 6], 6, &head, bk, wk + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_SCAN_BK_N(myht, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_CMP_KEY_N(myht, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_HASH_KEY_N(myht, &g_ctx[b * 8], 8, &head, bk, wk + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_SCAN_BK_N(myht, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_CMP_KEY_N(myht, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        /* keyonly warmup */
        const struct mykey **nwk = key_pool_keyonly + (size_t)w * BENCH_N;
        for (int i = 0; i < BENCH_N; i++) g_res_keyonly[i] = myht_keyonly_find(&head_keyonly, bk_keyonly, nodes_keyonly, nwk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_keyonly_hash_key(&g_ctx[i], &head_keyonly, bk_keyonly, nwk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_keyonly_scan_bk(&g_ctx[i], &head_keyonly, bk_keyonly);
        for (int i = 0; i < BENCH_N; i++) g_res_keyonly[i] = myht_keyonly_cmp_key(&g_ctx[i], nodes_keyonly);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_HASH_KEY4(myht_keyonly, &g_ctx[b * 4], &head_keyonly, bk_keyonly, nwk + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_SCAN_BK4(myht_keyonly, &g_ctx[b * 4], &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_CMP_KEY4(myht_keyonly, &g_ctx[b * 4], nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 4]);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_HASH_KEY_N(myht_keyonly, &g_ctx[b * 6], 6, &head_keyonly, bk_keyonly, nwk + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_SCAN_BK_N(myht_keyonly, &g_ctx[b * 6], 6, &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_CMP_KEY_N(myht_keyonly, &g_ctx[b * 6], 6, nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 6]);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_HASH_KEY_N(myht_keyonly, &g_ctx[b * 8], 8, &head_keyonly, bk_keyonly, nwk + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_SCAN_BK_N(myht_keyonly, &g_ctx[b * 8], 8, &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_CMP_KEY_N(myht_keyonly, &g_ctx[b * 8], 8, nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 8]);
        /* slot warmup */
        const struct mykey **swk = key_pool_slot + (size_t)w * BENCH_N;
        for (int i = 0; i < BENCH_N; i++) g_res_slot[i] = myht_slot_find(&head_slot, bk_slot, nodes_slot, swk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_slot_hash_key(&g_ctx[i], &head_slot, bk_slot, swk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_slot_scan_bk(&g_ctx[i], &head_slot, bk_slot);
        for (int i = 0; i < BENCH_N; i++) g_res_slot[i] = myht_slot_cmp_key(&g_ctx[i], nodes_slot);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_HASH_KEY4(myht_slot, &g_ctx[b * 4], &head_slot, bk_slot, swk + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_SCAN_BK4(myht_slot, &g_ctx[b * 4], &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N / 4; b++) RIX_HASH_CMP_KEY4(myht_slot, &g_ctx[b * 4], nodes_slot, (struct mynode_slot **)&g_res_slot[b * 4]);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_HASH_KEY_N(myht_slot, &g_ctx[b * 6], 6, &head_slot, bk_slot, swk + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_SCAN_BK_N(myht_slot, &g_ctx[b * 6], 6, &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N6 / 6; b++) RIX_HASH_CMP_KEY_N(myht_slot, &g_ctx[b * 6], 6, nodes_slot, (struct mynode_slot **)&g_res_slot[b * 6]);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_HASH_KEY_N(myht_slot, &g_ctx[b * 8], 8, &head_slot, bk_slot, swk + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_SCAN_BK_N(myht_slot, &g_ctx[b * 8], 8, &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N8 / 8; b++) RIX_HASH_CMP_KEY_N(myht_slot, &g_ctx[b * 8], 8, nodes_slot, (struct mynode_slot **)&g_res_slot[b * 8]);
    }

    /* ---- Measurement ------------------------------------------------- */
    /*
     * Measure each pattern in an independent loop. Since the previous
     * pattern traversed the same key pool, L3 is already thrashed ->
     * each pattern starts effectively cache-cold.
     */
    printf("  measuring...\n"); fflush(stdout);

    struct bench_result_s result[] = {
        /* -- fp variant ------------------------------------------------ */
        { "find (single)      ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [0]  */
        { "x1 (bk only)       ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [1]  */
        { "x1 (bk+node)       ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [2]  */
        { "x4 (bk only)       ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [3]  */
        { "x4 (bk+node)       ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [4]  */
        { "x2 (bk+node)       ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [5]  */
        { "x4 (key+bk) bulk   ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [6]  */
        { "x4 (key+bk) pipeline ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [7]  */
        { "x6 (bk+node)       ", UINT64_MAX, 0, NULL, 0u, BENCH_N6 }, /* [8]  */
        { "x6 (key+bk) pipeline ", UINT64_MAX, 0, NULL, 0u, BENCH_N6 }, /* [9]  */
        { "x8 (bk+node)       ", UINT64_MAX, 0, NULL, 0u, BENCH_N8 }, /* [10] */
        { "x8 (key+bk) pipeline ", UINT64_MAX, 0, NULL, 0u, BENCH_N8 }, /* [11] */
        /* -- keyonly variant ---------------------------------------------- */
        { "keyonly find (single) ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [12] */
        { "keyonly x1 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [13] */
        { "keyonly x4 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [14] */
        { "keyonly x6 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N6 }, /* [15] */
        { "keyonly x6 pipeline     ", UINT64_MAX, 0, NULL, 0u, BENCH_N6 }, /* [16] */
        { "keyonly x8 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N8 }, /* [17] */
        { "keyonly x8 pipeline     ", UINT64_MAX, 0, NULL, 0u, BENCH_N8 }, /* [18] */
        /* -- slot variant ---------------------------------------------- */
        { "slot find (single) ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [19] */
        { "slot x1 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [20] */
        { "slot x4 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [21] */
        { "slot x6 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N6 }, /* [22] */
        { "slot x6 pipeline     ", UINT64_MAX, 0, NULL, 0u, BENCH_N6 }, /* [23] */
        { "slot x8 (bk+node)  ", UINT64_MAX, 0, NULL, 0u, BENCH_N8 }, /* [24] */
        { "slot x8 pipeline     ", UINT64_MAX, 0, NULL, 0u, BENCH_N8 }, /* [25] */
        /* -- insert/remove --------------------------------------------- */
        { "remove fp          ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [26] */
        { "remove keyonly        ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [27] */
        { "remove slot        ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [28] */
        { "insert fp          ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [29] */
        { "insert keyonly        ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [30] */
        { "insert slot        ", UINT64_MAX, 0, NULL, 0u, BENCH_N  }, /* [31] */
    };
    const int NR = (int)(sizeof(result) / sizeof(result[0]));
    for (int p = 0; p < NR; p++) {
        result[p].samples = (uint64_t *)calloc(repeat, sizeof(result[p].samples[0]));
        if (!result[p].samples) { perror("calloc samples"); exit(1); }
        result[p].sample_count = repeat;
    }

    /* ---- [0] Sequential find ----------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = myht_find(&head, bk, nodes, ik[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[0], r, cy);
    }

    /* ---- [1] x1: hash_key -> scan_bk -> cmp_key (node not prefetched) -- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_hash_key(&g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            myht_scan_bk(&g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = myht_cmp_key(&g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[1], r, cy);
    }

    /* ---- [2] x1: hash_key -> scan_bk -> prefetch_node -> cmp_key --- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_hash_key(&g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            myht_scan_bk(&g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            myht_prefetch_node(&g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = myht_cmp_key(&g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[2], r, cy);
    }

    /* ---- [3] x4: hash_key4 -> scan_bk4 -> cmp_key4 (node not prefetched) */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[3], r, cy);
    }

    /* ---- [4] x4: hash_key4 -> scan_bk4 -> prefetch_node4 -> cmp_key4 */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_PREFETCH_NODE4(myht, &g_ctx[b * 4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[4], r, cy);
    }

    /* ---- [5] x2: hash_key2 -> scan_bk2 -> prefetch_node2 -> cmp_key2 */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH_HASH_KEY2(myht, &g_ctx[b * 2], &head, bk, ik + b * 2);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH_SCAN_BK2(myht, &g_ctx[b * 2], &head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH_PREFETCH_NODE2(myht, &g_ctx[b * 2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH_CMP_KEY2(myht, &g_ctx[b * 2], nodes, &g_res[b * 2]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[5], r, cy);
    }

    /* ---- [6] x4: prefetch_key x 256 -> hash_key4 -> scan_bk4 -> cmp_key4 */
    /*
     * Stage 0: pre-load all keys from DRAM.
     * Purpose: hide DRAM misses for key data in hash_fn(key).
     * Since key_field is at the start of the node, key comparison in
     * cmp_key is also hidden simultaneously.
     */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            rix_hash_prefetch_key(ik[i]);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[6], r, cy);
    }

    /* ---- [7] x4: N-ahead sliding window key prefetch ----------------- */
    /*
     * KPD4 = 16 batches (64 keys). If hash_key4 with key-in-cache takes
     * ~20 cy/batch, then 16 batches ~ 320 cy ~ DRAM latency -> 1 latency ahead.
     */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD4 && b < BENCH_N / 4; b++)
            for (int j = 0; j < 4; j++)
                rix_hash_prefetch_key(ik[b * 4 + j]);
        for (int b = 0; b < BENCH_N / 4; b++) {
            int pf = b + KPD4;
            if (pf < BENCH_N / 4)
                for (int j = 0; j < 4; j++)
                    rix_hash_prefetch_key(ik[pf * 4 + j]);
            RIX_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        }
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[7], r, cy);
    }

    /* ---- [8] x6: hash_key_n(6) -> scan_bk_n(6) -> prefetch_node_n(6) -> cmp_key_n(6) */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_HASH_KEY_N(myht, &g_ctx[b * 6], 6, &head, bk, ik + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_SCAN_BK_N(myht, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_PREFETCH_NODE_N(myht, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_CMP_KEY_N(myht, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[8], r, cy);
    }

    /* ---- [9] x6: N-ahead sliding window key prefetch ----------------- */
    /* KPD6 = 11 batches (66 keys) ~ 1 DRAM latency */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6 / 6; b++)
            for (int j = 0; j < 6; j++)
                rix_hash_prefetch_key(ik[b * 6 + j]);
        for (int b = 0; b < BENCH_N6 / 6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6 / 6)
                for (int j = 0; j < 6; j++)
                    rix_hash_prefetch_key(ik[pf * 6 + j]);
            RIX_HASH_HASH_KEY_N(myht, &g_ctx[b * 6], 6, &head, bk, ik + b * 6);
        }
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_SCAN_BK_N(myht, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_CMP_KEY_N(myht, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[9], r, cy);
    }

    /* ---- [10] x8: hash_key_n(8) -> scan_bk_n(8) -> prefetch_node_n(8) -> cmp_key_n(8) */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_HASH_KEY_N(myht, &g_ctx[b * 8], 8, &head, bk, ik + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_SCAN_BK_N(myht, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_PREFETCH_NODE_N(myht, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_CMP_KEY_N(myht, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[10], r, cy);
    }

    /* ---- [11] x8: N-ahead sliding window key prefetch ---------------- */
    /* KPD8 = 8 batches (64 keys) ~ 1 DRAM latency */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8 / 8; b++)
            for (int j = 0; j < 8; j++)
                rix_hash_prefetch_key(ik[b * 8 + j]);
        for (int b = 0; b < BENCH_N8 / 8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8 / 8)
                for (int j = 0; j < 8; j++)
                    rix_hash_prefetch_key(ik[pf * 8 + j]);
            RIX_HASH_HASH_KEY_N(myht, &g_ctx[b * 8], 8, &head, bk, ik + b * 8);
        }
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_SCAN_BK_N(myht, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_CMP_KEY_N(myht, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[11], r, cy);
    }

    /* ================================================================ */
    /* keyonly variant benchmarks                                          */
    /* ================================================================ */

    /* ---- [12] keyonly find (single) ------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_keyonly + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res_keyonly[i] = myht_keyonly_find(&head_keyonly, bk_keyonly, nodes_keyonly, ik[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[12], r, cy);
    }

    /* ---- [13] keyonly x1: hash_key -> scan_bk -> prefetch_node -> cmp_key */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_keyonly + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_keyonly_hash_key(&g_ctx[i], &head_keyonly, bk_keyonly, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            myht_keyonly_scan_bk(&g_ctx[i], &head_keyonly, bk_keyonly);
        for (int i = 0; i < BENCH_N; i++)
            myht_keyonly_prefetch_node(&g_ctx[i], nodes_keyonly);
        for (int i = 0; i < BENCH_N; i++)
            g_res_keyonly[i] = myht_keyonly_cmp_key(&g_ctx[i], nodes_keyonly);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[13], r, cy);
    }

    /* ---- [14] keyonly x4: hash_key4 -> scan_bk4 -> prefetch_node4 -> cmp_key4 */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_keyonly + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_HASH_KEY4(myht_keyonly, &g_ctx[b * 4], &head_keyonly, bk_keyonly, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_SCAN_BK4(myht_keyonly, &g_ctx[b * 4], &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_PREFETCH_NODE4(myht_keyonly, &g_ctx[b * 4], nodes_keyonly);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_CMP_KEY4(myht_keyonly, &g_ctx[b * 4], nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 4]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[14], r, cy);
    }

    /* ---- [15] keyonly x6: staged ---------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_keyonly + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_HASH_KEY_N(myht_keyonly, &g_ctx[b * 6], 6, &head_keyonly, bk_keyonly, ik + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_SCAN_BK_N(myht_keyonly, &g_ctx[b * 6], 6, &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_PREFETCH_NODE_N(myht_keyonly, &g_ctx[b * 6], 6, nodes_keyonly);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_CMP_KEY_N(myht_keyonly, &g_ctx[b * 6], 6, nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 6]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[15], r, cy);
    }

    /* ---- [16] keyonly x6: N-ahead key prefetch -------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_keyonly + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6 / 6; b++)
            for (int j = 0; j < 6; j++)
                rix_hash_prefetch_key(ik[b * 6 + j]);
        for (int b = 0; b < BENCH_N6 / 6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6 / 6)
                for (int j = 0; j < 6; j++)
                    rix_hash_prefetch_key(ik[pf * 6 + j]);
            RIX_HASH_HASH_KEY_N(myht_keyonly, &g_ctx[b * 6], 6, &head_keyonly, bk_keyonly, ik + b * 6);
        }
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_SCAN_BK_N(myht_keyonly, &g_ctx[b * 6], 6, &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_CMP_KEY_N(myht_keyonly, &g_ctx[b * 6], 6, nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 6]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[16], r, cy);
    }

    /* ---- [17] keyonly x8: staged ---------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_keyonly + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_HASH_KEY_N(myht_keyonly, &g_ctx[b * 8], 8, &head_keyonly, bk_keyonly, ik + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_SCAN_BK_N(myht_keyonly, &g_ctx[b * 8], 8, &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_PREFETCH_NODE_N(myht_keyonly, &g_ctx[b * 8], 8, nodes_keyonly);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_CMP_KEY_N(myht_keyonly, &g_ctx[b * 8], 8, nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 8]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[17], r, cy);
    }

    /* ---- [18] keyonly x8: N-ahead key prefetch -------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_keyonly + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8 / 8; b++)
            for (int j = 0; j < 8; j++)
                rix_hash_prefetch_key(ik[b * 8 + j]);
        for (int b = 0; b < BENCH_N8 / 8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8 / 8)
                for (int j = 0; j < 8; j++)
                    rix_hash_prefetch_key(ik[pf * 8 + j]);
            RIX_HASH_HASH_KEY_N(myht_keyonly, &g_ctx[b * 8], 8, &head_keyonly, bk_keyonly, ik + b * 8);
        }
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_SCAN_BK_N(myht_keyonly, &g_ctx[b * 8], 8, &head_keyonly, bk_keyonly);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_CMP_KEY_N(myht_keyonly, &g_ctx[b * 8], 8, nodes_keyonly, (struct mynode_keyonly **)&g_res_keyonly[b * 8]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[18], r, cy);
    }

    /* ================================================================ */
    /* slot variant benchmarks                                          */
    /* ================================================================ */

    /* ---- [19] slot find (single) ------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_slot + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res_slot[i] = myht_slot_find(&head_slot, bk_slot, nodes_slot, ik[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[19], r, cy);
    }

    /* ---- [20] slot x1: hash_key -> scan_bk -> prefetch_node -> cmp_key */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_slot + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_slot_hash_key(&g_ctx[i], &head_slot, bk_slot, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            myht_slot_scan_bk(&g_ctx[i], &head_slot, bk_slot);
        for (int i = 0; i < BENCH_N; i++)
            myht_slot_prefetch_node(&g_ctx[i], nodes_slot);
        for (int i = 0; i < BENCH_N; i++)
            g_res_slot[i] = myht_slot_cmp_key(&g_ctx[i], nodes_slot);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[20], r, cy);
    }

    /* ---- [21] slot x4: hash_key4 -> scan_bk4 -> prefetch_node4 -> cmp_key4 */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_slot + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_HASH_KEY4(myht_slot, &g_ctx[b * 4], &head_slot, bk_slot, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_SCAN_BK4(myht_slot, &g_ctx[b * 4], &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_PREFETCH_NODE4(myht_slot, &g_ctx[b * 4], nodes_slot);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH_CMP_KEY4(myht_slot, &g_ctx[b * 4], nodes_slot, (struct mynode_slot **)&g_res_slot[b * 4]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[21], r, cy);
    }

    /* ---- [22] slot x6: staged ---------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_slot + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_HASH_KEY_N(myht_slot, &g_ctx[b * 6], 6, &head_slot, bk_slot, ik + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_SCAN_BK_N(myht_slot, &g_ctx[b * 6], 6, &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_PREFETCH_NODE_N(myht_slot, &g_ctx[b * 6], 6, nodes_slot);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_CMP_KEY_N(myht_slot, &g_ctx[b * 6], 6, nodes_slot, (struct mynode_slot **)&g_res_slot[b * 6]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[22], r, cy);
    }

    /* ---- [23] slot x6: N-ahead key prefetch -------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_slot + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6 / 6; b++)
            for (int j = 0; j < 6; j++)
                rix_hash_prefetch_key(ik[b * 6 + j]);
        for (int b = 0; b < BENCH_N6 / 6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6 / 6)
                for (int j = 0; j < 6; j++)
                    rix_hash_prefetch_key(ik[pf * 6 + j]);
            RIX_HASH_HASH_KEY_N(myht_slot, &g_ctx[b * 6], 6, &head_slot, bk_slot, ik + b * 6);
        }
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_SCAN_BK_N(myht_slot, &g_ctx[b * 6], 6, &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH_CMP_KEY_N(myht_slot, &g_ctx[b * 6], 6, nodes_slot, (struct mynode_slot **)&g_res_slot[b * 6]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[23], r, cy);
    }

    /* ---- [24] slot x8: staged ---------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_slot + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_HASH_KEY_N(myht_slot, &g_ctx[b * 8], 8, &head_slot, bk_slot, ik + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_SCAN_BK_N(myht_slot, &g_ctx[b * 8], 8, &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_PREFETCH_NODE_N(myht_slot, &g_ctx[b * 8], 8, nodes_slot);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_CMP_KEY_N(myht_slot, &g_ctx[b * 8], 8, nodes_slot, (struct mynode_slot **)&g_res_slot[b * 8]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[24], r, cy);
    }

    /* ---- [25] slot x8: N-ahead key prefetch -------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_slot + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8 / 8; b++)
            for (int j = 0; j < 8; j++)
                rix_hash_prefetch_key(ik[b * 8 + j]);
        for (int b = 0; b < BENCH_N8 / 8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8 / 8)
                for (int j = 0; j < 8; j++)
                    rix_hash_prefetch_key(ik[pf * 8 + j]);
            RIX_HASH_HASH_KEY_N(myht_slot, &g_ctx[b * 8], 8, &head_slot, bk_slot, ik + b * 8);
        }
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_SCAN_BK_N(myht_slot, &g_ctx[b * 8], 8, &head_slot, bk_slot);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH_CMP_KEY_N(myht_slot, &g_ctx[b * 8], 8, nodes_slot, (struct mynode_slot **)&g_res_slot[b * 8]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[25], r, cy);
    }

    /* ================================================================ */
    /* insert/remove benchmarks (all variants)                          */
    /* ================================================================ */

    /* ---- [26] remove fp: hash_field & mask -> O(1) bucket lookup ----- */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_remove(&head, bk, nodes, &nodes[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[26], r, cy);
        for (int i = 0; i < BENCH_N; i++)
            myht_insert(&head, bk, nodes, &nodes[i]);
    }

    /* ---- [27] remove keyonly: re-hash key -> scan bk_0/bk_1 ------------ */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_keyonly_remove(&head_keyonly, bk_keyonly, nodes_keyonly, &nodes_keyonly[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[27], r, cy);
        for (int i = 0; i < BENCH_N; i++)
            myht_keyonly_insert(&head_keyonly, bk_keyonly, nodes_keyonly, &nodes_keyonly[i]);
    }

    /* ---- [28] remove slot: slot_field -> O(1) bucket+slot lookup ----- */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_slot_remove(&head_slot, bk_slot, nodes_slot, &nodes_slot[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[28], r, cy);
        for (int i = 0; i < BENCH_N; i++)
            myht_slot_insert(&head_slot, bk_slot, nodes_slot, &nodes_slot[i]);
    }

    /* ---- insert bench: pre-remove BENCH_N entries, then measure ----- */
    for (int i = 0; i < BENCH_N; i++)
        myht_remove(&head, bk, nodes, &nodes[i]);
    for (int i = 0; i < BENCH_N; i++)
        myht_keyonly_remove(&head_keyonly, bk_keyonly, nodes_keyonly, &nodes_keyonly[i]);
    for (int i = 0; i < BENCH_N; i++)
        myht_slot_remove(&head_slot, bk_slot, nodes_slot, &nodes_slot[i]);

    /* ---- [29] insert fp */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_insert(&head, bk, nodes, &nodes[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[29], r, cy);
        for (int i = 0; i < BENCH_N; i++)
            myht_remove(&head, bk, nodes, &nodes[i]);
    }

    /* ---- [30] insert keyonly */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_keyonly_insert(&head_keyonly, bk_keyonly, nodes_keyonly, &nodes_keyonly[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[30], r, cy);
        for (int i = 0; i < BENCH_N; i++)
            myht_keyonly_remove(&head_keyonly, bk_keyonly, nodes_keyonly, &nodes_keyonly[i]);
    }

    /* ---- [31] insert slot */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_slot_insert(&head_slot, bk_slot, nodes_slot, &nodes_slot[i]);
        uint64_t cy = tsc_end() - t0;
        record_result(&result[31], r, cy);
        for (int i = 0; i < BENCH_N; i++)
            myht_slot_remove(&head_slot, bk_slot, nodes_slot, &nodes_slot[i]);
    }

    /* ---- Output results ---------------------------------------------- */
    {
        /* MSHR theoretical minimum (after bk_0-only optimization):
         * 256 lookup x 3 cachelines (bk0x2, nodex1) = 768 fetches
         *   bk_1 is lazily loaded only on bk_0 miss (negligible at 80% fill)
         * MSHR capacity ~20, DRAM latency ~300 cycles
         * -> min: 768/20 x 300 = 11520 cycles/256 = 45 cycles/op  */
        double mshr_min = 11520.0;
        printf("\n");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "pattern", "min/256", "med/256", "min/op", "med/op");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "--------------------", "----------", "----------", "----------", "----------");
        for (int p = 0; p < NR; p++) {
            if (p == 12 || p == 19 || p == 26)
                printf("  %-20s  %10s  %10s  %10s  %10s\n",
                       p == 12 ? "-- keyonly find --" :
                       p == 19 ? "-- slot find --" :
                                 "-- insert/remove --", "", "", "", "");
            double med = result_median_cy(&result[p]);
            int ops = result[p].ops;
            printf("  %-20s  %10llu  %10llu  %10.2f  %10.2f\n",
                   result[p].label,
                   (unsigned long long)result[p].min_cy,
                   (unsigned long long)med,
                   (double)result[p].min_cy / ops,
                   med / ops);
        }
        printf("\n");
        printf("  MSHR theoretical min (20 MSHRs, 300cy latency, 5CL/lookup):\n");
        printf("    %.0f cycles/256 = %.1f cycles/op\n", mshr_min, mshr_min / BENCH_N);
        printf("  note: median is the primary comparable value across runs.\n");
        printf("        min reflects best-case throughput.\n");
        printf("        bk_mem=%.1f MB vs typical L3 cache -> median remains cold-biased.\n\n",
               bk_mem / 1e6);
        printf("  Comparable summary (median cycles/op across %u rounds)\n", repeat);
        printf("  %-10s %8s %8s %10s %10s %10s %10s %10s %10s %10s\n",
               "variant", "fill%", "bk0%", "find", "x4", "x6", "x8",
               "x6pipe", "x8pipe", "rm/ins");
        printf("  %-10s %8s %8s %10s %10s %10s %10s %10s %10s %10s\n",
               "----------", "--------", "--------", "----------",
               "----------", "----------", "----------", "----------",
               "----------", "----------");
        printf("  %-10s %8.1f %8.2f %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %4.2f/%4.2f\n",
               "fp",
               100.0 * (double)head.rhh_nb /
               ((double)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ),
               100.0 * (double)fp_bk0 / (double)(fp_bk0 + fp_bk1),
               result_median_op(&result[0]),
               result_median_op(&result[4]),
               result_median_op(&result[8]),
               result_median_op(&result[10]),
               result_median_op(&result[9]),
               result_median_op(&result[11]),
               result_median_op(&result[26]),
               result_median_op(&result[29]));
        printf("  %-10s %8.1f %8.2f %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %4.2f/%4.2f\n",
               "keyonly",
               100.0 * (double)head_keyonly.rhh_nb /
               ((double)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ),
               100.0 * (double)keyonly_bk0 / (double)(keyonly_bk0 + keyonly_bk1),
               result_median_op(&result[12]),
               result_median_op(&result[14]),
               result_median_op(&result[15]),
               result_median_op(&result[17]),
               result_median_op(&result[16]),
               result_median_op(&result[18]),
               result_median_op(&result[27]),
               result_median_op(&result[30]));
        printf("  %-10s %8.1f %8.2f %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %4.2f/%4.2f\n",
               "slot",
               100.0 * (double)head_slot.rhh_nb /
               ((double)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ),
               100.0 * (double)slot_bk0 / (double)(slot_bk0 + slot_bk1),
               result_median_op(&result[19]),
               result_median_op(&result[21]),
               result_median_op(&result[22]),
               result_median_op(&result[24]),
               result_median_op(&result[23]),
               result_median_op(&result[25]),
               result_median_op(&result[28]),
               result_median_op(&result[31]));
        printf("\n");
    }

    free_results(result, (unsigned)NR);
    free(key_pool);
    free(key_pool_keyonly);
    free(key_pool_slot);
    munmap(bk, bk_mem);
    munmap(bk_keyonly, bk_mem);
    munmap(bk_slot, bk_mem);
    munmap(nodes, node_mem);
    munmap(nodes_keyonly, node_keyonly_mem);
    munmap(nodes_slot, node_slot_mem);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(int argc, char **argv)
{
    unsigned table_n  = 100000000u; /* 100M */
    unsigned nb_bk    = 0;
    unsigned repeat   = 2000;
    int      rand_keys = 0; /* 0=sequential, 1=random */

    if (argc >= 2) table_n   = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) nb_bk     = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) repeat    = (unsigned)strtoul(argv[3], NULL, 10);
    if (argc >= 5) rand_keys = (int)strtoul(argv[4], NULL, 10);

    if (nb_bk == 0) {
        /* Default: target ~80% fill (slots = table_n / 0.8) */
        nb_bk = 1;
        while ((uint64_t)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ * 4 < (uint64_t)table_n * 5)
            nb_bk <<= 1;
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    bench_find(table_n, nb_bk, repeat, rand_keys);
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
