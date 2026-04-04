/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Unit tests for rix_hash_32.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "rix_hash_32.h"

/*---------------------------------------------------------------------------
 * Test node definition
 *---------------------------------------------------------------------------*/
typedef struct mynode_s {
    u32 key;
    u32 val;
} mynode_t;

/*---------------------------------------------------------------------------
 * Test helpers
 *---------------------------------------------------------------------------*/
#define NB_BUCKETS   256          /* must be power of 2 */
#define NB_NODES     (NB_BUCKETS * 16 / 2)  /* ~50% load factor */
#define INVALID_KEY  0xFFFFFFFFu  /* sentinel; never used as a real key */

RIX_HASH32_HEAD(myht32);
RIX_HASH32_GENERATE(myht32, mynode_t, key, INVALID_KEY)

static struct myht32              head;
static struct rix_hash32_bucket_s buckets[NB_BUCKETS];
static mynode_t                   nodes[NB_NODES];

static void
reset_table(void)
{
    RIX_HASH32_INIT(myht32, &head, buckets, NB_BUCKETS);
    memset(nodes, 0, sizeof(nodes));
}

#define PASS(fmt, ...) printf("  PASS: " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) do {                                                    \
    printf("  FAIL: " fmt " [%s:%d]\n", ##__VA_ARGS__, __FILE__, __LINE__);   \
    abort();                                                                   \
} while (0)

/*---------------------------------------------------------------------------
 * Test: basic insert and find
 *---------------------------------------------------------------------------*/
static void
test_insert_find(void)
{
    printf("[test_insert_find]\n");
    reset_table();

    nodes[0].key = 42;
    nodes[0].val = 99;

    mynode_t *r = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != NULL)
        FAIL("insert returned non-NULL: %p", (void *)r);

    if (head.rhh_nb != 1)
        FAIL("rhh_nb expected 1, got %u", head.rhh_nb);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 42);
    if (f != &nodes[0])
        FAIL("find returned wrong node: %p vs %p", (void *)f, (void *)&nodes[0]);

    if (f->val != 99)
        FAIL("val mismatch: %u", f->val);

    PASS("insert and find key=42");
}

/*---------------------------------------------------------------------------
 * Test: find miss
 *---------------------------------------------------------------------------*/
static void
test_find_miss(void)
{
    printf("[test_find_miss]\n");
    reset_table();

    nodes[0].key = 10;
    RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 999);
    if (f != NULL)
        FAIL("find_miss returned non-NULL: %p", (void *)f);

    PASS("find miss returns NULL");
}

/*---------------------------------------------------------------------------
 * Test: duplicate insert returns existing node
 *---------------------------------------------------------------------------*/
static void
test_duplicate_insert(void)
{
    printf("[test_duplicate_insert]\n");
    reset_table();

    nodes[0].key = 7;
    nodes[1].key = 7;

    mynode_t *r0 = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    if (r0 != NULL)
        FAIL("first insert returned non-NULL: %p", (void *)r0);

    mynode_t *r1 = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[1]);
    if (r1 != &nodes[0])
        FAIL("duplicate insert returned %p, expected %p",
             (void *)r1, (void *)&nodes[0]);

    if (head.rhh_nb != 1)
        FAIL("rhh_nb should be 1 after dup insert, got %u", head.rhh_nb);

    PASS("duplicate insert returns existing node");
}

/*---------------------------------------------------------------------------
 * Test: remove
 *---------------------------------------------------------------------------*/
static void
test_remove(void)
{
    printf("[test_remove]\n");
    reset_table();

    nodes[0].key = 55;
    RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);

    mynode_t *r = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != &nodes[0])
        FAIL("remove returned wrong node: %p", (void *)r);

    if (head.rhh_nb != 0)
        FAIL("rhh_nb expected 0 after remove, got %u", head.rhh_nb);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 55);
    if (f != NULL)
        FAIL("find after remove returned non-NULL: %p", (void *)f);

    PASS("remove and subsequent find");
}

/*---------------------------------------------------------------------------
 * Test: remove not-in-table returns NULL
 *---------------------------------------------------------------------------*/
