/* test_rix_hash.c
 *  RIX_HASH (cuckoo hash table) - unit & fuzz tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rix_hash.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

/* ================================================================== */
/* Node definition (128-bit key as struct)                             */
/* ================================================================== */
struct mykey {
    u64 hi;
    u64 lo;
};

struct mynode {
    u32  cur_hash; /* hash_field: hash of the current bucket (updated on kickout) */
    u32  _pad;     /* alignment padding */
    struct mykey key;   /* key_field: must be a struct, sizeof = 16 */
};

/* 16-byte key comparison using SSE2 (struct mykey = two u64) */
static RIX_FORCE_INLINE int
mykey_cmp(const struct mykey *a, const struct mykey *b)
{
    __m128i va = _mm_loadu_si128((const __m128i *)a);
    __m128i vb = _mm_loadu_si128((const __m128i *)b);
    return (_mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)) == 0xffff) ? 0 : 1;
}

RIX_HASH_HEAD(myht);
RIX_HASH_GENERATE(myht, mynode, key, cur_hash, mykey_cmp)

struct mynode_slot {
    u32  cur_hash; /* hash_field: hash of the current bucket */
    u16  slot;     /* slot_field: slot in current bucket */
    u16  _pad;
    struct mykey key;
};

RIX_HASH_HEAD(myht_slot);
RIX_HASH_GENERATE_SLOT(myht_slot, mynode_slot, key, cur_hash, slot, mykey_cmp)

/* ================================================================== */
/* keyonly variant: no hash_field (8-byte node)                           */
/* ================================================================== */
struct mynode_keyonly {
    struct mykey key;
};

RIX_HASH_HEAD(myht_keyonly);
RIX_HASH_GENERATE_KEYONLY(myht_keyonly, mynode_keyonly, key, mykey_cmp)

/* ================================================================== */
/* Globals for basic tests                                             */
/* ================================================================== */
#define NB_BASIC     20u
#define NB_BK_BASIC   4u  /* 4 x 16 = 64 slots for 20 nodes  */

static struct mynode          g_basic[NB_BASIC];
static struct rix_hash_bucket_s g_bk[NB_BK_BASIC] __attribute__((aligned(64)));
static struct myht            g_head;

static struct mynode_keyonly       g_keyonly[NB_BASIC];
static struct rix_hash_bucket_s g_bk_keyonly[NB_BK_BASIC] __attribute__((aligned(64)));
static struct myht_keyonly         g_head_keyonly;

static struct mynode_slot       g_slot[NB_BASIC];
static struct rix_hash_bucket_s g_bk_slot[NB_BK_BASIC] __attribute__((aligned(64)));
static struct myht_slot         g_head_slot;

static void
basic_init(void)
{
    memset(g_basic, 0, sizeof(g_basic));
    memset(g_bk,    0, sizeof(g_bk));
    RIX_HASH_INIT(myht, &g_head, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        g_basic[i].key.hi = (u64)(i + 1);
        g_basic[i].key.lo = 0xDEADC0DE00000000ULL;
    }
}

/* Insert all NB_BASIC nodes; abort on failure. */
static void
basic_insert_all(void)
{
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode *ret = myht_insert(&g_head, g_bk, g_basic, &g_basic[i]);
        if (ret != NULL)
            FAILF("insert[%u] failed (ret=%p)", i, (void *)ret);
    }
    if (g_head.rhh_nb != NB_BASIC)
        FAILF("nb after all inserts: expected %u got %u",
              NB_BASIC, g_head.rhh_nb);
}

static void
slot_basic_init(void)
{
    memset(g_slot,    0, sizeof(g_slot));
    memset(g_bk_slot, 0, sizeof(g_bk_slot));
    RIX_HASH_INIT(myht_slot, &g_head_slot, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        g_slot[i].key.hi = (u64)(i + 1);
        g_slot[i].key.lo = 0xDEADC0DE00000000ULL;
    }
}

static void
slot_basic_insert_all(void)
{
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_slot *ret =
            myht_slot_insert(&g_head_slot, g_bk_slot, g_slot, &g_slot[i]);
        if (ret != NULL)
            FAILF("slot insert[%u] failed (ret=%p)", i, (void *)ret);
    }
    if (g_head_slot.rhh_nb != NB_BASIC)
        FAILF("slot nb after all inserts: expected %u got %u",
              NB_BASIC, g_head_slot.rhh_nb);
}

static void
slot_verify_node(struct myht_slot *head,
                 struct rix_hash_bucket_s *bk,
                 struct mynode_slot *base,
                 struct mynode_slot *node)
{
    unsigned node_idx = RIX_IDX_FROM_PTR(base, node);
    unsigned bucket = node->cur_hash & head->rhh_mask;
    unsigned slot = (unsigned)node->slot;
    struct rix_hash_bucket_s *b = bk + bucket;

    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)
        FAILF("slot invariant: node_idx=%u slot=%u out of range", node_idx, slot);
    if (b->idx[slot] != node_idx)
        FAILF("slot invariant: node_idx=%u bucket=%u slot=%u idx=%u",
              node_idx, bucket, slot, b->idx[slot]);
    if (b->hash[slot] == 0u)
        FAILF("slot invariant: node_idx=%u bucket=%u slot=%u fp=0",
              node_idx, bucket, slot);
}

static void
slot_verify_present(struct myht_slot *head,
                    struct rix_hash_bucket_s *bk,
                    struct mynode_slot *base,
                    const unsigned char *present,
                    unsigned N)
{
    for (unsigned i = 0; i < N; i++) {
        struct mynode_slot *node = &base[i];
        struct mynode_slot *found =
            myht_slot_find(head, bk, base, &node->key);
        if (present[i + 1]) {
            if (found != node)
                FAILF("slot verify find[%u]: expected %p got %p",
                      i + 1, (void *)node, (void *)found);
            slot_verify_node(head, bk, base, node);
        } else if (found != NULL) {
            FAILF("slot verify find absent[%u]: got %p", i + 1, (void *)found);
        }
    }
}

/* ================================================================== */
/* test_init_empty                                                     */
/* ================================================================== */
static void
test_init_empty(void)
{
    printf("[T] init/empty\n");
    basic_init();

    if (g_head.rhh_nb != 0u)
        FAIL("init: rhh_nb not zero");
    if (g_head.rhh_mask != NB_BK_BASIC - 1u)
        FAILF("init: mask expected %u got %u", NB_BK_BASIC - 1u, g_head.rhh_mask);

    /* find on empty table must return NULL */
    struct mykey k = { 1ULL, 0xDEADC0DE00000000ULL };
    struct mynode *r = myht_find(&g_head, g_bk, g_basic, &k);
    if (r != NULL)
        FAIL("find on empty table should return NULL");

    /* remove on empty table: g_basic[0] has keys set but was never inserted */
    if (myht_remove(&g_head, g_bk, g_basic, &g_basic[0]) != NULL)
        FAIL("remove on empty table should return NULL");
}

/* ================================================================== */
/* test_insert_find_remove                                             */
/* ================================================================== */
static void
test_insert_find_remove(void)
{
    printf("[T] insert/find/remove\n");
    basic_init();
    basic_insert_all();

    /* Find each inserted node */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode *f = myht_find(&g_head, g_bk, g_basic, &g_basic[i].key);
        if (f == NULL)
            FAILF("find[%u] returned NULL", i);
        if (f != &g_basic[i])
            FAILF("find[%u] returned wrong node %p expected %p",
                  i, (void *)f, (void *)&g_basic[i]);
    }

    /* Find a non-existent key */
    struct mykey bad = { 9999ULL, 0xDEADC0DE00000000ULL };
    if (myht_find(&g_head, g_bk, g_basic, &bad) != NULL)
        FAIL("find of non-existent key should return NULL");

    /* Remove even-indexed nodes */
    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        struct mynode *rem = myht_remove(&g_head, g_bk, g_basic, &g_basic[i]);
        if (rem != &g_basic[i])
            FAILF("remove[%u] returned wrong node", i);
    }
    if (g_head.rhh_nb != NB_BASIC / 2)
        FAILF("nb after removals: expected %u got %u", NB_BASIC / 2, g_head.rhh_nb);

    /* Removed must not be found; remaining must be found */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode *f = myht_find(&g_head, g_bk, g_basic, &g_basic[i].key);
        if (i % 2 == 0) {
            if (f != NULL)
                FAILF("removed node[%u] still found", i);
        } else {
            if (f == NULL || f != &g_basic[i])
                FAILF("remaining node[%u] not found or wrong", i);
        }
    }

    /* Remove already-removed node must return NULL */
    if (myht_remove(&g_head, g_bk, g_basic, &g_basic[0]) != NULL)
        FAIL("re-remove of already-removed node should return NULL");
}

