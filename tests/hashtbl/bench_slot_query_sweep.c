#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "rix_hash.h"

struct mykey {
    uint64_t hi;
};

struct mynode_slot {
    uint32_t cur_hash;
    uint16_t slot;
    uint16_t _pad;
    struct mykey key;
};

static RIX_FORCE_INLINE int
mykey_cmp(const struct mykey *a, const struct mykey *b)
{
    return (a->hi != b->hi) ? 1 : 0;
}

RIX_HASH_HEAD(myht_slot);
RIX_HASH_GENERATE_SLOT(myht_slot, mynode_slot, key, cur_hash, slot, mykey_cmp)

#define MAX_QUERY 256u
#define KPD8 8

static struct rix_hash_find_ctx_s g_ctx[MAX_QUERY];
static struct mynode_slot *g_res[MAX_QUERY];
static volatile uintptr_t g_sink;
static uint64_t xr64 = 0x123456789abcdef0ULL;

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
median_u64(uint64_t *v, unsigned n)
{
    qsort(v, n, sizeof(v[0]), cmp_u64);
    if ((n & 1u) != 0u)
        return (double)v[n / 2u];
    return ((double)v[(n / 2u) - 1u] + (double)v[n / 2u]) * 0.5;
}

static void
consume_results(unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        g_sink ^= (uintptr_t)g_res[i];
}

static uint64_t
bench_single(struct myht_slot *head, struct rix_hash_bucket_s *bk_slot,
             struct mynode_slot *nodes_slot, const struct mykey *const *key_seq,
             unsigned query, unsigned rounds, unsigned total_keys)
{
    uint64_t t0 = tsc_start();

    for (unsigned r = 0; r < rounds; r++) {
        unsigned base = (r * query) % total_keys;
        for (unsigned i = 0; i < query; i++)
            g_res[i] = myht_slot_find(head, bk_slot, nodes_slot,
                                      key_seq[(base + i) % total_keys]);
        consume_results(query);
    }

    return tsc_end() - t0;
}

static uint64_t
bench_x1(struct myht_slot *head, struct rix_hash_bucket_s *bk_slot,
         struct mynode_slot *nodes_slot, const struct mykey *const *key_seq,
         unsigned query, unsigned rounds, unsigned total_keys)
{
    uint64_t t0 = tsc_start();

    for (unsigned r = 0; r < rounds; r++) {
        unsigned base = (r * query) % total_keys;
        for (unsigned i = 0; i < query; i++)
            myht_slot_hash_key(&g_ctx[i], head, bk_slot,
                               key_seq[(base + i) % total_keys]);
        for (unsigned i = 0; i < query; i++)
            myht_slot_scan_bk(&g_ctx[i], head, bk_slot);
        for (unsigned i = 0; i < query; i++)
            myht_slot_prefetch_node(&g_ctx[i], nodes_slot);
        for (unsigned i = 0; i < query; i++)
            g_res[i] = myht_slot_cmp_key(&g_ctx[i], nodes_slot);
        consume_results(query);
    }

    return tsc_end() - t0;
}

static uint64_t
bench_x8pipe(struct myht_slot *head, struct rix_hash_bucket_s *bk_slot,
             struct mynode_slot *nodes_slot, const struct mykey *const *key_seq,
             unsigned query, unsigned rounds, unsigned total_keys)
{
    const struct mykey *keys[MAX_QUERY];
    uint64_t t0 = tsc_start();

    for (unsigned r = 0; r < rounds; r++) {
        unsigned base = (r * query) % total_keys;
        unsigned groups = query / 8u;
        unsigned tail = query % 8u;

        for (unsigned i = 0; i < query; i++)
            keys[i] = key_seq[(base + i) % total_keys];

        for (unsigned b = 0; b < KPD8 && b < groups; b++) {
            for (unsigned j = 0; j < 8u; j++)
                rix_hash_prefetch_key(keys[b * 8u + j]);
        }
        for (unsigned b = 0; b < groups; b++) {
            unsigned pf = b + KPD8;
            if (pf < groups) {
                for (unsigned j = 0; j < 8u; j++)
                    rix_hash_prefetch_key(keys[pf * 8u + j]);
            }
            RIX_HASH_HASH_KEY_N(myht_slot, &g_ctx[b * 8u], 8,
                                head, bk_slot, keys + b * 8u);
        }
        for (unsigned b = 0; b < groups; b++)
            RIX_HASH_SCAN_BK_N(myht_slot, &g_ctx[b * 8u], 8, head, bk_slot);
        for (unsigned b = 0; b < groups; b++)
            RIX_HASH_CMP_KEY_N(myht_slot, &g_ctx[b * 8u], 8, nodes_slot,
                               (struct mynode_slot **)&g_res[b * 8u]);

        for (unsigned i = groups * 8u; i < (groups * 8u) + tail; i++)
            myht_slot_hash_key(&g_ctx[i], head, bk_slot, keys[i]);
        for (unsigned i = groups * 8u; i < (groups * 8u) + tail; i++)
            myht_slot_scan_bk(&g_ctx[i], head, bk_slot);
        for (unsigned i = groups * 8u; i < (groups * 8u) + tail; i++)
            myht_slot_prefetch_node(&g_ctx[i], nodes_slot);
        for (unsigned i = groups * 8u; i < (groups * 8u) + tail; i++)
            g_res[i] = myht_slot_cmp_key(&g_ctx[i], nodes_slot);

        consume_results(query);
    }

    return tsc_end() - t0;
}

