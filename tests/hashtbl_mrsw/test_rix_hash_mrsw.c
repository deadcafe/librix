/* test_rix_hash_mrsw.c
 *  RIX_HASH_MRSW - unit and concurrency tests
 */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mrsw_test_hook(const char *name, const char *event,
                           void *head, void *buckets,
                           unsigned bk, unsigned slot);

#define RIX_HASH_MRSW_HOOK(name, event, head, buckets, bk, slot)             \
    mrsw_test_hook((name), (event), (void *)(head), (void *)(buckets),       \
                   (bk), (slot))

#include "rix/rix_hash_mrsw.h"

#define FAIL(msg) do {                                                       \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n",                                  \
            __FILE__, __LINE__, __func__, (msg));                           \
    abort();                                                                \
} while (0)

#define FAILF(fmt, ...) do {                                                 \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n",                            \
            __FILE__, __LINE__, __func__, __VA_ARGS__);                     \
    abort();                                                                \
} while (0)

struct mykey {
    u64 hi;
    u64 lo;
};

struct mynode {
    u32 cur_hash;
    u32 value;
    struct mykey key;
};

static int
mykey_cmp(const struct mykey *a, const struct mykey *b)
{
    return memcmp(a, b, sizeof(*a));
}

RIX_HASH_MRSW_HEAD(myht_mrsw);
RIX_HASH_MRSW_GENERATE(myht_mrsw, mynode, key, cur_hash, mykey_cmp)

struct ctl_key {
    u32 id;
};

struct ctl_node {
    u32 cur_hash;
    struct ctl_key key;
};

static int
ctl_key_cmp(const struct ctl_key *a, const struct ctl_key *b)
{
    return (a->id == b->id) ? 0 : 1;
}

static union rix_hash_hash_u
ctl_hash_fn(const struct ctl_key *key, u32 mask)
{
    union rix_hash_hash_u h;

    (void)mask;
    if (key->id <= 15u) {
        h.val32[0] = 0u;
        h.val32[1] = 2u;
    } else if (key->id <= 30u) {
        h.val32[0] = 1u;
        h.val32[1] = 2u;
    } else {
        h.val32[0] = 0u;
        h.val32[1] = 1u;
    }
    return h;
}

RIX_HASH_MRSW_HEAD(ctlht);
RIX_HASH_MRSW_GENERATE_EX(ctlht, ctl_node, key, cur_hash,
                          ctl_key_cmp, ctl_hash_fn)

/* SLOT variant test fixture (mirrors mynode but with slot_field). */
struct myslot_node {
    u32 cur_hash;
    u32 value;
    u8  slot;
    u8  pad[3];
    struct mykey key;
};

RIX_HASH_MRSW_HEAD(myslot_mrsw);
RIX_HASH_MRSW_GENERATE_SLOT(myslot_mrsw, myslot_node, key, cur_hash, slot,
                            mykey_cmp)

/* KEYONLY variant test fixture (no hash_field, no slot_field in node). */
struct mykeyonly_node {
    u32 value;
    struct mykey key;
};

RIX_HASH_MRSW_HEAD(mykeyonly_mrsw);
RIX_HASH_MRSW_GENERATE_KEYONLY(mykeyonly_mrsw, mykeyonly_node, key, mykey_cmp)

#define NB_BASIC    20u
#define NB_BK_BASIC  4u

static struct mynode g_basic[NB_BASIC];
static struct rix_hash_bucket_s g_bk[NB_BK_BASIC]
    __attribute__((aligned(64)));
static struct myht_mrsw g_head;

#define CTL_NODES 64u
#define CTL_NB_BK  4u

static struct ctl_node g_ctl[CTL_NODES];
static struct rix_hash_bucket_s g_ctl_bk[CTL_NB_BK]
    __attribute__((aligned(64)));
static struct ctlht g_ctl_head;

static _Atomic int g_hook_enabled;
static _Atomic int g_hook_reached;
static _Atomic int g_hook_release;
static _Atomic int g_reader_retry_seen;
static _Atomic unsigned g_hook_bk;
static _Atomic unsigned g_hook_slot;
static const char *g_hook_event;

static void
test_nb_bk_hint_fill_target(void)
{
    static const struct {
        unsigned max_entries;
        unsigned nb_bk;
    } cases[] = {
        { 0u,       2u },
        { 1u,       2u },
        { 21u,      2u },
        { 22u,      4u },
        { 1024u,    128u },
        { 1048576u, 131072u },
    };

    printf("[T] mrsw nb_bk hint targets <=70%% slot fill\n");
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned nb = rix_hash_mrsw_nb_bk_hint(cases[i].max_entries);
        uint64_t slots = (uint64_t)nb * RIX_HASH_MRSW_BUCKET_ENTRY_SZ;

        if (nb != cases[i].nb_bk)
            FAILF("nb_bk_hint(%u)=%u expected %u",
                  cases[i].max_entries, nb, cases[i].nb_bk);
        if (cases[i].max_entries != 0u
            && (uint64_t)cases[i].max_entries * 100u > slots * 70u)
            FAILF("nb_bk_hint(%u) over 70%% fill with %u buckets",
                  cases[i].max_entries, nb);
    }
}

static void
mrsw_test_hook(const char *name, const char *event,
               void *head, void *buckets, unsigned bk, unsigned slot)
{
    (void)name;
    (void)head;
    (void)buckets;

    if (strcmp(event, "reader_retry") == 0)
        atomic_store_explicit(&g_reader_retry_seen, 1, memory_order_release);

    if (!atomic_load_explicit(&g_hook_enabled, memory_order_acquire))
        return;
    if (strcmp(event, g_hook_event) != 0)
        return;

    atomic_store_explicit(&g_hook_bk, bk, memory_order_relaxed);
    atomic_store_explicit(&g_hook_slot, slot, memory_order_relaxed);
    atomic_store_explicit(&g_hook_reached, 1, memory_order_release);
    while (!atomic_load_explicit(&g_hook_release, memory_order_acquire))
        sched_yield();
}

static void
hook_arm(const char *event)
{
    g_hook_event = event;
    atomic_store_explicit(&g_hook_reached, 0, memory_order_relaxed);
    atomic_store_explicit(&g_hook_release, 0, memory_order_relaxed);
    atomic_store_explicit(&g_hook_bk, UINT32_MAX, memory_order_relaxed);
    atomic_store_explicit(&g_hook_slot, UINT32_MAX, memory_order_relaxed);
    atomic_store_explicit(&g_hook_enabled, 1, memory_order_release);
}