/* ================================================================== */
/* test_duplicate                                                      */
/* ================================================================== */
static void
test_duplicate(void)
{
    printf("[T] duplicate insert\n");
    basic_init();

    /* Insert node[0] */
    struct mynode *r0 = myht_insert(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r0 != NULL)
        FAIL("first insert must return NULL");

    /* Re-insert the same node - must return the existing node */
    struct mynode *r1 = myht_insert(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r1 != &g_basic[0])
        FAILF("dup insert must return existing node %p, got %p",
              (void *)&g_basic[0], (void *)r1);
    if (g_head.rhh_nb != 1u)
        FAILF("nb must still be 1 after dup, got %u", g_head.rhh_nb);

    /*
     * Insert a different node object that has the same key value.
     * insert() writes cur_hash to elm before the dup check, but dup is a
     * stack variable so that write is harmless.  The dup check then finds
     * &g_basic[0] and returns early before any bucket slot is modified.
     */
    struct mynode dup;
    dup.key = g_basic[0].key;
    struct mynode *r2 = myht_insert(&g_head, g_bk, g_basic, &dup);
    if (r2 != &g_basic[0])
        FAILF("key-dup insert must return existing node %p, got %p",
              (void *)&g_basic[0], (void *)r2);
    if (g_head.rhh_nb != 1u)
        FAILF("nb must still be 1 after key-dup, got %u", g_head.rhh_nb);
}

static void
test_slot_insert_find_remove(void)
{
    printf("[T] slot insert/find/remove\n");
    slot_basic_init();
    slot_basic_insert_all();

    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_slot *f =
            myht_slot_find(&g_head_slot, g_bk_slot, g_slot, &g_slot[i].key);
        if (f == NULL)
            FAILF("slot find[%u] returned NULL", i);
        if (f != &g_slot[i])
            FAILF("slot find[%u] returned wrong node %p expected %p",
                  i, (void *)f, (void *)&g_slot[i]);
        slot_verify_node(&g_head_slot, g_bk_slot, g_slot, &g_slot[i]);
    }

    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        struct mynode_slot *rem =
            myht_slot_remove(&g_head_slot, g_bk_slot, g_slot, &g_slot[i]);
        if (rem != &g_slot[i])
            FAILF("slot remove[%u] returned wrong node", i);
    }
    if (g_head_slot.rhh_nb != NB_BASIC / 2)
        FAILF("slot nb after removals: expected %u got %u",
              NB_BASIC / 2, g_head_slot.rhh_nb);

    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_slot *f =
            myht_slot_find(&g_head_slot, g_bk_slot, g_slot, &g_slot[i].key);
        if (i % 2 == 0) {
            if (f != NULL)
                FAILF("slot removed node[%u] still found", i);
        } else {
            if (f == NULL || f != &g_slot[i])
                FAILF("slot remaining node[%u] not found or wrong", i);
            slot_verify_node(&g_head_slot, g_bk_slot, g_slot, &g_slot[i]);
        }
    }

    if (myht_slot_remove(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]) != NULL)
        FAIL("slot re-remove of already-removed node should return NULL");
}

static void
test_slot_duplicate(void)
{
    printf("[T] slot duplicate insert\n");
    slot_basic_init();

    struct mynode_slot *r0 =
        myht_slot_insert(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]);
    if (r0 != NULL)
        FAIL("slot first insert must return NULL");

    struct mynode_slot *r1 =
        myht_slot_insert(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]);
    if (r1 != &g_slot[0])
        FAILF("slot dup insert must return existing node %p, got %p",
              (void *)&g_slot[0], (void *)r1);
    slot_verify_node(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]);

    struct mynode_slot dup;
    memset(&dup, 0, sizeof(dup));
    dup.key = g_slot[0].key;
    struct mynode_slot *r2 =
        myht_slot_insert(&g_head_slot, g_bk_slot, g_slot, &dup);
    if (r2 != &g_slot[0])
        FAILF("slot key-dup insert must return existing node %p, got %p",
              (void *)&g_slot[0], (void *)r2);
    if (g_head_slot.rhh_nb != 1u)
        FAILF("slot nb must still be 1 after dup, got %u", g_head_slot.rhh_nb);
}

/* ================================================================== */
/* test_staged_find                                                    */
/* ================================================================== */
static void
test_staged_find(void)
{
    printf("[T] staged find x1/x2/x4\n");
    basic_init();
    basic_insert_all();

    struct mykey bad = { 9999ULL, 0xDEADC0DE00000000ULL };

    /* --- x1 staged --- */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct rix_hash_find_ctx_s ctx;
        RIX_HASH_HASH_KEY(myht, &ctx, &g_head, g_bk, &g_basic[i].key);
        RIX_HASH_SCAN_BK (myht, &ctx, &g_head, g_bk);
        struct mynode *r = RIX_HASH_CMP_KEY(myht, &ctx, g_basic);
        if (r == NULL || r != &g_basic[i])
            FAILF("staged x1 find[%u] mismatch", i);
    }
    {   /* non-existent */
        struct rix_hash_find_ctx_s ctx;
        RIX_HASH_HASH_KEY(myht, &ctx, &g_head, g_bk, &bad);
        RIX_HASH_SCAN_BK (myht, &ctx, &g_head, g_bk);
        if (RIX_HASH_CMP_KEY(myht, &ctx, g_basic) != NULL)
            FAIL("staged x1 find of non-existent should be NULL");
    }

    /* --- x2 staged --- */
    {
        struct rix_hash_find_ctx_s ctx2[2];
        const struct mykey *keys2[2] = { &g_basic[0].key, &bad };
        struct mynode *res2[2];

        RIX_HASH_HASH_KEY2(myht, ctx2, &g_head, g_bk, keys2);
        RIX_HASH_SCAN_BK2 (myht, ctx2, &g_head, g_bk);
        RIX_HASH_CMP_KEY2 (myht, ctx2, g_basic, res2);

        if (res2[0] != &g_basic[0])
            FAILF("staged x2[0] mismatch: got %p", (void *)res2[0]);
        if (res2[1] != NULL)
            FAILF("staged x2[1] non-existent should be NULL, got %p",
                  (void *)res2[1]);
    }

    /* --- x4 staged --- */
    {
        struct rix_hash_find_ctx_s ctx4[4];
        const struct mykey *keys4[4] = {
            &g_basic[3].key, &bad, &g_basic[7].key, &g_basic[11].key
        };
        struct mynode *res4[4];

        RIX_HASH_HASH_KEY4(myht, ctx4, &g_head, g_bk, keys4);
        RIX_HASH_SCAN_BK4 (myht, ctx4, &g_head, g_bk);
        RIX_HASH_CMP_KEY4 (myht, ctx4, g_basic, res4);

        if (res4[0] != &g_basic[3])
            FAILF("staged x4[0] mismatch: got %p", (void *)res4[0]);
        if (res4[1] != NULL)
            FAILF("staged x4[1] non-existent should be NULL, got %p",
                  (void *)res4[1]);
        if (res4[2] != &g_basic[7])
            FAILF("staged x4[2] mismatch: got %p", (void *)res4[2]);
        if (res4[3] != &g_basic[11])
            FAILF("staged x4[3] mismatch: got %p", (void *)res4[3]);
    }
}

/* ================================================================== */
/* test_walk                                                           */
/* ================================================================== */
static int walk_count_cb(struct mynode *node, void *arg)
{
    (void)node;
    unsigned *cnt = (unsigned *)arg;
    (*cnt)++;
    return 0;
}

static int walk_stop_cb(struct mynode *node, void *arg)
{
    (void)node;
    int *cnt = (int *)arg;
    (*cnt)++;
    return (*cnt >= 3) ? 99 : 0; /* stop after 3 */
}

static void
test_walk(void)
{
    printf("[T] walk\n");
    basic_init();
    basic_insert_all();

    unsigned cnt = 0;
    int ret = myht_walk(&g_head, g_bk, g_basic, walk_count_cb, &cnt);
    if (ret != 0)
        FAILF("walk returned non-zero: %d", ret);
    if (cnt != NB_BASIC)
        FAILF("walk count: expected %u got %u", NB_BASIC, cnt);

    /* Remove 5 nodes and walk again */
    for (unsigned i = 0; i < 5; i++)
        myht_remove(&g_head, g_bk, g_basic, &g_basic[i]);
    cnt = 0;
    myht_walk(&g_head, g_bk, g_basic, walk_count_cb, &cnt);
    if (cnt != NB_BASIC - 5)
        FAILF("walk after remove: expected %u got %u", NB_BASIC - 5, cnt);

    /* Early-stop callback */
    basic_init();
    basic_insert_all();
    int scnt = 0;
    ret = myht_walk(&g_head, g_bk, g_basic, walk_stop_cb, &scnt);
    if (ret != 99)
        FAILF("early-stop walk should return 99, got %d", ret);
    if (scnt != 3)
        FAILF("early-stop walk cb called %d times, expected 3", scnt);
}

/* ================================================================== */
/* Generic bytes-hash pair spread regression                          */
/* ================================================================== */
struct hash24_key {
    u32 v[6];
};

typedef union rix_hash_hash_u
(*hash_u32_fn_t)(u32 key, u32 mask);

