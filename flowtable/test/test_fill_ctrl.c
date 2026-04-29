/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ft_fill_ctrl.h"

static void
check_u64(const char *name, u64 got, u64 want)
{
    if (got != want) {
        fprintf(stderr, "%s: got=%llu want=%llu\n", name,
                (unsigned long long)got, (unsigned long long)want);
        exit(1);
    }
}

static void
check_unsigned(const char *name, unsigned got, unsigned want)
{
    if (got != want) {
        fprintf(stderr, "%s: got=%u want=%u\n", name, got, want);
        exit(1);
    }
}

static void
test_zone_timeout(void)
{
    struct ft_fill_ctrl ctrl;
    unsigned budget;
    unsigned start_bk;
    u64 tmo;

    ft_fill_ctrl_init(&ctrl, 1000u, 68u, 74u, FT_MISS_X1024(3),
                      1000000u, 125000u);

    ft_fill_ctrl_compute(&ctrl, 740u, 3u, 97u, 0u,
                         &budget, &start_bk, &tmo);
    check_u64("green timeout", tmo, 1000000u);

    ft_fill_ctrl_compute(&ctrl, 750u, 3u, 97u, start_bk,
                         &budget, &start_bk, &tmo);
    check_u64("yellow timeout", tmo, 250000u);

    ft_fill_ctrl_compute(&ctrl, 850u, 3u, 97u, start_bk,
                         &budget, &start_bk, &tmo);
    check_u64("red timeout", tmo, 125000u);

    ft_fill_ctrl_compute(&ctrl, 950u, 3u, 97u, start_bk,
                         &budget, &start_bk, &tmo);
    check_u64("critical timeout", tmo, 125000u);
}

static void
test_budget_pressure(void)
{
    struct ft_fill_ctrl ctrl;
    unsigned budget;
    unsigned start_bk;
    u64 tmo;

    ft_fill_ctrl_init(&ctrl, 1000u, 68u, 74u, FT_MISS_X1024(3),
                      1000000u, 125000u);

    ft_fill_ctrl_compute(&ctrl, 650u, 3u, 97u, 123u,
                         &budget, &start_bk, &tmo);
    check_unsigned("start_bk", start_bk, 123u);

    ft_fill_ctrl_compute(&ctrl, 741u, 3u, 97u, 456u,
                         &budget, &start_bk, &tmo);
    check_unsigned("ceiling budget", budget, ctrl.budget_max);
    check_unsigned("cursor", start_bk, 456u);
}

int
main(void)
{
    test_zone_timeout();
    test_budget_pressure();
    printf("fill_ctrl tests passed\n");
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
