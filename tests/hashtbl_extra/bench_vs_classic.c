/* bench_vs_classic.c - Matched comparison: classic slot vs slot_extra.
 *
 * Runs identical insert / find_hit / find_miss / remove workloads against
 * classic RIX_HASH_GENERATE_SLOT (128B bucket) and
 * RIX_HASH_GENERATE_SLOT_EXTRA (192B bucket) under the same N/BK/reps,
 * same key layout, same hugepage-backed allocations.
 *
 * Node layout is identical for both; only bucket size differs. The
 * delta therefore reflects the cost of the third cacheline for buckets
 * that are not actually consulted on the insert/find hot path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

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

RIX_HASH_HEAD(cht);
RIX_HASH_GENERATE_SLOT(cht, bnode, key, cur_hash, slot, bkey_cmp)

RIX_HASH_HEAD(eht);
RIX_HASH_GENERATE_SLOT_EXTRA(eht, bnode, key, cur_hash, slot, bkey_cmp)

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

#define N_KEYS   (1u << 16)
#define N_BK     (1u << 14)
#define REPS      8u

static void *
hugealloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    size_t node_mem = (size_t)N_KEYS * sizeof(struct bnode);
    size_t c_bk_mem = (size_t)N_BK   * sizeof(struct rix_hash_bucket_s);
    size_t e_bk_mem = (size_t)N_BK   * sizeof(struct rix_hash_bucket_extra_s);

    struct bnode *pool                      = hugealloc(node_mem);
    struct rix_hash_bucket_s *cbk           = hugealloc(c_bk_mem);
    struct rix_hash_bucket_extra_s *ebk     = hugealloc(e_bk_mem);
    if (!pool || !cbk || !ebk) {
        fprintf(stderr, "mmap failed\n");
        return 1;
    }

    for (unsigned i = 0; i < N_KEYS; i++) {
        pool[i].key.hi = (u64)(i + 1);
        pool[i].key.lo = (u64)i * 0xC6BC279692B5C323ULL;
    }

    u64 c_ins = 0, c_find = 0, c_miss = 0, c_rm = 0;
    u64 e_ins = 0, e_find = 0, e_miss = 0, e_rm = 0;

    for (unsigned r = 0; r < REPS; r++) {
        memset(cbk, 0, c_bk_mem);
        struct cht chead;
        RIX_HASH_INIT(cht, &chead, N_BK);

        u64 t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)cht_insert(&chead, cbk, pool, &pool[i]);
        u64 t1 = tsc_end();
        c_ins += (t1 - t0);

        t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)cht_find(&chead, cbk, pool, &pool[i].key);
        t1 = tsc_end();
        c_find += (t1 - t0);

        struct bkey miss;
        miss.hi = 0xDEADBEEFu;
        miss.lo = 0u;
        t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++) {
            miss.lo = i;
            (void)cht_find(&chead, cbk, pool, &miss);
        }
        t1 = tsc_end();
        c_miss += (t1 - t0);

        t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)cht_remove(&chead, cbk, pool, &pool[i]);
        t1 = tsc_end();
        c_rm += (t1 - t0);

        memset(ebk, 0, e_bk_mem);
        struct eht ehead;
        RIX_HASH_INIT(eht, &ehead, N_BK);

        t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)eht_insert(&ehead, ebk, pool, &pool[i], 0xABCDu + i);
        t1 = tsc_end();
        e_ins += (t1 - t0);

        t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)eht_find(&ehead, ebk, pool, &pool[i].key);
        t1 = tsc_end();
        e_find += (t1 - t0);

        miss.hi = 0xDEADBEEFu;
        miss.lo = 0u;
        t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++) {
            miss.lo = i;
            (void)eht_find(&ehead, ebk, pool, &miss);
        }
        t1 = tsc_end();
        e_miss += (t1 - t0);

        t0 = tsc_start();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)eht_remove(&ehead, ebk, pool, &pool[i]);
        t1 = tsc_end();
        e_rm += (t1 - t0);
    }

    double tot = (double)N_KEYS * REPS;
    printf("matched bench (N=%u, BK=%u, reps=%u)\n", N_KEYS, N_BK, REPS);
    printf("  classic bucket : %zu B    slot_extra bucket : %zu B\n",
           sizeof(struct rix_hash_bucket_s),
           sizeof(struct rix_hash_bucket_extra_s));
    printf("  %-12s %10s %10s %10s\n", "op", "classic", "extra", "delta");
    printf("  %-12s %10.2f %10.2f %+10.2f\n", "insert",
           c_ins / tot,  e_ins / tot,
           ((double)e_ins - (double)c_ins) / tot);
    printf("  %-12s %10.2f %10.2f %+10.2f\n", "find_hit",
           c_find / tot, e_find / tot,
           ((double)e_find - (double)c_find) / tot);
    printf("  %-12s %10.2f %10.2f %+10.2f\n", "find_miss",
           c_miss / tot, e_miss / tot,
           ((double)e_miss - (double)c_miss) / tot);
    printf("  %-12s %10.2f %10.2f %+10.2f\n", "remove",
           c_rm / tot,   e_rm / tot,
           ((double)e_rm - (double)c_rm) / tot);

    munmap(pool, node_mem);
    munmap(cbk, c_bk_mem);
    munmap(ebk, e_bk_mem);
    return 0;
}
