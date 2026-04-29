/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * usage_flowtable.c - readable flowtable API usage sample.
 *
 * This file is not a functional test and not a benchmark.  It is intentionally
 * small, linear sample code that shows the normal API shape.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flow_table.h"
#include "ft_fill_ctrl.h"

enum {
    SAMPLE_N = 1024u,
    SAMPLE_NOW = 1000000u,
};

struct sample_flow4_record {
    struct flow4_entry flow;
    unsigned payload;
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

struct sample_flow4_extra_record {
    struct flow4_extra_entry flow;
    unsigned payload;
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

static void *
xcalloc(size_t count, size_t size)
{
    void *p = calloc(count, size);

    if (p == NULL) {
        perror("calloc");
        exit(1);
    }
    return p;
}

static void *
xaligned_calloc(size_t count, size_t size, size_t align)
{
    void *p = NULL;
    size_t bytes = count * size;
    int rc;

    rc = posix_memalign(&p, align, bytes);
    if (rc != 0) {
        fprintf(stderr, "posix_memalign failed: %d\n", rc);
        exit(1);
    }
    memset(p, 0, bytes);
    return p;
}

static void
check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "usage sample failed: %s\n", what);
        exit(1);
    }
}

static struct flow4_key
flow4_key_make(unsigned i)
{
    struct flow4_key key = { 0 };

    key.family = 4u;
    key.proto = 6u;
    key.src_ip = UINT32_C(0x0a000000) | i;
    key.dst_ip = UINT32_C(0x0b000000) | (i + 1u);
    key.src_port = (u16)(10000u + i);
    key.dst_port = (u16)(20000u + i);
    key.vrfid = 1u;
    return key;
}

static struct flow4_extra_key
flow4_extra_key_make(unsigned i)
{
    struct flow4_extra_key key = { 0 };

    key.family = 4u;
    key.proto = 6u;
    key.src_addr = UINT32_C(0x0a000000) | i;
    key.dst_addr = UINT32_C(0x0b000000) | (i + 1u);
    key.src_port = (u16)(10000u + i);
    key.dst_port = (u16)(20000u + i);
    return key;
}

static int
walk_count_cb(u32 entry_idx, void *arg)
{
    unsigned *count = arg;

    (void)entry_idx;
    (*count)++;
    return 0;
}