static void
hook_release(void)
{
    atomic_store_explicit(&g_hook_release, 1, memory_order_release);
}

static void
hook_disarm(void)
{
    atomic_store_explicit(&g_hook_enabled, 0, memory_order_release);
    atomic_store_explicit(&g_hook_release, 0, memory_order_relaxed);
    atomic_store_explicit(&g_hook_reached, 0, memory_order_relaxed);
    atomic_store_explicit(&g_hook_bk, UINT32_MAX, memory_order_relaxed);
    atomic_store_explicit(&g_hook_slot, UINT32_MAX, memory_order_relaxed);
    g_hook_event = NULL;
}

static void
wait_reached(void)
{
    for (unsigned i = 0u; i < 10000000u; i++) {
        if (atomic_load_explicit(&g_hook_reached, memory_order_acquire))
            return;
        sched_yield();
    }
    FAIL("timed out waiting for hook");
}

static void
basic_init(void)
{
    memset(g_basic, 0, sizeof(g_basic));
    RIX_HASH_MRSW_INIT(myht_mrsw, &g_head, g_bk, NB_BK_BASIC);
    for (unsigned i = 0u; i < NB_BASIC; i++) {
        g_basic[i].key.hi = (u64)(i + 1u);
        g_basic[i].key.lo = UINT64_C(0xDEADC0DE00000000);
        g_basic[i].value = i + 100u;
    }
}

static void
basic_insert_all(void)
{
    for (unsigned i = 0u; i < NB_BASIC; i++) {
        struct mynode *ret =
            myht_mrsw_insert(&g_head, g_bk, g_basic, &g_basic[i]);
        if (ret != NULL)
            FAILF("insert[%u] failed ret=%p", i, (void *)ret);
    }
    if (atomic_load_explicit(&g_head.rhh_nb, memory_order_relaxed) != NB_BASIC)
        FAIL("rhh_nb mismatch after insert_all");
}

static void
test_init_empty(void)
{
    printf("[T] mrsw init/empty\n");
    basic_init();

    if (g_head.rhh_mask != NB_BK_BASIC - 1u)
        FAIL("mask mismatch");
    if (atomic_load_explicit(&g_head.rhh_nb, memory_order_relaxed) != 0u)
        FAIL("nb not zero");

    struct mykey k = { 1u, UINT64_C(0xDEADC0DE00000000) };
    if (myht_mrsw_find(&g_head, g_bk, g_basic, &k) != NULL)
        FAIL("find on empty table returned node");
    if (myht_mrsw_remove(&g_head, g_bk, g_basic, &g_basic[0]) != NULL)
        FAIL("remove on empty table returned node");
}

static void
test_insert_find_remove(void)
{
    printf("[T] mrsw insert/find/remove\n");
    basic_init();
    basic_insert_all();

    for (unsigned i = 0u; i < NB_BASIC; i++) {
        struct mynode *f =
            myht_mrsw_find(&g_head, g_bk, g_basic, &g_basic[i].key);
        if (f != &g_basic[i])
            FAILF("find[%u] mismatch got %p expected %p",
                  i, (void *)f, (void *)&g_basic[i]);
    }

    struct mykey bad = { 9999u, UINT64_C(0xDEADC0DE00000000) };
    if (myht_mrsw_find(&g_head, g_bk, g_basic, &bad) != NULL)
        FAIL("find of absent key returned node");

    for (unsigned i = 0u; i < NB_BASIC; i += 2u) {
        struct mynode *r =
            myht_mrsw_remove(&g_head, g_bk, g_basic, &g_basic[i]);
        if (r != &g_basic[i])
            FAILF("remove[%u] mismatch", i);
    }

    for (unsigned i = 0u; i < NB_BASIC; i++) {
        struct mynode *f =
            myht_mrsw_find(&g_head, g_bk, g_basic, &g_basic[i].key);
        if ((i & 1u) == 0u) {
            if (f != NULL)
                FAILF("removed node[%u] still found", i);
        } else if (f != &g_basic[i]) {
            FAILF("remaining node[%u] mismatch", i);
        }
    }
}

static void
test_duplicate(void)
{
    printf("[T] mrsw duplicate\n");
    basic_init();

    struct mynode *r0 =
        myht_mrsw_insert(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r0 != NULL)
        FAIL("first insert returned non-NULL");

    struct mynode *r1 =
        myht_mrsw_insert(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r1 != &g_basic[0])
        FAIL("same-node duplicate did not return existing node");

    struct mynode dup;
    memset(&dup, 0, sizeof(dup));
    dup.key = g_basic[0].key;
    struct mynode *r2 =
        myht_mrsw_insert(&g_head, g_bk, g_basic, &dup);
    if (r2 != &g_basic[0])
        FAIL("same-key duplicate did not return existing node");
    if (atomic_load_explicit(&g_head.rhh_nb, memory_order_relaxed) != 1u)
        FAIL("duplicate changed count");
}

static void
test_staged_find(void)
{
    printf("[T] mrsw staged find x1/x2/x4\n");
    basic_init();
    basic_insert_all();

    struct mykey bad = { 9999u, UINT64_C(0xDEADC0DE00000000) };
    struct rix_hash_mrsw_find_ctx_s ctx;

    RIX_HASH_MRSW_HASH_KEY(myht_mrsw, &ctx, &g_head, g_bk, &g_basic[3].key);
    RIX_HASH_MRSW_SCAN_BK(myht_mrsw, &ctx, &g_head, g_bk);
    if (RIX_HASH_MRSW_CMP_KEY(myht_mrsw, &ctx, g_basic) != &g_basic[3])
        FAIL("staged x1 mismatch");

    struct rix_hash_mrsw_find_ctx_s ctx2[2];
    const struct mykey *keys2[2] = { &g_basic[0].key, &bad };
    struct mynode *res2[2];
    RIX_HASH_MRSW_HASH_KEY2(myht_mrsw, ctx2, &g_head, g_bk, keys2);
    RIX_HASH_MRSW_SCAN_BK2(myht_mrsw, ctx2, &g_head, g_bk);
    RIX_HASH_MRSW_CMP_KEY2(myht_mrsw, ctx2, g_basic, res2);
    if (res2[0] != &g_basic[0] || res2[1] != NULL)
        FAIL("staged x2 mismatch");

    struct rix_hash_mrsw_find_ctx_s ctx4[4];
    const struct mykey *keys4[4] = {
        &g_basic[1].key, &bad, &g_basic[7].key, &g_basic[11].key
    };
    struct mynode *res4[4];
    RIX_HASH_MRSW_HASH_KEY4(myht_mrsw, ctx4, &g_head, g_bk, keys4);
    RIX_HASH_MRSW_SCAN_BK4(myht_mrsw, ctx4, &g_head, g_bk);
    RIX_HASH_MRSW_CMP_KEY4(myht_mrsw, ctx4, g_basic, res4);
    if (res4[0] != &g_basic[1] || res4[1] != NULL ||
        res4[2] != &g_basic[7] || res4[3] != &g_basic[11])
        FAIL("staged x4 mismatch");
}

