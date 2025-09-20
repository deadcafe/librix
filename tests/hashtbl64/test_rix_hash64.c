/* test_rix_hash64.c
 *  RIX_HASH64 (cuckoo hash table, 64-bit key) - unit & fuzz tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rix_hash64.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

/* ================================================================== */
/* Node definition                                                     */
/* ================================================================== */
typedef struct mynode_s {
    uint64_t key;
    uint64_t val;
} mynode_t;

#define INVALID_KEY  0xFFFFFFFFFFFFFFFFULL

RIX_HASH64_HEAD(myht64);
RIX_HASH64_GENERATE(myht64, mynode_t, key, INVALID_KEY)

/* ================================================================== */
/* Globals for basic tests                                             */
/* ================================================================== */
#define NB_BASIC     20u
#define NB_BK_BASIC   4u  /* 4 x 8 = 32 slots for 20 nodes */

static mynode_t                    g_basic[NB_BASIC];
static struct rix_hash64_bucket_s  g_bk[NB_BK_BASIC] __attribute__((aligned(64)));
static struct myht64               g_head;

static void
basic_init(void)
{
    memset(g_basic, 0, sizeof(g_basic));
    memset(g_bk,    0, sizeof(g_bk));
    RIX_HASH64_INIT(myht64, &g_head, g_bk, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        g_basic[i].key = (uint64_t)(i + 1);
        g_basic[i].val = i;
    }
}

/* ================================================================== */
/* Tests                                                               */
/* ================================================================== */
static void
test_init_empty(void)
{
    printf("[T] init/empty\n");
    basic_init();
    if (g_head.rhh_nb != 0)
        FAIL("rhh_nb != 0 after init");
    /* All slots must have invalid_key sentinel */
    for (unsigned b = 0; b < NB_BK_BASIC; b++)
        for (unsigned s = 0; s < RIX_HASH64_BUCKET_ENTRY_SZ; s++)
            if (g_bk[b].key[s] != INVALID_KEY)
                FAILF("key[%u][%u] not INVALID_KEY after init", b, s);
}

static void
test_insert_find_remove(void)
{
    printf("[T] insert/find/remove\n");
    basic_init();

    /* Insert all */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[i]);
        if (r != NULL)
            FAILF("insert[%u] failed (ret=%p)", i, (void *)r);
    }
    if (g_head.rhh_nb != NB_BASIC)
        FAILF("rhh_nb=%u expected %u", g_head.rhh_nb, NB_BASIC);

    /* Find all */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        mynode_t *r = RIX_HASH64_FIND(myht64, &g_head, g_bk, g_basic, g_basic[i].key);
        if (r != &g_basic[i])
            FAILF("find[%u] returned %p expected %p", i, (void *)r, (void *)&g_basic[i]);
    }

    /* Miss */
    mynode_t *miss = RIX_HASH64_FIND(myht64, &g_head, g_bk, g_basic, 0xDEAD000000000000ULL);
    if (miss != NULL)
        FAIL("miss find returned non-NULL");

    /* Remove half */
    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        mynode_t *r = RIX_HASH64_REMOVE(myht64, &g_head, g_bk, g_basic, &g_basic[i]);
        if (r != &g_basic[i])
            FAILF("remove[%u] returned %p", i, (void *)r);
    }
    if (g_head.rhh_nb != NB_BASIC / 2)
        FAILF("rhh_nb=%u after half remove", g_head.rhh_nb);

    /* Verify odd still found, even gone */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        mynode_t *r = RIX_HASH64_FIND(myht64, &g_head, g_bk, g_basic, g_basic[i].key);
        if (i % 2 == 1 && r != &g_basic[i])
            FAILF("odd[%u] not found after remove", i);
        if (i % 2 == 0 && r != NULL)
            FAILF("even[%u] still found after remove", i);
    }
}

static void
test_duplicate_insert(void)
{
    printf("[T] duplicate insert\n");
    basic_init();

    mynode_t *r = RIX_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[0]);
    if (r != NULL) FAIL("first insert failed");

    /* Insert duplicate */
    mynode_t dup = g_basic[0];
    r = RIX_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &dup);
    if (r != &g_basic[0])
        FAILF("dup insert: expected %p got %p", (void *)&g_basic[0], (void *)r);
}

