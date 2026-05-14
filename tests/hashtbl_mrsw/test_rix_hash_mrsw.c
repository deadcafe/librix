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
#include <time.h>

static void mrsw_test_hook(const char *name, const char *event,
                           void *head, void *buckets,
                           unsigned bk, unsigned slot);

#define RIX_HASH_MRSW_HOOK(name, event, head, buckets, bk, slot)             \
    mrsw_test_hook((name), (event), (void *)(head), (void *)(buckets),       \
                   (bk), (slot))

#include "rix/rix_hash_mr.h"

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

RIX_HASH_MRMW_HEAD(myht_mrmw);
RIX_HASH_MRMW_GENERATE(myht_mrmw, mynode, key, cur_hash, mykey_cmp)

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

static union rix_hash_hash_u
ctl_path_hash_fn(const struct ctl_key *key, u32 mask)
{
    union rix_hash_hash_u h;

    (void)mask;
    if (key->id <= 15u) {
        h.val32[0] = 0u;
        h.val32[1] = 2u;
    } else if (key->id <= 30u) {
        h.val32[0] = 1u;
        h.val32[1] = 2u;
    } else if (key->id <= 45u) {
        h.val32[0] = 2u;
        h.val32[1] = 3u;
    } else {
        h.val32[0] = 0u;
        h.val32[1] = 1u;
    }
    return h;
}

RIX_HASH_MRSW_HEAD(ctlht);
RIX_HASH_MRSW_GENERATE_EX(ctlht, ctl_node, key, cur_hash,
                          ctl_key_cmp, ctl_hash_fn)

RIX_HASH_MRMW_HEAD(ctlht_mrmw);
RIX_HASH_MRMW_GENERATE_EX(ctlht_mrmw, ctl_node, key, cur_hash,
                          ctl_key_cmp, ctl_hash_fn)

struct ctl_slot_node {
    u32 cur_hash;
    u8  slot;
    u8  pad[3];
    struct ctl_key key;
};

RIX_HASH_MRMW_HEAD(ctlslot_mrmw);
RIX_HASH_MRMW_GENERATE_SLOT_EX(ctlslot_mrmw, ctl_slot_node, key, cur_hash,
                               slot, ctl_key_cmp, ctl_hash_fn)

struct ctl_keyonly_node {
    struct ctl_key key;
};

RIX_HASH_MRMW_HEAD(ctlko_mrmw);
RIX_HASH_MRMW_GENERATE_KEYONLY_EX(ctlko_mrmw, ctl_keyonly_node, key,
                                  ctl_key_cmp, ctl_hash_fn)

struct ctl_extra_node {
    u32 cur_hash;
    u8  slot;
    u8  pad[3];
    struct ctl_key key;
};

RIX_HASH_MRMW_HEAD(ctlextra_mrmw);
RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_EX(ctlextra_mrmw, ctl_extra_node, key,
                                     cur_hash, slot, ctl_key_cmp, ctl_hash_fn)

RIX_HASH_MRMW_HEAD(ctlpath_mrmw);
RIX_HASH_MRMW_GENERATE_EX(ctlpath_mrmw, ctl_node, key, cur_hash,
                          ctl_key_cmp, ctl_path_hash_fn)

RIX_HASH_MRMW_HEAD(ctlpathslot_mrmw);
RIX_HASH_MRMW_GENERATE_SLOT_EX(ctlpathslot_mrmw, ctl_slot_node, key, cur_hash,
                               slot, ctl_key_cmp, ctl_path_hash_fn)

RIX_HASH_MRMW_HEAD(ctlpathko_mrmw);
RIX_HASH_MRMW_GENERATE_KEYONLY_EX(ctlpathko_mrmw, ctl_keyonly_node, key,
                                  ctl_key_cmp, ctl_path_hash_fn)

RIX_HASH_MRMW_HEAD(ctlpathextra_mrmw);
RIX_HASH_MRMW_GENERATE_SLOT_EXTRA_EX(ctlpathextra_mrmw, ctl_extra_node, key,
                                     cur_hash, slot, ctl_key_cmp,
                                     ctl_path_hash_fn)

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

RIX_HASH_MRMW_HEAD(myslot_mrmw);
RIX_HASH_MRMW_GENERATE_SLOT(myslot_mrmw, myslot_node, key, cur_hash, slot,
                            mykey_cmp)

/* KEYONLY variant test fixture (no hash_field, no slot_field in node). */
struct mykeyonly_node {
    u32 value;
    struct mykey key;
};

RIX_HASH_MRSW_HEAD(mykeyonly_mrsw);
RIX_HASH_MRSW_GENERATE_KEYONLY(mykeyonly_mrsw, mykeyonly_node, key, mykey_cmp)

RIX_HASH_MRMW_HEAD(mykeyonly_mrmw);
RIX_HASH_MRMW_GENERATE_KEYONLY(mykeyonly_mrmw, mykeyonly_node, key, mykey_cmp)

/* U32 variant test fixture (u32 key stored in bucket, no node aux). */
struct myu32_node {
    u32 key;
    u32 value;
};

RIX_HASH_MRSW_HEAD(myu32_mrsw);
RIX_HASH_MRSW_GENERATE_U32(myu32_mrsw, struct myu32_node, key)

RIX_HASH_MRMW_HEAD(myu32_mrmw);
RIX_HASH_MRMW_GENERATE_U32(myu32_mrmw, struct myu32_node, key)

/* U64 variant test fixture. */
struct myu64_node {
    u64 key;
    u64 value;
};

RIX_HASH_MRSW_HEAD(myu64_mrsw);
RIX_HASH_MRSW_GENERATE_U64(myu64_mrsw, struct myu64_node, key)

RIX_HASH_MRMW_HEAD(myu64_mrmw);
RIX_HASH_MRMW_GENERATE_U64(myu64_mrmw, struct myu64_node, key)

/* SLOT_EXTRA variant test fixture (mirrors slot, but with extra[] in bucket). */
struct myextra_node {
    u32 cur_hash;
    u8  slot;
    u8  pad[3];
    struct mykey key;
};

RIX_HASH_MRSW_HEAD(myextra_mrsw);
RIX_HASH_MRSW_GENERATE_SLOT_EXTRA(myextra_mrsw, myextra_node, key, cur_hash,
                                   slot, mykey_cmp)

RIX_HASH_MRMW_HEAD(myextra_mrmw);
RIX_HASH_MRMW_GENERATE_SLOT_EXTRA(myextra_mrmw, myextra_node, key, cur_hash,
                                   slot, mykey_cmp)

#define NB_BASIC    20u
#define NB_BK_BASIC  4u

static struct mynode g_basic[NB_BASIC];
static struct rix_hash_bucket_s g_bk[NB_BK_BASIC]
    __attribute__((aligned(64)));
static struct myht_mrsw g_head;

#define CTL_NODES 64u
#define CTL_NB_BK  4u

/* Shared MRMW kickout scratch buffer.  Sized for the largest MRMW table used
 * in this file; all MRMW tables attach this scratch after init.  Tests do not
 * run concurrent slow paths on different tables. */
#define MRMW_SCRATCH_MAX_NB 16384u
static unsigned
g_mrmw_scratch[RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(MRMW_SCRATCH_MAX_NB)];

static struct ctl_node g_ctl[CTL_NODES];
static struct rix_hash_bucket_s g_ctl_bk[CTL_NB_BK]
    __attribute__((aligned(64)));
static struct ctlht g_ctl_head;
static struct ctl_node g_ctl_mrmw[CTL_NODES];
static struct rix_hash_bucket_s g_ctl_mrmw_bk[CTL_NB_BK]
    __attribute__((aligned(64)));
static struct ctlht_mrmw g_ctl_mrmw_head;
static struct ctl_slot_node g_ctl_slot_mrmw[CTL_NODES];
static struct rix_hash_bucket_s g_ctl_slot_mrmw_bk[CTL_NB_BK]
    __attribute__((aligned(64)));
static struct ctlslot_mrmw g_ctl_slot_mrmw_head;
static struct ctl_keyonly_node g_ctl_ko_mrmw[CTL_NODES];
static struct rix_hash_bucket_s g_ctl_ko_mrmw_bk[CTL_NB_BK]
    __attribute__((aligned(64)));
static struct ctlko_mrmw g_ctl_ko_mrmw_head;
static struct ctl_extra_node g_ctl_extra_mrmw[CTL_NODES];
static struct rix_hash_bucket_extra_s g_ctl_extra_mrmw_bk[CTL_NB_BK]
    __attribute__((aligned(64)));
static struct ctlextra_mrmw g_ctl_extra_mrmw_head;

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
    _Atomic int done;
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
    atomic_store_explicit(&a->done, 1, memory_order_release);
    return NULL;
}