static void
sample_pure_flow4(void)
{
    struct sample_flow4_record *records;
    struct ft_table ft;
    struct ft_table_config cfg = { .ts_shift = FLOW_TIMESTAMP_DEFAULT_SHIFT };
    struct ft_table_stats stats;
    struct flow_status status;
    struct ft_maint_ctx maint;
    struct ft_table_result results[2];
    struct flow4_key keys[2];
    u32 idxv[2] = { 2u, 3u };
    u32 unused[8];
    u32 expired[8];
    void *buckets;
    void *new_buckets;
    size_t bucket_size;
    size_t new_bucket_size;
    unsigned next_bk = 0u;
    unsigned walked = 0u;
    unsigned n;

    records = xaligned_calloc(SAMPLE_N, sizeof(*records),
                              FT_TABLE_CACHE_LINE_SIZE);

    /*
     * The caller owns bucket memory.  The buffer may be unaligned; init
     * carves out an aligned power-of-2 bucket region internally.
     */
    bucket_size = ft_table_bucket_size(SAMPLE_N) + FT_TABLE_BUCKET_ALIGN;
    buckets = xcalloc(1u, bucket_size);

    /*
     * Normal user entry point: include flow_table.h and initialize through the
     * generic facade.  The typed macro derives stride and entry offset.
     */
    check(FT_TABLE_INIT_TYPED(&ft, FT_TABLE_VARIANT_FLOW4,
                              records, SAMPLE_N,
                              struct sample_flow4_record, flow,
                              buckets, bucket_size, &cfg) == 0,
          "pure init");

    /* Fill caller-owned records before registering their indices. */
    records[0].flow.key = flow4_key_make(1u);
    records[0].payload = 100u;
    check(FT_TABLE_ADD_IDX(&ft, 1u, SAMPLE_NOW) == 1u, "pure add idx");

    /*
     * Key-specific APIs are intentionally not hidden by FT_TABLE_* because
     * flow4/flow6/flowu key types differ.
     */
    check(ft_flow4_table_find(&ft, &records[0].flow.key, SAMPLE_NOW + 1u)
          == 1u, "pure find");

    /* Bulk add uses an in/out index array and returns unused indices. */
    records[1].flow.key = flow4_key_make(2u);
    records[2].flow.key = flow4_key_make(3u);
    n = FT_TABLE_ADD_IDX_BULK(&ft, idxv, 2u, FT_ADD_IGNORE,
                              SAMPLE_NOW + 2u, unused);
    (void)n;

    /* Bulk find writes one result per key. */
    keys[0] = flow4_key_make(1u);
    keys[1] = flow4_key_make(2u);
    ft_flow4_table_find_bulk(&ft, keys, 2u, SAMPLE_NOW + 3u, results);

    /* Stats and status are protocol-independent facade APIs. */
    FT_TABLE_STATS(&ft, &stats);
    FT_TABLE_STATUS(&ft, &status);
    printf("pure: entries=%u lookups=%llu\n",
           FT_TABLE_NB_ENTRIES(&ft),
           (unsigned long long)stats.core.lookups);

    /*
     * add_idx_bulk_maint combines add work with local expiration on buckets
     * touched by the add results.  max_unused - nb_keys is maint budget.
     */
    idxv[0] = 1u;
    (void)FT_TABLE_ADD_IDX_BULK_MAINT(&ft, idxv, 1u, FT_ADD_IGNORE,
                                      SAMPLE_NOW + 4u, 100u,
                                      unused, 4u, 1u);

    /*
     * Table-wide maintenance is explicit.  Keep next_bk and feed it back into
     * later calls to sweep continuously over time.
     */
    check(FT_TABLE_MAINT_CTX_INIT(&ft, &maint) == 0, "pure maint ctx");
    n = FT_TABLE_MAINTAIN(&maint, next_bk, SAMPLE_NOW + 5u, 100u,
                          expired, 8u, 1u, &next_bk);
    (void)n;

    /* Walk exposes registered entry indices in bucket order. */
    check(FT_TABLE_WALK(&ft, walk_count_cb, &walked) == 0, "pure walk");

    /*
     * Migrate only moves bucket state.  Records stay where the caller put
     * them.  Free the old bucket buffer after successful migration.
     */
    new_bucket_size = ft_table_bucket_mem_size(FT_TABLE_NB_BK(&ft) * 2u)
                    + FT_TABLE_BUCKET_ALIGN;
    new_buckets = xcalloc(1u, new_bucket_size);
    check(FT_TABLE_MIGRATE(&ft, new_buckets, new_bucket_size) == 0,
          "pure migrate");
    free(buckets);
    buckets = new_buckets;

    /* Delete by key or by index, then destroy the handle. */
    (void)ft_flow4_table_del_key_oneshot(&ft, &records[0].flow.key);
    (void)FT_TABLE_DEL_IDX(&ft, 2u);
    (void)FT_TABLE_DEL_IDX_BULK(&ft, idxv, 1u, unused);
    FT_TABLE_DESTROY(&ft);

    free(buckets);
    free(records);
}