static int
walk_count_cb(struct mynode *node, void *arg)
{
    (void)node;
    unsigned *cnt = (unsigned *)arg;
    (*cnt)++;
    return 0;
}

static void
test_walk(void)
{
    printf("[T] mrsw walk\n");
    basic_init();
    basic_insert_all();

    unsigned cnt = 0u;
    int rc = myht_mrsw_walk(&g_head, g_bk, g_basic, walk_count_cb, &cnt);
    if (rc != 0)
        FAILF("walk returned %d", rc);
    if (cnt != NB_BASIC)
        FAILF("walk count expected %u got %u", NB_BASIC, cnt);
}

struct op_arg {
    struct mynode *node;
    struct mynode *ret;
};

static void *
insert_thread(void *arg)
{
    struct op_arg *a = (struct op_arg *)arg;
    a->ret = myht_mrsw_insert(&g_head, g_bk, g_basic, a->node);
    return NULL;
}

static void *
remove_thread(void *arg)
{
    struct op_arg *a = (struct op_arg *)arg;
    a->ret = myht_mrsw_remove(&g_head, g_bk, g_basic, a->node);
    return NULL;
}

struct find_arg {
    const struct mykey *key;
    struct mynode *ret;
    _Atomic int done;
};

static void *
find_thread(void *arg)
{
    struct find_arg *a = (struct find_arg *)arg;
    a->ret = myht_mrsw_find(&g_head, g_bk, g_basic, a->key);
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

struct ctl_op_arg {
    struct ctl_node *node;
    struct ctl_node *ret;
};

struct ctl_find_arg {
    const struct ctl_key *key;
    struct ctl_node *ret;
};

static void *
ctl_insert_thread(void *arg)
{
    struct ctl_op_arg *a = (struct ctl_op_arg *)arg;
    a->ret = ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, a->node);
    return NULL;
}

static void *
ctl_find_thread(void *arg)
{
    struct ctl_find_arg *a = (struct ctl_find_arg *)arg;
    a->ret = ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, a->key);
    return NULL;
}

static void
assert_reader_completes(struct find_arg *fa)
{
    for (unsigned i = 0u; i < 1000000u; i++) {
        if (atomic_load_explicit(&fa->done, memory_order_acquire))
            return;
        sched_yield();
    }
    FAIL("reader did not complete for unrelated bucket");
}

static int
key_uses_bucket(const struct mykey *key, unsigned bk)
{
    struct rix_hash_mrsw_find_ctx_s ctx;

    myht_mrsw_hash_key(&ctx, &g_head, g_bk, key);
    return ctx.bk[0] == &g_bk[bk] || ctx.bk[1] == &g_bk[bk];
}

static const struct mykey *
find_key_outside_bucket(unsigned bk)
{
    for (unsigned i = 1u; i < NB_BASIC; i++) {
        if (!key_uses_bucket(&g_basic[i].key, bk))
            return &g_basic[i].key;
    }
    FAIL("could not find test key outside paused bucket");
    return NULL;
}

static void
test_forced_insert_interleaving(void)
{
    printf("[T] mrsw forced insert interleaving\n");
    basic_init();

    hook_arm("insert_idx");
    struct op_arg wa = { &g_basic[0], NULL };
    pthread_t wt;
    if (pthread_create(&wt, NULL, insert_thread, &wa) != 0)
        FAIL("pthread_create writer failed");
    wait_reached();

    unsigned paused_bk = atomic_load_explicit(&g_hook_bk, memory_order_relaxed);
    const struct mykey *other_key = find_key_outside_bucket(paused_bk);
    struct find_arg other = { other_key, NULL, ATOMIC_VAR_INIT(0) };
    pthread_t ot;
    if (pthread_create(&ot, NULL, find_thread, &other) != 0)
        FAIL("pthread_create unrelated reader failed");
    assert_reader_completes(&other);
    pthread_join(ot, NULL);
    if (other.ret != NULL)
        FAIL("unrelated absent key returned a node");

    struct find_arg fa = { &g_basic[0].key, NULL, ATOMIC_VAR_INIT(0) };
    pthread_t rt;
    if (pthread_create(&rt, NULL, find_thread, &fa) != 0)
        FAIL("pthread_create reader failed");
    assert_reader_completes(&fa);
    pthread_join(rt, NULL);
    if (fa.ret != NULL)
        FAIL("reader observed unpublished insert slot");

    hook_release();
    pthread_join(wt, NULL);
    hook_disarm();

    if (wa.ret != NULL)
        FAIL("insert returned non-NULL");
    if (myht_mrsw_find(&g_head, g_bk, g_basic, &g_basic[0].key) !=
        &g_basic[0])
        FAIL("node not found after insert completed");
}