static void
test_remove_miss(void)
{
    printf("[test_remove_miss]\n");
    reset_table();

    nodes[0].key = 3;
    /* not inserted */

    mynode_t *r = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != NULL)
        FAIL("remove_miss returned non-NULL: %p", (void *)r);

    PASS("remove of not-in-table returns NULL");
}

/*---------------------------------------------------------------------------
 * Test: invalid_key slots are written correctly by name_init
 *---------------------------------------------------------------------------*/
static void
test_init_invalid_key(void)
{
    printf("[test_init_invalid_key]\n");
    reset_table();

    /* After init, every key slot must hold INVALID_KEY and every idx RIX_NIL */
    unsigned bad_key = 0, bad_idx = 0;
    for (unsigned b = 0; b < NB_BUCKETS; b++) {
        for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
            if (buckets[b].key[s] != INVALID_KEY) bad_key++;
            if (buckets[b].idx[s] != (u32)RIX_NIL) bad_idx++;
        }
    }
    if (bad_key)
        FAIL("init: %u slots have key != INVALID_KEY", bad_key);
    if (bad_idx)
        FAIL("init: %u slots have idx != RIX_NIL", bad_idx);

    PASS("name_init fills all slots with invalid_key / RIX_NIL");
}

/*---------------------------------------------------------------------------
 * Test: remove restores invalid_key (not 0) to the freed slot
 *---------------------------------------------------------------------------*/
static void
test_remove_restores_invalid_key(void)
{
    printf("[test_remove_restores_invalid_key]\n");
    reset_table();

    nodes[0].key = 42;
    RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);

    /* All key slots must be INVALID_KEY again */
    unsigned bad = 0;
    for (unsigned b = 0; b < NB_BUCKETS; b++)
        for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++)
            if (buckets[b].idx[s] == (u32)RIX_NIL &&
                buckets[b].key[s] != INVALID_KEY)
                bad++;
    if (bad)
        FAIL("remove: %u empty slots still have non-invalid key", bad);

    PASS("remove restores invalid_key to freed slot");
}

/*---------------------------------------------------------------------------
 * Test: key = 0 is a valid key when invalid_key = UINT32_MAX
 *---------------------------------------------------------------------------*/
static void
test_key_zero(void)
{
    printf("[test_key_zero]\n");
    reset_table();   /* invalid_key = UINT32_MAX; key=0 is valid */

    nodes[0].key = 0;
    nodes[0].val = 123;

    mynode_t *r = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != NULL)
        FAIL("insert key=0 returned non-NULL: %p", (void *)r);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 0);
    if (f != &nodes[0])
        FAIL("find key=0 returned wrong node: %p", (void *)f);

    mynode_t *rem = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);
    if (rem != &nodes[0])
        FAIL("remove key=0 returned wrong node: %p", (void *)rem);

    mynode_t *f2 = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 0);
    if (f2 != NULL)
        FAIL("find key=0 after remove returned non-NULL: %p", (void *)f2);

    PASS("key=0 insert/find/remove (invalid_key=UINT32_MAX)");
}

/*---------------------------------------------------------------------------
 * Test: walk
 *---------------------------------------------------------------------------*/
static int
walk_cb(mynode_t *node, void *arg)
{
    unsigned *cnt = (unsigned *)arg;
    (*cnt)++;
    (void)node;
    return 0;
}

static void
test_walk(void)
{
    printf("[test_walk]\n");
    reset_table();

    /* Insert 8 nodes with distinct keys */
    for (int i = 0; i < 8; i++) {
        nodes[i].key = (u32)(100 + i);
        mynode_t *r  = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != NULL)
            FAIL("insert[%d] returned non-NULL", i);
    }

    unsigned cnt = 0;
    int wret = RIX_HASH32_WALK(myht32, &head, buckets, nodes, walk_cb, &cnt);
    if (wret != 0)
        FAIL("walk returned non-zero: %d", wret);
    if (cnt != 8)
        FAIL("walk visited %u nodes, expected 8", cnt);

    PASS("walk visits all %u inserted nodes", cnt);
}