static void
sample_extra_flow4(void)
{
    struct sample_flow4_extra_record *records;
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = {
        .ts_shift = FLOW_TIMESTAMP_DEFAULT_SHIFT,
    };
    struct ft_table_extra_stats stats;
    struct ft_maint_extra_ctx maint;
    u32 results[2];
    u32 idxv[2] = { 1u, 2u };
    u32 unused[8];
    u32 expired[8];
    void *buckets;
    size_t bucket_size;
    unsigned next_bk = 0u;

    records = xaligned_calloc(SAMPLE_N, sizeof(*records),
                              FT_TABLE_CACHE_LINE_SIZE);
    bucket_size = ft_table_extra_bucket_size(SAMPLE_N) + FT_TABLE_BUCKET_ALIGN;
    buckets = xcalloc(1u, bucket_size);

    /*
     * Slot-extra stores timestamps in bucket extra[] slots, reducing record
     * touches during maintenance.  It still uses the same FT_TABLE_* facade
     * for protocol-independent work.
     */
    check(FT_TABLE_INIT_TYPED(&ft, FT_TABLE_VARIANT_FLOW4,
                              records, SAMPLE_N,
                              struct sample_flow4_extra_record, flow,
                              buckets, bucket_size, &cfg) == 0,
          "extra init");

    records[0].flow.key = flow4_extra_key_make(1u);
    records[1].flow.key = flow4_extra_key_make(2u);
    check(FT_TABLE_ADD_IDX(&ft, 1u, SAMPLE_NOW) == 1u, "extra add idx");
    check(FT_TABLE_ADD_IDX_BULK(&ft, idxv, 2u, FT_ADD_IGNORE,
                                SAMPLE_NOW + 1u, unused) <= 2u,
          "extra add bulk");

    /*
     * Extra key-specific lookup uses compact extra key types.  Passing now=0
     * means non-touching lookup; non-zero refreshes bucket extra[] timestamp.
     */
    check(flow4_extra_table_find(&ft, &records[0].flow.key) == 1u,
          "extra find");
    check(flow4_extra_table_find_touch(&ft, &records[0].flow.key,
                                       SAMPLE_NOW + 2u) == 1u,
          "extra find touch");
    (void)flow4_extra_table_find_bulk(&ft, &records[0].flow.key, 2u,
                                      SAMPLE_NOW + 3u, results);

    FT_TABLE_STATS(&ft, &stats);
    printf("extra: entries=%u lookups=%llu\n",
           FT_TABLE_NB_ENTRIES(&ft),
           (unsigned long long)stats.core.lookups);

    /*
     * Maintenance facade dispatches on context type.  Pure uses
     * ft_maint_ctx; extra uses ft_maint_extra_ctx.
     */
    check(FT_TABLE_MAINT_CTX_INIT(&ft, &maint) == 0, "extra maint ctx");
    (void)FT_TABLE_MAINTAIN(&maint, next_bk, SAMPLE_NOW + 4u, 100u,
                            expired, 8u, 1u, &next_bk);

    (void)flow4_extra_table_del(&ft, &records[0].flow.key);
    FT_TABLE_FLUSH(&ft);
    FT_TABLE_DESTROY(&ft);

    free(buckets);
    free(records);
}

static void
sample_fill_controller(void)
{
    struct ft_fill_ctrl ctrl;
    unsigned sweep_budget;
    unsigned start_bk;
    u64 expire_tsc;

    /*
     * The controller is optional.  It helps keep fill in the Green band by
     * increasing sweep work and shortening timeout as fill rises.
     */
    ft_fill_ctrl_init(&ctrl, SAMPLE_N, 68u, 74u, FT_MISS_X1024(3),
                      23000000000ULL, 3000000000ULL);

    ft_fill_ctrl_compute(&ctrl,
                         700u,  /* current table fill */
                         8u,    /* previous batch add attempts */
                         248u,  /* previous batch lookup hits */
                         0u,    /* previous maintenance cursor */
                         &sweep_budget, &start_bk, &expire_tsc);

    /*
     * In a real datapath, pass start_bk/expire_tsc/sweep_budget to
     * FT_TABLE_MAINTAIN(), then keep the returned next_bk for the next batch.
     */
    printf("ctrl: sweep_budget=%u start_bk=%u expire_tsc=%llu\n",
           sweep_budget, start_bk, (unsigned long long)expire_tsc);
}

int
main(void)
{
    /*
     * Runtime dispatch setup.  FT_ARCH_AUTO allows the library to choose the
     * best compiled handler supported by the current CPU.
     */
    ft_arch_init(FT_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);

    sample_pure_flow4();
    sample_extra_flow4();
    sample_fill_controller();

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
