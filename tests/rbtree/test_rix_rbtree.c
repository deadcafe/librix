/* test_rix_rbtree.c
 *  RIX_RB
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "../../include/librix.h"

#define FAIL(msg) do { \
  fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
  abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
  fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
  abort(); \
} while (0)

struct node {
    int key;
    RIX_RB_ENTRY(node) rb; /* rbe_parent/rbe_left/rbe_right/rbe_color */
};

static struct node *g_nodes = NULL;
static unsigned g_cap = 0;

RIX_RB_HEAD(tree);

static int node_cmp(const struct node *a, const struct node *b){
    if (a->key < b->key) return -1;
    if (a->key > b->key) return +1;
    return 0;
}

RIX_RB_PROTOTYPE(tree, struct node, rb, node_cmp)
RIX_RB_GENERATE(tree, struct node, rb, node_cmp)

typedef struct { int *a; size_t n, cap; } vec_t;

static void vec_init(vec_t *v){ v->a=NULL; v->n=0; v->cap=0; }
static void vec_free(vec_t *v){ free(v->a); v->a=NULL; v->n=v->cap=0; }
static void vec_reserve(vec_t *v, size_t nc){
    if (v->cap>=nc) return;
    size_t c = v->cap ? v->cap : 8;
    while (c < nc) c <<= 1;
    int *na = (int*)realloc(v->a, c*sizeof(int));
    if (!na) { perror("realloc"); exit(1); }
    v->a=na; v->cap=c;
}
static size_t vec_lower_bound(const vec_t *v, int key){
    size_t lo=0, hi=v->n;
    while (lo<hi){
        size_t mid=(lo+hi)/2;
        if (v->a[mid] < key) lo=mid+1; else hi=mid;
    }
    return lo;
}
static int vec_insert_unique(vec_t *v, int key){
    size_t pos = vec_lower_bound(v, key);
    if (pos < v->n && v->a[pos]==key) return 0;
    vec_reserve(v, v->n+1);
    memmove(v->a+pos+1, v->a+pos, (v->n-pos)*sizeof(int));
    v->a[pos]=key; v->n++;
    return 1;
}
static int vec_erase_key(vec_t *v, int key){
    size_t pos = vec_lower_bound(v, key);
    if (pos >= v->n || v->a[pos]!=key) return 0;
    memmove(v->a+pos, v->a+pos+1, (v->n-pos-1)*sizeof(int));
    v->n--;
    return 1;
}

static struct node *np_from_idx(unsigned idx){ return idx? &g_nodes[idx-1] : NULL; }
static unsigned idx_from_ptr(const struct node *p){
    return RIX_IDX_FROM_PTR(g_nodes, (struct node*)p);
}

static int check_rb_black_height(struct node *x, int *bh_out){
    if (!x){
        *bh_out = 1;
        return 1;
    }
    struct node *L = RIX_RB_LEFT(x, g_nodes, rb);
    struct node *R = RIX_RB_RIGHT(x, g_nodes, rb);

    if (L && RIX_RB_PARENT(L, g_nodes, rb) != x) FAIL("parent mismatch (left)");
    if (R && RIX_RB_PARENT(R, g_nodes, rb) != x) FAIL("parent mismatch (right)");

    if (RIX_RB_COLOR(x, rb) == RIX_RB_RED){
        if (L && RIX_RB_COLOR(L, rb) != RIX_RB_BLACK) FAIL("red parent with red left child");
        if (R && RIX_RB_COLOR(R, rb) != RIX_RB_BLACK) FAIL("red parent with red right child");
    }

    int bl=0, br=0;
    if (!check_rb_black_height(L, &bl)) return 0;
    if (!check_rb_black_height(R, &br)) return 0;
    if (bl != br){
        FAILF("black-height mismatch: left=%d right=%d key=%d", bl, br, x->key);
        return 0;
    }
    *bh_out = bl + (RIX_RB_COLOR(x, rb)==RIX_RB_BLACK ? 1 : 0);
    return 1;
}

static void inorder_collect(struct node *x, vec_t *out){
    if (!x) return;
    inorder_collect(RIX_RB_LEFT(x, g_nodes, rb), out);
    vec_insert_unique(out, x->key);
    inorder_collect(RIX_RB_RIGHT(x, g_nodes, rb), out);
}

