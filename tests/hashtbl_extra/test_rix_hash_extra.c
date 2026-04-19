/* test_rix_hash_extra.c - slot_extra variant tests */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "rix_hash.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

static void
test_bucket_layout(void)
{
    printf("[T] bucket layout\n");
    if (sizeof(struct rix_hash_bucket_extra_s) != 192u)
        FAILF("bucket size expected 192, got %zu",
              sizeof(struct rix_hash_bucket_extra_s));
    if (_Alignof(struct rix_hash_bucket_extra_s) != 64u)
        FAILF("bucket align expected 64, got %zu",
              _Alignof(struct rix_hash_bucket_extra_s));
    if (offsetof(struct rix_hash_bucket_extra_s, extra) != 128u)
        FAILF("extra offset expected 128, got %zu",
              offsetof(struct rix_hash_bucket_extra_s, extra));
}

struct ek {
    u64 hi;
    u64 lo;
};

struct enode {
    u32  cur_hash;
    u16  slot;
    u16  _pad;
    struct ek key;
};

static int
ek_cmp(const struct ek *a, const struct ek *b)
{
    return (a->hi == b->hi && a->lo == b->lo) ? 0 : 1;
}

RIX_HASH_HEAD(eht);
RIX_HASH_GENERATE_SLOT_EXTRA(eht, enode, key, cur_hash, slot, ek_cmp)

#define NB_BASIC 20u
#define NB_BK_BASIC 4u

static struct enode e_basic[NB_BASIC];
static struct rix_hash_bucket_extra_s e_bk[NB_BK_BASIC]
    __attribute__((aligned(64)));
static struct eht e_head;

static void
e_basic_init(void)
{
    memset(e_basic, 0, sizeof(e_basic));
    memset(e_bk,    0, sizeof(e_bk));
    RIX_HASH_INIT(eht, &e_head, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        e_basic[i].key.hi = (u64)(i + 1);
        e_basic[i].key.lo = 0xDEADC0DE00000000ULL;
    }
}

static void
test_insert_find_remove_basic(void)
{
    printf("[T] insert/find/remove basic\n");
    e_basic_init();

    for (unsigned i = 0; i < NB_BASIC; i++) {
        u32 extra = 0x1000u + i;
        struct enode *dup = eht_insert(&e_head, e_bk, e_basic,
                                       &e_basic[i], extra);
        if (dup != NULL)
            FAILF("insert[%u] unexpected dup %p", i, (void *)dup);
    }
    if (e_head.rhh_nb != NB_BASIC)
        FAILF("rhh_nb expected %u got %u", NB_BASIC, e_head.rhh_nb);

    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct enode *f = eht_find(&e_head, e_bk, e_basic,
                                   &e_basic[i].key);
        if (f != &e_basic[i])
            FAILF("find[%u] expected %p got %p",
                  i, (void *)&e_basic[i], (void *)f);

        unsigned bk = f->cur_hash & e_head.rhh_mask;
        unsigned slot = f->slot;
        u32 expected = 0x1000u + i;
        if (e_bk[bk].extra[slot] != expected)
            FAILF("extra[%u]: expected 0x%x got 0x%x at bk=%u slot=%u",
                  i, expected, e_bk[bk].extra[slot], bk, slot);
    }

    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        struct enode *r = eht_remove(&e_head, e_bk, e_basic,
                                     &e_basic[i]);
        if (r != &e_basic[i])
            FAILF("remove[%u] expected %p got %p",
                  i, (void *)&e_basic[i], (void *)r);
    }
    if (e_head.rhh_nb != NB_BASIC / 2)
        FAILF("rhh_nb after removals: expected %u got %u",
              NB_BASIC / 2, e_head.rhh_nb);
}

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    test_bucket_layout();
    test_insert_find_remove_basic();
    printf("PASS\n");
    return 0;
}
