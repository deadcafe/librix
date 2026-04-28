/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * ft_fill_ctrl.h - adaptive fill-rate controller for flow tables.
 *
 * Two-loop design:
 *
 *   Inner loop (per-batch): fill_delta EWMA -> sweep_budget
 *     Measures how fast fill is changing and computes the sweep budget
 *     needed to bring fill back to setpoint.
 *
 *   Outer loop (per-batch): miss-rate EWMA -> expire_tsc
 *     Detects traffic anomaly (DoS) from the cache miss-rate ratio and
 *     shortens the expiry timeout proportionally, keeping fill stable
 *     even when new-flow arrival rate spikes.
 *
 * Steady-state model:
 *   fill = add_rate [flows/s] * timeout [s]
 *   timeout = fill_target / add_rate
 *
 * Example (N=1M, 70% target, 1 Mpps, 3% miss):
 *   add_rate = 1e6 * 0.03 = 30,000 flows/s
 *   timeout  = 700,000 / 30,000 ≈ 23 s  -> t_normal ≈ 23 * TSC_HZ
 *
 * Usage sketch:
 *   struct ft_fill_ctrl ctrl;
 *   ft_fill_ctrl_init(&ctrl, N, 68, 74,
 *                     FT_MISS_X1024(3),          // 3% normal miss
 *                     23ULL * tsc_hz,            // t_normal
 *                     3ULL  * tsc_hz);           // t_min (DoS floor)
 *
 *   unsigned prev_added = 0, prev_hits = 0, prev_next_bk = 0;
 *   for (;;) {
 *       unsigned budget, start_bk;
 *       u64 tmo;
 *       ft_fill_ctrl_compute(&ctrl, nb_entries(),
 *                            prev_added, prev_hits, prev_next_bk,
 *                            &budget, &start_bk, &tmo);
 *
 *       prev_added = ft_*_add_idx_bulk_maint(..., tmo, ...);
 *       prev_hits  = do_hits(...);
 *       prev_next_bk = start_bk;
 *       if (budget > 0)
 *           ft_*_maintain(start_bk, now, tmo, buf, budget, 1, &prev_next_bk);
 *   }
 */

#ifndef _FT_FILL_CTRL_H_
#define _FT_FILL_CTRL_H_

#include <stddef.h>
#include <string.h>

#include <rix/rix_defs_private.h>

/**
 * @brief Convert a miss-rate percentage to the x1024 fixed-point form used by
 *        ft_fill_ctrl.
 *
 * Example: FT_MISS_X1024(3) = 30  (3% expressed as 30/1024 ≈ 2.93%)
 */
#define FT_MISS_X1024(pct)  ((u32)((pct) * 1024u / 100u))

/**
 * @brief Adaptive fill-rate controller state.
 *
 * All fields are managed internally; callers should initialise with
 * ft_fill_ctrl_init() and drive the loop with ft_fill_ctrl_compute().
 */
struct ft_fill_ctrl {
    /* --- configuration (set once at ft_fill_ctrl_init()) --- */

    /** Target fill count (e.g. 68% * N). */
    unsigned  setpoint;

    /**
     * Emergency upper limit (e.g. 74% * N).
     * When current_fill >= ceiling the sweep budget is forced to budget_max.
     */
    unsigned  ceiling;

    /** Minimum non-zero sweep budget returned when excess > 0. */
    unsigned  budget_min;

    /** Maximum sweep budget per batch. */
    unsigned  budget_max;

    /**
     * Inner-loop EWMA decay shift for fill_delta.
     * Window ≈ 2^inner_shift batches (default 3 → ~8 batches).
     */
    unsigned  inner_shift;

    /**
     * Outer-loop EWMA decay shift for miss-rate.
     * Window ≈ 2^outer_shift batches (default 4 → ~16 batches).
     */
    unsigned  outer_shift;

    /** Baseline miss rate × 1024 (normal operating point). */
    u32       normal_miss_x1024;

    /**
     * Normal expire_tsc in raw TSC ticks.
     * Derive from seconds: t_normal = seconds * tsc_hz.
     */
    u64       t_normal;

    /**
     * Minimum expire_tsc in raw TSC ticks (DoS floor).
     * Returned when the miss-rate-adjusted timeout would go below this value.
     */
    u64       t_min;

    /* --- per-batch state (updated by ft_fill_ctrl_compute()) --- */

    /** Rolling sweep start position (bucket index). */
    unsigned  cursor;

    /**
     * EWMA of (fill[t] - fill[t-1]) in entries per batch.
     * Positive means fill is rising; negative means falling.
     */
    int       fill_delta_ewma;

    /** EWMA of miss rate × 1024. */
    u32       miss_rate_x1024;

    /** Fill count observed at the previous ft_fill_ctrl_compute() call. */
    unsigned  last_fill;
};