int
main(int argc, char **argv)
{
    unsigned total_keys = 262144u;
    unsigned repeat = 7u;
    unsigned hotset = 8192u;
    unsigned table_n = 1048576u;
    unsigned nb_bk = 131072u;
    size_t node_slot_mem;
    size_t bk_mem;
    struct mynode_slot *nodes_slot;
    struct rix_hash_bucket_s *bk_slot;
    struct myht_slot head_slot;
    const struct mykey **key_seq;
    uint64_t *samples_single;
    uint64_t *samples_x1;
    uint64_t *samples_x8pipe;

    if (argc > 1)
        total_keys = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc > 2)
        repeat = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc > 3)
        hotset = (unsigned)strtoul(argv[3], NULL, 10);
    if (argc > 4)
        table_n = (unsigned)strtoul(argv[4], NULL, 10);
    if (argc > 5)
        nb_bk = (unsigned)strtoul(argv[5], NULL, 10);

    if (total_keys == 0u || repeat == 0u || hotset == 0u || table_n == 0u ||
        nb_bk == 0u || hotset > table_n) {
        fprintf(stderr,
                "usage: %s [total_keys [repeat [hotset [table_n [nb_bk]]]]]\n",
                argv[0]);
        return 1;
    }

    node_slot_mem = (size_t)table_n * sizeof(*nodes_slot);
    bk_mem = (size_t)nb_bk * sizeof(*bk_slot);

    nodes_slot = (struct mynode_slot *)mmap(NULL, node_slot_mem,
                                            PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes_slot == MAP_FAILED) {
        perror("mmap nodes_slot");
        return 1;
    }
    madvise(nodes_slot, node_slot_mem, MADV_HUGEPAGE);
    for (unsigned i = 0; i < table_n; i++)
        nodes_slot[i].key.hi = (uint64_t)(i + 1u);

    bk_slot = (struct rix_hash_bucket_s *)mmap(NULL, bk_mem,
                                               PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk_slot == MAP_FAILED) {
        perror("mmap bk_slot");
        return 1;
    }
    madvise(bk_slot, bk_mem, MADV_HUGEPAGE);
    memset(bk_slot, 0, bk_mem);

    RIX_HASH_INIT(myht_slot, &head_slot, nb_bk);
    for (unsigned i = 0; i < table_n; i++)
        myht_slot_insert(&head_slot, bk_slot, nodes_slot, &nodes_slot[i]);

    key_seq = (const struct mykey **)malloc((size_t)total_keys * sizeof(*key_seq));
    samples_single = (uint64_t *)malloc((size_t)repeat * sizeof(*samples_single));
    samples_x1 = (uint64_t *)malloc((size_t)repeat * sizeof(*samples_x1));
    samples_x8pipe = (uint64_t *)malloc((size_t)repeat * sizeof(*samples_x8pipe));
    if (key_seq == NULL || samples_single == NULL ||
        samples_x1 == NULL || samples_x8pipe == NULL) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (unsigned i = 0; i < total_keys; i++) {
        unsigned idx = (unsigned)(xorshift64() % hotset);
        key_seq[i] = &nodes_slot[idx].key;
    }

    printf("query\tsingle\tx1\tx8pipe\n");
    for (unsigned query = 1; query <= MAX_QUERY; query++) {
        unsigned rounds = (total_keys + query - 1u) / query;

        for (unsigned rep = 0; rep < repeat; rep++) {
            if ((rep & 1u) == 0u) {
                samples_single[rep] =
                    bench_single(&head_slot, bk_slot, nodes_slot, key_seq,
                                 query, rounds, total_keys);
                samples_x1[rep] =
                    bench_x1(&head_slot, bk_slot, nodes_slot, key_seq,
                             query, rounds, total_keys);
                samples_x8pipe[rep] =
                    bench_x8pipe(&head_slot, bk_slot, nodes_slot, key_seq,
                                 query, rounds, total_keys);
            } else {
                samples_x8pipe[rep] =
                    bench_x8pipe(&head_slot, bk_slot, nodes_slot, key_seq,
                                 query, rounds, total_keys);
                samples_x1[rep] =
                    bench_x1(&head_slot, bk_slot, nodes_slot, key_seq,
                             query, rounds, total_keys);
                samples_single[rep] =
                    bench_single(&head_slot, bk_slot, nodes_slot, key_seq,
                                 query, rounds, total_keys);
            }
        }

        printf("%u\t%.4f\t%.4f\t%.4f\n",
               query,
               median_u64(samples_single, repeat) / ((double)rounds * (double)query),
               median_u64(samples_x1, repeat) / ((double)rounds * (double)query),
               median_u64(samples_x8pipe, repeat) / ((double)rounds * (double)query));
    }

    free(samples_x8pipe);
    free(samples_x1);
    free(samples_single);
    free((void *)key_seq);
    munmap(bk_slot, bk_mem);
    munmap(nodes_slot, node_slot_mem);
    return (int)(g_sink & 1u);
}