static void check_integrity_against_model(struct tree *h, const vec_t *model, const char *tag){
    if (RIX_PTR_FROM_IDX(g_nodes, RIX_NIL) != NULL) FAIL("PTR_FROM_IDX(NIL) must be NULL");
    if (idx_from_ptr(NULL) != RIX_NIL) FAIL("IDX_FROM_PTR(NULL) must be NIL");

    struct node *root = RIX_RB_ROOT(h, g_nodes);
    if (!RIX_RB_EMPTY(h)){
        if (RIX_RB_COLOR(root, rb) != RIX_RB_BLACK) FAILF("root not black tag=%s", tag);
        if (RIX_RB_PARENT(root, g_nodes, rb) != NULL) FAILF("root parent not NULL tag=%s", tag);
    }

    int bh=0; (void)check_rb_black_height(root, &bh);

    vec_t got; vec_init(&got);
    inorder_collect(root, &got);
    if (got.n != model->n){
        FAILF("size mismatch tag=%s model=%zu got=%zu", tag, model->n, got.n);
    }
    for (size_t i=0;i<got.n;i++){
        if (got.a[i] != model->a[i]){
            FAILF("order mismatch tag=%s at %zu model=%d got=%d", tag, i, model->a[i], got.a[i]);
        }
    }

    size_t cnt=0; long long sum=0;
    struct node *it;
    RIX_RB_FOREACH(it, tree, h, g_nodes){
        cnt++; sum += it->key;
        if (np_from_idx(idx_from_ptr(it)) != it) FAIL("PTR<->IDX roundtrip failed");
    }
    long long expected_sum=0; for (size_t i=0;i<model->n;i++) expected_sum += model->a[i];
    if (cnt != model->n) FAILF("FOREACH count mismatch tag=%s", tag);
    if (sum != expected_sum) FAILF("FOREACH sum mismatch tag=%s", tag);

    cnt=0; sum=0;
    RIX_RB_FOREACH_REVERSE(it, tree, h, g_nodes){
        cnt++; sum += it->key;
    }
    if (cnt != model->n) FAILF("FOREACH_REVERSE count mismatch tag=%s", tag);
    if (sum != expected_sum) FAILF("FOREACH_REVERSE sum mismatch tag=%s", tag);

    vec_free(&got);
}

static void test_init_empty(void){
    printf("[T] init/empty\n");
    struct tree h = RIX_RB_HEAD_INITIALIZER(h);
    RIX_RB_INIT(&h);
    if (!RIX_RB_EMPTY(&h)) FAIL("INIT not empty");
    if (RIX_RB_ROOT(&h, g_nodes) != NULL) FAIL("ROOT must be NULL");
    vec_t m; vec_init(&m);
    check_integrity_against_model(&h, &m, "empty");
    vec_free(&m);
}

static void test_basic_insert_find_minmax_nextprev(void){
    printf("[T] basic insert/find/minmax/nextprev\n");
    struct tree h; RIX_RB_INIT(&h);
    vec_t m; vec_init(&m);

    for (int i=1;i<=20;i++){
        g_nodes[i-1].key = i;
        if (RIX_RB_INSERT(tree, &h, g_nodes, &g_nodes[i-1]) != NULL) FAIL("unexpected duplicate on fresh insert");
        vec_insert_unique(&m, i);
    }
    check_integrity_against_model(&h, &m, "ins_1_20");

    /* FIND/NFIND */
    struct node key;
    for (int k=0;k<=22;k++){
        key.key = k;
        struct node *f = RIX_RB_FIND(tree, &h, g_nodes, &key);
        if ((k>=1 && k<=20) && (!f || f->key!=k)) FAIL("find failed");
        if ((k<1 || k>20) && f) FAIL("find should be NULL");

        struct node *nf = RIX_RB_NFIND(tree, &h, g_nodes, &key);
        int expect;
        if (k<=1) expect = 1;
        else if (k>20) expect = 0;
        else expect = k;
        if (expect==0){ if (nf) FAIL("nfind should be NULL"); }
        else { if (!nf || nf->key != expect) FAIL("nfind mismatch"); }
    }

    struct node *mn = RIX_RB_MIN(tree, &h, g_nodes);
    struct node *mx = RIX_RB_MAX(tree, &h, g_nodes);
    if (!mn || mn->key!=1) FAIL("min mismatch");
    if (!mx || mx->key!=20) FAIL("max mismatch");
    /* 1->...->20 */
    int cur = 1;
    for (struct node *it = mn; it; it = RIX_RB_NEXT(tree, g_nodes, it), cur++){
        if (it->key != cur) FAIL("next chain mismatch");
    }
    /* 20->...->1 */
    cur = 20;
    for (struct node *it = mx; it; it = RIX_RB_PREV(tree, g_nodes, it), cur--){
        if (it->key != cur) FAIL("prev chain mismatch");
    }

    int dels[] = { 1, 20, 10, 11, 5, 17 };
    for (size_t i=0;i<sizeof(dels)/sizeof(dels[0]); i++){
        int k = dels[i];
        struct node *p = &g_nodes[k-1];
        if (!vec_erase_key(&m, k)) FAIL("model erase failed");
        if (RIX_RB_REMOVE(tree, &h, g_nodes, p) != p) FAIL("remove must return the node itself");
        check_integrity_against_model(&h, &m, "basic_removal");
    }

    for (int k=2;k<=4;k++){
        struct node *dup = RIX_RB_INSERT(tree, &h, g_nodes, &g_nodes[k-1]);
        if (!dup || dup->key!=k) FAIL("duplicate insert should return existing node");
        check_integrity_against_model(&h, &m, "dup_insert_noop");
    }

    vec_free(&m);
}