/*---------------------------------------------------------------------------
 * Test: bulk insert / find at ~50% load
 *---------------------------------------------------------------------------*/
static void
test_bulk(void)
{
    printf("[test_bulk]\n");
    reset_table();

    unsigned inserted = 0;

    for (unsigned i = 0; i < NB_NODES; i++) {
        nodes[i].key = i + 1;   /* avoid key=0 here */
        nodes[i].val = i * 2;
        mynode_t *r = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r == NULL) {
            inserted++;
        } else if (r == &nodes[i]) {
            /* table full */
            break;
        }
        /* r == other: duplicate (should not happen with unique keys) */
    }

    if (inserted != head.rhh_nb)
        FAIL("rhh_nb mismatch: inserted=%u rhh_nb=%u", inserted, head.rhh_nb);

    /* Verify all inserted nodes are findable */
    unsigned found = 0;
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, nodes[i].key);
        if (f == NULL)
            FAIL("find failed for key=%u (i=%u)", nodes[i].key, i);
        if (f->val != nodes[i].val)
            FAIL("val mismatch for key=%u: got %u, expected %u",
                 nodes[i].key, f->val, nodes[i].val);
        found++;
    }

    PASS("bulk: inserted=%u found=%u load=%.1f%%",
         inserted, found, 100.0 * inserted / (NB_BUCKETS * 16));
}

/*---------------------------------------------------------------------------
 * Test: staged find x4
 *---------------------------------------------------------------------------*/
static void
test_staged_x4(void)
{
    printf("[test_staged_x4]\n");
    reset_table();

    /* Insert 4 nodes */
    for (int i = 0; i < 4; i++) {
        nodes[i].key = (u32)(200 + i);
        nodes[i].val = (u32)(i + 1);
        mynode_t *r  = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != NULL)
            FAIL("insert[%d] failed", i);
    }

    /* Staged x4 find */
    struct rix_hash32_find_ctx_s ctx[4];
    u32 keys[4] = { 200, 201, 202, 203 };
    mynode_t *results[4];

    RIX_HASH32_HASH_KEY4(myht32, ctx, &head, buckets, keys);
    RIX_HASH32_SCAN_BK4(myht32, ctx, &head, buckets);
    RIX_HASH32_PREFETCH_NODE4(myht32, ctx, nodes);
    RIX_HASH32_CMP_KEY4(myht32, ctx, nodes, results);

    for (int i = 0; i < 4; i++) {
        if (results[i] != &nodes[i])
            FAIL("staged x4: result[%d]=%p expected=%p",
                 i, (void *)results[i], (void *)&nodes[i]);
    }

    PASS("staged find x4 returns correct nodes");
}

/*---------------------------------------------------------------------------
 * Test: remove all inserted nodes
 *---------------------------------------------------------------------------*/
static void
test_remove_all(void)
{
    printf("[test_remove_all]\n");
    reset_table();

    int n = 32;
    for (int i = 0; i < n; i++) {
        nodes[i].key = (u32)(500 + i);
        mynode_t *r  = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != NULL)
            FAIL("insert[%d] failed", i);
    }

    for (int i = 0; i < n; i++) {
        mynode_t *r = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != &nodes[i])
            FAIL("remove[%d] returned %p, expected %p",
                 i, (void *)r, (void *)&nodes[i]);
    }

    if (head.rhh_nb != 0)
        FAIL("rhh_nb should be 0 after removing all, got %u", head.rhh_nb);

    /* Table should be empty: all finds return NULL */
    for (int i = 0; i < n; i++) {
        mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, nodes[i].key);
        if (f != NULL)
            FAIL("find after remove_all key=%u returned non-NULL",
                 nodes[i].key);
    }

    PASS("remove_all: %d nodes inserted and removed, table empty", n);
}

/*---------------------------------------------------------------------------
 * Test: high_fill - fill to 90%+, verify all entries findable after kickout
 *---------------------------------------------------------------------------*/
