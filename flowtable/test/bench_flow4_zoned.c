/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_flow4_zoned.c - fill-zone based expire controller benchmark.
 *
 * Simulates a flow cache under sustained traffic (1-2 Mpps, 3% miss rate)
 * with a zone-based maintenance controller:
 *
 *   fill < 75%  (Green):  add_idx_bulk_maint only (inline Phase 2)
 *   fill < 85%  (Yellow): inline + ft_*_maintain sweep, max_evict=SWEEP_Y
 *   fill < 95%  (Red):    inline + ft_*_maintain sweep, max_evict=SWEEP_R
 *
 * Table sizes: 1M (2^20) and 2M (2^21).
 * Traffic model:
 *   - 3% miss rate -> Q_ADD adds per batch
 *   - 97% hit rate -> Q_HIT find_touch per batch
 *   - 1 Mpps: Q_ADD=64 (2133-pkt batch)
 *   - 2 Mpps: Q_ADD=128 (4267-pkt batch, same fill pressure per unit time)
 *
 * Initial fill per scenario:
 *   Green  scenario: 70% fill, ~8% stale (inline maint sufficient)
 *   Yellow scenario: 80% fill, 20% stale (needs sweep to return to Green)
 *   Red    scenario: 90% fill, 30% stale (needs larger sweep)
 *
 * Reports: cy/add, cy/hit, cy/pkt (weighted 3:97), final fill %, zone counts.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <rix/rix_defs_private.h>
#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>

#include "flowtable/flow4_table.h"
#include "flowtable/flow4_extra_table.h"
#include "flowtable/flow_key.h"

/* ------------------------------------------------------------------ */
/* timing helpers                                                       */
/* ------------------------------------------------------------------ */

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
    if (p == MAP_FAILED)
        return NULL;
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

/* ------------------------------------------------------------------ */
/* configuration                                                        */
/* ------------------------------------------------------------------ */

#define TS_SHIFT_Z   4u
#define TMO_Z        (1u << 18)        /* expire after ~26s at 10 Mpps */
#define TS_STALE_Z   (1u << TS_SHIFT_Z)
#define NOW_INIT_Z   1000000u

/* zone fill thresholds */
#define PCT_GREEN_MIN  65u
#define PCT_GREEN_MAX  75u
#define PCT_YELLOW_MAX 85u
#define PCT_RED_MAX    95u

/* zone sweep: max evictions per maintain call */
#define SWEEP_Y_MAX  128u
#define SWEEP_R_MAX  512u

/* batch composition */
#define REPS_Z  5u
#define NWARM_Z 2u

/* ------------------------------------------------------------------ */
/* key fill helpers                                                     */
/* ------------------------------------------------------------------ */

static void
fill_c_keys(struct flow4_entry *pool, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; i++) {
        memset(&pool[i].key, 0, sizeof(pool[i].key));
        pool[i].key.family   = 4u;
        pool[i].key.proto    = 6u;
        pool[i].key.src_ip   = 0x0A000000u | i;
        pool[i].key.dst_ip   = 0x0B000000u | (i + 1u);
        pool[i].key.src_port = (u16)(i * 7u);
        pool[i].key.dst_port = (u16)(i * 11u);
    }
}

static void
fill_e_keys(struct flow4_extra_entry *pool, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; i++) {
        memset(&pool[i].key, 0, sizeof(pool[i].key));
        pool[i].key.family   = 4u;
        pool[i].key.proto    = 6u;
        pool[i].key.src_addr = 0x0A000000u | i;
        pool[i].key.dst_addr = 0x0B000000u | (i + 1u);
        pool[i].key.src_port = (u16)(i * 7u);
        pool[i].key.dst_port = (u16)(i * 11u);
    }
}

/* ------------------------------------------------------------------ */
/* circular free-pool ring                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    u32     *ring;
    unsigned head;
    unsigned tail;
    unsigned count;
    unsigned sz;   /* power-of-2 ring size */
} fring_t;

