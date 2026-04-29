/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_fill_ctrl.c - ft_fill_ctrl adaptive controller validation.
 *
 * Simulates 3 traffic phases against an extra-variant flow4 table (N=1M).
 *
 *   Phase 1 - Normal   (12000 batches): Q_add=64,  Q_hit=2048, miss=3%
 *   Phase 2 - DoS      ( 2000 batches): Q_add=512, Q_hit=2048, miss=20%
 *   Phase 3 - Recovery ( 4000 batches): Q_add=64,  Q_hit=2048, miss=3%
 *
 * Simulated time model:
 *   sim_now advances SIM_TICK=64 per batch.
 *   T_NORMAL_SIM = setpoint = 68% * N.
 *   -> steady-state fill = Q_add * T_NORMAL_SIM / SIM_TICK = setpoint.
 *   -> expiry period = T_NORMAL_SIM / SIM_TICK = setpoint / Q_add = ~11 K batches.
 *
 *   Phase 1 covers just over one expiry period so the first wave of
 *   pre-fill entries naturally expires and the system reaches true
 *   steady state.
 *
 *   Phase 2 DoS: Q_add=512, Q_hit=2048 -> miss rate = 512/2560 = 20%.
 *   Controller reduces expire_tsc to 3/20 * T_NORMAL = 15%.
 *   Entries from Phase 1 that are older than 15% * T_NORMAL become
 *   immediately stale and are swept out, stabilising fill.
 *
 * Real TSC is used only for cycle-count measurements.
 * Progress line printed every PROG_INTERVAL batches.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include <rix/rix_defs_private.h>
#include <rix/rix_hash.h>
#include <rix/rix_hash_arch.h>
#include <rix/rix_hash_slot_extra.h>

#include "flowtable/flow4_extra_table.h"
#include "flowtable/flow_key.h"
#include "ft_fill_ctrl.h"

/* ------------------------------------------------------------------ */
/* timing                                                               */
/* ------------------------------------------------------------------ */

static RIX_FORCE_INLINE u64
real_tsc(void)
{
    u32 lo, hi;
    __asm__ volatile ("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static u64
tsc_hz_calibrate(void)
{
    struct timespec t0, t1;
    u64 c0, c1;
    struct timespec tw = { 0, 100000000 };

    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);  c0 = real_tsc();
    nanosleep(&tw, NULL);
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);  c1 = real_tsc();

    u64 ns = (u64)(t1.tv_sec - t0.tv_sec) * 1000000000ULL
             + (u64)(t1.tv_nsec - t0.tv_nsec);
    return (c1 - c0) * 1000000000ULL / ns;
}

/* ------------------------------------------------------------------ */
/* memory                                                               */
/* ------------------------------------------------------------------ */

static void *
hugealloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

/* ------------------------------------------------------------------ */
/* key helpers                                                          */
/* ------------------------------------------------------------------ */

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
/* free-pool ring                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    u32     *ring;
    unsigned head, tail, count, mask;
} fring_t;

static void fring_init(fring_t *r, u32 *m, unsigned sz)
{ r->ring=m; r->head=r->tail=r->count=0u; r->mask=sz-1u; }

static void fring_push(fring_t *r, u32 idx)
{ r->ring[r->tail & r->mask]=idx; r->tail++; r->count++; }

static u32 fring_pop(fring_t *r)
{ u32 v=r->ring[r->head & r->mask]; r->head++; r->count--; return v; }

/* ------------------------------------------------------------------ */
/* per-batch progress                                                   */
/* ------------------------------------------------------------------ */

#define PROG_INTERVAL 2000u

typedef struct {
    u64      cy_add, cy_hit, cy_sweep;
    u64      n_add, n_hit;
    unsigned n_batches;
    unsigned fill_min, fill_max;
} pstat_t;

static void pstat_reset(pstat_t *s, unsigned fill)
{ memset(s,0,sizeof(*s)); s->fill_min=fill; s->fill_max=fill; }

static void
pstat_update(pstat_t *s, unsigned fill, u64 ca, u64 ch, u64 cs,
             unsigned n_add, unsigned n_hit)
{
    if (fill < s->fill_min) s->fill_min = fill;
    if (fill > s->fill_max) s->fill_max = fill;
    s->cy_add += ca; s->cy_hit += ch; s->cy_sweep += cs;
    s->n_add += n_add; s->n_hit += n_hit; s->n_batches++;
}