typedef union rix_hash_hash_u
(*hash_u64_fn_t)(u64 key, u32 mask);

static struct hash24_key
hash24_key_make(unsigned i)
{
    struct hash24_key key;

    key.v[0] = UINT32_C(0x0a000000) | (i & UINT32_C(0x00ffffff));
    key.v[1] = UINT32_C(0x14000000)
             | ((i * UINT32_C(2654435761)) & UINT32_C(0x00ffffff));
    key.v[2] = UINT32_C(0x00010000) | (i & UINT32_C(0x0000ffff));
    key.v[3] = UINT32_C(0x00020000) | ((i >> 5) & UINT32_C(0x0000ffff));
    key.v[4] = UINT32_C(0x00000006) | ((i & 1u) << 8);
    key.v[5] = UINT32_C(0x01000000) ^ (i * UINT32_C(2246822519));
    return key;
}

static u64
hash_u64_key_make(unsigned i)
{
    return ((u64)i << 32)
         ^ ((u64)i * UINT64_C(0x9e3779b97f4a7c15));
}

static void
test_u32_hash_pair_spread_impl(const char *label,
                               hash_u32_fn_t hash_fn,
                               unsigned min_unique_pairs,
                               unsigned min_outdeg)
{
    enum {
        NB_BK = 32u,
        MASK = NB_BK - 1u,
        SAMPLES = 4096u,
    };
    unsigned char pair_seen[NB_BK][NB_BK];
    unsigned unique_pairs = 0u;
    unsigned min_seen_outdeg = NB_BK;
    unsigned max_seen_outdeg = 0u;

    printf("[T] %s\n", label);
    memset(pair_seen, 0, sizeof(pair_seen));

    for (unsigned i = 0u; i < SAMPLES; i++) {
        union rix_hash_hash_u h = hash_fn((u32)i, MASK);
        unsigned bk0 = h.val32[0] & MASK;
        unsigned bk1 = h.val32[1] & MASK;

        if (bk0 == bk1)
            FAILF("%s chose identical buckets for sample %u", label, i);
        if ((h.val32[0] ^ h.val32[1]) == 0u)
            FAILF("%s produced zero fp for sample %u", label, i);
        if (!pair_seen[bk0][bk1]) {
            pair_seen[bk0][bk1] = 1u;
            unique_pairs++;
        }
    }

    for (unsigned bk = 0u; bk < NB_BK; bk++) {
        unsigned outdeg = 0u;

        for (unsigned other = 0u; other < NB_BK; other++) {
            if (other == bk)
                continue;
            if (pair_seen[bk][other])
                outdeg++;
        }
        if (outdeg < min_seen_outdeg)
            min_seen_outdeg = outdeg;
        if (outdeg > max_seen_outdeg)
            max_seen_outdeg = outdeg;
    }

    printf("  unique_pairs=%u min_outdeg=%u max_outdeg=%u\n",
           unique_pairs, min_seen_outdeg, max_seen_outdeg);
    if (unique_pairs < min_unique_pairs) {
        FAILF("%s pair spread too narrow: unique_pairs=%u (< %u)",
              label, unique_pairs, min_unique_pairs);
    }
    if (min_seen_outdeg < min_outdeg) {
        FAILF("%s out-degree too narrow: min_outdeg=%u (< %u)",
              label, min_seen_outdeg, min_outdeg);
    }
}

static void
test_u64_hash_pair_spread_impl(const char *label,
                               hash_u64_fn_t hash_fn,
                               unsigned min_unique_pairs,
                               unsigned min_outdeg)
{
    enum {
        NB_BK = 32u,
        MASK = NB_BK - 1u,
        SAMPLES = 4096u,
    };
    unsigned char pair_seen[NB_BK][NB_BK];
    unsigned unique_pairs = 0u;
    unsigned min_seen_outdeg = NB_BK;
    unsigned max_seen_outdeg = 0u;

    printf("[T] %s\n", label);
    memset(pair_seen, 0, sizeof(pair_seen));

    for (unsigned i = 0u; i < SAMPLES; i++) {
        union rix_hash_hash_u h = hash_fn(hash_u64_key_make(i), MASK);
        unsigned bk0 = h.val32[0] & MASK;
        unsigned bk1 = h.val32[1] & MASK;

        if (bk0 == bk1)
            FAILF("%s chose identical buckets for sample %u", label, i);
        if ((h.val32[0] ^ h.val32[1]) == 0u)
            FAILF("%s produced zero fp for sample %u", label, i);
        if (!pair_seen[bk0][bk1]) {
            pair_seen[bk0][bk1] = 1u;
            unique_pairs++;
        }
    }

    for (unsigned bk = 0u; bk < NB_BK; bk++) {
        unsigned outdeg = 0u;

        for (unsigned other = 0u; other < NB_BK; other++) {
            if (other == bk)
                continue;
            if (pair_seen[bk][other])
                outdeg++;
        }
        if (outdeg < min_seen_outdeg)
            min_seen_outdeg = outdeg;
        if (outdeg > max_seen_outdeg)
            max_seen_outdeg = outdeg;
    }

    printf("  unique_pairs=%u min_outdeg=%u max_outdeg=%u\n",
           unique_pairs, min_seen_outdeg, max_seen_outdeg);
    if (unique_pairs < min_unique_pairs) {
        FAILF("%s pair spread too narrow: unique_pairs=%u (< %u)",
              label, unique_pairs, min_unique_pairs);
    }
    if (min_seen_outdeg < min_outdeg) {
        FAILF("%s out-degree too narrow: min_outdeg=%u (< %u)",
              label, min_seen_outdeg, min_outdeg);
    }
}

static void
test_generic_hash_pair_spread(void)
{
    enum {
        NB_BK = 32u,
        MASK = NB_BK - 1u,
        SAMPLES = 4096u,
        MIN_UNIQUE_PAIRS = 128u,
    };
    unsigned char pair_seen[NB_BK][NB_BK];
    unsigned unique_pairs = 0u;
    unsigned min_outdeg = NB_BK;
    unsigned max_outdeg = 0u;

    printf("[T] generic bytes hash pair spread\n");
    memset(pair_seen, 0, sizeof(pair_seen));

    for (unsigned i = 0u; i < SAMPLES; i++) {
        struct hash24_key key = hash24_key_make(i + 1u);
        union rix_hash_hash_u h =
            rix_hash_bytes_GEN(&key, sizeof(key), MASK);
        unsigned bk0 = h.val32[0] & MASK;
        unsigned bk1 = h.val32[1] & MASK;
        unsigned lo;
        unsigned hi;

        if (bk0 == bk1)
            FAILF("generic hash chose identical buckets for sample %u", i);
        if ((h.val32[0] ^ h.val32[1]) == 0u)
            FAILF("generic hash produced zero fp for sample %u", i);

        lo = (bk0 < bk1) ? bk0 : bk1;
        hi = (bk0 < bk1) ? bk1 : bk0;
        if (!pair_seen[lo][hi]) {
            pair_seen[lo][hi] = 1u;
            unique_pairs++;
        }
    }

    for (unsigned bk = 0u; bk < NB_BK; bk++) {
        unsigned outdeg = 0u;

        for (unsigned other = 0u; other < NB_BK; other++) {
            if (other == bk)
                continue;
            if (pair_seen[bk < other ? bk : other]
                         [bk < other ? other : bk]) {
                outdeg++;
            }
        }
        if (outdeg < min_outdeg)
            min_outdeg = outdeg;
        if (outdeg > max_outdeg)
            max_outdeg = outdeg;
    }

    printf("  unique_pairs=%u min_outdeg=%u max_outdeg=%u\n",
           unique_pairs, min_outdeg, max_outdeg);
    if (unique_pairs < MIN_UNIQUE_PAIRS) {
        FAILF("generic hash pair spread too narrow: unique_pairs=%u (< %u)",
              unique_pairs, MIN_UNIQUE_PAIRS);
    }
    if (min_outdeg < 4u)
        FAILF("generic hash out-degree too narrow: min_outdeg=%u", min_outdeg);
}

static void
test_generic_hash_u32_pair_spread(void)
{
    test_u32_hash_pair_spread_impl("generic u32 hash pair spread",
                                   rix_hash_u32_GEN, 32u, 1u);
}

static void
test_generic_hash_u64_pair_spread(void)
{
    test_u64_hash_pair_spread_impl("generic u64 hash pair spread",
                                   rix_hash_u64_GEN, 128u, 4u);
}