/**
 * @brief Initialise a fill-rate controller.
 *
 * @param c                 Controller to initialise.
 * @param N                 Table capacity (max_entries).
 * @param setpoint_pct      Target fill percentage (e.g. 68).
 * @param ceiling_pct       Emergency upper limit percentage (e.g. 74).
 * @param normal_miss_x1024 Baseline miss rate × 1024 (use FT_MISS_X1024()).
 * @param t_normal          Normal expire_tsc in raw TSC ticks.
 * @param t_min             Minimum expire_tsc in raw TSC ticks (DoS floor).
 */
static inline void
ft_fill_ctrl_init(struct ft_fill_ctrl *c,
                  unsigned N,
                  unsigned setpoint_pct,
                  unsigned ceiling_pct,
                  u32      normal_miss_x1024,
                  u64      t_normal,
                  u64      t_min)
{
    memset(c, 0, sizeof(*c));
    c->setpoint          = N * setpoint_pct / 100u;
    c->ceiling           = N * ceiling_pct  / 100u;
    c->budget_min        = 16u;
    c->budget_max        = 512u;
    c->inner_shift       = 3u;
    c->outer_shift       = 4u;
    c->normal_miss_x1024 = normal_miss_x1024;
    c->t_normal          = t_normal;
    c->t_min             = t_min;
    c->miss_rate_x1024   = normal_miss_x1024;  /* warm start at baseline */
}

/**
 * @brief Per-batch control computation.
 *
 * Call once at the start of each batch.  Feeds back results from the
 * *previous* batch and outputs control parameters for *this* batch.
 *
 * Inner loop (sweep_budget):
 *   Updates fill_delta_ewma from (current_fill - last_fill).
 *   budget = max(0, excess + max(0, trend)), clamped to [budget_min, budget_max].
 *   Forced to budget_max when current_fill >= ceiling.
 *
 * Outer loop (expire_tsc):
 *   Updates miss_rate_x1024 from prev_n_added / (prev_n_added + prev_n_hits).
 *   expire_tsc = t_normal * normal_miss / current_miss, floored at t_min.
 *
 * @param c              Controller state.
 * @param current_fill   Current nb_entries() of the table.
 * @param prev_n_added   Add-path requests in the previous batch
 *                       (new flow attempts, regardless of table success).
 * @param prev_n_hits    Hit-path lookups in the previous batch.
 * @param prev_next_bk   next_bk returned by the previous sweep call
 *                       (pass 0 on the first batch).
 * @param sweep_budget   Out: max_expired to pass to ft_*_maintain().
 *                       0 means skip the sweep this batch.
 * @param start_bk       Out: start_bk to pass to ft_*_maintain().
 * @param expire_tsc     Out: timeout to pass to both add_idx_bulk_maint()
 *                       and ft_*_maintain().
 */
static inline void
ft_fill_ctrl_compute(struct ft_fill_ctrl *c,
                     unsigned  current_fill,
                     unsigned  prev_n_added,
                     unsigned  prev_n_hits,
                     unsigned  prev_next_bk,
                     unsigned *sweep_budget,
                     unsigned *start_bk,
                     u64      *expire_tsc)
{
    /* advance cursor from previous sweep */
    c->cursor = prev_next_bk;
    *start_bk = c->cursor;

    /* --- outer loop: miss-rate EWMA -> expire_tsc --- */
    {
        unsigned total = prev_n_added + prev_n_hits;
        u32 miss_x1024 = (total > 0u)
            ? (u32)((unsigned long long)prev_n_added * 1024u / total)
            : c->normal_miss_x1024;

        /* EWMA: ema += (x - ema) >> shift */
        c->miss_rate_x1024 = (u32)((int)c->miss_rate_x1024
            + (((int)miss_x1024 - (int)c->miss_rate_x1024) >> (int)c->outer_shift));

        /* expire_tsc = t_normal * normal_miss / current_miss */
        if (c->miss_rate_x1024 <= c->normal_miss_x1024) {
            *expire_tsc = c->t_normal;
        } else {
            u64 tmo = c->t_normal * (u64)c->normal_miss_x1024
                      / (u64)c->miss_rate_x1024;
            *expire_tsc = (tmo < c->t_min) ? c->t_min : tmo;
        }
    }

    /* --- inner loop: fill_delta EWMA -> sweep_budget --- */
    {
        int delta = (int)current_fill - (int)c->last_fill;
        c->fill_delta_ewma += (delta - c->fill_delta_ewma) >> (int)c->inner_shift;
        c->last_fill = current_fill;

        int excess   = (int)current_fill - (int)c->setpoint;
        int trend    = c->fill_delta_ewma;
        int required = excess + (trend > 0 ? trend : 0);

        if (current_fill >= c->ceiling) {
            /* emergency: above hard limit, force maximum */
            *sweep_budget = c->budget_max;
        } else if (required <= 0) {
            *sweep_budget = 0u;
        } else {
            unsigned b = (unsigned)required;
            if (b < c->budget_min) b = c->budget_min;
            if (b > c->budget_max) b = c->budget_max;
            *sweep_budget = b;
        }
    }
}

#endif /* _FT_FILL_CTRL_H_ */