static void
fring_init(fring_t *r, u32 *mem, unsigned sz)
{
    r->ring  = mem;
    r->head  = r->tail = r->count = 0u;
    r->sz    = sz;
}

static void
fring_push(fring_t *r, u32 idx)
{
    r->ring[r->tail & (r->sz - 1u)] = idx;
    r->tail++;
    r->count++;
}

static u32
fring_pop(fring_t *r)
{
    u32 idx = r->ring[r->head & (r->sz - 1u)];
    r->head++;
    r->count--;
    return idx;
}

/* ------------------------------------------------------------------ */
/* result tracking                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    u64      cy_add;   /* cumulative cycles for add+maint+sweep batches */
    u64      cy_hit;   /* cumulative cycles for find batches */
    unsigned n_green;
    unsigned n_yellow;
    unsigned n_red;
    unsigned n_batches;
} zstat_t;

/* ------------------------------------------------------------------ */
/* pure variant zoned benchmark                                      */
/* ------------------------------------------------------------------ */

static void
run_pure(const char *scenario,
            struct ft_table *ft,
            struct flow4_entry *pool,
            unsigned N,
            fring_t *fpool,
            u32 *hit_ring, unsigned n_fresh, unsigned *hit_pos,
            unsigned *sweep_cursor,
            u32 *add_buf, unsigned q_add,
            u32 *maint_buf,
            u32 *sweep_buf,
            u64 now,
            zstat_t *st)
{
    unsigned b;
    unsigned nb_bk     = ft_table_nb_bk(ft);
    unsigned maint_max = q_add * 2u;

    unsigned n_take = q_add;

    for (b = 0u; b < REPS_Z + NWARM_Z; b++) {
        unsigned fill    = ft_table_nb_entries(ft);
        unsigned pct     = fill * 100u / N;
        unsigned sw_max  = 0u;
        unsigned n_un, n_sw, i;
        u64 t0, t1;

        /* determine zone */
        if (pct < PCT_GREEN_MAX) {
            if (b >= NWARM_Z) st->n_green++;
            sw_max = 0u;
        } else if (pct < PCT_YELLOW_MAX) {
            if (b >= NWARM_Z) st->n_yellow++;
            sw_max = SWEEP_Y_MAX;
        } else {
            if (b >= NWARM_Z) st->n_red++;
            sw_max = SWEEP_R_MAX;
        }

        /* fill add batch */
        n_take = (fpool->count < q_add) ? fpool->count : q_add;
        for (i = 0u; i < n_take; i++)
            add_buf[i] = fring_pop(fpool);

        /* --- timed: add + inline maint + zone sweep --- */
        t0 = tsc_start();

        n_un = ft_table_add_idx_bulk_maint(ft, add_buf, n_take,
                                            FT_ADD_IGNORE, now, TMO_Z,
                                            maint_buf, maint_max, 1u);

        n_sw = 0u;
        if (sw_max > 0u) {
            unsigned start = *sweep_cursor & (nb_bk - 1u);
            unsigned next_bk = start;
            n_sw = ft_flow4_table_maintain(ft, start, now, TMO_Z,
                                            sweep_buf, sw_max, 1u, &next_bk);
            *sweep_cursor = next_bk;
        }

        t1 = tsc_end();
        if (b >= NWARM_Z)
            st->cy_add += (t1 - t0);

        /* return evicted to free pool */
        for (i = 0u; i < n_un; i++)
            if (maint_buf[i] != 0u)
                fring_push(fpool, maint_buf[i]);
        for (i = 0u; i < n_sw; i++)
            fring_push(fpool, sweep_buf[i]);

        /* --- timed: hits (find) --- */
        {
            unsigned q_hit = q_add * 32u;  /* 97:3 ratio */
            t0 = tsc_start();
            for (i = 0u; i < q_hit; i++) {
                unsigned idx = hit_ring[(*hit_pos)++ % n_fresh];
                (void)ft_flow4_table_find(ft, &pool[idx].key, now);
            }
            t1 = tsc_end();
            if (b >= NWARM_Z)
                st->cy_hit += (t1 - t0);
        }
    }
    st->n_batches += REPS_Z;

    printf("  pure/%-7s Q_add=%3u  cy/add=%7.1f  cy/hit=%7.1f"
           "  cy/pkt=%7.1f  fill=%4.1f%%  G=%u Y=%u R=%u\n",
           scenario, q_add,
           (double)st->cy_add / (double)(REPS_Z * n_take),
           (double)st->cy_hit / (double)(REPS_Z * q_add * 32u),
           ((double)st->cy_add + (double)st->cy_hit)
               / (double)(REPS_Z * (n_take + q_add * 32u)),
           (double)ft_table_nb_entries(ft) * 100.0 / (double)N,
           st->n_green, st->n_yellow, st->n_red);
}

