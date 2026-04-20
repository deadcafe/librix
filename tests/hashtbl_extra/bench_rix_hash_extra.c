/* bench_rix_hash_extra.c - slot_extra variant micro-benchmark.
 *
 * Measures cycles/op for insert, find_hit, find_miss, remove
 * at a configurable fill. Compare against
 * tests/hashtbl/bench_rix_hash.c for classic slot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "rix_hash.h"

struct bkey {
    u64 hi;
    u64 lo;
};

struct bnode {
    u32 cur_hash;
    u16 slot;
    u16 _pad;
    struct bkey key;
};

static RIX_FORCE_INLINE int
bkey_cmp(const struct bkey *a, const struct bkey *b)
{
    return (a->hi == b->hi && a->lo == b->lo) ? 0 : 1;
}

RIX_HASH_HEAD(bht);
RIX_HASH_GENERATE_SLOT_EXTRA(bht, bnode, key, cur_hash, slot, bkey_cmp)

static RIX_FORCE_INLINE u64
rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

#define N_KEYS   (1u << 16)       /* 65536 keys   */
#define N_BK     (1u << 14)       /* 16384 bkts   */
#define REPS      8u

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    static struct bnode *pool;
    static struct rix_hash_bucket_extra_s *bk;
    struct bht head;

    pool = calloc(N_KEYS, sizeof(*pool));
    bk   = aligned_alloc(64, (size_t)N_BK * sizeof(*bk));
    if (!pool || !bk) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    memset(bk, 0, (size_t)N_BK * sizeof(*bk));

    for (unsigned i = 0; i < N_KEYS; i++) {
        pool[i].key.hi = (u64)(i + 1);
        pool[i].key.lo = (u64)i * 0xC6BC279692B5C323ULL;
    }

    u64 ins_cy = 0, find_cy = 0, miss_cy = 0, rm_cy = 0;

    for (unsigned r = 0; r < REPS; r++) {
        memset(bk, 0, (size_t)N_BK * sizeof(*bk));
        RIX_HASH_INIT(bht, &head, N_BK);

        u64 t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)bht_insert(&head, bk, pool, &pool[i], 0xABCDu + i);
        u64 t1 = rdtsc();
        ins_cy += (t1 - t0);

        t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)bht_find(&head, bk, pool, &pool[i].key);
        t1 = rdtsc();
        find_cy += (t1 - t0);

        struct bkey miss;
        miss.hi = 0xDEADBEEFu;
        miss.lo = 0u;
        t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++) {
            miss.lo = i;
            (void)bht_find(&head, bk, pool, &miss);
        }
        t1 = rdtsc();
        miss_cy += (t1 - t0);

        t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)bht_remove(&head, bk, pool, &pool[i]);
        t1 = rdtsc();
        rm_cy += (t1 - t0);
    }

    u64 total_ops = (u64)N_KEYS * REPS;
    printf("slot_extra bench (N=%u, BK=%u, reps=%u)\n",
           N_KEYS, N_BK, REPS);
    printf("  insert     : %5.2f cy/op\n", (double)ins_cy / (double)total_ops);
    printf("  find_hit   : %5.2f cy/op\n", (double)find_cy / (double)total_ops);
    printf("  find_miss  : %5.2f cy/op\n", (double)miss_cy / (double)total_ops);
    printf("  remove     : %5.2f cy/op\n", (double)rm_cy / (double)total_ops);

    free(pool);
    free(bk);
    return 0;
}