static void *
ctl_mrmw_insert_thread(void *arg)
{
    struct ctl_op_arg *a = (struct ctl_op_arg *)arg;
    a->ret = ctlht_mrmw_insert(&g_ctl_mrmw_head, g_ctl_mrmw_bk,
                               g_ctl_mrmw, a->node);
    atomic_store_explicit(&a->done, 1, memory_order_release);
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

static void
ctl_mrmw_init(void)
{
    memset(g_ctl_mrmw, 0, sizeof(g_ctl_mrmw));
    ctlht_mrmw_init(&g_ctl_mrmw_head, g_ctl_mrmw_bk, CTL_NB_BK);
    ctlht_mrmw_attach_kickout_scratch(&g_ctl_mrmw_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < CTL_NODES; i++)
        g_ctl_mrmw[i].key.id = i + 1u;

    memset(g_ctl_slot_mrmw, 0, sizeof(g_ctl_slot_mrmw));
    ctlslot_mrmw_init(&g_ctl_slot_mrmw_head, g_ctl_slot_mrmw_bk, CTL_NB_BK);
    ctlslot_mrmw_attach_kickout_scratch(&g_ctl_slot_mrmw_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < CTL_NODES; i++)
        g_ctl_slot_mrmw[i].key.id = i + 1u;

    memset(g_ctl_ko_mrmw, 0, sizeof(g_ctl_ko_mrmw));
    ctlko_mrmw_init(&g_ctl_ko_mrmw_head, g_ctl_ko_mrmw_bk, CTL_NB_BK);
    ctlko_mrmw_attach_kickout_scratch(&g_ctl_ko_mrmw_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < CTL_NODES; i++)
        g_ctl_ko_mrmw[i].key.id = i + 1u;

    memset(g_ctl_extra_mrmw, 0, sizeof(g_ctl_extra_mrmw));
    ctlextra_mrmw_init(&g_ctl_extra_mrmw_head, g_ctl_extra_mrmw_bk,
                       CTL_NB_BK);
    ctlextra_mrmw_attach_kickout_scratch(&g_ctl_extra_mrmw_head,
                                         g_mrmw_scratch);
    for (unsigned i = 0u; i < CTL_NODES; i++)
        g_ctl_extra_mrmw[i].key.id = i + 1u;
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
count_idx_common(struct rix_hash_bucket_s *buckets, unsigned bk, u32 idx)
{
    unsigned count = 0u;

    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
        u32 ctrl = atomic_load_explicit(&buckets[bk].ctrl,
                                        memory_order_acquire);
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);
        u32 si = buckets[bk].idx[s];
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
    struct ctl_op_arg wa = { &g_ctl[30], NULL, ATOMIC_VAR_INIT(0) };
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

/* ---- U32 variant tests ---------------------------------------------- */

#define NB_U32_BASIC    300u
#define NB_BK_U32_BASIC  32u

static struct myu32_node g_u32[NB_U32_BASIC];
static struct rix_hash_bucket_s g_u32_bk[NB_BK_U32_BASIC]
    __attribute__((aligned(64)));
static struct myu32_mrsw g_u32_head;

static void
u32_init(void)
{
    memset(g_u32, 0, sizeof(g_u32));
    myu32_mrsw_init(&g_u32_head, g_u32_bk, NB_BK_U32_BASIC);
    for (unsigned i = 0u; i < NB_U32_BASIC; i++) {
        g_u32[i].key   = i + 1u;
        g_u32[i].value = i + 1000u;
    }
}

static void
test_u32_insert_find_remove(void)
{
    printf("[T] mrsw u32 insert/find/remove\n");
    u32_init();

    for (unsigned i = 0u; i < NB_U32_BASIC; i++) {
        struct myu32_node *r =
            myu32_mrsw_insert(&g_u32_head, g_u32_bk, g_u32, &g_u32[i]);
        if (r != NULL)
            FAILF("u32 insert[%u] failed (ret=%p)", i, (void *)r);
    }
    for (unsigned i = 0u; i < NB_U32_BASIC; i++) {
        struct myu32_node *f = myu32_mrsw_find(&g_u32_head, g_u32_bk,
                                               g_u32, g_u32[i].key);
        if (f != &g_u32[i])
            FAILF("u32 find[%u] mismatch", i);
    }
    if (myu32_mrsw_find(&g_u32_head, g_u32_bk, g_u32, 99999u) != NULL)
        FAIL("u32 find absent returned non-NULL");

    for (unsigned i = 0u; i < NB_U32_BASIC; i += 3u) {
        struct myu32_node *r = myu32_mrsw_remove(&g_u32_head, g_u32_bk,
                                                 g_u32, &g_u32[i]);
        if (r != &g_u32[i])
            FAILF("u32 remove[%u] failed", i);
    }
    for (unsigned i = 0u; i < NB_U32_BASIC; i++) {
        struct myu32_node *f = myu32_mrsw_find(&g_u32_head, g_u32_bk,
                                               g_u32, g_u32[i].key);
        if ((i % 3u) == 0u) {
            if (f != NULL)
                FAILF("u32 removed[%u] still found", i);
        } else if (f != &g_u32[i]) {
            FAILF("u32 remaining[%u] mismatch", i);
        }
    }
}

static void
test_u32_duplicate(void)
{
    printf("[T] mrsw u32 duplicate\n");
    u32_init();

    if (myu32_mrsw_insert(&g_u32_head, g_u32_bk, g_u32, &g_u32[0]) != NULL)
        FAIL("u32 first insert returned non-NULL");
    if (myu32_mrsw_insert(&g_u32_head, g_u32_bk, g_u32, &g_u32[0])
        != &g_u32[0])
        FAIL("u32 same-node duplicate did not return existing");

    struct myu32_node dup;
    memset(&dup, 0, sizeof(dup));
    dup.key = g_u32[0].key;
    if (myu32_mrsw_insert(&g_u32_head, g_u32_bk, g_u32, &dup) != &g_u32[0])
        FAIL("u32 same-key duplicate did not return existing");
    if (atomic_load_explicit(&g_u32_head.rhh_nb, memory_order_relaxed) != 1u)
        FAIL("u32 duplicate changed count");
}

static void
test_u32_staged_remove_at(void)
{
    printf("[T] mrsw u32 staged/remove_at API\n");
    u32_init();

    for (unsigned i = 0u; i < NB_U32_BASIC; i++) {
        if (myu32_mrsw_insert(&g_u32_head, g_u32_bk, g_u32, &g_u32[i]) != NULL)
            FAILF("u32 staged setup insert[%u] failed", i);
    }

    struct rix_hash_mrsw_u32_find_ctx_s ctx[4];
    u32 keys[4] = {
        g_u32[1].key, g_u32[7].key, 999999u, g_u32[13].key
    };
    struct myu32_node *res[4];
    RIX_HASH_MRSW_HASH_KEY_N_MASKED(myu32_mrsw, ctx, 4u, &g_u32_head,
                                    g_u32_bk, keys, g_u32_head.rhh_mask,
                                    g_u32_head.rhh_mask);
    RIX_HASH_MRSW_SCAN_BK_N(myu32_mrsw, ctx, 4u, &g_u32_head, g_u32_bk);
    RIX_HASH_MRSW_PREFETCH_NODE_N(myu32_mrsw, ctx, 4u, g_u32);
    RIX_HASH_MRSW_CMP_KEY_N(myu32_mrsw, ctx, 4u, g_u32, res);
    if (res[0] != &g_u32[1] || res[1] != &g_u32[7] ||
        res[2] != NULL || res[3] != &g_u32[13])
        FAIL("u32 staged N mismatch");

    unsigned bk;
    unsigned slot;
    if (!locate_idx_common(g_u32_bk, g_u32_head.rhh_mask, 8u, &bk, &slot))
        FAIL("u32 remove_at target not located");
    if (RIX_HASH_MRSW_REMOVE_AT(myu32_mrsw, &g_u32_head, g_u32_bk, bk, slot)
        != 8u)
        FAIL("u32 remove_at returned wrong idx");
    if (myu32_mrsw_find(&g_u32_head, g_u32_bk, g_u32, g_u32[7].key) != NULL)
        FAIL("u32 remove_at target still found");
}

static _Atomic int g_u32_stop;
static _Atomic int g_u32_fail;

static void *
u32_stress_reader(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(0x9e3779b9u ^ (tid * 2654435761u));
    while (!atomic_load_explicit(&g_u32_stop, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        unsigned i = x % NB_U32_BASIC;
        struct myu32_node *f = myu32_mrsw_find(&g_u32_head, g_u32_bk,
                                               g_u32, g_u32[i].key);
        if (f != NULL && f != &g_u32[i]) {
            atomic_store_explicit(&g_u32_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_u32_stress(void)
{
    printf("[T] mrsw u32 reader/writer stress\n");
    u32_init();
    for (unsigned i = 0u; i < 200u; i++) {
        if (myu32_mrsw_insert(&g_u32_head, g_u32_bk, g_u32, &g_u32[i]) != NULL)
            FAILF("u32 stress setup insert[%u] failed", i);
    }
    atomic_store_explicit(&g_u32_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_u32_fail, 0, memory_order_relaxed);
    enum { NR = 4 };
    pthread_t readers[NR];
    for (uintptr_t i = 0u; i < NR; i++)
        if (pthread_create(&readers[i], NULL, u32_stress_reader,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create u32 reader failed");

    for (unsigned iter = 0u; iter < 50000u; iter++) {
        unsigned i = 200u + (iter % 64u);
        struct myu32_node *r = myu32_mrsw_find(&g_u32_head, g_u32_bk,
                                               g_u32, g_u32[i].key);
        if (r == NULL) {
            if (myu32_mrsw_insert(&g_u32_head, g_u32_bk, g_u32,
                                  &g_u32[i]) != NULL)
                FAILF("u32 stress insert[%u] failed", i);
        } else {
            if (myu32_mrsw_remove(&g_u32_head, g_u32_bk, g_u32,
                                  &g_u32[i]) != &g_u32[i])
                FAILF("u32 stress remove[%u] failed", i);
        }
        if (atomic_load_explicit(&g_u32_fail, memory_order_acquire))
            break;
    }
    atomic_store_explicit(&g_u32_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < NR; i++)
        pthread_join(readers[i], NULL);
    if (atomic_load_explicit(&g_u32_fail, memory_order_acquire))
        FAIL("u32 stress reader observed wrong node");
}

/* ---- U64 variant tests ---------------------------------------------- */

#define NB_U64_BASIC    300u
#define NB_BK_U64_BASIC  32u

static struct myu64_node g_u64[NB_U64_BASIC];
static struct rix_hash64_bucket_s g_u64_bk[NB_BK_U64_BASIC]
    __attribute__((aligned(64)));
static struct myu64_mrsw g_u64_head;

static int
locate_idx_u64(struct rix_hash64_bucket_s *buckets, unsigned mask,
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
u64_init(void)
{
    memset(g_u64, 0, sizeof(g_u64));
    myu64_mrsw_init(&g_u64_head, g_u64_bk, NB_BK_U64_BASIC);
    for (unsigned i = 0u; i < NB_U64_BASIC; i++) {
        g_u64[i].key   = UINT64_C(0xDEADBEEF00000000) | (u64)(i + 1u);
        g_u64[i].value = i + 1000u;
    }
}

static void
test_u64_insert_find_remove(void)
{
    printf("[T] mrsw u64 insert/find/remove\n");
    u64_init();

    for (unsigned i = 0u; i < NB_U64_BASIC; i++) {
        struct myu64_node *r =
            myu64_mrsw_insert(&g_u64_head, g_u64_bk, g_u64, &g_u64[i]);
        if (r != NULL)
            FAILF("u64 insert[%u] failed (ret=%p)", i, (void *)r);
    }
    for (unsigned i = 0u; i < NB_U64_BASIC; i++) {
        struct myu64_node *f = myu64_mrsw_find(&g_u64_head, g_u64_bk,
                                               g_u64, g_u64[i].key);
        if (f != &g_u64[i])
            FAILF("u64 find[%u] mismatch", i);
    }
    if (myu64_mrsw_find(&g_u64_head, g_u64_bk, g_u64,
                        UINT64_C(0xC0FFEE)) != NULL)
        FAIL("u64 find absent returned non-NULL");

    for (unsigned i = 0u; i < NB_U64_BASIC; i += 3u) {
        struct myu64_node *r = myu64_mrsw_remove(&g_u64_head, g_u64_bk,
                                                 g_u64, &g_u64[i]);
        if (r != &g_u64[i])
            FAILF("u64 remove[%u] failed", i);
    }
    for (unsigned i = 0u; i < NB_U64_BASIC; i++) {
        struct myu64_node *f = myu64_mrsw_find(&g_u64_head, g_u64_bk,
                                               g_u64, g_u64[i].key);
        if ((i % 3u) == 0u) {
            if (f != NULL)
                FAILF("u64 removed[%u] still found", i);
        } else if (f != &g_u64[i]) {
            FAILF("u64 remaining[%u] mismatch", i);
        }
    }
}

static void
test_u64_duplicate(void)
{
    printf("[T] mrsw u64 duplicate\n");
    u64_init();

    if (myu64_mrsw_insert(&g_u64_head, g_u64_bk, g_u64, &g_u64[0]) != NULL)
        FAIL("u64 first insert returned non-NULL");
    if (myu64_mrsw_insert(&g_u64_head, g_u64_bk, g_u64, &g_u64[0])
        != &g_u64[0])
        FAIL("u64 same-node duplicate did not return existing");

    struct myu64_node dup;
    memset(&dup, 0, sizeof(dup));
    dup.key = g_u64[0].key;
    if (myu64_mrsw_insert(&g_u64_head, g_u64_bk, g_u64, &dup) != &g_u64[0])
        FAIL("u64 same-key duplicate did not return existing");
    if (atomic_load_explicit(&g_u64_head.rhh_nb, memory_order_relaxed) != 1u)
        FAIL("u64 duplicate changed count");
}

static void
test_u64_staged_remove_at(void)
{
    printf("[T] mrsw u64 staged/remove_at API\n");
    u64_init();

    for (unsigned i = 0u; i < NB_U64_BASIC; i++) {
        if (myu64_mrsw_insert(&g_u64_head, g_u64_bk, g_u64, &g_u64[i]) != NULL)
            FAILF("u64 staged setup insert[%u] failed", i);
    }

    struct rix_hash_mrsw_u64_find_ctx_s ctx[4];
    u64 keys[4] = {
        g_u64[1].key, g_u64[7].key, UINT64_C(0xC0FFEE), g_u64[13].key
    };
    struct myu64_node *res[4];
    RIX_HASH_MRSW_HASH_KEY_N_MASKED(myu64_mrsw, ctx, 4u, &g_u64_head,
                                    g_u64_bk, keys, g_u64_head.rhh_mask,
                                    g_u64_head.rhh_mask);
    RIX_HASH_MRSW_SCAN_BK_N(myu64_mrsw, ctx, 4u, &g_u64_head, g_u64_bk);
    RIX_HASH_MRSW_PREFETCH_NODE_N(myu64_mrsw, ctx, 4u, g_u64);
    RIX_HASH_MRSW_CMP_KEY_N(myu64_mrsw, ctx, 4u, g_u64, res);
    if (res[0] != &g_u64[1] || res[1] != &g_u64[7] ||
        res[2] != NULL || res[3] != &g_u64[13])
        FAIL("u64 staged N mismatch");

    unsigned bk;
    unsigned slot;
    if (!locate_idx_u64(g_u64_bk, g_u64_head.rhh_mask, 8u, &bk, &slot))
        FAIL("u64 remove_at target not located");
    if (RIX_HASH_MRSW_REMOVE_AT(myu64_mrsw, &g_u64_head, g_u64_bk, bk, slot)
        != 8u)
        FAIL("u64 remove_at returned wrong idx");
    if (myu64_mrsw_find(&g_u64_head, g_u64_bk, g_u64, g_u64[7].key) != NULL)
        FAIL("u64 remove_at target still found");
}

static _Atomic int g_u64_stop;
static _Atomic int g_u64_fail;

static void *
u64_stress_reader(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(0x9e3779b9u ^ (tid * 2654435761u));
    while (!atomic_load_explicit(&g_u64_stop, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        unsigned i = x % NB_U64_BASIC;
        struct myu64_node *f = myu64_mrsw_find(&g_u64_head, g_u64_bk,
                                               g_u64, g_u64[i].key);
        if (f != NULL && f != &g_u64[i]) {
            atomic_store_explicit(&g_u64_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_u64_stress(void)
{
    printf("[T] mrsw u64 reader/writer stress\n");
    u64_init();
    for (unsigned i = 0u; i < 200u; i++) {
        if (myu64_mrsw_insert(&g_u64_head, g_u64_bk, g_u64, &g_u64[i]) != NULL)
            FAILF("u64 stress setup insert[%u] failed", i);
    }
    atomic_store_explicit(&g_u64_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_u64_fail, 0, memory_order_relaxed);
    enum { NR = 4 };
    pthread_t readers[NR];
    for (uintptr_t i = 0u; i < NR; i++)
        if (pthread_create(&readers[i], NULL, u64_stress_reader,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create u64 reader failed");

    for (unsigned iter = 0u; iter < 50000u; iter++) {
        unsigned i = 200u + (iter % 64u);
        struct myu64_node *r = myu64_mrsw_find(&g_u64_head, g_u64_bk,
                                               g_u64, g_u64[i].key);
        if (r == NULL) {
            if (myu64_mrsw_insert(&g_u64_head, g_u64_bk, g_u64,
                                  &g_u64[i]) != NULL)
                FAILF("u64 stress insert[%u] failed", i);
        } else {
            if (myu64_mrsw_remove(&g_u64_head, g_u64_bk, g_u64,
                                  &g_u64[i]) != &g_u64[i])
                FAILF("u64 stress remove[%u] failed", i);
        }
        if (atomic_load_explicit(&g_u64_fail, memory_order_acquire))
            break;
    }
    atomic_store_explicit(&g_u64_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < NR; i++)
        pthread_join(readers[i], NULL);
    if (atomic_load_explicit(&g_u64_fail, memory_order_acquire))
        FAIL("u64 stress reader observed wrong node");
}

/* ---- SLOT_EXTRA variant tests --------------------------------------- */

#define NB_EXTRA_BASIC    300u
#define NB_BK_EXTRA_BASIC  32u

static struct myextra_node g_xn[NB_EXTRA_BASIC];
static struct rix_hash_bucket_extra_s g_xn_bk[NB_BK_EXTRA_BASIC]
    __attribute__((aligned(64)));
static struct myextra_mrsw g_xn_head;

static int
locate_idx_extra(struct rix_hash_bucket_extra_s *buckets, unsigned mask,
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
xn_init(void)
{
    memset(g_xn, 0, sizeof(g_xn));
    myextra_mrsw_init(&g_xn_head, g_xn_bk, NB_BK_EXTRA_BASIC);
    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i++) {
        g_xn[i].key.hi = (u64)(i + 1u);
        g_xn[i].key.lo = UINT64_C(0xEEEE000000000000) ^ (u64)i;
    }
}

static void
test_extra_insert_find_remove(void)
{
    printf("[T] mrsw slot_extra insert/find/remove\n");
    xn_init();

    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i++) {
        u32 extra = 0xAA00u + i;
        struct myextra_node *r =
            myextra_mrsw_insert(&g_xn_head, g_xn_bk, g_xn, &g_xn[i], extra);
        if (r != NULL)
            FAILF("extra insert[%u] failed", i);
    }
    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i++) {
        struct myextra_node *f = myextra_mrsw_find(&g_xn_head, g_xn_bk,
                                                   g_xn, &g_xn[i].key);
        if (f != &g_xn[i])
            FAILF("extra find[%u] mismatch", i);
        unsigned bk = (unsigned)(f->cur_hash & g_xn_head.rhh_mask);
        unsigned slot = (unsigned)f->slot;
        if (g_xn_bk[bk].extra[slot] != (u32)(0xAA00u + i))
            FAILF("extra[%u] mismatch: bk=%u slot=%u got=0x%x",
                  i, bk, slot, g_xn_bk[bk].extra[slot]);
    }
    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i += 3u) {
        struct myextra_node *r = myextra_mrsw_remove(&g_xn_head, g_xn_bk,
                                                     g_xn, &g_xn[i]);
        if (r != &g_xn[i])
            FAILF("extra remove[%u] failed", i);
    }
    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i++) {
        struct myextra_node *f = myextra_mrsw_find(&g_xn_head, g_xn_bk,
                                                   g_xn, &g_xn[i].key);
        if ((i % 3u) == 0u) {
            if (f != NULL)
                FAILF("extra removed[%u] still found", i);
        } else if (f != &g_xn[i]) {
            FAILF("extra remaining[%u] mismatch", i);
        }
    }
}

static void
test_extra_kickout_carries_extra(void)
{
    printf("[T] mrsw slot_extra extra survives kickout\n");
    xn_init();

    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i++) {
        u32 extra = 0xBEEF0000u | i;
        if (myextra_mrsw_insert(&g_xn_head, g_xn_bk, g_xn, &g_xn[i], extra)
            != NULL)
            FAILF("extra kickout setup insert[%u] failed", i);
    }
    /* Every entry should still report its original extra value, even after
     * any kickout reshuffles. */
    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i++) {
        unsigned bk = (unsigned)(g_xn[i].cur_hash & g_xn_head.rhh_mask);
        unsigned slot = (unsigned)g_xn[i].slot;
        u32 expected = 0xBEEF0000u | i;
        if (g_xn_bk[bk].extra[slot] != expected)
            FAILF("extra[%u] kickout-lost: bk=%u slot=%u got=0x%x exp=0x%x",
                  i, bk, slot, g_xn_bk[bk].extra[slot], expected);
    }
}

static void
test_extra_staged_remove_at(void)
{
    printf("[T] mrsw slot_extra staged/remove_at API\n");
    xn_init();

    for (unsigned i = 0u; i < NB_EXTRA_BASIC; i++) {
        if (myextra_mrsw_insert(&g_xn_head, g_xn_bk, g_xn, &g_xn[i],
                                0xCC000000u | i) != NULL)
            FAILF("extra staged setup insert[%u] failed", i);
    }

    struct rix_hash_mrsw_find_ctx_s ctx[4];
    struct mykey bad = { 999999u, UINT64_C(0xEEEEDEAD00000000) };
    const struct mykey *keys[4] = {
        &g_xn[1].key, &g_xn[7].key, &bad, &g_xn[13].key
    };
    struct myextra_node *res[4];
    RIX_HASH_MRSW_HASH_KEY_N_MASKED(myextra_mrsw, ctx, 4u, &g_xn_head,
                                    g_xn_bk, keys, g_xn_head.rhh_mask,
                                    g_xn_head.rhh_mask);
    RIX_HASH_MRSW_SCAN_BK_N(myextra_mrsw, ctx, 4u, &g_xn_head, g_xn_bk);
    RIX_HASH_MRSW_PREFETCH_NODE_N(myextra_mrsw, ctx, 4u, g_xn);
    RIX_HASH_MRSW_CMP_KEY_N(myextra_mrsw, ctx, 4u, g_xn, res);
    if (res[0] != &g_xn[1] || res[1] != &g_xn[7] ||
        res[2] != NULL || res[3] != &g_xn[13])
        FAIL("extra staged N mismatch");

    unsigned bk;
    unsigned slot;
    if (!locate_idx_extra(g_xn_bk, g_xn_head.rhh_mask, 8u, &bk, &slot))
        FAIL("extra remove_at target not located");
    if (RIX_HASH_MRSW_REMOVE_AT(myextra_mrsw, &g_xn_head, g_xn_bk, bk, slot)
        != 8u)
        FAIL("extra remove_at returned wrong idx");
    if (myextra_mrsw_find(&g_xn_head, g_xn_bk, g_xn, &g_xn[7].key) != NULL)
        FAIL("extra remove_at target still found");
}

static _Atomic int g_xn_stop;
static _Atomic int g_xn_fail;

static void *
xn_stress_reader(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(0x9e3779b9u ^ (tid * 2654435761u));
    while (!atomic_load_explicit(&g_xn_stop, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        unsigned i = x % NB_EXTRA_BASIC;
        struct myextra_node *f = myextra_mrsw_find(&g_xn_head, g_xn_bk,
                                                   g_xn, &g_xn[i].key);
        if (f != NULL && f != &g_xn[i]) {
            atomic_store_explicit(&g_xn_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_extra_stress(void)
{
    printf("[T] mrsw slot_extra reader/writer stress\n");
    xn_init();
    for (unsigned i = 0u; i < 200u; i++) {
        if (myextra_mrsw_insert(&g_xn_head, g_xn_bk, g_xn, &g_xn[i],
                                0xCAFE0000u | i) != NULL)
            FAILF("extra stress setup insert[%u] failed", i);
    }
    atomic_store_explicit(&g_xn_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_xn_fail, 0, memory_order_relaxed);
    enum { NR = 4 };
    pthread_t readers[NR];
    for (uintptr_t i = 0u; i < NR; i++)
        if (pthread_create(&readers[i], NULL, xn_stress_reader,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create extra reader failed");
    for (unsigned iter = 0u; iter < 50000u; iter++) {
        unsigned i = 200u + (iter % 64u);
        struct myextra_node *r = myextra_mrsw_find(&g_xn_head, g_xn_bk,
                                                   g_xn, &g_xn[i].key);
        if (r == NULL) {
            if (myextra_mrsw_insert(&g_xn_head, g_xn_bk, g_xn, &g_xn[i],
                                    iter) != NULL)
                FAILF("extra stress insert[%u] failed", i);
        } else {
            if (myextra_mrsw_remove(&g_xn_head, g_xn_bk, g_xn,
                                    &g_xn[i]) != &g_xn[i])
                FAILF("extra stress remove[%u] failed", i);
        }
        if (atomic_load_explicit(&g_xn_fail, memory_order_acquire))
            break;
    }
    atomic_store_explicit(&g_xn_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < NR; i++)
        pthread_join(readers[i], NULL);
    if (atomic_load_explicit(&g_xn_fail, memory_order_acquire))
        FAIL("extra stress reader observed wrong node");
}

#define MRMW_N       1024u
#define MRMW_NB_BK   1024u
#define MRMW_WRITERS    4u
#define MRMW_READERS    4u

static struct mynode g_mrmw[MRMW_N];
static struct rix_hash_bucket_s g_mrmw_bk[MRMW_NB_BK]
    __attribute__((aligned(64)));
static struct myht_mrmw g_mrmw_head;
static struct myslot_node g_mrmw_slot[MRMW_N];
static struct rix_hash_bucket_s g_mrmw_slot_bk[MRMW_NB_BK]
    __attribute__((aligned(64)));
static struct myslot_mrmw g_mrmw_slot_head;
static struct mykeyonly_node g_mrmw_ko[MRMW_N];
static struct rix_hash_bucket_s g_mrmw_ko_bk[MRMW_NB_BK]
    __attribute__((aligned(64)));
static struct mykeyonly_mrmw g_mrmw_ko_head;
static struct myu32_node g_mrmw_u32[MRMW_N];
static struct rix_hash_bucket_s g_mrmw_u32_bk[MRMW_NB_BK]
    __attribute__((aligned(64)));
static struct myu32_mrmw g_mrmw_u32_head;
static struct myu64_node g_mrmw_u64[MRMW_N];
static struct rix_hash64_bucket_s g_mrmw_u64_bk[MRMW_NB_BK]
    __attribute__((aligned(64)));
static struct myu64_mrmw g_mrmw_u64_head;
static struct myextra_node g_mrmw_xn[MRMW_N];
static struct rix_hash_bucket_extra_s g_mrmw_xn_bk[MRMW_NB_BK]
    __attribute__((aligned(64)));
static struct myextra_mrmw g_mrmw_xn_head;
RIX_STATIC_ASSERT(MRMW_NB_BK <= MRMW_SCRATCH_MAX_NB,
                  "MRMW_NB_BK must not exceed shared scratch capacity");
static _Atomic int g_mrmw_start;
static _Atomic int g_mrmw_stop;
static _Atomic int g_mrmw_fail;

/* Concurrent insert+remove churn.  Each writer repeats { insert my keys,
 * remove my keys }.  Multiple writers running in parallel keep the table at
 * a high fill level so insert_slow runs often, and a slow-path move from one
 * writer may relocate another writer's entries between its insert and remove.
 * If remove uses stale hash_field/slot_field to locate the entry, remove
 * fails and the test trips g_mrmw_fail. */
#define MRMW_CHURN_NB_BK    16384u
#define MRMW_CHURN_PER      49152u
#define MRMW_CHURN_WRITERS  4u
#define MRMW_CHURN_N       (MRMW_CHURN_PER * MRMW_CHURN_WRITERS)
#define MRMW_CHURN_ITERS    4u

static struct mynode g_mrmw_churn[MRMW_CHURN_N];
static struct rix_hash_bucket_s g_mrmw_churn_bk[MRMW_CHURN_NB_BK]
    __attribute__((aligned(64)));
static struct myht_mrmw g_mrmw_churn_head;

static struct myslot_node g_mrmw_churn_slot[MRMW_CHURN_N];
static struct rix_hash_bucket_s g_mrmw_churn_slot_bk[MRMW_CHURN_NB_BK]
    __attribute__((aligned(64)));
static struct myslot_mrmw g_mrmw_churn_slot_head;

static struct myextra_node g_mrmw_churn_xn[MRMW_CHURN_N];
static struct rix_hash_bucket_extra_s g_mrmw_churn_xn_bk[MRMW_CHURN_NB_BK]
    __attribute__((aligned(64)));
static struct myextra_mrmw g_mrmw_churn_xn_head;

static struct myu32_node g_mrmw_churn_u32[MRMW_CHURN_N];
static struct rix_hash_bucket_s g_mrmw_churn_u32_bk[MRMW_CHURN_NB_BK]
    __attribute__((aligned(64)));
static struct myu32_mrmw g_mrmw_churn_u32_head;

static struct myu64_node g_mrmw_churn_u64[MRMW_CHURN_N];
static struct rix_hash64_bucket_s g_mrmw_churn_u64_bk[MRMW_CHURN_NB_BK]
    __attribute__((aligned(64)));
static struct myu64_mrmw g_mrmw_churn_u64_head;

static void
mrmw_init(void)
{
    memset(g_mrmw, 0, sizeof(g_mrmw));
    myht_mrmw_init(&g_mrmw_head, g_mrmw_bk, MRMW_NB_BK);
    myht_mrmw_attach_kickout_scratch(&g_mrmw_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_N; i++) {
        g_mrmw[i].key.hi = UINT64_C(0xABC0000000000000) | (u64)i;
        g_mrmw[i].key.lo = UINT64_C(0x1234000000000000) ^ (u64)(i * 17u);
        g_mrmw[i].value = i;
    }
}

static void
mrmw_slot_init(void)
{
    memset(g_mrmw_slot, 0, sizeof(g_mrmw_slot));
    myslot_mrmw_init(&g_mrmw_slot_head, g_mrmw_slot_bk, MRMW_NB_BK);
    myslot_mrmw_attach_kickout_scratch(&g_mrmw_slot_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_N; i++) {
        g_mrmw_slot[i].key.hi = UINT64_C(0xABC1000000000000) | (u64)i;
        g_mrmw_slot[i].key.lo = UINT64_C(0x1235000000000000) ^ (u64)(i * 19u);
        g_mrmw_slot[i].value = i;
    }
}

static void
mrmw_ko_init(void)
{
    memset(g_mrmw_ko, 0, sizeof(g_mrmw_ko));
    mykeyonly_mrmw_init(&g_mrmw_ko_head, g_mrmw_ko_bk, MRMW_NB_BK);
    mykeyonly_mrmw_attach_kickout_scratch(&g_mrmw_ko_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_N; i++) {
        g_mrmw_ko[i].key.hi = UINT64_C(0xABC2000000000000) | (u64)i;
        g_mrmw_ko[i].key.lo = UINT64_C(0x1236000000000000) ^ (u64)(i * 23u);
        g_mrmw_ko[i].value = i;
    }
}

static void
mrmw_u32_init(void)
{
    memset(g_mrmw_u32, 0, sizeof(g_mrmw_u32));
    myu32_mrmw_init(&g_mrmw_u32_head, g_mrmw_u32_bk, MRMW_NB_BK);
    myu32_mrmw_attach_kickout_scratch(&g_mrmw_u32_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_N; i++) {
        g_mrmw_u32[i].key = i + 1u;
        g_mrmw_u32[i].value = i + 3000u;
    }
}

static void
mrmw_u64_init(void)
{
    memset(g_mrmw_u64, 0, sizeof(g_mrmw_u64));
    myu64_mrmw_init(&g_mrmw_u64_head, g_mrmw_u64_bk, MRMW_NB_BK);
    myu64_mrmw_attach_kickout_scratch(&g_mrmw_u64_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_N; i++) {
        g_mrmw_u64[i].key = UINT64_C(0xDADA000000000000) | (u64)(i + 1u);
        g_mrmw_u64[i].value = i + 4000u;
    }
}

static void
mrmw_xn_init(void)
{
    memset(g_mrmw_xn, 0, sizeof(g_mrmw_xn));
    myextra_mrmw_init(&g_mrmw_xn_head, g_mrmw_xn_bk, MRMW_NB_BK);
    myextra_mrmw_attach_kickout_scratch(&g_mrmw_xn_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_N; i++) {
        g_mrmw_xn[i].key.hi = UINT64_C(0xABC3000000000000) | (u64)i;
        g_mrmw_xn[i].key.lo = UINT64_C(0x1237000000000000) ^ (u64)(i * 29u);
    }
}

struct mrmw_worker_arg {
    unsigned begin;
    unsigned end;
};

static void
mrmw_wait_start(void)
{
    while (!atomic_load_explicit(&g_mrmw_start, memory_order_acquire))
        sched_yield();
}

static void *
mrmw_insert_worker(void *arg)
{
    struct mrmw_worker_arg *a = (struct mrmw_worker_arg *)arg;

    mrmw_wait_start();
    for (unsigned i = a->begin; i < a->end; i++) {
        struct mynode *ret =
            myht_mrmw_insert(&g_mrmw_head, g_mrmw_bk, g_mrmw, &g_mrmw[i]);
        if (ret != NULL) {
            atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void *
mrmw_remove_worker(void *arg)
{
    struct mrmw_worker_arg *a = (struct mrmw_worker_arg *)arg;

    mrmw_wait_start();
    for (unsigned i = a->begin; i < a->end; i++) {
        struct mynode *ret =
            myht_mrmw_remove(&g_mrmw_head, g_mrmw_bk, g_mrmw, &g_mrmw[i]);
        if (ret != &g_mrmw[i]) {
            atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void *
mrmw_reader_worker(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    u32 x = (u32)(0x9e3779b9u ^ (tid * 2654435761u));

    mrmw_wait_start();
    while (!atomic_load_explicit(&g_mrmw_stop, memory_order_acquire)) {
        x = x * 1664525u + 1013904223u;
        unsigned i = x & (MRMW_N - 1u);
        struct mynode *ret =
            myht_mrmw_find(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                           &g_mrmw[i].key);
        if (ret != NULL && ret != &g_mrmw[i]) {
            atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
            return NULL;
        }
    }
    return NULL;
}

static void
test_mrmw_insert_find_remove(void)
{
    printf("[T] mrmw fp insert/find/remove\n");
    mrmw_init();

    for (unsigned i = 0u; i < 32u; i++) {
        if (myht_mrmw_insert(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                             &g_mrmw[i]) != NULL)
            FAILF("mrmw basic insert[%u] failed", i);
    }
    for (unsigned i = 0u; i < 32u; i++) {
        if (myht_mrmw_find(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                           &g_mrmw[i].key) != &g_mrmw[i])
            FAILF("mrmw basic find[%u] failed", i);
    }
    for (unsigned i = 0u; i < 32u; i += 2u) {
        if (myht_mrmw_remove(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                             &g_mrmw[i]) != &g_mrmw[i])
            FAILF("mrmw basic remove[%u] failed", i);
    }
    for (unsigned i = 0u; i < 32u; i++) {
        struct mynode *ret =
            myht_mrmw_find(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                           &g_mrmw[i].key);
        if (((i & 1u) == 0u && ret != NULL) ||
            ((i & 1u) != 0u && ret != &g_mrmw[i]))
            FAILF("mrmw post-remove find[%u] mismatch", i);
    }
}

static void
test_mrmw_duplicate_remove_at(void)
{
    printf("[T] mrmw fp duplicate/remove_at\n");
    mrmw_init();

    if (myht_mrmw_insert(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                         &g_mrmw[7]) != NULL)
        FAIL("mrmw duplicate first insert returned non-NULL");
    if (myht_mrmw_insert(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                         &g_mrmw[7]) != &g_mrmw[7])
        FAIL("mrmw duplicate same-node did not return existing");

    struct mynode dup;
    memset(&dup, 0, sizeof(dup));
    dup.key = g_mrmw[7].key;
    if (myht_mrmw_insert(&g_mrmw_head, g_mrmw_bk, g_mrmw, &dup)
        != &g_mrmw[7])
        FAIL("mrmw duplicate same-key did not return existing");
    if (atomic_load_explicit(&g_mrmw_head.rhh_nb, memory_order_relaxed) != 1u)
        FAIL("mrmw duplicate changed count");

    unsigned bk;
    unsigned slot;
    if (!locate_idx_common(g_mrmw_bk, g_mrmw_head.rhh_mask, 8u, &bk, &slot))
        FAIL("mrmw remove_at target not located");
    if (RIX_HASH_MRMW_REMOVE_AT(myht_mrmw, &g_mrmw_head, g_mrmw_bk, bk, slot)
        != 8u)
        FAIL("mrmw remove_at returned wrong idx");
    if (myht_mrmw_find(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                       &g_mrmw[7].key) != NULL)
        FAIL("mrmw remove_at target still found");
}

static void
test_mrmw_staged_api(void)
{
    printf("[T] mrmw fp staged API\n");
    mrmw_init();

    for (unsigned i = 0u; i < 16u; i++) {
        if (RIX_HASH_MRMW_INSERT(myht_mrmw, &g_mrmw_head, g_mrmw_bk,
                                 g_mrmw, &g_mrmw[i]) != NULL)
            FAILF("mrmw staged setup insert[%u] failed", i);
    }

    struct rix_hash_mrsw_find_ctx_s ctx[4];
    const struct mykey *keys[4] = {
        &g_mrmw[1].key, &g_mrmw[3].key, &g_mrmw[5].key, &g_mrmw[7].key
    };
    struct mynode *res[4];

    RIX_HASH_MRMW_HASH_KEY_N(myht_mrmw, ctx, 4u, &g_mrmw_head,
                             g_mrmw_bk, keys);
    RIX_HASH_MRMW_SCAN_BK_N(myht_mrmw, ctx, 4u, &g_mrmw_head, g_mrmw_bk);
    RIX_HASH_MRMW_PREFETCH_NODE_N(myht_mrmw, ctx, 4u, g_mrmw);
    RIX_HASH_MRMW_CMP_KEY_N(myht_mrmw, ctx, 4u, g_mrmw, res);
    if (res[0] != &g_mrmw[1] || res[1] != &g_mrmw[3] ||
        res[2] != &g_mrmw[5] || res[3] != &g_mrmw[7])
        FAIL("mrmw staged result mismatch");
}

static void
test_mrmw_slot_insert_find_remove(void)
{
    printf("[T] mrmw slot insert/find/remove\n");
    mrmw_slot_init();

    for (unsigned i = 0u; i < 64u; i++) {
        if (myslot_mrmw_insert(&g_mrmw_slot_head, g_mrmw_slot_bk,
                               g_mrmw_slot, &g_mrmw_slot[i]) != NULL)
            FAILF("mrmw slot insert[%u] failed", i);
    }
    for (unsigned i = 0u; i < 64u; i++) {
        unsigned bk = (unsigned)(g_mrmw_slot[i].cur_hash &
                                 g_mrmw_slot_head.rhh_mask);
        unsigned slot = (unsigned)g_mrmw_slot[i].slot;
        if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)
            FAILF("mrmw slot field out of range[%u]: %u", i, slot);
        if (g_mrmw_slot_bk[bk].idx[slot] != i + 1u)
            FAILF("mrmw slot field mismatch[%u]: bk=%u slot=%u idx=%u",
                  i, bk, slot, g_mrmw_slot_bk[bk].idx[slot]);
        if (myslot_mrmw_find(&g_mrmw_slot_head, g_mrmw_slot_bk,
                             g_mrmw_slot, &g_mrmw_slot[i].key)
            != &g_mrmw_slot[i])
            FAILF("mrmw slot find[%u] failed", i);
    }
    for (unsigned i = 0u; i < 64u; i += 2u) {
        if (myslot_mrmw_remove(&g_mrmw_slot_head, g_mrmw_slot_bk,
                               g_mrmw_slot, &g_mrmw_slot[i])
            != &g_mrmw_slot[i])
            FAILF("mrmw slot remove[%u] failed", i);
    }
}

static void
test_mrmw_keyonly_insert_find_remove(void)
{
    printf("[T] mrmw keyonly insert/find/remove\n");
    mrmw_ko_init();

    for (unsigned i = 0u; i < 64u; i++) {
        if (mykeyonly_mrmw_insert(&g_mrmw_ko_head, g_mrmw_ko_bk,
                                  g_mrmw_ko, &g_mrmw_ko[i]) != NULL)
            FAILF("mrmw keyonly insert[%u] failed", i);
    }
    if (mykeyonly_mrmw_insert(&g_mrmw_ko_head, g_mrmw_ko_bk,
                              g_mrmw_ko, &g_mrmw_ko[7]) != &g_mrmw_ko[7])
        FAIL("mrmw keyonly duplicate did not return existing");
    for (unsigned i = 0u; i < 64u; i++) {
        if (mykeyonly_mrmw_find(&g_mrmw_ko_head, g_mrmw_ko_bk,
                                g_mrmw_ko, &g_mrmw_ko[i].key)
            != &g_mrmw_ko[i])
            FAILF("mrmw keyonly find[%u] failed", i);
    }
    for (unsigned i = 0u; i < 64u; i += 2u) {
        if (mykeyonly_mrmw_remove(&g_mrmw_ko_head, g_mrmw_ko_bk,
                                  g_mrmw_ko, &g_mrmw_ko[i])
            != &g_mrmw_ko[i])
            FAILF("mrmw keyonly remove[%u] failed", i);
    }
}

static void
test_mrmw_u32_insert_find_remove(void)
{
    printf("[T] mrmw u32 insert/find/remove\n");
    mrmw_u32_init();

    for (unsigned i = 0u; i < 64u; i++) {
        if (myu32_mrmw_insert(&g_mrmw_u32_head, g_mrmw_u32_bk,
                              g_mrmw_u32, &g_mrmw_u32[i]) != NULL)
            FAILF("mrmw u32 insert[%u] failed", i);
    }
    if (myu32_mrmw_insert(&g_mrmw_u32_head, g_mrmw_u32_bk,
                          g_mrmw_u32, &g_mrmw_u32[7]) != &g_mrmw_u32[7])
        FAIL("mrmw u32 duplicate did not return existing");

    struct rix_hash_mrsw_u32_find_ctx_s ctx[4];
    u32 keys[4] = {
        g_mrmw_u32[1].key, g_mrmw_u32[3].key, 999999u, g_mrmw_u32[7].key
    };
    struct myu32_node *res[4];
    RIX_HASH_MRMW_HASH_KEY_N(myu32_mrmw, ctx, 4u, &g_mrmw_u32_head,
                             g_mrmw_u32_bk, keys);
    RIX_HASH_MRMW_SCAN_BK_N(myu32_mrmw, ctx, 4u, &g_mrmw_u32_head,
                            g_mrmw_u32_bk);
    RIX_HASH_MRMW_PREFETCH_NODE_N(myu32_mrmw, ctx, 4u, g_mrmw_u32);
    RIX_HASH_MRMW_CMP_KEY_N(myu32_mrmw, ctx, 4u, g_mrmw_u32, res);
    if (res[0] != &g_mrmw_u32[1] || res[1] != &g_mrmw_u32[3] ||
        res[2] != NULL || res[3] != &g_mrmw_u32[7])
        FAIL("mrmw u32 staged result mismatch");

    unsigned bk;
    unsigned slot;
    if (!locate_idx_common(g_mrmw_u32_bk, g_mrmw_u32_head.rhh_mask, 8u,
                           &bk, &slot))
        FAIL("mrmw u32 remove_at target not located");
    if (RIX_HASH_MRMW_REMOVE_AT(myu32_mrmw, &g_mrmw_u32_head,
                               g_mrmw_u32_bk, bk, slot) != 8u)
        FAIL("mrmw u32 remove_at returned wrong idx");
    if (myu32_mrmw_find(&g_mrmw_u32_head, g_mrmw_u32_bk,
                        g_mrmw_u32, g_mrmw_u32[7].key) != NULL)
        FAIL("mrmw u32 remove_at target still found");
}

static void
test_mrmw_u64_insert_find_remove(void)
{
    printf("[T] mrmw u64 insert/find/remove\n");
    mrmw_u64_init();

    for (unsigned i = 0u; i < 64u; i++) {
        if (myu64_mrmw_insert(&g_mrmw_u64_head, g_mrmw_u64_bk,
                              g_mrmw_u64, &g_mrmw_u64[i]) != NULL)
            FAILF("mrmw u64 insert[%u] failed", i);
    }
    if (myu64_mrmw_insert(&g_mrmw_u64_head, g_mrmw_u64_bk,
                          g_mrmw_u64, &g_mrmw_u64[7]) != &g_mrmw_u64[7])
        FAIL("mrmw u64 duplicate did not return existing");

    struct rix_hash_mrsw_u64_find_ctx_s ctx[4];
    u64 keys[4] = {
        g_mrmw_u64[1].key, g_mrmw_u64[3].key,
        UINT64_C(0xFACE000000000000), g_mrmw_u64[7].key
    };
    struct myu64_node *res[4];
    RIX_HASH_MRMW_HASH_KEY_N(myu64_mrmw, ctx, 4u, &g_mrmw_u64_head,
                             g_mrmw_u64_bk, keys);
    RIX_HASH_MRMW_SCAN_BK_N(myu64_mrmw, ctx, 4u, &g_mrmw_u64_head,
                            g_mrmw_u64_bk);
    RIX_HASH_MRMW_PREFETCH_NODE_N(myu64_mrmw, ctx, 4u, g_mrmw_u64);
    RIX_HASH_MRMW_CMP_KEY_N(myu64_mrmw, ctx, 4u, g_mrmw_u64, res);
    if (res[0] != &g_mrmw_u64[1] || res[1] != &g_mrmw_u64[3] ||
        res[2] != NULL || res[3] != &g_mrmw_u64[7])
        FAIL("mrmw u64 staged result mismatch");

    unsigned bk;
    unsigned slot;
    if (!locate_idx_u64(g_mrmw_u64_bk, g_mrmw_u64_head.rhh_mask, 8u,
                        &bk, &slot))
        FAIL("mrmw u64 remove_at target not located");
    if (RIX_HASH_MRMW_REMOVE_AT(myu64_mrmw, &g_mrmw_u64_head,
                               g_mrmw_u64_bk, bk, slot) != 8u)
        FAIL("mrmw u64 remove_at returned wrong idx");
    if (myu64_mrmw_find(&g_mrmw_u64_head, g_mrmw_u64_bk,
                        g_mrmw_u64, g_mrmw_u64[7].key) != NULL)
        FAIL("mrmw u64 remove_at target still found");
}

static void
test_mrmw_extra_insert_find_remove(void)
{
    printf("[T] mrmw slot_extra insert/find/remove\n");
    mrmw_xn_init();

    for (unsigned i = 0u; i < 64u; i++) {
        u32 extra = 0xDAD00000u | i;
        if (myextra_mrmw_insert(&g_mrmw_xn_head, g_mrmw_xn_bk,
                                g_mrmw_xn, &g_mrmw_xn[i], extra) != NULL)
            FAILF("mrmw extra insert[%u] failed", i);
    }
    if (myextra_mrmw_insert(&g_mrmw_xn_head, g_mrmw_xn_bk,
                            g_mrmw_xn, &g_mrmw_xn[7], 0u)
        != &g_mrmw_xn[7])
        FAIL("mrmw extra duplicate did not return existing");

    struct rix_hash_mrsw_find_ctx_s ctx[4];
    struct mykey bad = { UINT64_C(0xBAD), UINT64_C(0xBADBAD) };
    const struct mykey *keys[4] = {
        &g_mrmw_xn[1].key, &g_mrmw_xn[3].key, &bad, &g_mrmw_xn[7].key
    };
    struct myextra_node *res[4];
    RIX_HASH_MRMW_HASH_KEY_N(myextra_mrmw, ctx, 4u, &g_mrmw_xn_head,
                             g_mrmw_xn_bk, keys);
    RIX_HASH_MRMW_SCAN_BK_N(myextra_mrmw, ctx, 4u, &g_mrmw_xn_head,
                            g_mrmw_xn_bk);
    RIX_HASH_MRMW_PREFETCH_NODE_N(myextra_mrmw, ctx, 4u, g_mrmw_xn);
    RIX_HASH_MRMW_CMP_KEY_N(myextra_mrmw, ctx, 4u, g_mrmw_xn, res);
    if (res[0] != &g_mrmw_xn[1] || res[1] != &g_mrmw_xn[3] ||
        res[2] != NULL || res[3] != &g_mrmw_xn[7])
        FAIL("mrmw extra staged result mismatch");

    for (unsigned i = 0u; i < 64u; i++) {
        struct myextra_node *f =
            myextra_mrmw_find(&g_mrmw_xn_head, g_mrmw_xn_bk,
                              g_mrmw_xn, &g_mrmw_xn[i].key);
        if (f != &g_mrmw_xn[i])
            FAILF("mrmw extra find[%u] failed", i);
        unsigned bk = (unsigned)(f->cur_hash & g_mrmw_xn_head.rhh_mask);
        unsigned slot = (unsigned)f->slot;
        if (g_mrmw_xn_bk[bk].extra[slot] != (u32)(0xDAD00000u | i))
            FAILF("mrmw extra value[%u] mismatch", i);
    }

    unsigned bk;
    unsigned slot;
    if (!locate_idx_extra(g_mrmw_xn_bk, g_mrmw_xn_head.rhh_mask, 8u,
                          &bk, &slot))
        FAIL("mrmw extra remove_at target not located");
    if (RIX_HASH_MRMW_REMOVE_AT(myextra_mrmw, &g_mrmw_xn_head,
                               g_mrmw_xn_bk, bk, slot) != 8u)
        FAIL("mrmw extra remove_at returned wrong idx");
    if (myextra_mrmw_find(&g_mrmw_xn_head, g_mrmw_xn_bk,
                          g_mrmw_xn, &g_mrmw_xn[7].key) != NULL)
        FAIL("mrmw extra remove_at target still found");
}

static void
test_mrmw_insert_slow_controlled_variants(void)
{
    printf("[T] mrmw controlled insert_slow variants\n");
    ctl_mrmw_init();

    for (unsigned i = 0u; i < 30u; i++) {
        if (ctlht_mrmw_insert(&g_ctl_mrmw_head, g_ctl_mrmw_bk,
                              g_ctl_mrmw, &g_ctl_mrmw[i]) != NULL)
            FAILF("mrmw fp slow setup insert[%u] failed", i);
        if (ctlslot_mrmw_insert(&g_ctl_slot_mrmw_head, g_ctl_slot_mrmw_bk,
                                g_ctl_slot_mrmw, &g_ctl_slot_mrmw[i]) != NULL)
            FAILF("mrmw slot slow setup insert[%u] failed", i);
        if (ctlko_mrmw_insert(&g_ctl_ko_mrmw_head, g_ctl_ko_mrmw_bk,
                              g_ctl_ko_mrmw, &g_ctl_ko_mrmw[i]) != NULL)
            FAILF("mrmw keyonly slow setup insert[%u] failed", i);
        if (ctlextra_mrmw_insert(&g_ctl_extra_mrmw_head,
                                 g_ctl_extra_mrmw_bk, g_ctl_extra_mrmw,
                                 &g_ctl_extra_mrmw[i], 0x51070000u | i)
            != NULL)
            FAILF("mrmw extra slow setup insert[%u] failed", i);
    }

    if (ctlht_mrmw_insert(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                          &g_ctl_mrmw[30]) != NULL)
        FAIL("mrmw fp insert_slow failed");
    if (ctlslot_mrmw_insert(&g_ctl_slot_mrmw_head, g_ctl_slot_mrmw_bk,
                            g_ctl_slot_mrmw, &g_ctl_slot_mrmw[30]) != NULL)
        FAIL("mrmw slot insert_slow failed");
    if (ctlko_mrmw_insert(&g_ctl_ko_mrmw_head, g_ctl_ko_mrmw_bk,
                          g_ctl_ko_mrmw, &g_ctl_ko_mrmw[30]) != NULL)
        FAIL("mrmw keyonly insert_slow failed");
    if (ctlextra_mrmw_insert(&g_ctl_extra_mrmw_head, g_ctl_extra_mrmw_bk,
                             g_ctl_extra_mrmw, &g_ctl_extra_mrmw[30],
                             0x5107001eu) != NULL)
        FAIL("mrmw extra insert_slow failed");

    for (unsigned i = 0u; i <= 30u; i++) {
        if (ctlht_mrmw_find(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                            &g_ctl_mrmw[i].key) != &g_ctl_mrmw[i])
            FAILF("mrmw fp slow find[%u] failed", i);
        if (ctlslot_mrmw_find(&g_ctl_slot_mrmw_head, g_ctl_slot_mrmw_bk,
                              g_ctl_slot_mrmw, &g_ctl_slot_mrmw[i].key)
            != &g_ctl_slot_mrmw[i])
            FAILF("mrmw slot slow find[%u] failed", i);
        unsigned bk = (unsigned)(g_ctl_slot_mrmw[i].cur_hash &
                                 g_ctl_slot_mrmw_head.rhh_mask);
        unsigned slot = (unsigned)g_ctl_slot_mrmw[i].slot;
        if (g_ctl_slot_mrmw_bk[bk].idx[slot] != i + 1u)
            FAILF("mrmw slot slow slot[%u] mismatch", i);
        if (ctlko_mrmw_find(&g_ctl_ko_mrmw_head, g_ctl_ko_mrmw_bk,
                            g_ctl_ko_mrmw, &g_ctl_ko_mrmw[i].key)
            != &g_ctl_ko_mrmw[i])
            FAILF("mrmw keyonly slow find[%u] failed", i);
        if (ctlextra_mrmw_find(&g_ctl_extra_mrmw_head, g_ctl_extra_mrmw_bk,
                               g_ctl_extra_mrmw, &g_ctl_extra_mrmw[i].key)
            != &g_ctl_extra_mrmw[i])
            FAILF("mrmw extra slow find[%u] failed", i);
        bk = (unsigned)(g_ctl_extra_mrmw[i].cur_hash &
                        g_ctl_extra_mrmw_head.rhh_mask);
        slot = (unsigned)g_ctl_extra_mrmw[i].slot;
        if (g_ctl_extra_mrmw_bk[bk].idx[slot] != i + 1u)
            FAILF("mrmw extra slow slot[%u] mismatch", i);
        if (g_ctl_extra_mrmw_bk[bk].extra[slot] != (0x51070000u | i))
            FAILF("mrmw extra slow extra[%u] mismatch", i);
    }
}

static void
test_mrmw_insert_slow_publish_before_unpublish(void)
{
    printf("[T] mrmw insert_slow publish-before-unpublish\n");
    ctl_mrmw_init();

    for (unsigned i = 0u; i < 30u; i++) {
        if (ctlht_mrmw_insert(&g_ctl_mrmw_head, g_ctl_mrmw_bk,
                              g_ctl_mrmw, &g_ctl_mrmw[i]) != NULL)
            FAILF("mrmw handoff setup insert[%u] failed", i);
    }

    hook_arm("move_before_old_clear");
    struct ctl_op_arg wa = { &g_ctl_mrmw[30], NULL, ATOMIC_VAR_INIT(0) };
    pthread_t wt;
    if (pthread_create(&wt, NULL, ctl_mrmw_insert_thread, &wa) != 0)
        FAIL("pthread_create mrmw handoff writer failed");
    wait_reached();

    if (count_idx_common(g_ctl_mrmw_bk, 0u, 1u) != 1u)
        FAIL("mrmw slow victim disappeared from old bucket before clear");
    if (count_idx_common(g_ctl_mrmw_bk, 2u, 1u) != 1u)
        FAIL("mrmw slow victim was not visible in alternate before clear");
    if (ctlht_mrmw_find(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                        &g_ctl_mrmw[0].key) != &g_ctl_mrmw[0])
        FAIL("mrmw slow victim find failed during handoff");

    hook_release();
    pthread_join(wt, NULL);
    hook_disarm();

    if (wa.ret != NULL)
        FAIL("mrmw slow handoff insert returned non-NULL");
    if (ctlht_mrmw_find(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                        &g_ctl_mrmw[30].key) != &g_ctl_mrmw[30])
        FAIL("mrmw slow inserted node not found after handoff");
}

static void
test_mrmw_insert_slow_duplicate_recheck_race(void)
{
    printf("[T] mrmw insert_slow duplicate recheck race\n");
    ctl_mrmw_init();

    for (unsigned i = 0u; i < 30u; i++) {
        if (ctlht_mrmw_insert(&g_ctl_mrmw_head, g_ctl_mrmw_bk,
                              g_ctl_mrmw, &g_ctl_mrmw[i]) != NULL)
            FAILF("mrmw duplicate recheck setup insert[%u] failed", i);
    }
    g_ctl_mrmw[31].key = g_ctl_mrmw[30].key;

    hook_arm("insert_slow_before_lock");
    struct ctl_op_arg wa = { &g_ctl_mrmw[30], NULL, ATOMIC_VAR_INIT(0) };
    pthread_t wt;
    if (pthread_create(&wt, NULL, ctl_mrmw_insert_thread, &wa) != 0)
        FAIL("pthread_create mrmw duplicate recheck writer failed");
    wait_reached();

    atomic_store_explicit(&g_hook_enabled, 0, memory_order_release);
    if (ctlht_mrmw_insert(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                          &g_ctl_mrmw[31]) != NULL)
        FAIL("mrmw duplicate recheck competing insert failed");

    hook_release();
    pthread_join(wt, NULL);
    hook_disarm();

    if (wa.ret != &g_ctl_mrmw[31])
        FAIL("mrmw slow insert did not return duplicate inserted by racer");
    if (atomic_load_explicit(&g_ctl_mrmw_head.rhh_nb, memory_order_relaxed)
        != 31u)
        FAIL("mrmw duplicate recheck changed entry count");
    if (ctlht_mrmw_find(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                        &g_ctl_mrmw[30].key) != &g_ctl_mrmw[31])
        FAIL("mrmw duplicate recheck find returned wrong node");
}

static void
test_mrmw_insert_slow_writer_serialization(void)
{
    printf("[T] mrmw insert_slow writer serialization\n");
    ctl_mrmw_init();

    for (unsigned i = 0u; i < 30u; i++) {
        if (ctlht_mrmw_insert(&g_ctl_mrmw_head, g_ctl_mrmw_bk,
                              g_ctl_mrmw, &g_ctl_mrmw[i]) != NULL)
            FAILF("mrmw serialization setup insert[%u] failed", i);
    }

    hook_arm("move_before_old_clear");
    struct ctl_op_arg a0 = { &g_ctl_mrmw[30], NULL, ATOMIC_VAR_INIT(0) };
    struct ctl_op_arg a1 = { &g_ctl_mrmw[31], NULL, ATOMIC_VAR_INIT(0) };
    pthread_t t0;
    pthread_t t1;
    if (pthread_create(&t0, NULL, ctl_mrmw_insert_thread, &a0) != 0)
        FAIL("pthread_create mrmw first slow writer failed");
    wait_reached();
    if (pthread_create(&t1, NULL, ctl_mrmw_insert_thread, &a1) != 0)
        FAIL("pthread_create mrmw second slow writer failed");

    for (unsigned i = 0u; i < 100000u; i++) {
        if (atomic_load_explicit(&a1.done, memory_order_acquire))
            FAIL("second mrmw slow writer completed while first held locks");
        sched_yield();
    }

    hook_release();
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
    hook_disarm();

    if (a0.ret != NULL || a1.ret != NULL)
        FAIL("mrmw serialized slow insert returned non-NULL");
    if (ctlht_mrmw_find(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                        &g_ctl_mrmw[30].key) != &g_ctl_mrmw[30])
        FAIL("first serialized slow insert not found");
    if (ctlht_mrmw_find(&g_ctl_mrmw_head, g_ctl_mrmw_bk, g_ctl_mrmw,
                        &g_ctl_mrmw[31].key) != &g_ctl_mrmw[31])
        FAIL("second serialized slow insert not found");
}

static void
test_mrmw_insert_slow_multihop_variants(void)
{
    printf("[T] mrmw insert_slow multi-hop variants\n");
    struct ctl_node fp[46];
    struct ctl_slot_node slot_nodes[46];
    struct ctl_keyonly_node ko[46];
    struct ctl_extra_node extra_nodes[46];
    struct rix_hash_bucket_s fp_bk[4] __attribute__((aligned(64)));
    struct rix_hash_bucket_s slot_bk[4] __attribute__((aligned(64)));
    struct rix_hash_bucket_s ko_bk[4] __attribute__((aligned(64)));
    struct rix_hash_bucket_extra_s extra_bk[4] __attribute__((aligned(64)));
    struct ctlpath_mrmw fp_head;
    struct ctlpathslot_mrmw slot_head;
    struct ctlpathko_mrmw ko_head;
    struct ctlpathextra_mrmw extra_head;

    memset(fp, 0, sizeof(fp));
    memset(slot_nodes, 0, sizeof(slot_nodes));
    memset(ko, 0, sizeof(ko));
    memset(extra_nodes, 0, sizeof(extra_nodes));
    ctlpath_mrmw_init(&fp_head, fp_bk, 4u);
    ctlpath_mrmw_attach_kickout_scratch(&fp_head, g_mrmw_scratch);
    ctlpathslot_mrmw_init(&slot_head, slot_bk, 4u);
    ctlpathslot_mrmw_attach_kickout_scratch(&slot_head, g_mrmw_scratch);
    ctlpathko_mrmw_init(&ko_head, ko_bk, 4u);
    ctlpathko_mrmw_attach_kickout_scratch(&ko_head, g_mrmw_scratch);
    ctlpathextra_mrmw_init(&extra_head, extra_bk, 4u);
    ctlpathextra_mrmw_attach_kickout_scratch(&extra_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < 46u; i++) {
        fp[i].key.id = i + 1u;
        slot_nodes[i].key.id = i + 1u;
        ko[i].key.id = i + 1u;
        extra_nodes[i].key.id = i + 1u;
    }

    for (unsigned i = 0u; i < 45u; i++) {
        if (ctlpath_mrmw_insert(&fp_head, fp_bk, fp, &fp[i]) != NULL)
            FAILF("mrmw fp multihop setup insert[%u] failed", i);
        if (ctlpathslot_mrmw_insert(&slot_head, slot_bk, slot_nodes,
                                    &slot_nodes[i]) != NULL)
            FAILF("mrmw slot multihop setup insert[%u] failed", i);
        if (ctlpathko_mrmw_insert(&ko_head, ko_bk, ko, &ko[i]) != NULL)
            FAILF("mrmw keyonly multihop setup insert[%u] failed", i);
        if (ctlpathextra_mrmw_insert(&extra_head, extra_bk, extra_nodes,
                                     &extra_nodes[i], 0x4d480000u | i) != NULL)
            FAILF("mrmw extra multihop setup insert[%u] failed", i);
    }

    if (ctlpath_mrmw_insert(&fp_head, fp_bk, fp, &fp[45]) != NULL)
        FAIL("mrmw fp multihop insert_slow failed");
    if (ctlpathslot_mrmw_insert(&slot_head, slot_bk, slot_nodes,
                                &slot_nodes[45]) != NULL)
        FAIL("mrmw slot multihop insert_slow failed");
    if (ctlpathko_mrmw_insert(&ko_head, ko_bk, ko, &ko[45]) != NULL)
        FAIL("mrmw keyonly multihop insert_slow failed");
    if (ctlpathextra_mrmw_insert(&extra_head, extra_bk, extra_nodes,
                                 &extra_nodes[45], 0x4d48002du) != NULL)
        FAIL("mrmw extra multihop insert_slow failed");

    for (unsigned i = 0u; i < 46u; i++) {
        if (ctlpath_mrmw_find(&fp_head, fp_bk, fp, &fp[i].key) != &fp[i])
            FAILF("mrmw fp multihop find[%u] failed", i);
        if (ctlpathslot_mrmw_find(&slot_head, slot_bk, slot_nodes,
                                  &slot_nodes[i].key) != &slot_nodes[i])
            FAILF("mrmw slot multihop find[%u] failed", i);
        unsigned bk = (unsigned)(slot_nodes[i].cur_hash & slot_head.rhh_mask);
        unsigned sl = (unsigned)slot_nodes[i].slot;
        if (slot_bk[bk].idx[sl] != i + 1u)
            FAILF("mrmw slot multihop slot[%u] mismatch", i);
        if (ctlpathko_mrmw_find(&ko_head, ko_bk, ko, &ko[i].key) != &ko[i])
            FAILF("mrmw keyonly multihop find[%u] failed", i);
        if (ctlpathextra_mrmw_find(&extra_head, extra_bk, extra_nodes,
                                   &extra_nodes[i].key) != &extra_nodes[i])
            FAILF("mrmw extra multihop find[%u] failed", i);
        bk = (unsigned)(extra_nodes[i].cur_hash & extra_head.rhh_mask);
        sl = (unsigned)extra_nodes[i].slot;
        if (extra_bk[bk].idx[sl] != i + 1u)
            FAILF("mrmw extra multihop slot[%u] mismatch", i);
        if (extra_bk[bk].extra[sl] != (0x4d480000u | i))
            FAILF("mrmw extra multihop extra[%u] mismatch", i);
    }
}

static u32
find_u32_pair_key(unsigned b0, unsigned b1, const u32 *used, unsigned nused)
{
    for (u32 k = 1u; k != 0u; k++) {
        union rix_hash_hash_u h = rix_hash_arch->hash_u32(k, 3u);
        if ((h.val32[0] & 3u) != b0 || (h.val32[1] & 3u) != b1)
            continue;
        int seen = 0;
        for (unsigned i = 0u; i < nused; i++) {
            if (used[i] == k) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            return k;
    }
    FAIL("could not find u32 pair key");
    return 0u;
}

static u64
find_u64_pair_key(unsigned b0, unsigned b1, const u64 *used, unsigned nused)
{
    for (u64 k = 1u; k != 0u; k++) {
        union rix_hash_hash_u h = rix_hash_arch->hash_u64(k, 3u);
        if ((h.val32[0] & 3u) != b0 || (h.val32[1] & 3u) != b1)
            continue;
        int seen = 0;
        for (unsigned i = 0u; i < nused; i++) {
            if (used[i] == k) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            return k;
    }
    FAIL("could not find u64 pair key");
    return 0u;
}

static void
test_mrmw_insert_slow_u32_u64(void)
{
    printf("[T] mrmw u32/u64 insert_slow multi-hop\n");
    struct myu32_node n32[46];
    struct myu64_node n64[46];
    struct rix_hash_bucket_s b32[4] __attribute__((aligned(64)));
    struct rix_hash64_bucket_s b64[4] __attribute__((aligned(64)));
    struct myu32_mrmw h32;
    struct myu64_mrmw h64;
    u32 used32[46];
    u64 used64[46];

    memset(n32, 0, sizeof(n32));
    memset(n64, 0, sizeof(n64));
    myu32_mrmw_init(&h32, b32, 4u);
    myu32_mrmw_attach_kickout_scratch(&h32, g_mrmw_scratch);
    myu64_mrmw_init(&h64, b64, 4u);
    myu64_mrmw_attach_kickout_scratch(&h64, g_mrmw_scratch);

    for (unsigned i = 0u; i < 15u; i++) {
        used32[i] = find_u32_pair_key(0u, 2u, used32, i);
        n32[i].key = used32[i];
        used64[i] = find_u64_pair_key(0u, 2u, used64, i);
        n64[i].key = used64[i];
    }
    for (unsigned i = 15u; i < 30u; i++) {
        used32[i] = find_u32_pair_key(1u, 2u, used32, i);
        n32[i].key = used32[i];
        used64[i] = find_u64_pair_key(1u, 2u, used64, i);
        n64[i].key = used64[i];
    }
    for (unsigned i = 30u; i < 45u; i++) {
        used32[i] = find_u32_pair_key(2u, 3u, used32, i);
        n32[i].key = used32[i];
        used64[i] = find_u64_pair_key(2u, 3u, used64, i);
        n64[i].key = used64[i];
    }
    used32[45] = find_u32_pair_key(0u, 1u, used32, 45u);
    n32[45].key = used32[45];
    used64[45] = find_u64_pair_key(0u, 1u, used64, 45u);
    n64[45].key = used64[45];

    for (unsigned i = 0u; i < 45u; i++) {
        if (myu32_mrmw_insert(&h32, b32, n32, &n32[i]) != NULL)
            FAILF("mrmw u32 slow setup insert[%u] failed", i);
        if (myu64_mrmw_insert(&h64, b64, n64, &n64[i]) != NULL)
            FAILF("mrmw u64 slow setup insert[%u] failed", i);
    }
    if (myu32_mrmw_insert(&h32, b32, n32, &n32[45]) != NULL)
        FAIL("mrmw u32 insert_slow failed");
    if (myu64_mrmw_insert(&h64, b64, n64, &n64[45]) != NULL)
        FAIL("mrmw u64 insert_slow failed");

    for (unsigned i = 0u; i < 46u; i++) {
        if (myu32_mrmw_find(&h32, b32, n32, n32[i].key) != &n32[i])
            FAILF("mrmw u32 slow find[%u] failed", i);
        if (myu64_mrmw_find(&h64, b64, n64, n64[i].key) != &n64[i])
            FAILF("mrmw u64 slow find[%u] failed", i);
    }
}

static void
test_mrmw_insert_slow_full_failure(void)
{
    printf("[T] mrmw u32 insert_slow full failure\n");
    struct myu32_node nodes[61];
    struct rix_hash_bucket_s buckets[4] __attribute__((aligned(64)));
    struct myu32_mrmw head;
    u32 used[61];

    memset(nodes, 0, sizeof(nodes));
    myu32_mrmw_init(&head, buckets, 4u);
    myu32_mrmw_attach_kickout_scratch(&head, g_mrmw_scratch);

    for (unsigned i = 0u; i < 15u; i++)
        nodes[i].key = used[i] = find_u32_pair_key(0u, 1u, used, i);
    for (unsigned i = 15u; i < 30u; i++)
        nodes[i].key = used[i] = find_u32_pair_key(1u, 2u, used, i);
    for (unsigned i = 30u; i < 45u; i++)
        nodes[i].key = used[i] = find_u32_pair_key(2u, 3u, used, i);
    for (unsigned i = 45u; i < 60u; i++)
        nodes[i].key = used[i] = find_u32_pair_key(3u, 0u, used, i);
    nodes[60].key = used[60] = find_u32_pair_key(0u, 2u, used, 60u);

    for (unsigned i = 0u; i < 60u; i++) {
        if (myu32_mrmw_insert(&head, buckets, nodes, &nodes[i]) != NULL)
            FAILF("mrmw full setup insert[%u] failed", i);
    }
    if (atomic_load_explicit(&head.rhh_nb, memory_order_relaxed) != 60u)
        FAIL("mrmw full setup count mismatch");
    if (myu32_mrmw_insert(&head, buckets, nodes, &nodes[60]) != &nodes[60])
        FAIL("mrmw full insert did not fail with input node");
    if (atomic_load_explicit(&head.rhh_nb, memory_order_relaxed) != 60u)
        FAIL("mrmw full failure changed count");
    for (unsigned i = 0u; i < 60u; i++) {
        if (myu32_mrmw_find(&head, buckets, nodes, nodes[i].key) != &nodes[i])
            FAILF("mrmw full failure lost entry[%u]", i);
    }
}

#define MRMW_DUP_THREADS 8u

static struct mynode g_mrmw_dup[MRMW_DUP_THREADS];
static struct rix_hash_bucket_s g_mrmw_dup_bk[16]
    __attribute__((aligned(64)));
static struct myht_mrmw g_mrmw_dup_head;
static _Atomic unsigned g_mrmw_dup_inserted;

static void *
mrmw_duplicate_insert_worker(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;

    mrmw_wait_start();
    struct mynode *ret =
        myht_mrmw_insert(&g_mrmw_dup_head, g_mrmw_dup_bk, g_mrmw_dup,
                         &g_mrmw_dup[idx]);
    if (ret == NULL) {
        atomic_fetch_add_explicit(&g_mrmw_dup_inserted, 1u,
                                  memory_order_relaxed);
    } else if (mykey_cmp(&ret->key, &g_mrmw_dup[idx].key) != 0) {
        atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
    }
    return NULL;
}

static void
test_mrmw_duplicate_race(void)
{
    printf("[T] mrmw duplicate insert race\n");
    memset(g_mrmw_dup, 0, sizeof(g_mrmw_dup));
    myht_mrmw_init(&g_mrmw_dup_head, g_mrmw_dup_bk, 16u);
    myht_mrmw_attach_kickout_scratch(&g_mrmw_dup_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_DUP_THREADS; i++) {
        g_mrmw_dup[i].key.hi = UINT64_C(0xD00D000000000000);
        g_mrmw_dup[i].key.lo = UINT64_C(0xCAFE000000000000);
        g_mrmw_dup[i].value = i;
    }

    atomic_store_explicit(&g_mrmw_start, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mrmw_fail, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mrmw_dup_inserted, 0u, memory_order_relaxed);
    pthread_t threads[MRMW_DUP_THREADS];
    for (uintptr_t i = 0u; i < MRMW_DUP_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, mrmw_duplicate_insert_worker,
                           (void *)i) != 0)
            FAIL("pthread_create mrmw duplicate writer failed");
    }
    atomic_store_explicit(&g_mrmw_start, 1, memory_order_release);
    for (unsigned i = 0u; i < MRMW_DUP_THREADS; i++)
        pthread_join(threads[i], NULL);

    if (atomic_load_explicit(&g_mrmw_fail, memory_order_acquire))
        FAIL("mrmw duplicate race returned wrong key");
    if (atomic_load_explicit(&g_mrmw_dup_inserted, memory_order_relaxed) != 1u)
        FAIL("mrmw duplicate race did not publish exactly one entry");
    if (atomic_load_explicit(&g_mrmw_dup_head.rhh_nb, memory_order_relaxed)
        != 1u)
        FAIL("mrmw duplicate race count mismatch");
    if (myht_mrmw_find(&g_mrmw_dup_head, g_mrmw_dup_bk, g_mrmw_dup,
                       &g_mrmw_dup[0].key) == NULL)
        FAIL("mrmw duplicate race entry not found");
}

static void *
mrmw_churn_fp_worker(void *arg)
{
    struct mrmw_worker_arg *a = (struct mrmw_worker_arg *)arg;

    mrmw_wait_start();
    for (unsigned iter = 0u; iter < MRMW_CHURN_ITERS; iter++) {
        unsigned hi = a->begin;
        for (unsigned i = a->begin; i < a->end; i++) {
            struct mynode *ins = myht_mrmw_insert(
                &g_mrmw_churn_head, g_mrmw_churn_bk, g_mrmw_churn,
                &g_mrmw_churn[i]);
            if (ins != NULL)
                break;
            hi = i + 1u;
        }
        for (unsigned i = a->begin; i < hi; i++) {
            struct mynode *rem = myht_mrmw_remove(
                &g_mrmw_churn_head, g_mrmw_churn_bk, g_mrmw_churn,
                &g_mrmw_churn[i]);
            if (rem != &g_mrmw_churn[i]) {
                atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
                return NULL;
            }
        }
    }
    return NULL;
}

static void *
mrmw_churn_slot_worker(void *arg)
{
    struct mrmw_worker_arg *a = (struct mrmw_worker_arg *)arg;

    mrmw_wait_start();
    for (unsigned iter = 0u; iter < MRMW_CHURN_ITERS; iter++) {
        unsigned hi = a->begin;
        for (unsigned i = a->begin; i < a->end; i++) {
            struct myslot_node *ins = myslot_mrmw_insert(
                &g_mrmw_churn_slot_head, g_mrmw_churn_slot_bk,
                g_mrmw_churn_slot, &g_mrmw_churn_slot[i]);
            if (ins != NULL)
                break;
            hi = i + 1u;
        }
        for (unsigned i = a->begin; i < hi; i++) {
            struct myslot_node *rem = myslot_mrmw_remove(
                &g_mrmw_churn_slot_head, g_mrmw_churn_slot_bk,
                g_mrmw_churn_slot, &g_mrmw_churn_slot[i]);
            if (rem != &g_mrmw_churn_slot[i]) {
                atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
                return NULL;
            }
        }
    }
    return NULL;
}

static void *
mrmw_churn_u32_worker(void *arg)
{
    struct mrmw_worker_arg *a = (struct mrmw_worker_arg *)arg;

    mrmw_wait_start();
    for (unsigned iter = 0u; iter < MRMW_CHURN_ITERS; iter++) {
        unsigned hi = a->begin;
        for (unsigned i = a->begin; i < a->end; i++) {
            struct myu32_node *ins = myu32_mrmw_insert(
                &g_mrmw_churn_u32_head, g_mrmw_churn_u32_bk,
                g_mrmw_churn_u32, &g_mrmw_churn_u32[i]);
            if (ins != NULL)
                break;
            hi = i + 1u;
        }
        for (unsigned i = a->begin; i < hi; i++) {
            struct myu32_node *rem = myu32_mrmw_remove(
                &g_mrmw_churn_u32_head, g_mrmw_churn_u32_bk,
                g_mrmw_churn_u32, &g_mrmw_churn_u32[i]);
            if (rem != &g_mrmw_churn_u32[i]) {
                atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
                return NULL;
            }
        }
    }
    return NULL;
}

static void *
mrmw_churn_u64_worker(void *arg)
{
    struct mrmw_worker_arg *a = (struct mrmw_worker_arg *)arg;

    mrmw_wait_start();
    for (unsigned iter = 0u; iter < MRMW_CHURN_ITERS; iter++) {
        unsigned hi = a->begin;
        for (unsigned i = a->begin; i < a->end; i++) {
            struct myu64_node *ins = myu64_mrmw_insert(
                &g_mrmw_churn_u64_head, g_mrmw_churn_u64_bk,
                g_mrmw_churn_u64, &g_mrmw_churn_u64[i]);
            if (ins != NULL)
                break;
            hi = i + 1u;
        }
        for (unsigned i = a->begin; i < hi; i++) {
            struct myu64_node *rem = myu64_mrmw_remove(
                &g_mrmw_churn_u64_head, g_mrmw_churn_u64_bk,
                g_mrmw_churn_u64, &g_mrmw_churn_u64[i]);
            if (rem != &g_mrmw_churn_u64[i]) {
                atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
                return NULL;
            }
        }
    }
    return NULL;
}

static void *
mrmw_churn_extra_worker(void *arg)
{
    struct mrmw_worker_arg *a = (struct mrmw_worker_arg *)arg;

    mrmw_wait_start();
    for (unsigned iter = 0u; iter < MRMW_CHURN_ITERS; iter++) {
        unsigned hi = a->begin;
        for (unsigned i = a->begin; i < a->end; i++) {
            struct myextra_node *ins = myextra_mrmw_insert(
                &g_mrmw_churn_xn_head, g_mrmw_churn_xn_bk,
                g_mrmw_churn_xn, &g_mrmw_churn_xn[i], (u32)(i + 1u));
            if (ins != NULL)
                break;
            hi = i + 1u;
        }
        for (unsigned i = a->begin; i < hi; i++) {
            struct myextra_node *rem = myextra_mrmw_remove(
                &g_mrmw_churn_xn_head, g_mrmw_churn_xn_bk,
                g_mrmw_churn_xn, &g_mrmw_churn_xn[i]);
            if (rem != &g_mrmw_churn_xn[i]) {
                atomic_store_explicit(&g_mrmw_fail, 1, memory_order_release);
                return NULL;
            }
        }
    }
    return NULL;
}

static void
test_mrmw_churn_run(const char *label, void *(*worker)(void *),
                    unsigned final_nb)
{
    printf("[T] mrmw churn insert+remove %s\n", label);
    atomic_store_explicit(&g_mrmw_start, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mrmw_fail, 0, memory_order_relaxed);
    pthread_t writers[MRMW_CHURN_WRITERS];
    struct mrmw_worker_arg args[MRMW_CHURN_WRITERS];
    for (unsigned i = 0u; i < MRMW_CHURN_WRITERS; i++) {
        args[i].begin = i * MRMW_CHURN_PER;
        args[i].end = (i + 1u) * MRMW_CHURN_PER;
        if (pthread_create(&writers[i], NULL, worker, &args[i]) != 0)
            FAIL("pthread_create mrmw churn writer failed");
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    atomic_store_explicit(&g_mrmw_start, 1, memory_order_release);
    for (unsigned i = 0u; i < MRMW_CHURN_WRITERS; i++)
        pthread_join(writers[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (atomic_load_explicit(&g_mrmw_fail, memory_order_acquire))
        FAILF("mrmw churn %s detected remove failure", label);
    double ns = (double)(t1.tv_sec - t0.tv_sec) * 1.0e9
              + (double)(t1.tv_nsec - t0.tv_nsec);
    double total_ops = (double)MRMW_CHURN_WRITERS * (double)MRMW_CHURN_PER
                     * (double)MRMW_CHURN_ITERS * 2.0;
    printf("    %s: %.3f ms (%.1f ns/op, %.2f Mops/s)\n",
           label, ns / 1.0e6, ns / total_ops, total_ops * 1.0e3 / ns);
    (void)final_nb;
}

static void
test_mrmw_churn_insert_remove(void)
{
    memset(g_mrmw_churn, 0, sizeof(g_mrmw_churn));
    myht_mrmw_init(&g_mrmw_churn_head, g_mrmw_churn_bk, MRMW_CHURN_NB_BK);
    myht_mrmw_attach_kickout_scratch(&g_mrmw_churn_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_CHURN_N; i++) {
        g_mrmw_churn[i].key.hi = UINT64_C(0xCEE1000000000000) | (u64)i;
        g_mrmw_churn[i].key.lo = UINT64_C(0x9999000000000000) ^ (u64)(i * 31u);
        g_mrmw_churn[i].value = i;
    }
    test_mrmw_churn_run("fp", mrmw_churn_fp_worker, 0u);
    if (atomic_load_explicit(&g_mrmw_churn_head.rhh_nb, memory_order_relaxed)
        != 0u)
        FAIL("mrmw churn fp final count must be zero");

    memset(g_mrmw_churn_slot, 0, sizeof(g_mrmw_churn_slot));
    myslot_mrmw_init(&g_mrmw_churn_slot_head, g_mrmw_churn_slot_bk,
                     MRMW_CHURN_NB_BK);
    myslot_mrmw_attach_kickout_scratch(&g_mrmw_churn_slot_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_CHURN_N; i++) {
        g_mrmw_churn_slot[i].key.hi = UINT64_C(0xCEE2000000000000) | (u64)i;
        g_mrmw_churn_slot[i].key.lo = UINT64_C(0x9999000000000000)
                                    ^ (u64)(i * 37u);
        g_mrmw_churn_slot[i].value = i;
    }
    test_mrmw_churn_run("slot", mrmw_churn_slot_worker, 0u);
    if (atomic_load_explicit(&g_mrmw_churn_slot_head.rhh_nb,
                             memory_order_relaxed) != 0u)
        FAIL("mrmw churn slot final count must be zero");

    memset(g_mrmw_churn_u32, 0, sizeof(g_mrmw_churn_u32));
    myu32_mrmw_init(&g_mrmw_churn_u32_head, g_mrmw_churn_u32_bk,
                    MRMW_CHURN_NB_BK);
    myu32_mrmw_attach_kickout_scratch(&g_mrmw_churn_u32_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_CHURN_N; i++)
        g_mrmw_churn_u32[i].key = i + 1u;
    test_mrmw_churn_run("u32", mrmw_churn_u32_worker, 0u);
    if (atomic_load_explicit(&g_mrmw_churn_u32_head.rhh_nb,
                             memory_order_relaxed) != 0u)
        FAIL("mrmw churn u32 final count must be zero");

    memset(g_mrmw_churn_u64, 0, sizeof(g_mrmw_churn_u64));
    myu64_mrmw_init(&g_mrmw_churn_u64_head, g_mrmw_churn_u64_bk,
                    MRMW_CHURN_NB_BK);
    myu64_mrmw_attach_kickout_scratch(&g_mrmw_churn_u64_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_CHURN_N; i++)
        g_mrmw_churn_u64[i].key = UINT64_C(0xC0DE000000000000) | (u64)(i + 1u);
    test_mrmw_churn_run("u64", mrmw_churn_u64_worker, 0u);
    if (atomic_load_explicit(&g_mrmw_churn_u64_head.rhh_nb,
                             memory_order_relaxed) != 0u)
        FAIL("mrmw churn u64 final count must be zero");

    memset(g_mrmw_churn_xn, 0, sizeof(g_mrmw_churn_xn));
    myextra_mrmw_init(&g_mrmw_churn_xn_head, g_mrmw_churn_xn_bk,
                      MRMW_CHURN_NB_BK);
    myextra_mrmw_attach_kickout_scratch(&g_mrmw_churn_xn_head, g_mrmw_scratch);
    for (unsigned i = 0u; i < MRMW_CHURN_N; i++) {
        g_mrmw_churn_xn[i].key.hi = UINT64_C(0xCEE3000000000000) | (u64)i;
        g_mrmw_churn_xn[i].key.lo = UINT64_C(0x9999000000000000)
                                  ^ (u64)(i * 41u);
    }
    test_mrmw_churn_run("extra", mrmw_churn_extra_worker, 0u);
    if (atomic_load_explicit(&g_mrmw_churn_xn_head.rhh_nb,
                             memory_order_relaxed) != 0u)
        FAIL("mrmw churn extra final count must be zero");
}

static void
test_mrmw_multi_writer_stress(void)
{
    printf("[T] mrmw fp multi-writer stress\n");
    mrmw_init();
    atomic_store_explicit(&g_mrmw_start, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mrmw_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mrmw_fail, 0, memory_order_relaxed);

    pthread_t writers[MRMW_WRITERS];
    pthread_t readers[MRMW_READERS];
    struct mrmw_worker_arg args[MRMW_WRITERS];
    unsigned step = MRMW_N / MRMW_WRITERS;

    for (unsigned i = 0u; i < MRMW_WRITERS; i++) {
        args[i].begin = i * step;
        args[i].end = (i == MRMW_WRITERS - 1u) ? MRMW_N : (i + 1u) * step;
        if (pthread_create(&writers[i], NULL, mrmw_insert_worker,
                           &args[i]) != 0)
            FAIL("pthread_create mrmw writer failed");
    }
    for (uintptr_t i = 0u; i < MRMW_READERS; i++) {
        if (pthread_create(&readers[i], NULL, mrmw_reader_worker,
                           (void *)(i + 1u)) != 0)
            FAIL("pthread_create mrmw reader failed");
    }

    atomic_store_explicit(&g_mrmw_start, 1, memory_order_release);
    for (unsigned i = 0u; i < MRMW_WRITERS; i++)
        pthread_join(writers[i], NULL);
    atomic_store_explicit(&g_mrmw_stop, 1, memory_order_release);
    for (unsigned i = 0u; i < MRMW_READERS; i++)
        pthread_join(readers[i], NULL);

    if (atomic_load_explicit(&g_mrmw_fail, memory_order_acquire))
        FAIL("mrmw insert/read stress failed");
    if (atomic_load_explicit(&g_mrmw_head.rhh_nb, memory_order_relaxed) !=
        MRMW_N)
        FAIL("mrmw inserted count mismatch");
    for (unsigned i = 0u; i < MRMW_N; i++) {
        if (myht_mrmw_find(&g_mrmw_head, g_mrmw_bk, g_mrmw,
                           &g_mrmw[i].key) != &g_mrmw[i])
            FAILF("mrmw final find[%u] failed", i);
    }

    atomic_store_explicit(&g_mrmw_start, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mrmw_fail, 0, memory_order_relaxed);
    for (unsigned i = 0u; i < MRMW_WRITERS; i++) {
        if (pthread_create(&writers[i], NULL, mrmw_remove_worker,
                           &args[i]) != 0)
            FAIL("pthread_create mrmw remove writer failed");
    }
    atomic_store_explicit(&g_mrmw_start, 1, memory_order_release);
    for (unsigned i = 0u; i < MRMW_WRITERS; i++)
        pthread_join(writers[i], NULL);
    if (atomic_load_explicit(&g_mrmw_fail, memory_order_acquire))
        FAIL("mrmw remove stress failed");
    if (atomic_load_explicit(&g_mrmw_head.rhh_nb, memory_order_relaxed) != 0u)
        FAIL("mrmw removed count mismatch");
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
    test_u32_insert_find_remove();
    test_u32_duplicate();
    test_u32_staged_remove_at();
    test_u32_stress();
    test_u64_insert_find_remove();
    test_u64_duplicate();
    test_u64_staged_remove_at();
    test_u64_stress();
    test_extra_insert_find_remove();
    test_extra_kickout_carries_extra();
    test_extra_staged_remove_at();
    test_extra_stress();
    test_mrmw_insert_find_remove();
    test_mrmw_duplicate_remove_at();
    test_mrmw_staged_api();
    test_mrmw_slot_insert_find_remove();
    test_mrmw_keyonly_insert_find_remove();
    test_mrmw_u32_insert_find_remove();
    test_mrmw_u64_insert_find_remove();
    test_mrmw_extra_insert_find_remove();
    test_mrmw_insert_slow_controlled_variants();
    test_mrmw_insert_slow_publish_before_unpublish();
    test_mrmw_insert_slow_duplicate_recheck_race();
    test_mrmw_insert_slow_writer_serialization();
    test_mrmw_insert_slow_multihop_variants();
    test_mrmw_insert_slow_u32_u64();
    test_mrmw_insert_slow_full_failure();
    test_mrmw_duplicate_race();
    test_mrmw_churn_insert_remove();
    test_mrmw_multi_writer_stress();

    printf("OK\n");
    return 0;
}