static void
test_forced_remove_interleaving(void)
{
    printf("[T] mrsw forced remove interleaving\n");
    basic_init();
    if (myht_mrsw_insert(&g_head, g_bk, g_basic, &g_basic[0]) != NULL)
        FAIL("setup insert failed");

    hook_arm("remove_hash");
    struct op_arg wa = { &g_basic[0], NULL };
    pthread_t wt;
    if (pthread_create(&wt, NULL, remove_thread, &wa) != 0)
        FAIL("pthread_create writer failed");
    wait_reached();

    unsigned paused_bk = atomic_load_explicit(&g_hook_bk, memory_order_relaxed);
    const struct mykey *other_key = find_key_outside_bucket(paused_bk);
    struct find_arg other = { other_key, NULL, ATOMIC_VAR_INIT(0) };
    pthread_t ot;
    if (pthread_create(&ot, NULL, find_thread, &other) != 0)
        FAIL("pthread_create unrelated reader failed");
    assert_reader_completes(&other);
    pthread_join(ot, NULL);
    if (other.ret != NULL)
        FAIL("unrelated absent key returned a node");

    struct find_arg fa = { &g_basic[0].key, NULL, ATOMIC_VAR_INIT(0) };
    pthread_t rt;
    if (pthread_create(&rt, NULL, find_thread, &fa) != 0)
        FAIL("pthread_create reader failed");
    assert_reader_completes(&fa);
    pthread_join(rt, NULL);
    if (fa.ret != NULL)
        FAIL("reader found node after valid bit was cleared");

    hook_release();
    pthread_join(wt, NULL);
    hook_disarm();

    if (wa.ret != &g_basic[0])
        FAIL("remove returned wrong node");
    if (myht_mrsw_find(&g_head, g_bk, g_basic, &g_basic[0].key) != NULL)
        FAIL("removed node found after writer completed");
}

static void
ctl_init(void)
{
    memset(g_ctl, 0, sizeof(g_ctl));
    ctlht_init(&g_ctl_head, g_ctl_bk, CTL_NB_BK);
    for (unsigned i = 0u; i < CTL_NODES; i++)
        g_ctl[i].key.id = i + 1u;
}

static unsigned
ctl_count_idx_in_bucket(unsigned bk, u32 idx)
{
    unsigned count = 0u;

    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
        u32 ctrl = atomic_load_explicit(&g_ctl_bk[bk].ctrl,
                                        memory_order_acquire);
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);
        u32 si = g_ctl_bk[bk].idx[s];
        if ((valid & (UINT32_C(1) << s)) != 0u && si == idx)
            count++;
    }
    return count;
}

static unsigned
ctl_count_raw_idx_in_bucket(unsigned bk, u32 idx)
{
    unsigned count = 0u;

    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
        if (g_ctl_bk[bk].idx[s] == idx)
            count++;
    }
    return count;
}

static void
test_ctrl_slot_hash_hit_masked(void)
{
    printf("[T] mrsw ctrl slot hash hit is masked\n");
    ctl_init();

    /*
     * ctl key id=1 has fp=2 and primary bucket 0.  Store 2 in ctrl so the
     * SIMD hash scan sees a match at physical word 15.  Because valid=0, that
     * bit must be masked out and no slot may be reported.
     */
    atomic_store_explicit(&g_ctl_bk[0].ctrl, UINT32_C(2),
                          memory_order_release);
    if (ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[0].key) != NULL)
        FAIL("ctrl word was treated as a valid hash slot");
}

static void
test_stale_payload_ignored_and_reused(void)
{
    printf("[T] mrsw stale payload ignored/reused\n");
    ctl_init();

    if (ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[30]) != NULL)
        FAIL("setup insert id31 failed");
    if (ctl_count_idx_in_bucket(0u, 31u) != 1u)
        FAIL("id31 was not visible in bucket 0");

    if (ctlht_remove(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[30]) != &g_ctl[30])
        FAIL("remove id31 failed");
    if (ctl_count_idx_in_bucket(0u, 31u) != 0u)
        FAIL("id31 remained valid after remove");
    if (ctl_count_raw_idx_in_bucket(0u, 31u) != 1u)
        FAIL("remove unexpectedly cleared stale idx payload");
    if (ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[30].key) != NULL)
        FAIL("stale invalid payload was found");

    if (ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[31]) != NULL)
        FAIL("insert id32 after stale remove failed");
    if (ctl_count_idx_in_bucket(0u, 31u) != 0u)
        FAIL("old id31 became visible after slot reuse");
    if (ctl_count_idx_in_bucket(0u, 32u) != 1u)
        FAIL("id32 did not reuse the freed valid slot");
    if (ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[30].key) != NULL)
        FAIL("old key found after same-slot reuse");
    if (ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[31].key) !=
        &g_ctl[31])
        FAIL("new key not found after same-slot reuse");
}

static void
test_forced_kickout_publish_before_unpublish(void)
{
    printf("[T] mrsw forced kickout publish-before-unpublish\n");
    ctl_init();

    for (unsigned i = 0u; i < 30u; i++) {
        struct ctl_node *ret =
            ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[i]);
        if (ret != NULL)
            FAILF("ctl setup insert[%u] failed ret=%p", i, (void *)ret);
    }

    hook_arm("move_before_old_clear");
    struct ctl_op_arg wa = { &g_ctl[30], NULL };
    pthread_t wt;
    if (pthread_create(&wt, NULL, ctl_insert_thread, &wa) != 0)
        FAIL("pthread_create ctl writer failed");
    wait_reached();

    /*
     * The victim g_ctl[0] starts in bucket 0 and moves to bucket 2.  At this
     * hook point the alternate bucket has already been published and the old
     * slot has not yet been cleared, so both registrations must exist.
     */
    if (ctl_count_idx_in_bucket(0u, 1u) != 1u)
        FAIL("victim disappeared from old bucket before old clear");
    if (ctl_count_idx_in_bucket(2u, 1u) != 1u)
        FAIL("victim was not published to alternate bucket before old clear");

    struct ctl_node *victim =
        ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[0].key);
    if (victim != &g_ctl[0])
        FAIL("victim find failed during kickout handoff");

    hook_release();
    pthread_join(wt, NULL);
    hook_disarm();

    if (wa.ret != NULL)
        FAIL("kickout insert returned non-NULL");
    if (ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[0].key) != &g_ctl[0])
        FAIL("victim find failed after kickout");
    if (ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[30].key) !=
        &g_ctl[30])
        FAIL("new node find failed after kickout");
}

static void
test_forced_kickout_between_reader_verifies(void)
{
    printf("[T] mrsw forced insert between miss verifies\n");
    ctl_init();

    for (unsigned i = 0u; i < 30u; i++) {
        struct ctl_node *ret =
            ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[i]);
        if (ret != NULL)
            FAILF("ctl setup insert[%u] failed ret=%p", i, (void *)ret);
    }

    atomic_store_explicit(&g_reader_retry_seen, 0, memory_order_relaxed);
    hook_arm("reader_before_verify");

    struct ctl_find_arg ra = { &g_ctl[30].key, NULL };
    pthread_t rt;
    if (pthread_create(&rt, NULL, ctl_find_thread, &ra) != 0)
        FAIL("pthread_create ctl reader failed");
    wait_reached();

    /*
     * The reader has missed the new key and has not yet performed the final
     * miss verify.  Insert publishes the key and kickouts g_ctl[0].  The miss
     * path must notice a ctrl change, retry, and find the newly inserted key.
     */
    struct ctl_node *wr =
        ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[30]);
    if (wr != NULL)
        FAIL("ctl kickout insert failed");

    hook_release();
    pthread_join(rt, NULL);
    hook_disarm();

    if (ra.ret != &g_ctl[30])
        FAIL("reader did not find inserted key after retry");
    if (!atomic_load_explicit(&g_reader_retry_seen, memory_order_acquire))
        FAIL("reader did not retry after miss bucket changed");
}