static void
test_high_fill(void)
{
    printf("[test_high_fill]\n");

    /* 64 buckets x 16 slots = 1024 slots. Insert 960 (~94% fill). */
    const unsigned HF_NB_BK = 64u;
    const unsigned HF_SLOTS = HF_NB_BK * RIX_HASH_BUCKET_ENTRY_SZ;
    const unsigned HF_N     = 960u;

    mynode_t *hf_nodes = (mynode_t *)calloc(HF_N, sizeof(mynode_t));
    struct rix_hash32_bucket_s *hf_bk =
        (struct rix_hash32_bucket_s *)aligned_alloc(64, HF_NB_BK * sizeof(*hf_bk));
    if (!hf_nodes || !hf_bk) { perror("alloc"); abort(); }

    for (unsigned i = 0; i < HF_N; i++) {
        hf_nodes[i].key = i + 1;
        hf_nodes[i].val = i * 3;
    }

    struct myht32 hf_head;
    RIX_HASH32_INIT(myht32, &hf_head, hf_bk, HF_NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < HF_N; i++) {
        mynode_t *r = RIX_HASH32_INSERT(myht32, &hf_head, hf_bk, hf_nodes, &hf_nodes[i]);
        if (r == NULL)
            inserted++;
        else if (r == &hf_nodes[i])
            break;
        else
            FAIL("high_fill insert: unexpected duplicate");
    }
    printf("  inserted %u / %u (%.1f%%)\n", inserted, HF_SLOTS,
           100.0 * inserted / HF_SLOTS);

    if (hf_head.rhh_nb != inserted)
        FAIL("high_fill rhh_nb mismatch");

    /* Every inserted entry must be findable */
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH32_FIND(myht32, &hf_head, hf_bk, hf_nodes, hf_nodes[i].key);
        if (f != &hf_nodes[i])
            FAIL("high_fill find failed for key=%u", hf_nodes[i].key);
        if (f->val != hf_nodes[i].val)
            FAIL("high_fill val mismatch for key=%u", hf_nodes[i].key);
    }

    /* Remove every other, verify remaining */
    unsigned removed = 0;
    for (unsigned i = 0; i < inserted; i += 2) {
        mynode_t *r = RIX_HASH32_REMOVE(myht32, &hf_head, hf_bk, hf_nodes, &hf_nodes[i]);
        if (r != &hf_nodes[i])
            FAIL("high_fill remove[%u] failed", i);
        removed++;
    }
    if (hf_head.rhh_nb != inserted - removed)
        FAIL("high_fill rhh_nb mismatch after remove");
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH32_FIND(myht32, &hf_head, hf_bk, hf_nodes, hf_nodes[i].key);
        if (i % 2 == 0) {
            if (f != NULL) FAIL("high_fill removed[%u] still found", i);
        } else {
            if (f != &hf_nodes[i]) FAIL("high_fill remaining[%u] not found", i);
        }
    }

    PASS("high_fill: inserted=%u (%.1f%%), remove/verify OK",
         inserted, 100.0 * inserted / HF_SLOTS);
    free(hf_nodes); free(hf_bk);
}

/*---------------------------------------------------------------------------
 * Test: max_fill - 128 buckets, insert until first failure
 *---------------------------------------------------------------------------*/