/* ------------------------------------------------------------------ */
/* extra variant zoned benchmark                                        */
/* ------------------------------------------------------------------ */

static void
run_extra(const char *scenario,
          struct ft_table_extra *ft,
          struct flow4_extra_entry *pool,
          unsigned N,
          fring_t *fpool,
          u32 *hit_ring, unsigned n_fresh, unsigned *hit_pos,
          unsigned *sweep_cursor,
          u32 *add_buf, unsigned q_add,
          u32 *maint_buf,
          u32 *sweep_buf,
          u64 now,
          struct ft_maint_extra_ctx *mctx,
          zstat_t *st)
{
    unsigned b;
    unsigned nb_bk     = ft_table_extra_nb_bk(ft);
    unsigned maint_max = q_add * 2u;

    unsigned n_take = q_add;

    for (b = 0u; b < REPS_Z + NWARM_Z; b++) {
        unsigned fill    = ft_table_extra_nb_entries(ft);
        unsigned pct     = fill * 100u / N;
        unsigned sw_max  = 0u;
        unsigned n_un, n_sw, i;
        u64 t0, t1;

        if (pct < PCT_GREEN_MAX) {
            if (b >= NWARM_Z) st->n_green++;
            sw_max = 0u;
        } else if (pct < PCT_YELLOW_MAX) {
            if (b >= NWARM_Z) st->n_yellow++;
            sw_max = SWEEP_Y_MAX;
        } else {
            if (b >= NWARM_Z) st->n_red++;
            sw_max = SWEEP_R_MAX;
        }

        n_take = (fpool->count < q_add) ? fpool->count : q_add;
        for (i = 0u; i < n_take; i++)
            add_buf[i] = fring_pop(fpool);

        t0 = tsc_start();

        n_un = ft_table_extra_add_idx_bulk_maint(ft, add_buf, n_take,
                                                  FT_ADD_IGNORE, now, TMO_Z,
                                                  maint_buf, maint_max, 1u);

        n_sw = 0u;
        if (sw_max > 0u) {
            unsigned start = *sweep_cursor & (nb_bk - 1u);
            unsigned next_bk = start;
            n_sw = ft_table_extra_maintain(mctx, start, now, TMO_Z,
                                            sweep_buf, sw_max, 1u, &next_bk);
            *sweep_cursor = next_bk;
        }

        t1 = tsc_end();
        if (b >= NWARM_Z)
            st->cy_add += (t1 - t0);

        for (i = 0u; i < n_un; i++)
            if (maint_buf[i] != 0u)
                fring_push(fpool, maint_buf[i]);
        for (i = 0u; i < n_sw; i++)
            fring_push(fpool, sweep_buf[i]);

        {
            unsigned q_hit = q_add * 32u;
            t0 = tsc_start();
            for (i = 0u; i < q_hit; i++) {
                unsigned idx = hit_ring[(*hit_pos)++ % n_fresh];
                (void)flow4_extra_table_find_touch(ft, &pool[idx].key, now);
            }
            t1 = tsc_end();
            if (b >= NWARM_Z)
                st->cy_hit += (t1 - t0);
        }
    }
    st->n_batches += REPS_Z;

    printf("  extra  /%-7s Q_add=%3u  cy/add=%7.1f  cy/hit=%7.1f"
           "  cy/pkt=%7.1f  fill=%4.1f%%  G=%u Y=%u R=%u\n",
           scenario, q_add,
           (double)st->cy_add / (double)(REPS_Z * n_take),
           (double)st->cy_hit / (double)(REPS_Z * q_add * 32u),
           ((double)st->cy_add + (double)st->cy_hit)
               / (double)(REPS_Z * (n_take + q_add * 32u)),
           (double)ft_table_extra_nb_entries(ft) * 100.0 / (double)N,
           st->n_green, st->n_yellow, st->n_red);
}