static void
test_staged_find(void)
{
    printf("[T] staged find x1/x2/x4\n");
    basic_init();
    for (unsigned i = 0; i < NB_BASIC; i++)
        RIX_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[i]);

    /* x1 staged */
    {
        struct rix_hash64_find_ctx_s ctx;
        RIX_HASH64_HASH_KEY(myht64, &ctx, &g_head, g_bk, g_basic[3].key);
        RIX_HASH64_SCAN_BK (myht64, &ctx, &g_head, g_bk);
        mynode_t *r = RIX_HASH64_CMP_KEY(myht64, &ctx, g_basic);
        if (r != &g_basic[3]) FAIL("x1 staged find failed");
    }

    /* x2 staged */
    {
        struct rix_hash64_find_ctx_s ctx2[2];
        uint64_t keys2[2] = { g_basic[0].key, g_basic[1].key };
        mynode_t *res2[2];
        RIX_HASH64_HASH_KEY_N(myht64, ctx2, 2, &g_head, g_bk, keys2);
        RIX_HASH64_SCAN_BK_N (myht64, ctx2, 2, &g_head, g_bk);
        RIX_HASH64_PREFETCH_NODE_N(myht64, ctx2, 2, g_basic);
        RIX_HASH64_CMP_KEY_N (myht64, ctx2, 2, g_basic, res2);
        if (res2[0] != &g_basic[0] || res2[1] != &g_basic[1])
            FAIL("x2 staged find failed");
    }

    /* x4 staged */
    {
        struct rix_hash64_find_ctx_s ctx4[4];
        uint64_t keys4[4] = {
            g_basic[4].key, g_basic[5].key, g_basic[6].key, g_basic[7].key
        };
        mynode_t *res4[4];
        RIX_HASH64_HASH_KEY4(myht64, ctx4, &g_head, g_bk, keys4);
        RIX_HASH64_SCAN_BK4 (myht64, ctx4, &g_head, g_bk);
        RIX_HASH64_PREFETCH_NODE4(myht64, ctx4, g_basic);
        RIX_HASH64_CMP_KEY4 (myht64, ctx4, g_basic, res4);
        for (int i = 0; i < 4; i++)
            if (res4[i] != &g_basic[4 + i])
                FAILF("x4 staged find [%d] failed", i);
    }
}

static unsigned g_walk_visited;

static int
walk_cb(mynode_t *n, void *arg)
{
    (void)arg;
    g_walk_visited++;
    if (n < g_basic || n >= g_basic + NB_BASIC)
        FAIL("walk: node out of range");
    return 0;
}

static void
test_walk(void)
{
    printf("[T] walk\n");
    basic_init();
    for (unsigned i = 0; i < NB_BASIC; i++)
        RIX_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[i]);

    g_walk_visited = 0;
    RIX_HASH64_WALK(myht64, &g_head, g_bk, g_basic, walk_cb, NULL);
    if (g_walk_visited != NB_BASIC)
        FAILF("walk visited %u expected %u", g_walk_visited, NB_BASIC);
}

/* ================================================================== */
/* test_remove_miss - remove not-in-table and double remove            */
/* ================================================================== */
static void
test_remove_miss(void)
{
    printf("[T] remove_miss\n");
    basic_init();

    /* Remove a node that was never inserted -> should return NULL */
    mynode_t *r = RIX_HASH64_REMOVE(myht64, &g_head, g_bk, g_basic, &g_basic[0]);
    if (r != NULL)
        FAILF("remove_miss: not-in-table returned %p, expected NULL", (void *)r);

    /* Insert, remove, then remove again -> second remove returns NULL */
    RIX_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[1]);
    r = RIX_HASH64_REMOVE(myht64, &g_head, g_bk, g_basic, &g_basic[1]);
    if (r != &g_basic[1])
        FAILF("remove_miss: first remove returned %p", (void *)r);
    r = RIX_HASH64_REMOVE(myht64, &g_head, g_bk, g_basic, &g_basic[1]);
    if (r != NULL)
        FAILF("remove_miss: double remove returned %p, expected NULL", (void *)r);
}