static void
test_hash_bytes_fast_dispatch(void)
{
    enum { MASK = 31u };
    struct hash24_key key = hash24_key_make(0x1234u);
    union rix_hash_hash_u fast_hash;
    union rix_hash_hash_u expect_hash;

    printf("[T] hash bytes fast dispatch\n");

    rix_hash_arch_init(0u);
    fast_hash = rix_hash_bytes_fast(&key, sizeof(key), MASK);
#if defined(__x86_64__) && defined(__SSE4_2__)
    expect_hash = rix_hash_bytes_CRC32(&key, sizeof(key), MASK);
    if (rix_hash_arch->hash_bytes != rix_hash_bytes_CRC32)
        FAIL("arch_init(0) did not bind hash_bytes to CRC32");
#else
    expect_hash = rix_hash_bytes_GEN(&key, sizeof(key), MASK);
    if (rix_hash_arch->hash_bytes != rix_hash_bytes_GEN)
        FAIL("arch_init(0) did not bind hash_bytes to GEN");
#endif
    if (fast_hash.val32[0] != expect_hash.val32[0]
        || fast_hash.val32[1] != expect_hash.val32[1]) {
        FAILF("arch_init(0) dispatch mismatch: fast=(%u,%u) expect=(%u,%u)",
              fast_hash.val32[0], fast_hash.val32[1],
              expect_hash.val32[0], expect_hash.val32[1]);
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    fast_hash = rix_hash_bytes_fast(&key, sizeof(key), MASK);
#if defined(__x86_64__) && defined(__SSE4_2__)
    expect_hash = rix_hash_bytes_CRC32(&key, sizeof(key), MASK);
    if (rix_hash_arch->hash_bytes != rix_hash_bytes_CRC32)
        FAIL("arch_init(AUTO) did not bind hash_bytes to CRC32");
#else
    expect_hash = rix_hash_bytes_GEN(&key, sizeof(key), MASK);
    if (rix_hash_arch->hash_bytes != rix_hash_bytes_GEN)
        FAIL("arch_init(AUTO) did not bind hash_bytes to GEN");
#endif
    if (fast_hash.val32[0] != expect_hash.val32[0]
        || fast_hash.val32[1] != expect_hash.val32[1]) {
        FAILF("arch_init(AUTO) dispatch mismatch: fast=(%u,%u) expect=(%u,%u)",
              fast_hash.val32[0], fast_hash.val32[1],
              expect_hash.val32[0], expect_hash.val32[1]);
    }
}

static void
test_hash_u32_fast_dispatch(void)
{
    enum { MASK = 31u };
    u32 key = UINT32_C(0x12345678);
    union rix_hash_hash_u fast_hash;
    union rix_hash_hash_u expect_hash;

    printf("[T] hash u32 fast dispatch\n");

    rix_hash_arch_init(0u);
    fast_hash = rix_hash_arch->hash_u32(key, MASK);
#if defined(__x86_64__) && defined(__SSE4_2__)
    expect_hash = rix_hash_u32_CRC32(key, MASK);
    if (rix_hash_arch->hash_u32 != rix_hash_u32_CRC32)
        FAIL("arch_init(0) did not bind hash_u32 to CRC32");
#else
    expect_hash = rix_hash_u32_GEN(key, MASK);
    if (rix_hash_arch->hash_u32 != rix_hash_u32_GEN)
        FAIL("arch_init(0) did not bind hash_u32 to GEN");
#endif
    if (fast_hash.val32[0] != expect_hash.val32[0]
        || fast_hash.val32[1] != expect_hash.val32[1]) {
        FAILF("arch_init(0) u32 dispatch mismatch: fast=(%u,%u) expect=(%u,%u)",
              fast_hash.val32[0], fast_hash.val32[1],
              expect_hash.val32[0], expect_hash.val32[1]);
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    fast_hash = rix_hash_arch->hash_u32(key, MASK);
#if defined(__x86_64__) && defined(__SSE4_2__)
    expect_hash = rix_hash_u32_CRC32(key, MASK);
    if (rix_hash_arch->hash_u32 != rix_hash_u32_CRC32)
        FAIL("arch_init(AUTO) did not bind hash_u32 to CRC32");
#else
    expect_hash = rix_hash_u32_GEN(key, MASK);
    if (rix_hash_arch->hash_u32 != rix_hash_u32_GEN)
        FAIL("arch_init(AUTO) did not bind hash_u32 to GEN");
#endif
    if (fast_hash.val32[0] != expect_hash.val32[0]
        || fast_hash.val32[1] != expect_hash.val32[1]) {
        FAILF("arch_init(AUTO) u32 dispatch mismatch: fast=(%u,%u) expect=(%u,%u)",
              fast_hash.val32[0], fast_hash.val32[1],
              expect_hash.val32[0], expect_hash.val32[1]);
    }
}

static void
test_hash_u64_fast_dispatch(void)
{
    enum { MASK = 31u };
    u64 key = UINT64_C(0x123456789abcdef0);
    union rix_hash_hash_u fast_hash;
    union rix_hash_hash_u expect_hash;

    printf("[T] hash u64 fast dispatch\n");

    rix_hash_arch_init(0u);
    fast_hash = rix_hash_arch->hash_u64(key, MASK);
#if defined(__x86_64__) && defined(__SSE4_2__)
    expect_hash = rix_hash_u64_CRC32(key, MASK);
    if (rix_hash_arch->hash_u64 != rix_hash_u64_CRC32)
        FAIL("arch_init(0) did not bind hash_u64 to CRC32");
#else
    expect_hash = rix_hash_u64_GEN(key, MASK);
    if (rix_hash_arch->hash_u64 != rix_hash_u64_GEN)
        FAIL("arch_init(0) did not bind hash_u64 to GEN");
#endif
    if (fast_hash.val32[0] != expect_hash.val32[0]
        || fast_hash.val32[1] != expect_hash.val32[1]) {
        FAILF("arch_init(0) u64 dispatch mismatch: fast=(%u,%u) expect=(%u,%u)",
              fast_hash.val32[0], fast_hash.val32[1],
              expect_hash.val32[0], expect_hash.val32[1]);
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    fast_hash = rix_hash_arch->hash_u64(key, MASK);
#if defined(__x86_64__) && defined(__SSE4_2__)
    expect_hash = rix_hash_u64_CRC32(key, MASK);
    if (rix_hash_arch->hash_u64 != rix_hash_u64_CRC32)
        FAIL("arch_init(AUTO) did not bind hash_u64 to CRC32");
#else
    expect_hash = rix_hash_u64_GEN(key, MASK);
    if (rix_hash_arch->hash_u64 != rix_hash_u64_GEN)
        FAIL("arch_init(AUTO) did not bind hash_u64 to GEN");
#endif
    if (fast_hash.val32[0] != expect_hash.val32[0]
        || fast_hash.val32[1] != expect_hash.val32[1]) {
        FAILF("arch_init(AUTO) u64 dispatch mismatch: fast=(%u,%u) expect=(%u,%u)",
              fast_hash.val32[0], fast_hash.val32[1],
              expect_hash.val32[0], expect_hash.val32[1]);
    }
}

#if defined(__x86_64__) && defined(__SSE4_2__)
static void
test_crc32_hash_pair_spread(void)
{
    enum {
        NB_BK = 32u,
        MASK = NB_BK - 1u,
        SAMPLES = 4096u,
        MIN_UNIQUE_PAIRS = 128u,
    };
    unsigned char pair_seen[NB_BK][NB_BK];
    unsigned unique_pairs = 0u;
    unsigned min_outdeg = NB_BK;
    unsigned max_outdeg = 0u;

    printf("[T] crc32 bytes hash pair spread\n");
    memset(pair_seen, 0, sizeof(pair_seen));

    for (unsigned i = 0u; i < SAMPLES; i++) {
        struct hash24_key key = hash24_key_make(i + 1u);
        union rix_hash_hash_u h =
            rix_hash_bytes_CRC32(&key, sizeof(key), MASK);
        unsigned bk0 = h.val32[0] & MASK;
        unsigned bk1 = h.val32[1] & MASK;
        unsigned lo;
        unsigned hi;

        if (bk0 == bk1)
            FAILF("crc32 hash chose identical buckets for sample %u", i);
        if ((h.val32[0] ^ h.val32[1]) == 0u)
            FAILF("crc32 hash produced zero fp for sample %u", i);

        lo = (bk0 < bk1) ? bk0 : bk1;
        hi = (bk0 < bk1) ? bk1 : bk0;
        if (!pair_seen[lo][hi]) {
            pair_seen[lo][hi] = 1u;
            unique_pairs++;
        }
    }

    for (unsigned bk = 0u; bk < NB_BK; bk++) {
        unsigned outdeg = 0u;

        for (unsigned other = 0u; other < NB_BK; other++) {
            if (other == bk)
                continue;
            if (pair_seen[bk < other ? bk : other]
                         [bk < other ? other : bk]) {
                outdeg++;
            }
        }
        if (outdeg < min_outdeg)
            min_outdeg = outdeg;
        if (outdeg > max_outdeg)
            max_outdeg = outdeg;
    }

    printf("  unique_pairs=%u min_outdeg=%u max_outdeg=%u\n",
           unique_pairs, min_outdeg, max_outdeg);
    if (unique_pairs < MIN_UNIQUE_PAIRS) {
        FAILF("crc32 hash pair spread too narrow: unique_pairs=%u (< %u)",
              unique_pairs, MIN_UNIQUE_PAIRS);
    }
    if (min_outdeg < 4u)
        FAILF("crc32 hash out-degree too narrow: min_outdeg=%u", min_outdeg);
}

static void
test_crc32_hash_u32_pair_spread(void)
{
    test_u32_hash_pair_spread_impl("crc32 u32 hash pair spread",
                                   rix_hash_u32_CRC32, 900u, 28u);
}

static void
test_crc32_hash_u64_pair_spread(void)
{
    test_u64_hash_pair_spread_impl("crc32 u64 hash pair spread",
                                   rix_hash_u64_CRC32, 900u, 28u);
}
#endif

/* ================================================================== */
/* Fuzz test                                                           */
/* ================================================================== */
static unsigned xr_fuzz;

static unsigned
xorshift32(void)
{
    unsigned x = xr_fuzz;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (xr_fuzz = x);
}

static unsigned
rnd_in(unsigned lo, unsigned hi)
{
    return lo + (xorshift32() % (hi - lo + 1));
}

static void
test_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[T] fuzz seed=%u N=%u nb_bk=%u ops=%u\n", seed, N, nb_bk, ops);
    xr_fuzz = seed ? seed : 0xC0FFEE11u;

    /* Allocate node pool */
    struct mynode *nodes = (struct mynode *)calloc((size_t)N, sizeof(struct mynode));
    if (!nodes) { perror("calloc nodes"); exit(1); }

    /* Assign unique keys: key.hi = 1-origin index, key.lo = 0 */
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0;
    }

    /* Allocate aligned bucket array (zeroed) */
    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign");
        exit(1);
    }
    memset(bk, 0, bk_sz);

    /* Head */
    struct myht head;
    RIX_HASH_INIT(myht, &head, nb_bk);

    /* Model: present[i] = 1 means nodes[i-1] is in the table */
    unsigned char *present = (unsigned char *)calloc((size_t)N + 1, 1);
    if (!present) { perror("calloc present"); exit(1); }

    unsigned in_table = 0; /* shadow of head.rhh_nb */

    for (unsigned step = 0; step < ops; step++) {
        unsigned op = xorshift32() % 100;

        if (op < 60) {
            /* Insert */
            unsigned idx = rnd_in(1, N); /* 1-origin */
            struct mynode *elm = &nodes[idx - 1];
            struct mynode *ret = myht_insert(&head, bk, nodes, elm);

            if (present[idx]) {
                /* Must return the existing node */
                if (ret != elm)
                    FAILF("fuzz insert dup[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret == elm) {
                    /* Table full - acceptable, just skip */
                } else if (ret != NULL) {
                    FAILF("fuzz insert[%u]: unexpected ret %p", idx, (void *)ret);
                } else {
                    present[idx] = 1;
                    in_table++;
                }
            }

        } else if (op < 80) {
            /* Remove */
            unsigned idx = rnd_in(1, N);
            struct mynode *elm = &nodes[idx - 1];
            struct mynode *ret = myht_remove(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("fuzz remove[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
                present[idx] = 0;
                in_table--;
            } else {
                if (ret != NULL)
                    FAILF("fuzz remove absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }

        } else {
            /* Find */
            unsigned idx = rnd_in(1, N);
            struct mynode *elm = &nodes[idx - 1];
            struct mynode *ret = myht_find(&head, bk, nodes, &elm->key);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("fuzz find[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret != NULL)
                    FAILF("fuzz find absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        }

        /* Periodically verify rhh_nb matches the model */
        if ((step & 0x3FFu) == 0u) {
            if (head.rhh_nb != in_table)
                FAILF("fuzz step %u: rhh_nb=%u model=%u",
                      step, head.rhh_nb, in_table);
        }
    }

    /* Final consistency check */
    if (head.rhh_nb != in_table)
        FAILF("fuzz final: rhh_nb=%u model=%u", head.rhh_nb, in_table);

    /* Walk must visit exactly in_table nodes */
    unsigned walk_cnt = 0;
    myht_walk(&head, bk, nodes, walk_count_cb, &walk_cnt);
    if (walk_cnt != in_table)
        FAILF("fuzz walk: expected %u got %u", in_table, walk_cnt);

    free(present);
    free(bk);
    free(nodes);
}

static void
test_slot_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[T] slot fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           seed, N, nb_bk, ops);
    xr_fuzz = seed ? seed : 0xC0FFEE11u;

    struct mynode_slot *nodes =
        (struct mynode_slot *)calloc((size_t)N, sizeof(struct mynode_slot));
    if (!nodes) { perror("calloc slot nodes"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)nb_bk * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_slot head;
    RIX_HASH_INIT(myht_slot, &head, nb_bk);

    unsigned char *present = (unsigned char *)calloc((size_t)N + 1, 1);
    if (!present) { perror("calloc slot present"); exit(1); }
    unsigned in_table = 0;

    for (unsigned step = 0; step < ops; step++) {
        unsigned op = xorshift32() % 100;

        if (op < 60) {
            unsigned idx = rnd_in(1, N);
            struct mynode_slot *elm = &nodes[idx - 1];
            struct mynode_slot *ret = myht_slot_insert(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("slot fuzz insert dup[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret == elm) {
                    /* table full - acceptable */
                } else if (ret != NULL) {
                    FAILF("slot fuzz insert[%u]: unexpected ret %p",
                          idx, (void *)ret);
                } else {
                    present[idx] = 1;
                    in_table++;
                    slot_verify_node(&head, bk, nodes, elm);
                }
            }
        } else if (op < 80) {
            unsigned idx = rnd_in(1, N);
            struct mynode_slot *elm = &nodes[idx - 1];
            struct mynode_slot *ret = myht_slot_remove(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("slot fuzz remove[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
                present[idx] = 0;
                in_table--;
            } else {
                if (ret != NULL)
                    FAILF("slot fuzz remove absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        } else {
            unsigned idx = rnd_in(1, N);
            struct mynode_slot *elm = &nodes[idx - 1];
            struct mynode_slot *ret = myht_slot_find(&head, bk, nodes, &elm->key);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("slot fuzz find[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
                slot_verify_node(&head, bk, nodes, elm);
            } else {
                if (ret != NULL)
                    FAILF("slot fuzz find absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        }

        if ((step & 0x3FFu) == 0u) {
            if (head.rhh_nb != in_table)
                FAILF("slot fuzz step %u: rhh_nb=%u model=%u",
                      step, head.rhh_nb, in_table);
            slot_verify_present(&head, bk, nodes, present, N);
        }
    }

    if (head.rhh_nb != in_table)
        FAILF("slot fuzz final: rhh_nb=%u model=%u", head.rhh_nb, in_table);
    slot_verify_present(&head, bk, nodes, present, N);

    free(present);
    free(bk);
    free(nodes);
}

/* ================================================================== */
/* keyonly helpers                                                        */
/* ================================================================== */
static void
keyonly_init(void)
{
    memset(g_keyonly,    0, sizeof(g_keyonly));
    memset(g_bk_keyonly, 0, sizeof(g_bk_keyonly));
    RIX_HASH_INIT(myht_keyonly, &g_head_keyonly, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        g_keyonly[i].key.hi = (u64)(i + 1);
        g_keyonly[i].key.lo = 0xDEADC0DE00000000ULL;
    }
}

static void
keyonly_insert_all(void)
{
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_keyonly *ret =
            myht_keyonly_insert(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[i]);
        if (ret != NULL)
            FAILF("keyonly insert[%u] failed (ret=%p)", i, (void *)ret);
    }
    if (g_head_keyonly.rhh_nb != NB_BASIC)
        FAILF("keyonly nb after all inserts: expected %u got %u",
              NB_BASIC, g_head_keyonly.rhh_nb);
}

/* ================================================================== */
/* test_keyonly_duplicate                                                 */
/* ================================================================== */
static void
test_keyonly_duplicate(void)
{
    printf("[T] keyonly duplicate insert\n");
    keyonly_init();

    /* Insert node[0] */
    struct mynode_keyonly *r0 =
        myht_keyonly_insert(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[0]);
    if (r0 != NULL)
        FAIL("keyonly first insert must return NULL");

    /* Re-insert same node - must return existing node */
    struct mynode_keyonly *r1 =
        myht_keyonly_insert(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[0]);
    if (r1 != &g_keyonly[0])
        FAILF("keyonly dup insert must return existing node %p, got %p",
              (void *)&g_keyonly[0], (void *)r1);
    if (g_head_keyonly.rhh_nb != 1u)
        FAILF("keyonly nb must still be 1 after dup, got %u", g_head_keyonly.rhh_nb);

    /* Different node object with same key value */
    struct mynode_keyonly dup;
    dup.key = g_keyonly[0].key;
    struct mynode_keyonly *r2 =
        myht_keyonly_insert(&g_head_keyonly, g_bk_keyonly, g_keyonly, &dup);
    if (r2 != &g_keyonly[0])
        FAILF("keyonly key-dup insert must return existing node %p, got %p",
              (void *)&g_keyonly[0], (void *)r2);
    if (g_head_keyonly.rhh_nb != 1u)
        FAILF("keyonly nb must still be 1 after key-dup, got %u",
              g_head_keyonly.rhh_nb);
}

/* ================================================================== */
/* test_keyonly_insert_find_remove                                        */
/* ================================================================== */
static void
test_keyonly_insert_find_remove(void)
{
    printf("[T] keyonly insert/find/remove\n");
    keyonly_init();
    keyonly_insert_all();

    /* Find each inserted node */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_keyonly *f =
            myht_keyonly_find(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[i].key);
        if (f == NULL)
            FAILF("keyonly find[%u] returned NULL", i);
        if (f != &g_keyonly[i])
            FAILF("keyonly find[%u] returned wrong node %p expected %p",
                  i, (void *)f, (void *)&g_keyonly[i]);
    }

    /* Non-existent key */
    struct mykey bad = { 9999ULL, 0xDEADC0DE00000000ULL };
    if (myht_keyonly_find(&g_head_keyonly, g_bk_keyonly, g_keyonly, &bad) != NULL)
        FAIL("keyonly find of non-existent key should return NULL");

    /* Remove even-indexed nodes */
    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        struct mynode_keyonly *rem =
            myht_keyonly_remove(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[i]);
        if (rem != &g_keyonly[i])
            FAILF("keyonly remove[%u] returned wrong node", i);
    }
    if (g_head_keyonly.rhh_nb != NB_BASIC / 2)
        FAILF("keyonly nb after removals: expected %u got %u",
              NB_BASIC / 2, g_head_keyonly.rhh_nb);

    /* Removed must not be found; remaining must be found */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_keyonly *f =
            myht_keyonly_find(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[i].key);
        if (i % 2 == 0) {
            if (f != NULL)
                FAILF("keyonly removed node[%u] still found", i);
        } else {
            if (f == NULL || f != &g_keyonly[i])
                FAILF("keyonly remaining node[%u] not found or wrong", i);
        }
    }

    /* Re-remove already-removed node must return NULL */
    if (myht_keyonly_remove(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[0]) != NULL)
        FAIL("keyonly re-remove of already-removed node should return NULL");
}

/* ================================================================== */
/* test_keyonly_fuzz                                                      */
/* ================================================================== */
static int walk_keyonly_count_cb(struct mynode_keyonly *node, void *arg)
{
    (void)node;
    unsigned *cnt = (unsigned *)arg;
    (*cnt)++;
    return 0;
}

static void
test_keyonly_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[T] keyonly fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           seed, N, nb_bk, ops);
    xr_fuzz = seed ? seed : 0xC0FFEE11u;

    struct mynode_keyonly *nodes =
        (struct mynode_keyonly *)calloc((size_t)N, sizeof(struct mynode_keyonly));
    if (!nodes) { perror("calloc keyonly nodes"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_keyonly head;
    RIX_HASH_INIT(myht_keyonly, &head, nb_bk);

    unsigned char *present = (unsigned char *)calloc((size_t)N + 1, 1);
    if (!present) { perror("calloc present"); exit(1); }
    unsigned in_table = 0;

    for (unsigned step = 0; step < ops; step++) {
        unsigned op = xorshift32() % 100;

        if (op < 60) {
            unsigned idx = rnd_in(1, N);
            struct mynode_keyonly *elm = &nodes[idx - 1];
            struct mynode_keyonly *ret =
                myht_keyonly_insert(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("keyonly fuzz insert dup[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret == elm) {
                    /* table full - skip */
                } else if (ret != NULL) {
                    FAILF("keyonly fuzz insert[%u]: unexpected ret %p",
                          idx, (void *)ret);
                } else {
                    present[idx] = 1;
                    in_table++;
                }
            }
        } else if (op < 80) {
            unsigned idx = rnd_in(1, N);
            struct mynode_keyonly *elm = &nodes[idx - 1];
            struct mynode_keyonly *ret =
                myht_keyonly_remove(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("keyonly fuzz remove[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
                present[idx] = 0;
                in_table--;
            } else {
                if (ret != NULL)
                    FAILF("keyonly fuzz remove absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        } else {
            unsigned idx = rnd_in(1, N);
            struct mynode_keyonly *elm = &nodes[idx - 1];
            struct mynode_keyonly *ret =
                myht_keyonly_find(&head, bk, nodes, &elm->key);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("keyonly fuzz find[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret != NULL)
                    FAILF("keyonly fuzz find absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        }

        if ((step & 0x3FFu) == 0u) {
            if (head.rhh_nb != in_table)
                FAILF("keyonly fuzz step %u: rhh_nb=%u model=%u",
                      step, head.rhh_nb, in_table);
        }
    }

    if (head.rhh_nb != in_table)
        FAILF("keyonly fuzz final: rhh_nb=%u model=%u", head.rhh_nb, in_table);

    unsigned walk_cnt = 0;
    myht_keyonly_walk(&head, bk, nodes, walk_keyonly_count_cb, &walk_cnt);
    if (walk_cnt != in_table)
        FAILF("keyonly fuzz walk: expected %u got %u", in_table, walk_cnt);

    free(present);
    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_high_fill - fill to 90%+, verify all entries findable          */
/* ================================================================== */
static void
test_high_fill(void)
{
    printf("[T] high_fill (90%%+)\n");

    /*
     * 64 buckets x 16 slots = 1024 total slots.
     * Insert 960 entries (~94% fill) to force long kickout chains.
     */
    const unsigned NB_BK = 64u;
    const unsigned N     = 960u;

    struct mynode *nodes = (struct mynode *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xA1A2A3A400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht head;
    RIX_HASH_INIT(myht, &head, NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode *ret = myht_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            /* table full - stop filling */
            break;
        } else {
            FAILF("high_fill insert[%u]: unexpected ret %p", i, (void *)ret);
        }
    }
    printf("  inserted %u / %u (%.1f%%)\n",
           inserted, NB_BK * RIX_HASH_BUCKET_ENTRY_SZ,
           100.0 * inserted / (NB_BK * RIX_HASH_BUCKET_ENTRY_SZ));

    if (head.rhh_nb != inserted)
        FAILF("high_fill rhh_nb=%u expected %u", head.rhh_nb, inserted);

    /* Every inserted entry must be findable (exercises alt-bucket lookup) */
    for (unsigned i = 0; i < inserted; i++) {
        struct mynode *f = myht_find(&head, bk, nodes, &nodes[i].key);
        if (f != &nodes[i])
            FAILF("high_fill find[%u] failed: got %p expected %p",
                  i, (void *)f, (void *)&nodes[i]);
    }

    /* Remove every other entry, verify remaining still findable */
    unsigned removed = 0;
    for (unsigned i = 0; i < inserted; i += 2) {
        struct mynode *r = myht_remove(&head, bk, nodes, &nodes[i]);
        if (r != &nodes[i])
            FAILF("high_fill remove[%u] failed", i);
        removed++;
    }
    if (head.rhh_nb != inserted - removed)
        FAILF("high_fill after remove: rhh_nb=%u expected %u",
              head.rhh_nb, inserted - removed);

    for (unsigned i = 0; i < inserted; i++) {
        struct mynode *f = myht_find(&head, bk, nodes, &nodes[i].key);
        if (i % 2 == 0) {
            if (f != NULL)
                FAILF("high_fill removed[%u] still found", i);
        } else {
            if (f != &nodes[i])
                FAILF("high_fill remaining[%u] not found", i);
        }
    }

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_max_fill - fill greedily, stop at first failure, verify all     */
/* ================================================================== */
static void
test_max_fill(void)
{
    printf("[T] max_fill\n");

    /*
     * Use more buckets so 95% fill stays well below kickout-exhaustion
     * threshold.  128 bk x 16 slots = 2048 capacity; fill 1945 (~95%).
     */
    const unsigned NB_BK = 128u;
    const unsigned CAPACITY = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ; /* 2048 */
    const unsigned N = (CAPACITY * 95u) / 100u;                  /* 1945 */

    struct mynode *nodes =
        (struct mynode *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xB1B2B3B400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht head;
    RIX_HASH_INIT(myht, &head, NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode *ret = myht_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            /* Kickout exhaustion: stop here once the table rejects inserts. */
            break;
        } else {
            FAILF("max_fill insert[%u]: unexpected ret %p", i, (void *)ret);
        }
    }
    printf("  inserted %u / %u (%.1f%%)\n",
           inserted, CAPACITY,
           100.0 * inserted / CAPACITY);

    if (head.rhh_nb != inserted)
        FAILF("max_fill rhh_nb=%u expected %u", head.rhh_nb, inserted);

    /* Every inserted entry must still be findable */
    for (unsigned i = 0; i < inserted; i++) {
        struct mynode *f = myht_find(&head, bk, nodes, &nodes[i].key);
        if (f != &nodes[i])
            FAILF("max_fill find[%u] failed", i);
    }

    /* Walk count must match */
    unsigned walk_cnt = 0;
    myht_walk(&head, bk, nodes, walk_count_cb, &walk_cnt);
    if (walk_cnt != inserted)
        FAILF("max_fill walk: expected %u got %u", inserted, walk_cnt);

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_kickout_safety - verify failed insert does not lose a victim   */
/* ================================================================== */
static void
test_kickout_safety(void)
{
    printf("[T] kickout_safety\n");

    const unsigned NB_BK = 32u;
    const unsigned CAPACITY = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ; /* 512 */
    const unsigned N = CAPACITY + 64u;

    struct mynode *nodes = (struct mynode *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xB1B2B3B400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht head;
    RIX_HASH_INIT(myht, &head, NB_BK);

    /* Phase 1: fill greedily */
    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode *ret = myht_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL)
            inserted++;
        else if (ret == &nodes[i])
            break;
    }

    /* Phase 2: keep inserting past the first failure to trigger kickout
     * chain exhaustion.  Count how many previously-findable entries
     * become unreachable. */
    unsigned pre_nb = head.rhh_nb;
    unsigned lost = 0;
    for (unsigned i = inserted; i < N; i++) {
        struct mynode *ret = myht_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            /* Check for victim loss after each failed insert */
            for (unsigned j = 0; j < inserted && j < i; j++) {
                struct mynode *f = myht_find(&head, bk, nodes, &nodes[j].key);
                if (f != &nodes[j]) {
                    lost++;
                    break; /* one is enough to confirm */
                }
            }
            if (lost > 0) break;
        }
    }

    printf("  pre_nb=%u post_nb=%u lost_victims=%u\n",
           pre_nb, head.rhh_nb, lost);

    if (lost > 0)
        FAILF("kickout_safety: victim loss detected (%u)", lost);

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_slot_high_fill - slot variant integrity after kickout chains    */
/* ================================================================== */
static void
test_slot_high_fill(void)
{
    printf("[T] slot_high_fill (90%%+)\n");

    const unsigned NB_BK = 64u;
    const unsigned N     = 960u;

    struct mynode_slot *nodes =
        (struct mynode_slot *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xC1C2C3C400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_slot head;
    RIX_HASH_INIT(myht_slot, &head, NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode_slot *ret = myht_slot_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            break;
        } else {
            FAILF("slot_high_fill insert[%u]: unexpected ret", i);
        }
    }
    printf("  inserted %u / %u\n", inserted, NB_BK * RIX_HASH_BUCKET_ENTRY_SZ);

    /* Verify cur_hash/slot integrity for every inserted entry */
    for (unsigned i = 0; i < inserted; i++) {
        struct mynode_slot *f =
            myht_slot_find(&head, bk, nodes, &nodes[i].key);
        if (f != &nodes[i])
            FAILF("slot_high_fill find[%u] failed", i);
        slot_verify_node(&head, bk, nodes, &nodes[i]);
    }

    /* Remove every 3rd entry, verify slot integrity of remaining */
    unsigned removed = 0;
    for (unsigned i = 0; i < inserted; i += 3) {
        struct mynode_slot *r = myht_slot_remove(&head, bk, nodes, &nodes[i]);
        if (r != &nodes[i])
            FAILF("slot_high_fill remove[%u] failed", i);
        removed++;
    }
    if (head.rhh_nb != inserted - removed)
        FAILF("slot_high_fill rhh_nb=%u expected %u",
              head.rhh_nb, inserted - removed);

    for (unsigned i = 0; i < inserted; i++) {
        struct mynode_slot *f =
            myht_slot_find(&head, bk, nodes, &nodes[i].key);
        if (i % 3 == 0) {
            if (f != NULL)
                FAILF("slot_high_fill removed[%u] still found", i);
        } else {
            if (f != &nodes[i])
                FAILF("slot_high_fill remaining[%u] not found", i);
            slot_verify_node(&head, bk, nodes, &nodes[i]);
        }
    }

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_keyonly_high_fill - keyonly variant at high fill                     */
/* ================================================================== */
static void
test_keyonly_high_fill(void)
{
    printf("[T] keyonly_high_fill (90%%+)\n");

    const unsigned NB_BK = 64u;
    const unsigned N     = 960u;

    struct mynode_keyonly *nodes =
        (struct mynode_keyonly *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xD1D2D3D400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_keyonly head;
    RIX_HASH_INIT(myht_keyonly, &head, NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode_keyonly *ret =
            myht_keyonly_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            break;
        } else {
            FAILF("keyonly_high_fill insert[%u]: unexpected ret", i);
        }
    }
    printf("  inserted %u / %u\n", inserted, NB_BK * RIX_HASH_BUCKET_ENTRY_SZ);

    for (unsigned i = 0; i < inserted; i++) {
        struct mynode_keyonly *f =
            myht_keyonly_find(&head, bk, nodes, &nodes[i].key);
        if (f != &nodes[i])
            FAILF("keyonly_high_fill find[%u] failed", i);
    }

    /* Remove half, verify remaining */
    unsigned removed = 0;
    for (unsigned i = 0; i < inserted; i += 2) {
        struct mynode_keyonly *r =
            myht_keyonly_remove(&head, bk, nodes, &nodes[i]);
        if (r != &nodes[i])
            FAILF("keyonly_high_fill remove[%u] failed", i);
        removed++;
    }
    if (head.rhh_nb != inserted - removed)
        FAILF("keyonly_high_fill rhh_nb=%u expected %u",
              head.rhh_nb, inserted - removed);

    for (unsigned i = 0; i < inserted; i++) {
        struct mynode_keyonly *f =
            myht_keyonly_find(&head, bk, nodes, &nodes[i].key);
        if (i % 2 == 0) {
            if (f != NULL)
                FAILF("keyonly_high_fill removed[%u] still found", i);
        } else {
            if (f != &nodes[i])
                FAILF("keyonly_high_fill remaining[%u] not found", i);
        }
    }

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_remove_miss - fp variant: remove node not in the table         */
/* ================================================================== */
static void
test_remove_miss(void)
{
    printf("[T] remove_miss\n");
    basic_init();

    /* Remove a node that was never inserted */
    struct mynode *ret = myht_remove(&g_head, g_bk, g_basic, &g_basic[0]);
    if (ret != NULL)
        FAIL("remove of never-inserted node should return NULL");

    /* Insert node[0], remove it, then remove again (double remove) */
    struct mynode *r0 = myht_insert(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r0 != NULL)
        FAIL("insert should succeed");

    struct mynode *r1 = myht_remove(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r1 != &g_basic[0])
        FAIL("first remove should return the node");

    struct mynode *r2 = myht_remove(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r2 != NULL)
        FAIL("second remove (double remove) should return NULL");

    if (g_head.rhh_nb != 0u)
        FAILF("nb after double remove: expected 0 got %u", g_head.rhh_nb);
}

/* ================================================================== */
/* test_slot_remove_miss - slot variant: remove node not in the table   */
/* ================================================================== */
static void
test_slot_remove_miss(void)
{
    printf("[T] slot_remove_miss\n");
    slot_basic_init();

    /* Remove a node that was never inserted */
    struct mynode_slot *ret =
        myht_slot_remove(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]);
    if (ret != NULL)
        FAIL("slot remove of never-inserted node should return NULL");

    /* Insert node[0], remove it, then remove again (double remove) */
    struct mynode_slot *r0 =
        myht_slot_insert(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]);
    if (r0 != NULL)
        FAIL("slot insert should succeed");

    struct mynode_slot *r1 =
        myht_slot_remove(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]);
    if (r1 != &g_slot[0])
        FAIL("slot first remove should return the node");

    struct mynode_slot *r2 =
        myht_slot_remove(&g_head_slot, g_bk_slot, g_slot, &g_slot[0]);
    if (r2 != NULL)
        FAIL("slot second remove (double remove) should return NULL");

    if (g_head_slot.rhh_nb != 0u)
        FAILF("slot nb after double remove: expected 0 got %u",
              g_head_slot.rhh_nb);
}

/* ================================================================== */
/* test_keyonly_remove_miss - keyonly variant: remove node not in the table  */
/* ================================================================== */
static void
test_keyonly_remove_miss(void)
{
    printf("[T] keyonly_remove_miss\n");
    keyonly_init();

    /* Remove a node that was never inserted */
    struct mynode_keyonly *ret =
        myht_keyonly_remove(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[0]);
    if (ret != NULL)
        FAIL("keyonly remove of never-inserted node should return NULL");

    /* Insert node[0], remove it, then remove again (double remove) */
    struct mynode_keyonly *r0 =
        myht_keyonly_insert(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[0]);
    if (r0 != NULL)
        FAIL("keyonly insert should succeed");

    struct mynode_keyonly *r1 =
        myht_keyonly_remove(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[0]);
    if (r1 != &g_keyonly[0])
        FAIL("keyonly first remove should return the node");

    struct mynode_keyonly *r2 =
        myht_keyonly_remove(&g_head_keyonly, g_bk_keyonly, g_keyonly, &g_keyonly[0]);
    if (r2 != NULL)
        FAIL("keyonly second remove (double remove) should return NULL");

    if (g_head_keyonly.rhh_nb != 0u)
        FAILF("keyonly nb after double remove: expected 0 got %u",
              g_head_keyonly.rhh_nb);
}

/* ================================================================== */
/* test_slot_kickout_safety - slot variant failed insert safety        */
/* ================================================================== */
static void
test_slot_kickout_safety(void)
{
    printf("[T] slot_kickout_safety\n");

    const unsigned NB_BK = 32u;
    const unsigned CAPACITY = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ; /* 512 */
    const unsigned N = CAPACITY + 64u;

    struct mynode_slot *nodes =
        (struct mynode_slot *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xB1B2B3B400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_slot head;
    RIX_HASH_INIT(myht_slot, &head, NB_BK);

    /* Phase 1: fill greedily */
    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode_slot *ret = myht_slot_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL)
            inserted++;
        else if (ret == &nodes[i])
            break;
    }

    /* Phase 2: keep inserting past the first failure to trigger kickout
     * chain exhaustion.  Count how many previously-findable entries
     * become unreachable. */
    unsigned pre_nb = head.rhh_nb;
    unsigned lost = 0;
    for (unsigned i = inserted; i < N; i++) {
        struct mynode_slot *ret = myht_slot_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            /* Check for victim loss after each failed insert */
            for (unsigned j = 0; j < inserted && j < i; j++) {
                struct mynode_slot *f =
                    myht_slot_find(&head, bk, nodes, &nodes[j].key);
                if (f != &nodes[j]) {
                    lost++;
                    break; /* one is enough to confirm */
                }
            }
            if (lost > 0) break;
        }
    }

    printf("  pre_nb=%u post_nb=%u lost_victims=%u\n",
           pre_nb, head.rhh_nb, lost);

    if (lost > 0)
        FAILF("slot_kickout_safety: victim loss detected (%u)", lost);

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_keyonly_kickout_safety - keyonly variant failed insert safety  */
/* ================================================================== */
static void
test_keyonly_kickout_safety(void)
{
    printf("[T] keyonly_kickout_safety\n");

    const unsigned NB_BK = 32u;
    const unsigned CAPACITY = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ; /* 512 */
    const unsigned N = CAPACITY + 64u;

    struct mynode_keyonly *nodes =
        (struct mynode_keyonly *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xB1B2B3B400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_keyonly head;
    RIX_HASH_INIT(myht_keyonly, &head, NB_BK);

    /* Phase 1: fill greedily */
    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode_keyonly *ret =
            myht_keyonly_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL)
            inserted++;
        else if (ret == &nodes[i])
            break;
    }

    /* Phase 2: keep inserting past the first failure to trigger kickout
     * chain exhaustion. */
    unsigned pre_nb = head.rhh_nb;
    unsigned lost = 0;
    for (unsigned i = inserted; i < N; i++) {
        struct mynode_keyonly *ret =
            myht_keyonly_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            /* Check for victim loss after each failed insert */
            for (unsigned j = 0; j < inserted && j < i; j++) {
                struct mynode_keyonly *f =
                    myht_keyonly_find(&head, bk, nodes, &nodes[j].key);
                if (f != &nodes[j]) {
                    lost++;
                    break; /* one is enough to confirm */
                }
            }
            if (lost > 0) break;
        }
    }

    printf("  pre_nb=%u post_nb=%u lost_victims=%u\n",
           pre_nb, head.rhh_nb, lost);

    if (lost > 0)
        FAILF("keyonly_kickout_safety: victim loss detected (%u)", lost);

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_slot_max_fill - slot variant: fill until first failure          */
/* ================================================================== */
static void
test_slot_max_fill(void)
{
    printf("[T] slot_max_fill\n");

    const unsigned NB_BK = 128u;
    const unsigned CAPACITY = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ; /* 2048 */
    const unsigned N = (CAPACITY * 95u) / 100u;                  /* 1945 */

    struct mynode_slot *nodes =
        (struct mynode_slot *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xB1B2B3B400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_slot head;
    RIX_HASH_INIT(myht_slot, &head, NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode_slot *ret = myht_slot_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            break;
        } else {
            FAILF("slot_max_fill insert[%u]: unexpected ret %p",
                  i, (void *)ret);
        }
    }
    printf("  inserted %u / %u (%.1f%%)\n",
           inserted, CAPACITY,
           100.0 * inserted / CAPACITY);

    if (head.rhh_nb != inserted)
        FAILF("slot_max_fill rhh_nb=%u expected %u", head.rhh_nb, inserted);

    /* Every inserted entry must still be findable */
    for (unsigned i = 0; i < inserted; i++) {
        struct mynode_slot *f =
            myht_slot_find(&head, bk, nodes, &nodes[i].key);
        if (f != &nodes[i])
            FAILF("slot_max_fill find[%u] failed", i);
        slot_verify_node(&head, bk, nodes, &nodes[i]);
    }

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* test_keyonly_max_fill - keyonly variant: fill until first failure         */
/* ================================================================== */
static void
test_keyonly_max_fill(void)
{
    printf("[T] keyonly_max_fill\n");

    const unsigned NB_BK = 128u;
    const unsigned CAPACITY = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ; /* 2048 */
    const unsigned N = (CAPACITY * 95u) / 100u;                  /* 1945 */

    struct mynode_keyonly *nodes =
        (struct mynode_keyonly *)calloc((size_t)N, sizeof(*nodes));
    if (!nodes) { perror("calloc"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (u64)(i + 1);
        nodes[i].key.lo = 0xB1B2B3B400000000ULL;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)NB_BK * sizeof(*bk);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_keyonly head;
    RIX_HASH_INIT(myht_keyonly, &head, NB_BK);

    unsigned inserted = 0;
    for (unsigned i = 0; i < N; i++) {
        struct mynode_keyonly *ret =
            myht_keyonly_insert(&head, bk, nodes, &nodes[i]);
        if (ret == NULL) {
            inserted++;
        } else if (ret == &nodes[i]) {
            break;
        } else {
            FAILF("keyonly_max_fill insert[%u]: unexpected ret %p",
                  i, (void *)ret);
        }
    }
    printf("  inserted %u / %u (%.1f%%)\n",
           inserted, CAPACITY,
           100.0 * inserted / CAPACITY);

    if (head.rhh_nb != inserted)
        FAILF("keyonly_max_fill rhh_nb=%u expected %u", head.rhh_nb, inserted);

    /* Every inserted entry must still be findable */
    for (unsigned i = 0; i < inserted; i++) {
        struct mynode_keyonly *f =
            myht_keyonly_find(&head, bk, nodes, &nodes[i].key);
        if (f != &nodes[i])
            FAILF("keyonly_max_fill find[%u] failed", i);
    }

    free(bk);
    free(nodes);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(int argc, char **argv)
{
    unsigned seed   = 0xC0FFEE11u;
    unsigned N      = 512u;
    unsigned nb_bk  = 64u;  /* 64 x 16 = 1024 slots; N/capacity = 0.5 */
    unsigned ops    = 200000u;

    if (argc >= 2) seed  = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) N     = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) nb_bk = (unsigned)strtoul(argv[3], NULL, 10);
    if (argc >= 5) ops   = (unsigned)strtoul(argv[4], NULL, 10);

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    test_init_empty();
    test_insert_find_remove();
    test_duplicate();
    test_remove_miss();
    test_staged_find();
    test_walk();
    test_generic_hash_pair_spread();
    test_generic_hash_u32_pair_spread();
    test_generic_hash_u64_pair_spread();
    test_hash_bytes_fast_dispatch();
    test_hash_u32_fast_dispatch();
    test_hash_u64_fast_dispatch();
#if defined(__x86_64__) && defined(__SSE4_2__)
    test_crc32_hash_pair_spread();
    test_crc32_hash_u32_pair_spread();
    test_crc32_hash_u64_pair_spread();
#endif
    test_fuzz(seed, N, nb_bk, ops);
    test_slot_insert_find_remove();
    test_slot_duplicate();
    test_slot_remove_miss();
    test_slot_fuzz(seed, N, nb_bk, ops);

    test_keyonly_insert_find_remove();
    test_keyonly_duplicate();
    test_keyonly_remove_miss();
    test_keyonly_fuzz(seed, N, nb_bk, ops);

    /* High-fill / hash-conflict tests (all 3 variants) */
    test_high_fill();
    test_max_fill();
    test_kickout_safety();
    test_slot_high_fill();
    test_slot_max_fill();
    test_slot_kickout_safety();
    test_keyonly_high_fill();
    test_keyonly_max_fill();
    test_keyonly_kickout_safety();

    /* Aggressive fuzz: 98% fill ratio (N ~ nb_bk x 16) */
    test_fuzz(seed, 1000, 64, 500000);
    test_slot_fuzz(seed, 1000, 64, 500000);
    test_keyonly_fuzz(seed, 1000, 64, 500000);

    printf("ALL RIX_HASH TESTS PASSED\n");
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