static void
test_max_fill(void)
{
    printf("[test_max_fill]\n");

    const unsigned MF_NB_BK = 128u;
    const unsigned MF_SLOTS = MF_NB_BK * RIX_HASH_BUCKET_ENTRY_SZ;
    const unsigned MF_N     = MF_SLOTS + 64u;  /* try beyond capacity */

    mynode_t *mf_nodes = (mynode_t *)calloc(MF_N, sizeof(mynode_t));
    struct rix_hash32_bucket_s *mf_bk =
        (struct rix_hash32_bucket_s *)aligned_alloc(64, MF_NB_BK * sizeof(*mf_bk));
    if (!mf_nodes || !mf_bk) { perror("alloc"); abort(); }

    for (unsigned i = 0; i < MF_N; i++) {
        mf_nodes[i].key = i + 1;
        mf_nodes[i].val = i;
    }

    struct myht32 mf_head;
    RIX_HASH32_INIT(myht32, &mf_head, mf_bk, MF_NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < MF_N; i++) {
        mynode_t *r = RIX_HASH32_INSERT(myht32, &mf_head, mf_bk, mf_nodes, &mf_nodes[i]);
        if (r == NULL)
            inserted++;
        else if (r == &mf_nodes[i])
            break;
    }

    if (mf_head.rhh_nb != inserted)
        FAIL("max_fill rhh_nb mismatch: rhh_nb=%u inserted=%u",
             mf_head.rhh_nb, inserted);

    /* Every inserted entry must be findable */
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH32_FIND(myht32, &mf_head, mf_bk, mf_nodes, mf_nodes[i].key);
        if (f != &mf_nodes[i])
            FAIL("max_fill find failed for key=%u", mf_nodes[i].key);
    }

    printf("  inserted %u / %u (%.1f%%)\n", inserted, MF_SLOTS,
           100.0 * inserted / MF_SLOTS);
    PASS("max_fill: inserted=%u/%u (%.1f%%)",
         inserted, MF_SLOTS, 100.0 * inserted / MF_SLOTS);

    free(mf_nodes); free(mf_bk);
}

/*---------------------------------------------------------------------------
 * Test: kickout_safety - verify no victim loss on failed insert
 *---------------------------------------------------------------------------*/
static void
test_kickout_safety(void)
{
    printf("[test_kickout_safety]\n");

    const unsigned KC_NB_BK = 32u;
    const unsigned KC_CAP   = KC_NB_BK * RIX_HASH_BUCKET_ENTRY_SZ; /* 512 */
    const unsigned KC_N     = KC_CAP + 64u;

    mynode_t *kc_nodes = (mynode_t *)calloc(KC_N, sizeof(mynode_t));
    struct rix_hash32_bucket_s *kc_bk =
        (struct rix_hash32_bucket_s *)aligned_alloc(64, KC_NB_BK * sizeof(*kc_bk));
    if (!kc_nodes || !kc_bk) { perror("alloc"); abort(); }

    for (unsigned i = 0; i < KC_N; i++) {
        kc_nodes[i].key = i + 1;
        kc_nodes[i].val = i;
    }

    struct myht32 kc_head;
    RIX_HASH32_INIT(myht32, &kc_head, kc_bk, KC_NB_BK);

    /* Phase 1: fill until first failure */
    unsigned inserted = 0;
    for (unsigned i = 0; i < KC_N; i++) {
        mynode_t *r = RIX_HASH32_INSERT(myht32, &kc_head, kc_bk, kc_nodes, &kc_nodes[i]);
        if (r == NULL)
            inserted++;
        else if (r == &kc_nodes[i])
            break;
    }

    /* Phase 2: force more failed inserts, check no victim loss */
    unsigned pre_nb = kc_head.rhh_nb;
    unsigned lost = 0;
    for (unsigned i = inserted; i < KC_N; i++) {
        mynode_t *r = RIX_HASH32_INSERT(myht32, &kc_head, kc_bk, kc_nodes, &kc_nodes[i]);
        if (r == NULL) {
            inserted++;
        } else if (r == &kc_nodes[i]) {
            /* Failed insert - verify all previously inserted entries */
            for (unsigned j = 0; j < inserted; j++) {
                mynode_t *f = RIX_HASH32_FIND(myht32, &kc_head, kc_bk, kc_nodes, kc_nodes[j].key);
                if (f != &kc_nodes[j]) {
                    lost++;
                    break;
                }
            }
            if (lost > 0) break;
        }
    }

    printf("  pre_nb=%u post_nb=%u lost_victims=%u\n",
           pre_nb, kc_head.rhh_nb, lost);

    if (lost == 0)
        PASS("kickout_safety: no victim loss");
    else
        FAIL("kickout_safety: victim loss detected (%u)", lost);

    free(kc_nodes); free(kc_bk);
}

/*---------------------------------------------------------------------------
 * Test: fuzz - random insert/find/remove with model checking
 *---------------------------------------------------------------------------*/
static u32 xr_fuzz;

static u32
xorshift32(void)
{
    u32 x = xr_fuzz;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (xr_fuzz = x);
}