/* ================================================================== */
/* test_max_fill - 128 buckets, insert until first failure             */
/* ================================================================== */
static void
test_max_fill(void)
{
    printf("[T] max_fill (128 buckets)\n");

    const unsigned MF_NB_BK = 128u;
    const unsigned MF_SLOTS = MF_NB_BK * RIX_HASH64_BUCKET_ENTRY_SZ;
    const unsigned MF_N     = MF_SLOTS + 64u;  /* try beyond capacity */

    mynode_t *mf_nodes = (mynode_t *)calloc(MF_N, sizeof(mynode_t));
    struct rix_hash64_bucket_s *mf_bk =
        (struct rix_hash64_bucket_s *)aligned_alloc(64, MF_NB_BK * sizeof(*mf_bk));
    if (!mf_nodes || !mf_bk) { perror("alloc"); abort(); }

    for (unsigned i = 0; i < MF_N; i++) {
        mf_nodes[i].key = (uint64_t)(i + 1);
        mf_nodes[i].val = (uint64_t)i;
    }

    struct myht64 mf_head;
    RIX_HASH64_INIT(myht64, &mf_head, mf_bk, MF_NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < MF_N; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &mf_head, mf_bk, mf_nodes, &mf_nodes[i]);
        if (r == NULL)
            inserted++;
        else if (r == &mf_nodes[i])
            break;
    }

    if (mf_head.rhh_nb != inserted)
        FAILF("max_fill rhh_nb=%u expected %u", mf_head.rhh_nb, inserted);

    /* Every inserted entry must be findable */
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH64_FIND(myht64, &mf_head, mf_bk, mf_nodes, mf_nodes[i].key);
        if (f != &mf_nodes[i])
            FAILF("max_fill find[%u] failed", i);
    }

    printf("  inserted %u / %u (%.1f%%)\n", inserted, MF_SLOTS,
           100.0 * inserted / MF_SLOTS);

    free(mf_nodes); free(mf_bk);
}

/* ================================================================== */
/* Fuzz test                                                           */
/* ================================================================== */
static void
test_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[T] fuzz seed=%u N=%u nb_bk=%u ops=%u\n", seed, N, nb_bk, ops);

    mynode_t                   *nodes   = calloc(N, sizeof(*nodes));
    struct rix_hash64_bucket_s *buckets = calloc(nb_bk, sizeof(*buckets));
    int                        *in_tbl  = calloc(N, sizeof(int));
    if (!nodes || !buckets || !in_tbl) { perror("calloc"); abort(); }

    struct myht64 head;
    RIX_HASH64_INIT(myht64, &head, buckets, nb_bk);
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key = (uint64_t)(i + 1);
        nodes[i].val = i;
    }

    /* Simple LCG */
    uint64_t rng = seed;
    for (unsigned op = 0; op < ops; op++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        unsigned idx = (unsigned)((rng >> 33) % N);
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        int do_insert = (int)((rng >> 63) & 1);

        if (do_insert && !in_tbl[idx]) {
            mynode_t *r = RIX_HASH64_INSERT(myht64, &head, buckets, nodes, &nodes[idx]);
            if (r == NULL) {
                in_tbl[idx] = 1;
            } else if (r != &nodes[idx]) {
                FAILF("fuzz insert[%u] returned unexpected %p", idx, (void *)r);
            }
        } else if (!do_insert && in_tbl[idx]) {
            mynode_t *r = RIX_HASH64_REMOVE(myht64, &head, buckets, nodes, &nodes[idx]);
            if (r != &nodes[idx])
                FAILF("fuzz remove[%u] failed", idx);
            in_tbl[idx] = 0;
        }

        /* Verify find */
        mynode_t *r = RIX_HASH64_FIND(myht64, &head, buckets, nodes, nodes[idx].key);
        if (in_tbl[idx] && r != &nodes[idx])
            FAILF("fuzz find[%u] missing", idx);
        if (!in_tbl[idx] && r != NULL)
            FAILF("fuzz find[%u] ghost", idx);
    }

    free(nodes); free(buckets); free(in_tbl);
}