static void pstat_print(const char *label, const pstat_t *s,
                         unsigned N)
{
    unsigned nb = s->n_batches ? s->n_batches : 1u;
    u64 n_add = s->n_add ? s->n_add : 1u;
    u64 n_hit = s->n_hit ? s->n_hit : 1u;
    u64 n_pkt = (s->n_add + s->n_hit) ? (s->n_add + s->n_hit) : 1u;

    printf("  [%-14s] fill=%4.1f%%..%4.1f%%  cy/add=%6.1f  cy/hit=%6.1f"
           "  cy/sweep-batch=%6.1f  cy/pkt=%6.1f\n",
           label,
           (double)s->fill_min * 100.0 / (double)N,
           (double)s->fill_max * 100.0 / (double)N,
           (double)s->cy_add / (double)n_add,
           (double)s->cy_hit / (double)n_hit,
           (double)s->cy_sweep / (double)nb,
           (double)(s->cy_add + s->cy_hit + s->cy_sweep) / (double)n_pkt);
}

/* ------------------------------------------------------------------ */
/* simulation loop                                                      */
/* ------------------------------------------------------------------ */

static void
run_phase(const char *label,
          struct ft_table_extra *ft,
          struct ft_maint_extra_ctx *mctx,
          struct flow4_extra_entry *pool,
          unsigned N,
          fring_t *fpool,
          u32 *hit_ring, unsigned n_fresh,
          unsigned q_add, unsigned q_hit,
          unsigned n_batches,
          u64 sim_tick,
          u64 *sim_now_io,
          struct ft_fill_ctrl *ctrl,
          u64 t_normal,
          u32 *add_buf, u32 *maint_buf,
          u32 *sweep_buf, unsigned sweep_cap)
{
    pstat_t st;
    unsigned hit_pos      = 0u;
    unsigned prev_added   = 0u;
    unsigned prev_hits    = q_hit;
    unsigned prev_next_bk = 0u;
    u64 sim_now           = *sim_now_io;
    unsigned b;

    pstat_reset(&st, ft_table_extra_nb_entries(ft));

    printf("  --- %s: Q_add=%u Q_hit=%u batches=%u ---\n",
           label, q_add, q_hit, n_batches);

    for (b = 0u; b < n_batches; b++) {
        unsigned budget, start_bk, n_take, n_un, n_sw, i;
        u64 tmo, t0, t1, cy_s;
        unsigned fill = ft_table_extra_nb_entries(ft);

        ft_fill_ctrl_compute(ctrl, fill,
                             prev_added, prev_hits, prev_next_bk,
                             &budget, &start_bk, &tmo);
        sim_now += sim_tick;

        /* add + inline maint */
        n_take = (fpool->count < q_add) ? fpool->count : q_add;
        for (i = 0u; i < n_take; i++)
            add_buf[i] = fring_pop(fpool);

        t0 = real_tsc();
        n_un = ft_table_extra_add_idx_bulk_maint(ft, add_buf, n_take,
                                                  FT_ADD_IGNORE, sim_now, tmo,
                                                  maint_buf, n_take * 2u, 1u);
        t1 = real_tsc();
        u64 cy_a = t1 - t0;

        for (i = 0u; i < n_un; i++)
            if (maint_buf[i] != 0u)
                fring_push(fpool, maint_buf[i]);

        /* hits */
        t0 = real_tsc();
        for (i = 0u; i < q_hit; i++) {
            unsigned idx = hit_ring[hit_pos++ % n_fresh];
            (void)flow4_extra_table_find_touch(ft, &pool[idx].key, sim_now);
        }
        t1 = real_tsc();
        u64 cy_h = t1 - t0;

        /* sweep */
        cy_s = 0u;
        prev_next_bk = start_bk;
        n_sw = 0u;
        if (budget > 0u) {
            unsigned cap = budget < sweep_cap ? budget : sweep_cap;
            t0 = real_tsc();
            n_sw = ft_table_extra_maintain(mctx, start_bk, sim_now, tmo,
                                            sweep_buf, cap, 1u, &prev_next_bk);
            t1 = real_tsc();
            cy_s = t1 - t0;
            for (i = 0u; i < n_sw; i++)
                fring_push(fpool, sweep_buf[i]);
        }

        prev_added = n_take;
        prev_hits  = q_hit;

        fill = ft_table_extra_nb_entries(ft);
        pstat_update(&st, fill, cy_a, cy_h, cy_s, n_take, q_hit);

        if ((b + 1u) % PROG_INTERVAL == 0u) {
            double tmo_pct = (double)tmo * 100.0 / (double)t_normal;
            printf("    batch %5u: fill=%5.1f%%  tmo=%5.1f%%  budget=%4u"
                   "  swept=%4u  free=%u\n",
                   b + 1u,
                   (double)fill * 100.0 / (double)N,
                   tmo_pct, budget, n_sw, fpool->count);
        }
    }

    *sim_now_io = sim_now;
    pstat_print(label, &st, N);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    unsigned rix_arch  = RIX_HASH_ARCH_AUTO;
    unsigned ft_arch_f = FT_ARCH_AUTO;
    const char *arch_name = "auto";

    /* optional first argument: gen | sse | avx2 | avx512 | auto */
    if (argc >= 2) {
        if (strcmp(argv[1], "gen") == 0) {
            rix_arch = 0u; ft_arch_f = FT_ARCH_GEN; arch_name = "gen";
        } else if (strcmp(argv[1], "sse") == 0) {
            rix_arch = RIX_HASH_ARCH_SSE; ft_arch_f = FT_ARCH_SSE; arch_name = "sse";
        } else if (strcmp(argv[1], "avx2") == 0) {
            rix_arch = RIX_HASH_ARCH_AVX2; ft_arch_f = FT_ARCH_AVX2; arch_name = "avx2";
        } else if (strcmp(argv[1], "avx512") == 0) {
            rix_arch = RIX_HASH_ARCH_AVX512;
            ft_arch_f = FT_ARCH_AVX512;
            arch_name = "avx512";
        }
    }

    rix_hash_arch_init(rix_arch);
    ft_arch_init(ft_arch_f);
    ft_arch_extra_init(ft_arch_f);

    printf("ft_fill_ctrl adaptive controller benchmark (arch=%s)\n", arch_name);

    u64 tsc_hz = tsc_hz_calibrate();
    printf("  TSC: %.3f GHz\n\n", (double)tsc_hz / 1e9);

    /* --- table memory --- */
    const unsigned N      = 1u << 20;
    const unsigned TS_SHIFT = 4u;

    size_t pe_sz = (size_t)N * sizeof(struct flow4_extra_entry);
    size_t be_sz = flow4_extra_table_bucket_size(N);

    struct flow4_extra_entry *pool = hugealloc(pe_sz);
    void *bk = hugealloc(be_sz);
    assert(pool && bk);
    fill_e_keys(pool, N);

    /* --- buffers (DoS needs Q_add=512) --- */
    const unsigned Q_ADD_MAX  = 512u;
    const unsigned SWEEP_CAP  = 512u;

    u32 *add_buf   = calloc(Q_ADD_MAX,    sizeof(u32));
    u32 *maint_buf = calloc(Q_ADD_MAX*2u, sizeof(u32));
    u32 *sweep_buf = calloc(SWEEP_CAP,    sizeof(u32));
    u32 *ring_mem  = calloc(N,            sizeof(u32));
    assert(add_buf && maint_buf && sweep_buf && ring_mem);

    /*
     * Simulated time parameters:
     *   SIM_TICK  = Q_ADD_NORMAL = 64
     *   T_NORMAL  = setpoint     (so steady-state fill = setpoint)
     *   expiry period = T_NORMAL / SIM_TICK = setpoint / Q_ADD ~= 11 K batches
     *   T_MIN     = T_NORMAL / 8  (DoS floor)
     */
    const unsigned Q_ADD_NORMAL = 64u;
    const unsigned Q_HIT        = 2048u;   /* fixed: not scaled with q_add */
    const u64 SIM_TICK    = (u64)Q_ADD_NORMAL;
    const unsigned SETPT   = N * 68u / 100u;
    const u64 T_NORMAL    = (u64)SETPT;
    const u64 T_MIN       = T_NORMAL / 8u;
    const u64 SIM_NOW_START = T_NORMAL * 2u;

    /*
     * hit ring: only Q_HIT "hot" entries (indices 1..Q_HIT).
     * These entries are touched every batch via find_touch and stay alive.
     * All other pre-fill entries (Q_HIT+1..SETPT) are never touched and
     * expire naturally after T_NORMAL, giving the correct turnover rate.
     */
    const unsigned N_HOT = Q_HIT;
    u32 *hit_ring = calloc(N_HOT, sizeof(u32));
    assert(hit_ring);
    for (unsigned i = 0u; i < N_HOT; i++)
        hit_ring[i] = i + 1u;

    /* free pool: N - SETPT entries */
    fring_t fpool;
    fring_init(&fpool, ring_mem, N);
    for (unsigned i = SETPT + 1u; i <= N; i++)
        fring_push(&fpool, i);

    /* --- init table --- */
    struct ft_table_extra       ft;
    struct ft_table_extra_config cfg = { .ts_shift = TS_SHIFT };
    struct ft_maint_extra_ctx   mctx;

    {
        int rc = flow4_extra_table_init(&ft, pool, N, bk, be_sz, &cfg);
        (void)rc;
        assert(rc == 0);
        rc = ft_table_extra_maint_ctx_init(&ft, &mctx);
        assert(rc == 0);
    }

    /*
     * Pre-fill with uniform age distribution [0, T_NORMAL].
     * Entry i: ts = SIM_NOW_START - i * T_NORMAL / SETPT.
     * At this distribution, exactly SIM_TICK entries expire per batch
     * at the start of Phase 1, matching Q_ADD_NORMAL=64 adds/batch.
     */
    {
        u32 idxv[1], unused[1];
        for (unsigned i = 0u; i < SETPT; i++) {
            u64 ts = SIM_NOW_START - (u64)i * T_NORMAL / (u64)SETPT;
            idxv[0] = i + 1u;
            (void)ft_table_extra_add_idx_bulk(&ft, idxv, 1u,
                                               FT_ADD_IGNORE, ts, unused);
        }
    }

    printf("  N=1M  setpoint=%u(%.0f%%)  ceiling=74%%\n", SETPT,
           (double)SETPT * 100.0 / (double)N);
    printf("  T_NORMAL=%llu sim-ticks  T_MIN=%llu  SIM_TICK=%llu\n",
           (unsigned long long)T_NORMAL,
           (unsigned long long)T_MIN,
           (unsigned long long)SIM_TICK);
    printf("  expiry period ~%u batches  initial fill %.1f%%\n\n",
           SETPT / Q_ADD_NORMAL,
           (double)ft_table_extra_nb_entries(&ft) * 100.0 / (double)N);

    /* --- controller --- */
    struct ft_fill_ctrl ctrl;
    ft_fill_ctrl_init(&ctrl, N, 68u, 74u, FT_MISS_X1024(3),
                      T_NORMAL, T_MIN);
    ctrl.budget_max = SWEEP_CAP;

    u64 sim_now = SIM_NOW_START;

    /*
     * Phase 1: normal traffic (12 K batches > one expiry period).
     * Shows fill converging to and holding at setpoint once the first wave
     * of pre-fill entries starts expiring at ~batch 11 K.
     */
    run_phase("Phase1:normal", &ft, &mctx, pool, N, &fpool,
              hit_ring, N_HOT,
              Q_ADD_NORMAL, Q_HIT, 12000u,
              SIM_TICK, &sim_now, &ctrl, T_NORMAL,
              add_buf, maint_buf, sweep_buf, SWEEP_CAP);

    /*
     * Phase 2: DoS attack (Q_add=512, Q_hit=2048 -> miss=20%).
     * Controller detects elevated miss rate, reduces expire_tsc to ~15%
     * of T_NORMAL.  Phase-1 entries older than 15% of T_NORMAL become
     * immediately stale; sweep evicts them, fill is pulled back down.
     */
    run_phase("Phase2:DoS", &ft, &mctx, pool, N, &fpool,
              hit_ring, N_HOT,
              512u, Q_HIT, 2000u,
              SIM_TICK, &sim_now, &ctrl, T_NORMAL,
              add_buf, maint_buf, sweep_buf, SWEEP_CAP);

    /*
     * Phase 3: recovery (Q_add=64, Q_hit=2048 -> miss=3%).
     * Miss rate drops; controller gradually restores expire_tsc toward
     * T_NORMAL.  Fill stabilises around setpoint again.
     */
    run_phase("Phase3:recovery", &ft, &mctx, pool, N, &fpool,
              hit_ring, N_HOT,
              Q_ADD_NORMAL, Q_HIT, 4000u,
              SIM_TICK, &sim_now, &ctrl, T_NORMAL,
              add_buf, maint_buf, sweep_buf, SWEEP_CAP);

    printf("  final fill: %u / %u (%.1f%%)\n",
           ft_table_extra_nb_entries(&ft), N,
           (double)ft_table_extra_nb_entries(&ft) * 100.0 / (double)N);

    ft_table_extra_destroy(&ft);
    munmap(pool, pe_sz); munmap(bk, be_sz);
    free(add_buf); free(maint_buf); free(sweep_buf);
    free(ring_mem); free(hit_ring);
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