/* ------------------------------------------------------------------ */
/* scenario setup helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * Setup a scenario:
 *  - fill_pct:   total fill level (e.g. 70, 80, 90)
 *  - stale_pct:  fraction of filled entries that are stale (e.g. 8, 20, 30)
 *  - fresh entries: fill_pct - stale_pct -> used as hit targets
 *  - free pool:  100 - fill_pct -> available for adds
 *  - stale entries: available for Phase 2 eviction
 */
typedef struct {
    u32     *hit_ring;    /* indices of fresh (non-stale) entries */
    unsigned n_fresh;
    u32     *ring_mem;    /* ring memory for fring (size N, power-of-2) */
    fring_t  fpool;       /* free indices */
} scenario_state_t;

static void
setup_scenario_pure(scenario_state_t *ss,
                        struct ft_table *ft,
                        struct flow4_entry *pool,
                        unsigned N,
                        unsigned fill_pct,
                        unsigned stale_pct_of_fill)
{
    unsigned n_fill      = N * fill_pct / 100u;
    unsigned n_stale     = n_fill * stale_pct_of_fill / 100u;
    unsigned n_fresh_fill = n_fill - n_stale;
    unsigned i;
    u32 *idxv = calloc(N, sizeof(u32));
    u32 *unused = calloc(N, sizeof(u32));

    assert(idxv && unused);
    (void)pool;

    /* fresh entries: indices 1 .. n_fresh_fill */
    for (i = 0u; i < n_fresh_fill; i++) idxv[i] = i + 1u;
    (void)ft_flow4_table_add_idx_bulk(ft, idxv, n_fresh_fill,
                                       FT_ADD_IGNORE, NOW_INIT_Z, unused);

    /* stale entries: indices n_fresh_fill+1 .. n_fill */
    for (i = 0u; i < n_stale; i++) idxv[i] = n_fresh_fill + 1u + i;
    (void)ft_flow4_table_add_idx_bulk(ft, idxv, n_stale,
                                       FT_ADD_IGNORE, TS_STALE_Z, unused);

    /* hit ring: fresh entry indices */
    ss->n_fresh   = n_fresh_fill;
    ss->hit_ring  = calloc(n_fresh_fill ? n_fresh_fill : 1u, sizeof(u32));
    assert(ss->hit_ring);
    for (i = 0u; i < n_fresh_fill; i++)
        ss->hit_ring[i] = i + 1u;

    /* free pool ring (power-of-2 >= N) */
    unsigned ring_sz = N;  /* already power-of-2 if N is */
    ss->ring_mem = calloc(ring_sz, sizeof(u32));
    assert(ss->ring_mem);
    fring_init(&ss->fpool, ss->ring_mem, ring_sz);
    /* free indices: n_fill+1 .. N */
    for (i = n_fill + 1u; i <= N; i++)
        fring_push(&ss->fpool, i);

    free(idxv);
    free(unused);
}