static void
test_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[test_fuzz] seed=%u N=%u nb_bk=%u ops=%u\n", seed, N, nb_bk, ops);
    xr_fuzz = seed ? seed : 0xC0FFEE11u;

    mynode_t *fz_nodes = (mynode_t *)calloc(N, sizeof(mynode_t));
    struct rix_hash32_bucket_s *fz_bk =
        (struct rix_hash32_bucket_s *)aligned_alloc(64, nb_bk * sizeof(*fz_bk));
    int *in_tbl = (int *)calloc(N, sizeof(int));
    if (!fz_nodes || !fz_bk || !in_tbl) { perror("alloc"); abort(); }

    for (unsigned i = 0; i < N; i++) {
        fz_nodes[i].key = i + 1;
        fz_nodes[i].val = i;
    }

    struct myht32 fz_head;
    RIX_HASH32_INIT(myht32, &fz_head, fz_bk, nb_bk);

    unsigned in_table = 0;

    for (unsigned step = 0; step < ops; step++) {
        unsigned op = xorshift32() % 100;
        unsigned idx = xorshift32() % N;

        if (op < 60 && !in_tbl[idx]) {
            /* Insert */
            mynode_t *r = RIX_HASH32_INSERT(myht32, &fz_head, fz_bk, fz_nodes, &fz_nodes[idx]);
            if (r == NULL) {
                in_tbl[idx] = 1;
                in_table++;
            } else if (r != &fz_nodes[idx]) {
                FAIL("fuzz insert[%u]: unexpected ret %p", idx, (void *)r);
            }
        } else if (op >= 60 && op < 80 && in_tbl[idx]) {
            /* Remove */
            mynode_t *r = RIX_HASH32_REMOVE(myht32, &fz_head, fz_bk, fz_nodes, &fz_nodes[idx]);
            if (r != &fz_nodes[idx])
                FAIL("fuzz remove[%u]: expected %p got %p",
                     idx, (void *)&fz_nodes[idx], (void *)r);
            in_tbl[idx] = 0;
            in_table--;
        } else {
            /* Find */
            mynode_t *r = RIX_HASH32_FIND(myht32, &fz_head, fz_bk, fz_nodes, fz_nodes[idx].key);
            if (in_tbl[idx] && r != &fz_nodes[idx])
                FAIL("fuzz find[%u] missing", idx);
            if (!in_tbl[idx] && r != NULL)
                FAIL("fuzz find[%u] ghost", idx);
        }

        /* Periodic rhh_nb check */
        if ((step & 0x3FFu) == 0u && fz_head.rhh_nb != in_table)
            FAIL("fuzz step %u: rhh_nb=%u model=%u", step, fz_head.rhh_nb, in_table);
    }

    if (fz_head.rhh_nb != in_table)
        FAIL("fuzz final: rhh_nb=%u model=%u", fz_head.rhh_nb, in_table);

    /* Walk count must match */
    unsigned walk_cnt = 0;
    int wret = RIX_HASH32_WALK(myht32, &fz_head, fz_bk, fz_nodes, walk_cb, &walk_cnt);
    (void)wret;
    if (walk_cnt != in_table)
        FAIL("fuzz walk: expected %u got %u", in_table, walk_cnt);

    PASS("fuzz: %u ops, final in_table=%u", ops, in_table);
    free(fz_nodes); free(fz_bk); free(in_tbl);
}

/*---------------------------------------------------------------------------
 * main
 *---------------------------------------------------------------------------*/
int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    printf("=== rix_hash32 tests ===\n");

    test_insert_find();
    test_find_miss();
    test_duplicate_insert();
    test_remove();
    test_remove_miss();
    test_init_invalid_key();
    test_remove_restores_invalid_key();
    test_key_zero();
    test_walk();
    test_bulk();
    test_staged_x4();
    test_remove_all();
    test_high_fill();
    test_max_fill();
    test_kickout_safety();
    test_fuzz(3237998097u, 512, 64, 200000);
    test_fuzz(3237998097u, 1000, 64, 500000);

    printf("ALL RIX_HASH32 TESTS PASSED\n");
    return 0;
}