/* ================================================================== */
/* test_high_fill - fill to 90%+, verify all entries findable          */
/* ================================================================== */
static void
test_high_fill(void)
{
    printf("[T] high_fill (90%%+)\n");

    /* 64 buckets x 16 slots = 1024 slots. Insert 960 (~94% fill). */
    const unsigned HF_NB_BK = 64u;
    const unsigned HF_SLOTS = HF_NB_BK * RIX_HASH64_BUCKET_ENTRY_SZ;
    const unsigned HF_N     = 960u;

    mynode_t *hf_nodes = (mynode_t *)calloc(HF_N, sizeof(mynode_t));
    struct rix_hash64_bucket_s *hf_bk =
        (struct rix_hash64_bucket_s *)aligned_alloc(64, HF_NB_BK * sizeof(*hf_bk));
    if (!hf_nodes || !hf_bk) { perror("alloc"); abort(); }

    for (unsigned i = 0; i < HF_N; i++) {
        hf_nodes[i].key = (uint64_t)(i + 1);
        hf_nodes[i].val = (uint64_t)(i * 3);
    }

    struct myht64 hf_head;
    RIX_HASH64_INIT(myht64, &hf_head, hf_bk, HF_NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < HF_N; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &hf_head, hf_bk, hf_nodes, &hf_nodes[i]);
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
        FAILF("high_fill rhh_nb=%u expected %u", hf_head.rhh_nb, inserted);

    /* Every inserted entry must be findable */
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH64_FIND(myht64, &hf_head, hf_bk, hf_nodes, hf_nodes[i].key);
        if (f != &hf_nodes[i])
            FAILF("high_fill find[%u] failed", i);
        if (f->val != hf_nodes[i].val)
            FAILF("high_fill val mismatch for i=%u", i);
    }

    /* Remove every other, verify remaining */
    for (unsigned i = 0; i < inserted; i += 2) {
        mynode_t *r = RIX_HASH64_REMOVE(myht64, &hf_head, hf_bk, hf_nodes, &hf_nodes[i]);
        if (r != &hf_nodes[i])
            FAILF("high_fill remove[%u] failed", i);
    }
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH64_FIND(myht64, &hf_head, hf_bk, hf_nodes, hf_nodes[i].key);
        if (i % 2 == 0) {
            if (f != NULL) FAILF("high_fill removed[%u] still found", i);
        } else {
            if (f != &hf_nodes[i]) FAILF("high_fill remaining[%u] not found", i);
        }
    }

    free(hf_nodes); free(hf_bk);
}

/* ================================================================== */
/* test_kickout_safety - verify no victim loss on failed insert        */
/* ================================================================== */
static void
test_kickout_safety(void)
{
    printf("[T] kickout_safety\n");

    const unsigned KC_NB_BK = 32u;
    const unsigned KC_CAP   = KC_NB_BK * RIX_HASH64_BUCKET_ENTRY_SZ; /* 512 */
    const unsigned KC_N     = KC_CAP + 64u;

    mynode_t *kc_nodes = (mynode_t *)calloc(KC_N, sizeof(mynode_t));
    struct rix_hash64_bucket_s *kc_bk =
        (struct rix_hash64_bucket_s *)aligned_alloc(64, KC_NB_BK * sizeof(*kc_bk));
    if (!kc_nodes || !kc_bk) { perror("alloc"); abort(); }

    for (unsigned i = 0; i < KC_N; i++) {
        kc_nodes[i].key = (uint64_t)(i + 1);
        kc_nodes[i].val = (uint64_t)i;
    }

    struct myht64 kc_head;
    RIX_HASH64_INIT(myht64, &kc_head, kc_bk, KC_NB_BK);

    /* Phase 1: fill until first failure */
    unsigned inserted = 0;
    for (unsigned i = 0; i < KC_N; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &kc_head, kc_bk, kc_nodes, &kc_nodes[i]);
        if (r == NULL)
            inserted++;
        else if (r == &kc_nodes[i])
            break;
    }

    /* Phase 2: force more failed inserts, check no victim loss */
    unsigned pre_nb = kc_head.rhh_nb;
    unsigned lost = 0;
    for (unsigned i = inserted; i < KC_N; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &kc_head, kc_bk, kc_nodes, &kc_nodes[i]);
        if (r == NULL) {
            inserted++;
        } else if (r == &kc_nodes[i]) {
            for (unsigned j = 0; j < inserted; j++) {
                mynode_t *f = RIX_HASH64_FIND(myht64, &kc_head, kc_bk, kc_nodes, kc_nodes[j].key);
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

    if (lost > 0)
        FAILF("kickout_safety: victim loss detected (%u)", lost);

    free(kc_nodes); free(kc_bk);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    test_init_empty();
    test_insert_find_remove();
    test_duplicate_insert();
    test_staged_find();
    test_walk();
    test_remove_miss();
    test_max_fill();
    test_high_fill();
    test_kickout_safety();
    test_fuzz(3237998097u, 512, 64, 200000);
    test_fuzz(3237998097u, 1000, 64, 500000);

    printf("ALL RIX_HASH64 TESTS PASSED\n");
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