static void
setup_scenario_extra(scenario_state_t *ss,
                      struct ft_table_extra *ft,
                      struct flow4_extra_entry *pool,
                      unsigned N,
                      unsigned fill_pct,
                      unsigned stale_pct_of_fill)
{
    unsigned n_fill      = N * fill_pct / 100u;
    unsigned n_stale     = n_fill * stale_pct_of_fill / 100u;
    unsigned n_fresh_fill = n_fill - n_stale;
    unsigned i;
    u32 *idxv = calloc(N, sizeof(u32));
    u32 *unused = calloc(N, sizeof(u32));

    assert(idxv && unused);
    (void)pool;  /* keys already filled */

    for (i = 0u; i < n_fresh_fill; i++) idxv[i] = i + 1u;
    (void)ft_table_extra_add_idx_bulk(ft, idxv, n_fresh_fill,
                                       FT_ADD_IGNORE, NOW_INIT_Z, unused);

    for (i = 0u; i < n_stale; i++) idxv[i] = n_fresh_fill + 1u + i;
    (void)ft_table_extra_add_idx_bulk(ft, idxv, n_stale,
                                       FT_ADD_IGNORE, TS_STALE_Z, unused);

    ss->n_fresh  = n_fresh_fill;
    ss->hit_ring = calloc(n_fresh_fill ? n_fresh_fill : 1u, sizeof(u32));
    assert(ss->hit_ring);
    for (i = 0u; i < n_fresh_fill; i++)
        ss->hit_ring[i] = i + 1u;

    unsigned ring_sz = N;
    ss->ring_mem = calloc(ring_sz, sizeof(u32));
    assert(ss->ring_mem);
    fring_init(&ss->fpool, ss->ring_mem, ring_sz);
    for (i = n_fill + 1u; i <= N; i++)
        fring_push(&ss->fpool, i);

    free(idxv);
    free(unused);
}

static void
free_scenario(scenario_state_t *ss)
{
    free(ss->hit_ring);
    free(ss->ring_mem);
    memset(ss, 0, sizeof(*ss));
}

/* ------------------------------------------------------------------ */
/* bench one table size                                                 */
/* ------------------------------------------------------------------ */