static unsigned xr = 0xC0FFEE11u;
static unsigned xorshift32(void){
    unsigned x=xr; x^=x<<13; x^=x>>17; x^=x<<5; xr=x; return x;
}
static unsigned rnd_in(unsigned lo, unsigned hi){
    return lo + (xorshift32() % (hi - lo + 1));
}

static unsigned char *present = NULL;

static void test_fuzz(unsigned seed, unsigned N, unsigned ops){
    printf("[T] fuzz seed=%u N=%u ops=%u\n", seed, N, ops);
    xr = seed ? seed : 0xC0FFEE11u;

    struct tree h; RIX_RB_INIT(&h);
    vec_t m; vec_init(&m);
    present = (unsigned char*)calloc((size_t)N+1, 1);
    if (!present){ perror("calloc present"); exit(1); }

    for (unsigned i=1;i<=N;i++){
        g_nodes[i-1].key = (int)i;
        g_nodes[i-1].rb.rbe_parent = g_nodes[i-1].rb.rbe_left = g_nodes[i-1].rb.rbe_right = RIX_NIL;
        g_nodes[i-1].rb.rbe_color  = RIX_RB_RED;
    }

    for (unsigned step=0; step<ops; step++){
        unsigned op = xorshift32()%100;
        if (op < 60){
            unsigned idx = rnd_in(1,N);
            struct node *p = &g_nodes[idx-1];

            struct node *ret = RIX_RB_INSERT(tree, &h, g_nodes, p);
            if (present[idx]){
                if (!ret || ret != p) FAIL("duplicate insert must return existing node");
            } else {
                if (ret != NULL) FAIL("fresh insert must return NULL");
                present[idx]=1;
                if (!vec_insert_unique(&m, p->key)) FAIL("model unique insert failed");
            }
        } else if (op < 85){
            unsigned idx = rnd_in(1,N);
            if (!present[idx]) continue;
            struct node *p = &g_nodes[idx-1];
            if (!vec_erase_key(&m, p->key)) FAIL("model erase failed (fuzz)");
            if (RIX_RB_REMOVE(tree, &h, g_nodes, p) != p) FAIL("remove returns self");
            present[idx]=0;
        } else {
            int k = (int)rnd_in(1, N*2);
            struct node key = { .key = k };
            struct node *f  = RIX_RB_FIND(tree, &h, g_nodes, &key);
            size_t pos = vec_lower_bound(&m, k);
            if (pos < m.n && m.a[pos]==k){
                if (!f || f->key!=k) FAIL("fuzz FIND mismatch");
            } else {
                if (f) FAIL("fuzz FIND should be NULL");
            }
            struct node *nf = RIX_RB_NFIND(tree, &h, g_nodes, &key);
            if (pos == m.n){
                if (nf) FAIL("fuzz NFIND should be NULL");
            } else {
                if (!nf || nf->key != m.a[pos]) FAIL("fuzz NFIND mismatch");
            }
        }

        if ((step & 0x3FFu) == 0u){
            check_integrity_against_model(&h, &m, "fuzz_periodic");
        }
    }

    check_integrity_against_model(&h, &m, "fuzz_final");

    free(present); present=NULL;
    vec_free(&m);
}

/* ===== main ===== */
int main(int argc, char **argv){
    unsigned seed = 0xC0FFEE11u;
    unsigned N    = 2048;
    unsigned ops  = 200000;

    if (argc>=2) seed = (unsigned)strtoul(argv[1],NULL,10);
    if (argc>=3) N    = (unsigned)strtoul(argv[2],NULL,10);
    if (argc>=4) ops  = (unsigned)strtoul(argv[3],NULL,10);

    g_cap = N;
    g_nodes = (struct node*)calloc((size_t)N, sizeof(struct node));
    if (!g_nodes){ perror("calloc g_nodes"); return 1; }

    test_init_empty();
    test_basic_insert_find_minmax_nextprev();
    test_fuzz(seed, N, ops);

    free(g_nodes);
    printf("ALL RIX_RB TESTS PASSED \n");
    return 0;
}