static void
test_reader_retries_same_slot_reuse(void)
{
    printf("[T] mrsw reader retries missed insert\n");
    ctl_init();

    atomic_store_explicit(&g_reader_retry_seen, 0, memory_order_relaxed);
    hook_arm("reader_before_verify");

    struct ctl_find_arg ra = { &g_ctl[31].key, NULL };
    pthread_t rt;
    if (pthread_create(&rt, NULL, ctl_find_thread, &ra) != 0)
        FAIL("pthread_create ctl reader failed");
    wait_reached();

    /*
     * The reader has missed id32 but has not performed the final ctrl verify.
     * Publishing id32 before the verify must force a retry so the reader does
     * not return a false negative.
     */
    if (ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[31]) != NULL)
        FAIL("insert id32 during paused reader failed");

    hook_release();
    pthread_join(rt, NULL);
    hook_disarm();

    if (ra.ret != &g_ctl[31])
        FAIL("reader did not find id32 after retry");
    if (!atomic_load_explicit(&g_reader_retry_seen, memory_order_acquire))
        FAIL("reader did not retry after missed insert");
}

#define REUSE_NR_READERS 4u
#define REUSE_ITERS     100000u

static _Atomic int g_reuse_stop;
static _Atomic int g_reuse_fail;

static void *
same_slot_reuse_reader(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(UINT32_C(0x85ebca6b) ^ tid);

    while (!atomic_load_explicit(&g_reuse_stop, memory_order_acquire)) {
        x = x * UINT32_C(1103515245) + UINT32_C(12345);
        unsigned n = 30u + (x & 1u);
        struct ctl_node *ret =
            ctlht_find(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[n].key);
        if (ret != NULL && ret != &g_ctl[n]) {
            atomic_store_explicit(&g_reuse_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_same_slot_reuse_stress(void)
{
    printf("[T] mrsw same-slot reuse stress\n");
    ctl_init();

    if (ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[30]) != NULL)
        FAIL("setup insert id31 failed");

    atomic_store_explicit(&g_reuse_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_reuse_fail, 0, memory_order_relaxed);

    pthread_t readers[REUSE_NR_READERS];
    for (uintptr_t i = 0u; i < REUSE_NR_READERS; i++) {
        if (pthread_create(&readers[i], NULL, same_slot_reuse_reader,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create same-slot reader failed");
    }

    unsigned cur = 30u;
    for (unsigned i = 0u; i < REUSE_ITERS; i++) {
        unsigned next = (cur == 30u) ? 31u : 30u;
        if (ctlht_remove(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[cur]) !=
            &g_ctl[cur])
            FAIL("same-slot stress remove failed");
        if (ctlht_insert(&g_ctl_head, g_ctl_bk, g_ctl, &g_ctl[next]) != NULL)
            FAIL("same-slot stress insert failed");
        cur = next;
        if (atomic_load_explicit(&g_reuse_fail, memory_order_acquire))
            break;
    }

    atomic_store_explicit(&g_reuse_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < REUSE_NR_READERS; i++)
        pthread_join(readers[i], NULL);

    if (atomic_load_explicit(&g_reuse_fail, memory_order_acquire))
        FAIL("same-slot reader observed wrong node");
}

#define STRESS_N     1024u
#define STRESS_NB_BK  128u
#define STRESS_PERM   512u
#define STRESS_CHURN  256u

static struct mynode g_stress[STRESS_N];
static struct rix_hash_bucket_s g_stress_bk[STRESS_NB_BK]
    __attribute__((aligned(64)));
static struct myht_mrsw g_stress_head;
static _Atomic int g_stress_stop;
static _Atomic int g_stress_fail;

static void
stress_init(void)
{
    memset(g_stress, 0, sizeof(g_stress));
    myht_mrsw_init(&g_stress_head, g_stress_bk, STRESS_NB_BK);
    for (unsigned i = 0u; i < STRESS_N; i++) {
        g_stress[i].key.hi = (u64)(i + 1u);
        g_stress[i].key.lo = UINT64_C(0xBEEF000000000000) ^ (u64)i;
        g_stress[i].value = i;
    }
    for (unsigned i = 0u; i < STRESS_PERM; i++) {
        if (myht_mrsw_insert(&g_stress_head, g_stress_bk,
                             g_stress, &g_stress[i]) != NULL)
            FAILF("stress permanent insert[%u] failed", i);
    }
}

static void *
stress_reader(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(0x9e3779b9u ^ (tid * 2654435761u));

    while (!atomic_load_explicit(&g_stress_stop, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        unsigned i = x & (STRESS_N - 1u);
        struct mynode *f =
            myht_mrsw_find(&g_stress_head, g_stress_bk,
                           g_stress, &g_stress[i].key);
        if (f != NULL && mykey_cmp(&f->key, &g_stress[i].key) != 0) {
            atomic_store_explicit(&g_stress_fail, 1, memory_order_release);
            return NULL;
        }
        if (i < STRESS_PERM && f != &g_stress[i]) {
            atomic_store_explicit(&g_stress_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_stress(void)
{
    printf("[T] mrsw reader/writer stress\n");
    stress_init();
    atomic_store_explicit(&g_stress_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stress_fail, 0, memory_order_relaxed);

    enum { NR = 4 };
    pthread_t readers[NR];
    for (uintptr_t i = 0u; i < NR; i++) {
        if (pthread_create(&readers[i], NULL, stress_reader,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create reader failed");
    }

    for (unsigned iter = 0u; iter < 50000u; iter++) {
        unsigned i = STRESS_PERM + (iter % STRESS_CHURN);
        struct mynode *ret =
            myht_mrsw_find(&g_stress_head, g_stress_bk,
                           g_stress, &g_stress[i].key);
        if (ret == NULL) {
            if (myht_mrsw_insert(&g_stress_head, g_stress_bk,
                                 g_stress, &g_stress[i]) != NULL)
                FAILF("stress insert[%u] failed", i);
        } else {
            if (ret != &g_stress[i])
                FAIL("stress find returned wrong node to writer");
            if (myht_mrsw_remove(&g_stress_head, g_stress_bk,
                                 g_stress, &g_stress[i]) != &g_stress[i])
                FAILF("stress remove[%u] failed", i);
        }
        if (atomic_load_explicit(&g_stress_fail, memory_order_acquire))
            break;
    }

    atomic_store_explicit(&g_stress_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < NR; i++)
        pthread_join(readers[i], NULL);

    if (atomic_load_explicit(&g_stress_fail, memory_order_acquire))
        FAIL("stress reader observed invalid result");
}

/* ---- SLOT variant tests --------------------------------------------- */

#define NB_SLOT_BASIC    300u
#define NB_BK_SLOT_BASIC  32u

static struct myslot_node g_slot[NB_SLOT_BASIC];
static struct rix_hash_bucket_s g_slot_bk[NB_BK_SLOT_BASIC]
    __attribute__((aligned(64)));
static struct myslot_mrsw g_slot_head;

static void
slot_init(void)
{
    memset(g_slot, 0, sizeof(g_slot));
    myslot_mrsw_init(&g_slot_head, g_slot_bk, NB_BK_SLOT_BASIC);
    for (unsigned i = 0u; i < NB_SLOT_BASIC; i++) {
        g_slot[i].key.hi = (u64)(i + 1u);
        g_slot[i].key.lo = UINT64_C(0x5101010100000000) ^ (u64)i;
        g_slot[i].value  = i + 1u;
    }
}

static void
test_slot_insert_find_remove(void)
{
    printf("[T] mrsw slot insert/find/remove\n");
    slot_init();

    for (unsigned i = 0u; i < NB_SLOT_BASIC; i++) {
        struct myslot_node *r =
            myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot, &g_slot[i]);
        if (r != NULL)
            FAILF("slot insert[%u] failed", i);
    }
    if (atomic_load_explicit(&g_slot_head.rhh_nb, memory_order_relaxed)
        != NB_SLOT_BASIC)
        FAIL("slot rhh_nb mismatch");

    for (unsigned i = 0u; i < NB_SLOT_BASIC; i++) {
        struct myslot_node *f = myslot_mrsw_find(&g_slot_head, g_slot_bk,
                                                 g_slot, &g_slot[i].key);
        if (f != &g_slot[i])
            FAILF("slot find[%u] mismatch", i);
        unsigned bk = (unsigned)(f->cur_hash & g_slot_head.rhh_mask);
        unsigned slot = (unsigned)f->slot;
        if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)
            FAILF("slot field out of range[%u]: %u", i, slot);
        if (g_slot_bk[bk].idx[slot] != i + 1u)
            FAILF("slot field disagrees with bucket[%u]: bk=%u slot=%u idx=%u",
                  i, bk, slot, g_slot_bk[bk].idx[slot]);
    }

    for (unsigned i = 0u; i < NB_SLOT_BASIC; i += 3u) {
        struct myslot_node *r = myslot_mrsw_remove(&g_slot_head, g_slot_bk,
                                                   g_slot, &g_slot[i]);
        if (r != &g_slot[i])
            FAILF("slot remove[%u] mismatch", i);
    }

    for (unsigned i = 0u; i < NB_SLOT_BASIC; i++) {
        struct myslot_node *f = myslot_mrsw_find(&g_slot_head, g_slot_bk,
                                                 g_slot, &g_slot[i].key);
        if ((i % 3u) == 0u) {
            if (f != NULL)
                FAILF("slot removed node[%u] still found", i);
        } else if (f != &g_slot[i]) {
            FAILF("slot remaining[%u] mismatch", i);
        }
    }
}

static void
test_slot_duplicate(void)
{
    printf("[T] mrsw slot duplicate\n");
    slot_init();

    if (myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot, &g_slot[0])
        != NULL)
        FAIL("slot first insert returned non-NULL");
    if (myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot, &g_slot[0])
        != &g_slot[0])
        FAIL("slot same-node duplicate did not return existing");

    struct myslot_node dup;
    memset(&dup, 0, sizeof(dup));
    dup.key = g_slot[0].key;
    if (myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot, &dup)
        != &g_slot[0])
        FAIL("slot same-key duplicate did not return existing");
    if (atomic_load_explicit(&g_slot_head.rhh_nb, memory_order_relaxed)
        != 1u)
        FAIL("slot duplicate changed count");
}

static void
test_slot_walk(void)
{
    printf("[T] mrsw slot walk\n");
    slot_init();
    for (unsigned i = 0u; i < 100u; i++) {
        if (myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot, &g_slot[i])
            != NULL)
            FAILF("slot walk setup insert[%u] failed", i);
    }
    unsigned cnt = 0u;
    int rc = myslot_mrsw_walk(&g_slot_head, g_slot_bk, g_slot,
                              (int (*)(struct myslot_node *, void *))
                              walk_count_cb, &cnt);
    if (rc != 0)
        FAILF("slot walk returned %d", rc);
    if (cnt != 100u)
        FAILF("slot walk count expected 100 got %u", cnt);
}

static void
test_slot_kickout_field_consistency(void)
{
    printf("[T] mrsw slot field tracks kickout moves\n");
    slot_init();

    for (unsigned i = 0u; i < NB_SLOT_BASIC; i++) {
        if (myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot, &g_slot[i])
            != NULL)
            FAILF("slot kickout setup insert[%u] failed", i);
    }
    /* Verify slot_field on every published node still locates the node. */
    for (unsigned i = 0u; i < NB_SLOT_BASIC; i++) {
        unsigned bk = (unsigned)(g_slot[i].cur_hash & g_slot_head.rhh_mask);
        unsigned slot = (unsigned)g_slot[i].slot;
        u32 ctrl = atomic_load_explicit(&g_slot_bk[bk].ctrl,
                                        memory_order_acquire);
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);
        if ((valid & (UINT32_C(1) << slot)) == 0u)
            FAILF("slot[%u] points to unpublished slot bk=%u slot=%u",
                  i, bk, slot);
        if (g_slot_bk[bk].idx[slot] != i + 1u)
            FAILF("slot[%u] disagrees with bucket bk=%u slot=%u idx=%u",
                  i, bk, slot, g_slot_bk[bk].idx[slot]);
    }
    /* Remove every node via O(1) slot remove and verify count reaches 0. */
    for (unsigned i = 0u; i < NB_SLOT_BASIC; i++) {
        if (myslot_mrsw_remove(&g_slot_head, g_slot_bk, g_slot, &g_slot[i])
            != &g_slot[i])
            FAILF("slot remove[%u] failed", i);
    }
    if (atomic_load_explicit(&g_slot_head.rhh_nb, memory_order_relaxed) != 0u)
        FAIL("slot rhh_nb not zero after full remove");
}

static _Atomic int g_slot_stop;
static _Atomic int g_slot_fail;

static void *
slot_stress_reader(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(0x9e3779b9u ^ (tid * 2654435761u));
    while (!atomic_load_explicit(&g_slot_stop, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        unsigned i = x % NB_SLOT_BASIC;
        struct myslot_node *f = myslot_mrsw_find(&g_slot_head, g_slot_bk,
                                                 g_slot, &g_slot[i].key);
        if (f != NULL && f != &g_slot[i]) {
            atomic_store_explicit(&g_slot_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_slot_stress(void)
{
    printf("[T] mrsw slot reader/writer stress\n");
    slot_init();
    /* Permanent set so readers always have hits. */
    for (unsigned i = 0u; i < 200u; i++) {
        if (myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot, &g_slot[i])
            != NULL)
            FAILF("slot stress setup insert[%u] failed", i);
    }

    atomic_store_explicit(&g_slot_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_slot_fail, 0, memory_order_relaxed);
    enum { NR = 4 };
    pthread_t readers[NR];
    for (uintptr_t i = 0u; i < NR; i++) {
        if (pthread_create(&readers[i], NULL, slot_stress_reader,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create slot reader failed");
    }

    for (unsigned iter = 0u; iter < 50000u; iter++) {
        unsigned i = 200u + (iter % 64u);
        struct myslot_node *r = myslot_mrsw_find(&g_slot_head, g_slot_bk,
                                                 g_slot, &g_slot[i].key);
        if (r == NULL) {
            if (myslot_mrsw_insert(&g_slot_head, g_slot_bk, g_slot,
                                   &g_slot[i]) != NULL)
                FAILF("slot stress insert[%u] failed", i);
        } else {
            if (myslot_mrsw_remove(&g_slot_head, g_slot_bk, g_slot,
                                   &g_slot[i]) != &g_slot[i])
                FAILF("slot stress remove[%u] failed", i);
        }
        if (atomic_load_explicit(&g_slot_fail, memory_order_acquire))
            break;
    }

    atomic_store_explicit(&g_slot_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < NR; i++)
        pthread_join(readers[i], NULL);

    if (atomic_load_explicit(&g_slot_fail, memory_order_acquire))
        FAIL("slot stress reader observed wrong node");
}

/* ---- KEYONLY variant tests ------------------------------------------ */

#define NB_KEYONLY_BASIC    300u
#define NB_BK_KEYONLY_BASIC  32u

static struct mykeyonly_node g_ko[NB_KEYONLY_BASIC];
static struct rix_hash_bucket_s g_ko_bk[NB_BK_KEYONLY_BASIC]
    __attribute__((aligned(64)));
static struct mykeyonly_mrsw g_ko_head;

static int
locate_idx_common(struct rix_hash_bucket_s *buckets, unsigned mask,
                  u32 idx, unsigned *bk_out, unsigned *slot_out)
{
    for (unsigned b = 0u; b <= mask; b++) {
        u32 ctrl = atomic_load_explicit(&buckets[b].ctrl,
                                        memory_order_acquire);
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
            if ((valid & (UINT32_C(1) << s)) != 0u &&
                buckets[b].idx[s] == idx) {
                *bk_out = b;
                *slot_out = s;
                return 1;
            }
        }
    }
    return 0;
}

static void
ko_init(void)
{
    memset(g_ko, 0, sizeof(g_ko));
    mykeyonly_mrsw_init(&g_ko_head, g_ko_bk, NB_BK_KEYONLY_BASIC);
    for (unsigned i = 0u; i < NB_KEYONLY_BASIC; i++) {
        g_ko[i].key.hi = (u64)(i + 1u);
        g_ko[i].key.lo = UINT64_C(0xC0DE000000000000) ^ (u64)i;
        g_ko[i].value  = i + 1u;
    }
}

static void
test_keyonly_insert_find_remove(void)
{
    printf("[T] mrsw keyonly insert/find/remove\n");
    ko_init();

    for (unsigned i = 0u; i < NB_KEYONLY_BASIC; i++) {
        struct mykeyonly_node *r =
            mykeyonly_mrsw_insert(&g_ko_head, g_ko_bk, g_ko, &g_ko[i]);
        if (r != NULL)
            FAILF("keyonly insert[%u] failed", i);
    }
    if (atomic_load_explicit(&g_ko_head.rhh_nb, memory_order_relaxed)
        != NB_KEYONLY_BASIC)
        FAIL("keyonly rhh_nb mismatch");

    for (unsigned i = 0u; i < NB_KEYONLY_BASIC; i++) {
        struct mykeyonly_node *f = mykeyonly_mrsw_find(&g_ko_head, g_ko_bk,
                                                       g_ko, &g_ko[i].key);
        if (f != &g_ko[i])
            FAILF("keyonly find[%u] mismatch", i);
    }
    for (unsigned i = 0u; i < NB_KEYONLY_BASIC; i += 3u) {
        struct mykeyonly_node *r = mykeyonly_mrsw_remove(&g_ko_head, g_ko_bk,
                                                         g_ko, &g_ko[i]);
        if (r != &g_ko[i])
            FAILF("keyonly remove[%u] failed", i);
    }
    for (unsigned i = 0u; i < NB_KEYONLY_BASIC; i++) {
        struct mykeyonly_node *f = mykeyonly_mrsw_find(&g_ko_head, g_ko_bk,
                                                       g_ko, &g_ko[i].key);
        if ((i % 3u) == 0u) {
            if (f != NULL)
                FAILF("keyonly removed[%u] still found", i);
        } else if (f != &g_ko[i]) {
            FAILF("keyonly remaining[%u] mismatch", i);
        }
    }
}

static void
test_keyonly_duplicate(void)
{
    printf("[T] mrsw keyonly duplicate\n");
    ko_init();

    if (mykeyonly_mrsw_insert(&g_ko_head, g_ko_bk, g_ko, &g_ko[0]) != NULL)
        FAIL("keyonly first insert returned non-NULL");
    if (mykeyonly_mrsw_insert(&g_ko_head, g_ko_bk, g_ko, &g_ko[0])
        != &g_ko[0])
        FAIL("keyonly same-node duplicate did not return existing");

    struct mykeyonly_node dup;
    memset(&dup, 0, sizeof(dup));
    dup.key = g_ko[0].key;
    if (mykeyonly_mrsw_insert(&g_ko_head, g_ko_bk, g_ko, &dup) != &g_ko[0])
        FAIL("keyonly same-key duplicate did not return existing");
    if (atomic_load_explicit(&g_ko_head.rhh_nb, memory_order_relaxed) != 1u)
        FAIL("keyonly duplicate changed count");
}

static void
test_keyonly_staged_remove_at(void)
{
    printf("[T] mrsw keyonly staged/remove_at API\n");
    ko_init();

    for (unsigned i = 0u; i < NB_KEYONLY_BASIC; i++) {
        if (mykeyonly_mrsw_insert(&g_ko_head, g_ko_bk, g_ko, &g_ko[i]) != NULL)
            FAILF("keyonly staged setup insert[%u] failed", i);
    }

    struct rix_hash_mrsw_find_ctx_s ctx[4];
    const struct mykey *keys[4] = {
        &g_ko[1].key, &g_ko[7].key, &g_ko[11].key, &g_ko[13].key
    };
    struct mykeyonly_node *res[4];
    RIX_HASH_MRSW_HASH_KEY_N_MASKED(mykeyonly_mrsw, ctx, 4u, &g_ko_head,
                                    g_ko_bk, keys, g_ko_head.rhh_mask,
                                    g_ko_head.rhh_mask);
    RIX_HASH_MRSW_SCAN_BK_N(mykeyonly_mrsw, ctx, 4u, &g_ko_head, g_ko_bk);
    RIX_HASH_MRSW_PREFETCH_NODE_N(mykeyonly_mrsw, ctx, 4u, g_ko);
    RIX_HASH_MRSW_CMP_KEY_N(mykeyonly_mrsw, ctx, 4u, g_ko, res);
    if (res[0] != &g_ko[1] || res[1] != &g_ko[7] ||
        res[2] != &g_ko[11] || res[3] != &g_ko[13])
        FAIL("keyonly staged N mismatch");

    unsigned bk;
    unsigned slot;
    if (!locate_idx_common(g_ko_bk, g_ko_head.rhh_mask, 8u, &bk, &slot))
        FAIL("keyonly remove_at target not located");
    if (RIX_HASH_MRSW_REMOVE_AT(mykeyonly_mrsw, &g_ko_head, g_ko_bk, bk, slot)
        != 8u)
        FAIL("keyonly remove_at returned wrong idx");
    if (mykeyonly_mrsw_find(&g_ko_head, g_ko_bk, g_ko, &g_ko[7].key) != NULL)
        FAIL("keyonly remove_at target still found");
}

static _Atomic int g_ko_stop;
static _Atomic int g_ko_fail;

static void *
ko_stress_reader(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(0x9e3779b9u ^ (tid * 2654435761u));
    while (!atomic_load_explicit(&g_ko_stop, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        unsigned i = x % NB_KEYONLY_BASIC;
        struct mykeyonly_node *f = mykeyonly_mrsw_find(&g_ko_head, g_ko_bk,
                                                       g_ko, &g_ko[i].key);
        if (f != NULL && f != &g_ko[i]) {
            atomic_store_explicit(&g_ko_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_keyonly_stress(void)
{
    printf("[T] mrsw keyonly reader/writer stress\n");
    ko_init();
    for (unsigned i = 0u; i < 200u; i++) {
        if (mykeyonly_mrsw_insert(&g_ko_head, g_ko_bk, g_ko, &g_ko[i]) != NULL)
            FAILF("keyonly stress setup insert[%u] failed", i);
    }
    atomic_store_explicit(&g_ko_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ko_fail, 0, memory_order_relaxed);
    enum { NR = 4 };
    pthread_t readers[NR];
    for (uintptr_t i = 0u; i < NR; i++)
        if (pthread_create(&readers[i], NULL, ko_stress_reader,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create keyonly reader failed");

    for (unsigned iter = 0u; iter < 50000u; iter++) {
        unsigned i = 200u + (iter % 64u);
        struct mykeyonly_node *r = mykeyonly_mrsw_find(&g_ko_head, g_ko_bk,
                                                       g_ko, &g_ko[i].key);
        if (r == NULL) {
            if (mykeyonly_mrsw_insert(&g_ko_head, g_ko_bk, g_ko,
                                      &g_ko[i]) != NULL)
                FAILF("keyonly stress insert[%u] failed", i);
        } else {
            if (mykeyonly_mrsw_remove(&g_ko_head, g_ko_bk, g_ko,
                                      &g_ko[i]) != &g_ko[i])
                FAILF("keyonly stress remove[%u] failed", i);
        }
        if (atomic_load_explicit(&g_ko_fail, memory_order_acquire))
            break;
    }

    atomic_store_explicit(&g_ko_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < NR; i++)
        pthread_join(readers[i], NULL);
    if (atomic_load_explicit(&g_ko_fail, memory_order_acquire))
        FAIL("keyonly stress reader observed wrong node");
}

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    test_nb_bk_hint_fill_target();
    test_init_empty();
    test_insert_find_remove();
    test_duplicate();
    test_staged_find();
    test_walk();
    test_forced_insert_interleaving();
    test_forced_remove_interleaving();
    test_ctrl_slot_hash_hit_masked();
    test_stale_payload_ignored_and_reused();
    test_forced_kickout_publish_before_unpublish();
    test_forced_kickout_between_reader_verifies();
    test_reader_retries_same_slot_reuse();
    test_same_slot_reuse_stress();
    test_stress();
    test_slot_insert_find_remove();
    test_slot_duplicate();
    test_slot_walk();
    test_slot_kickout_field_consistency();
    test_slot_stress();
    test_keyonly_insert_find_remove();
    test_keyonly_duplicate();
    test_keyonly_staged_remove_at();
    test_keyonly_stress();

    printf("OK\n");
    return 0;
}