static void
bench_one_size(unsigned N)
{
    size_t pc_sz = (size_t)N * sizeof(struct flow4_entry);
    size_t pe_sz = (size_t)N * sizeof(struct flow4_extra_entry);
    size_t bc_sz = ft_table_bucket_size(N);
    size_t be_sz = flow4_extra_table_bucket_size(N);

    struct flow4_entry       *pool_c = hugealloc(pc_sz);
    struct flow4_extra_entry *pool_e = hugealloc(pe_sz);
    void *bk_c = hugealloc(bc_sz);
    void *bk_e = hugealloc(be_sz);

    assert(pool_c && pool_e && bk_c && bk_e);

    fill_c_keys(pool_c, N);
    fill_e_keys(pool_e, N);

    struct ft_table             ft_c;
    struct ft_table_extra       ft_e;
    struct ft_table_config      cfg_c = { .ts_shift = TS_SHIFT_Z };
    struct ft_table_extra_config cfg_e = { .ts_shift = TS_SHIFT_Z };
    struct ft_maint_extra_ctx   mctx;

    /* per-scenario shared buffers (worst-case sizing) */
    const unsigned Q_MAX   = 128u;
    const unsigned MMAX    = Q_MAX * 2u;
    const unsigned SWEEPMAX = SWEEP_R_MAX;

    u32 *add_buf   = calloc(Q_MAX,   sizeof(u32));
    u32 *maint_buf = calloc(MMAX,    sizeof(u32));
    u32 *sweep_buf = calloc(SWEEPMAX, sizeof(u32));
    assert(add_buf && maint_buf && sweep_buf);

    static const struct {
        const char *name;
        unsigned fill_pct;
        unsigned stale_pct;
    } scenarios[] = {
        { "green",  70u,  8u },
        { "yellow", 80u, 20u },
        { "red",    90u, 30u },
    };

    static const unsigned q_adds[] = { 64u, 128u }; /* 1 Mpps, 2 Mpps */

    printf("\n--- N=%uM (pure pool=%zuMB  extra pool=%zuMB"
           "  bk_c=%zuMB  bk_e=%zuMB) ---\n",
           N >> 20,
           pc_sz >> 20, pe_sz >> 20,
           bc_sz >> 20, be_sz >> 20);
    printf("  (Green<75%%  Yellow<85%%  Red<95%%"
           "  sweep_y=%u  sweep_r=%u  reps=%u)\n",
           SWEEP_Y_MAX, SWEEP_R_MAX, REPS_Z);
    printf("  %-15s %-7s %10s %10s %10s %8s  zones\n",
           "variant/zone", "Q_add", "cy/add", "cy/hit", "cy/pkt", "fill");

    for (unsigned si = 0u; si < 3u; si++) {
        for (unsigned qi = 0u; qi < 2u; qi++) {
            unsigned q_add = q_adds[qi];
            scenario_state_t ss_c, ss_e;
            unsigned hit_pos_c = 0u, hit_pos_e = 0u;
            unsigned sweep_c = 0u, sweep_e = 0u;
            int rc;
            (void)rc;

            /* --- pure --- */
            memset(bk_c, 0, bc_sz);
            for (unsigned i = 0u; i < N; i++)
                memset(&pool_c[i].meta, 0, sizeof(pool_c[i].meta));
            rc = FT_FLOW4_TABLE_INIT_TYPED(&ft_c, pool_c, N,
                                            struct flow4_entry, key,
                                            bk_c, bc_sz, &cfg_c);
            assert(rc == 0);
            setup_scenario_pure(&ss_c, &ft_c, pool_c, N,
                                   scenarios[si].fill_pct,
                                   scenarios[si].stale_pct);
            {
                zstat_t st;
                memset(&st, 0, sizeof(st));
                run_pure(scenarios[si].name, &ft_c, pool_c, N,
                            &ss_c.fpool,
                            ss_c.hit_ring, ss_c.n_fresh, &hit_pos_c,
                            &sweep_c,
                            add_buf, q_add, maint_buf, sweep_buf,
                            NOW_INIT_Z, &st);
            }
            free_scenario(&ss_c);
            ft_flow4_table_destroy(&ft_c);

            /* --- extra --- */
            memset(bk_e, 0, be_sz);
            for (unsigned i = 0u; i < N; i++)
                memset(&pool_e[i].meta, 0, sizeof(pool_e[i].meta));
            rc = flow4_extra_table_init(&ft_e, pool_e, N, bk_e, be_sz, &cfg_e);
            assert(rc == 0);
            rc = ft_table_extra_maint_ctx_init(&ft_e, &mctx);
            assert(rc == 0);
            setup_scenario_extra(&ss_e, &ft_e, pool_e, N,
                                  scenarios[si].fill_pct,
                                  scenarios[si].stale_pct);
            {
                zstat_t st;
                memset(&st, 0, sizeof(st));
                run_extra(scenarios[si].name, &ft_e, pool_e, N,
                          &ss_e.fpool,
                          ss_e.hit_ring, ss_e.n_fresh, &hit_pos_e,
                          &sweep_e,
                          add_buf, q_add, maint_buf, sweep_buf,
                          NOW_INIT_Z, &mctx, &st);
            }
            free_scenario(&ss_e);
            ft_table_extra_destroy(&ft_e);
        }
        printf("\n");
    }

    free(add_buf);
    free(maint_buf);
    free(sweep_buf);
    munmap(pool_c, pc_sz);
    munmap(pool_e, pe_sz);
    munmap(bk_c, bc_sz);
    munmap(bk_e, be_sz);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    ft_arch_init(FT_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);

    printf("flow4 zoned fill-rate control benchmark\n");
    printf("  zone: Green<75%%  Yellow<85%%  Red<95%%\n");
    printf("  Q_add=64  -> 1 Mpps equivalent (3%% miss, 2133-pkt batch)\n");
    printf("  Q_add=128 -> 2 Mpps equivalent (3%% miss, 4267-pkt batch)\n");

    bench_one_size(1u << 20);  /* 1M */
    bench_one_size(1u << 21);  /* 2M */

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
