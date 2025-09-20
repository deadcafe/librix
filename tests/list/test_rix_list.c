/* test_rix_list.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
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
    int val;
    RIX_LIST_ENTRY(node) link;
};

static struct node *g_nodes = NULL;
static unsigned int g_cap = 0;

#define NODEP(idx1) (&g_nodes[(idx1)-1])

typedef struct {
    unsigned int *a;
    size_t n, cap;
} vec_t;

static void vec_init(vec_t *v){ v->a=NULL; v->n=0; v->cap=0; }
static void vec_reserve(vec_t *v, size_t cap){
    if (v->cap>=cap) return;
    size_t nc = v->cap ? v->cap : 8;
    while (nc < cap) nc <<= 1;
    v->a = (unsigned int*)realloc(v->a, nc*sizeof(unsigned int));
    v->cap = nc;
}
static void vec_free(vec_t *v){ free(v->a); v->a=NULL; v->n=v->cap=0; }
static void vec_clear(vec_t *v){ v->n=0; }
static void vec_push_back(vec_t *v, unsigned int x){ vec_reserve(v, v->n+1); v->a[v->n++] = x; }
static void vec_push_front(vec_t *v, unsigned int x){
    vec_reserve(v, v->n+1);
    memmove(v->a+1, v->a, v->n*sizeof(unsigned int));
    v->a[0] = x; v->n++;
}
static ssize_t vec_find(const vec_t *v, unsigned int x){
    for (size_t i=0; i<v->n; i++) if (v->a[i]==x) return (ssize_t)i;
    return -1;
}
static void vec_insert_at(vec_t *v, size_t pos, unsigned int x){
    assert(pos<=v->n);
    vec_reserve(v, v->n+1);
    memmove(v->a+pos+1, v->a+pos, (v->n-pos)*sizeof(unsigned int));
    v->a[pos]=x; v->n++;
}
static int vec_remove_val(vec_t *v, unsigned int x){
    ssize_t p = vec_find(v,x);
    if (p<0) return 0;
    memmove(v->a+p, v->a+p+1, (v->n-p-1)*sizeof(unsigned int));
    v->n--;
    return 1;
}

RIX_LIST_HEAD(qhead, node);

static void dump_vec(const vec_t *v, const char *name){
    fprintf(stderr, "%s:", name);
    for (size_t i=0;i<v->n;i++) fprintf(stderr, " %u", v->a[i]);
    fputc('\n', stderr);
}

static void extract_forward(struct qhead *h, vec_t *out){
    vec_clear(out);
    for (struct node *it = RIX_LIST_FIRST(h, g_nodes);
         it; it = RIX_LIST_NEXT(it, g_nodes, link)) {
        vec_push_back(out, RIX_IDX_FROM_PTR(g_nodes, it));
    }
}

static void check_integrity_against_model(struct qhead *h,
                                          const vec_t *model,
                                          const char *tag)
{
    if (RIX_LIST_EMPTY(h)) {
        if (h->rlh_first != RIX_NIL)
            FAILF("EMPTY but first not NIL tag=%s", tag);
    } else {
        if (h->rlh_first == RIX_NIL)
            FAILF("NON-EMPTY but first is NIL tag=%s", tag);
    }

    vec_t fw; vec_init(&fw); extract_forward(h, &fw);
    if (fw.n != model->n) {
        dump_vec(&fw, "forward"); dump_vec(model, "model");
        FAILF("length mismatch (forward) tag=%s", tag);
    }
    for (size_t i=0; i<fw.n; i++){
        if (fw.a[i] != model->a[i]) {
            dump_vec(&fw, "forward"); dump_vec(model, "model");
            FAILF("order mismatch (forward) at i=%zu tag=%s", i, tag);
        }
    }

    if (fw.n != 0){
        unsigned first = fw.a[0], last = fw.a[fw.n-1];
        if (h->rlh_first != first) FAILF("head first mismatch tag=%s", tag);
        if (NODEP(first)->link.rle_prev != RIX_NIL) FAILF("first.prev != NIL tag=%s", tag);
        if (NODEP(last )->link.rle_next != RIX_NIL) FAILF("last.next != NIL tag=%s", tag);
    }

    for (size_t i=0; i<fw.n; i++){
        unsigned cur  = fw.a[i];
        unsigned prev = (i? fw.a[i-1] : RIX_NIL);
        unsigned next = (i+1<fw.n? fw.a[i+1] : RIX_NIL);

        if (NODEP(cur)->link.rle_prev != prev)
            FAILF("prev link broken at idx=%u tag=%s", cur, tag);
        if (NODEP(cur)->link.rle_next != next)
            FAILF("next link broken at idx=%u tag=%s", cur, tag);
        if (next != RIX_NIL && NODEP(next)->link.rle_prev != cur)
            FAILF("next->prev != cur at idx=%u tag=%s", cur, tag);

        if (RIX_PTR_FROM_IDX(g_nodes, RIX_NIL) != NULL) FAIL("PTR_FROM_IDX(NIL) must be NULL");
        if (RIX_IDX_FROM_PTR(g_nodes, (struct node*)NULL) != RIX_NIL) FAIL("IDX_FROM_PTR(NULL) must be NIL");
        if (RIX_IDX_FROM_PTR(g_nodes, NODEP(cur)) != cur) FAIL("IDX<->PTR roundtrip failed");
    }

    vec_free(&fw);
}

static void model_insert_head(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_front(m,x); }
static void model_insert_after(vec_t *m, unsigned base, unsigned x){
    ssize_t p = vec_find(m,base); assert(p>=0); assert(vec_find(m,x)<0);
    vec_insert_at(m, (size_t)p+1, x);
}
static void model_insert_before(vec_t *m, unsigned base, unsigned x){
    ssize_t p = vec_find(m,base); assert(p>=0); assert(vec_find(m,x)<0);
    vec_insert_at(m, (size_t)p, x);
}
static void model_remove_valM(vec_t *m, unsigned x){ int ok=vec_remove_val(m,x); assert(ok); }
static void model_swap(vec_t *a, vec_t *b){ vec_t t=*a; *a=*b; *b=t; }

static void list_push_tail(struct qhead *h, unsigned idx){
    if (RIX_LIST_EMPTY(h)) {
        RIX_LIST_INSERT_HEAD(h, g_nodes, NODEP(idx), link);
    } else {
        struct node *it = RIX_LIST_FIRST(h, g_nodes);
        while (RIX_LIST_NEXT(it, g_nodes, link)) it = RIX_LIST_NEXT(it, g_nodes, link);
        RIX_LIST_INSERT_AFTER(h, g_nodes, it, NODEP(idx), link);
    }
}

static void test_init_empty(void){
    printf("[T] init/empty\n");
    struct qhead h = RIX_LIST_HEAD_INITIALIZER(h);
    if (!RIX_LIST_EMPTY(&h)) FAIL("HEAD_INITIALIZER not empty");
    if (h.rlh_first!=RIX_NIL) FAIL("HEAD_INITIALIZER first not NIL");
    RIX_LIST_INIT(&h);
    if (!RIX_LIST_EMPTY(&h)) FAIL("INIT not empty");
    if (RIX_LIST_FIRST(&h, g_nodes)!=NULL) FAIL("FIRST must be NULL on empty");
}

static void test_insert_remove(void){
    printf("[T] insert/remove scenarios\n");
    struct qhead h; RIX_LIST_INIT(&h);
    vec_t m; vec_init(&m);

    unsigned a=1,b=2,c=3,d=4,e=5;

    RIX_LIST_INSERT_HEAD(&h, g_nodes, NODEP(a), link); model_insert_head(&m,a);
    check_integrity_against_model(&h,&m,"ins_head_a");

    RIX_LIST_INSERT_AFTER(&h, g_nodes, NODEP(a), NODEP(b), link); model_insert_after(&m,a,b);
    check_integrity_against_model(&h,&m,"after_a_b");

    RIX_LIST_INSERT_BEFORE(&h, g_nodes, NODEP(a), NODEP(c), link); model_insert_before(&m,a,c);
    check_integrity_against_model(&h,&m,"before_a_c");

    RIX_LIST_INSERT_AFTER(&h, g_nodes, NODEP(b), NODEP(d), link); model_insert_after(&m,b,d);
    check_integrity_against_model(&h,&m,"after_b_d");

    RIX_LIST_INSERT_BEFORE(&h, g_nodes, NODEP(d), NODEP(e), link); model_insert_before(&m,d,e);
    check_integrity_against_model(&h,&m,"before_d_e");

    RIX_LIST_REMOVE(&h, g_nodes, NODEP(c), link); model_remove_valM(&m,c);
    check_integrity_against_model(&h,&m,"rm_head_c");

    RIX_LIST_REMOVE(&h, g_nodes, NODEP(d), link); model_remove_valM(&m,d);
    check_integrity_against_model(&h,&m,"rm_mid_d");

    RIX_LIST_REMOVE(&h, g_nodes, NODEP(b), link); model_remove_valM(&m,b);
    check_integrity_against_model(&h,&m,"rm_mid_b");

    RIX_LIST_REMOVE(&h, g_nodes, NODEP(e), link); model_remove_valM(&m,e);
    check_integrity_against_model(&h,&m,"rm_tail_e");

    RIX_LIST_REMOVE(&h, g_nodes, NODEP(a), link); model_remove_valM(&m,a);
    check_integrity_against_model(&h,&m,"rm_last_a");

    vec_free(&m);
}

static void test_foreach_safe(void){
    printf("[T] foreach/safe\n");
    struct qhead h; RIX_LIST_INIT(&h);
    vec_t m; vec_init(&m);

    for (unsigned i=1;i<=16;i++){
        list_push_tail(&h, i);
        vec_push_back(&m, i);
    }
    check_integrity_against_model(&h,&m,"fill_1_16");

    struct node *it,*tmp;
    RIX_LIST_FOREACH_SAFE(it, &h, g_nodes, link, tmp){
        unsigned idx = RIX_IDX_FROM_PTR(g_nodes, it);
        if ((idx & 1u)==0){
            RIX_LIST_REMOVE(&h, g_nodes, it, link);
            model_remove_valM(&m, idx);
        }
    }
    check_integrity_against_model(&h,&m,"remove_evens_safe");

    unsigned long sum=0, expected=0;
    for (it = RIX_LIST_FIRST(&h, g_nodes); it; it = RIX_LIST_NEXT(it, g_nodes, link))
        sum += RIX_IDX_FROM_PTR(g_nodes, it);
    for (size_t i=0;i<m.n;i++) expected += m.a[i];
    if (sum!=expected) FAIL("foreach sum mismatch");

    vec_free(&m);
}

static void test_swap(void){
    printf("[T] swap\n");
    struct qhead h1, h2; RIX_LIST_INIT(&h1); RIX_LIST_INIT(&h2);
    vec_t m1, m2; vec_init(&m1); vec_init(&m2);

    for (unsigned i=1;i<=5;i++){ list_push_tail(&h1, i); vec_push_back(&m1,i); }
    for (unsigned i=6;i<=10;i++){ list_push_tail(&h2, i); vec_push_back(&m2,i); }
    check_integrity_against_model(&h1,&m1,"h1_init");
    check_integrity_against_model(&h2,&m2,"h2_init");

    RIX_LIST_SWAP(&h1,&h2,g_nodes,node,link); model_swap(&m1,&m2);
    check_integrity_against_model(&h1,&m1,"swap1");
    check_integrity_against_model(&h2,&m2,"swap2");

    for (unsigned i=11;i<=15;i++){ list_push_tail(&h1, i); vec_push_back(&m1,i); }
    for (unsigned i=16;i<=20;i++){ RIX_LIST_INSERT_HEAD(&h2, g_nodes, NODEP(i), link); model_insert_head(&m2,i); }
    check_integrity_against_model(&h1,&m1,"pre_swapA");
    check_integrity_against_model(&h2,&m2,"pre_swapB");

    RIX_LIST_SWAP(&h1,&h2,g_nodes,node,link); model_swap(&m1,&m2);
    check_integrity_against_model(&h1,&m1,"swapA");
    check_integrity_against_model(&h2,&m2,"swapB");

    vec_free(&m1); vec_free(&m2);
}

static unsigned xrnd_state = 0x12345678u;
static unsigned xrnd(void){ unsigned x=xrnd_state; x^=x<<13; x^=x>>17; x^=x<<5; xrnd_state=x; return x; }
static unsigned rnd_in(unsigned lo, unsigned hi){ return lo + (xrnd() % (hi - lo + 1)); }

static void test_fuzz(unsigned seed, unsigned N, unsigned ops){
    printf("[T] fuzz seed=%u N=%u ops=%u\n", seed, N, ops);
    xrnd_state = seed ? seed : 0xDEADBABEu;

    struct qhead hA, hB; RIX_LIST_INIT(&hA); RIX_LIST_INIT(&hB);
    vec_t mA, mB; vec_init(&mA); vec_init(&mB);
    unsigned char *where = (unsigned char*)calloc((size_t)g_cap+1,1); /* 1-origin: 0..g_cap */

    for (unsigned step=0; step<ops; step++){
        unsigned op = xrnd()%100;

        if (op<6){
            /* SWAP */
            RIX_LIST_SWAP(&hA,&hB,g_nodes,node,link); model_swap(&mA,&mB);
            memset(where,0,(size_t)g_cap+1);
            for (size_t i=0;i<mA.n;i++) where[mA.a[i]]=1;
            for (size_t i=0;i<mB.n;i++) where[mB.a[i]]=2;
            check_integrity_against_model(&hA,&mA,"fuzz_swap_A");
            check_integrity_against_model(&hB,&mB,"fuzz_swap_B");
            continue;
        }

        struct qhead *h = (xrnd()&1) ? &hA : &hB;
        vec_t *m        = (h==&hA) ? &mA : &mB;
        unsigned char myid = (h==&hA) ? 1 : 2;

        if (op<40){
            unsigned idx = rnd_in(1, N);
            if (where[idx]!=0){
                if (where[idx]==1){ RIX_LIST_REMOVE(&hA, g_nodes, NODEP(idx), link); model_remove_valM(&mA, idx); }
                else              { RIX_LIST_REMOVE(&hB, g_nodes, NODEP(idx), link); model_remove_valM(&mB, idx); }
                where[idx]=0;
            }
            if (m->n==0 || (xrnd()%3)==0){
                RIX_LIST_INSERT_HEAD(h, g_nodes, NODEP(idx), link); model_insert_head(m, idx);
            } else if ((xrnd()%2)==0){
                unsigned base = m->a[xrnd()%m->n];
                RIX_LIST_INSERT_AFTER(h, g_nodes, NODEP(base), NODEP(idx), link);
                model_insert_after(m, base, idx);
            } else {
                unsigned base = m->a[xrnd()%m->n];
                RIX_LIST_INSERT_BEFORE(h, g_nodes, NODEP(base), NODEP(idx), link);
                model_insert_before(m, base, idx);
            }
            where[idx]=myid;
            check_integrity_against_model(h,m,"fuzz_insert");
        } else if (op<75){
            if (m->n>0){
                if ((xrnd()%2)==0){
                    unsigned idx = m->a[0];
                    RIX_LIST_REMOVE(h, g_nodes, NODEP(idx), link);
                    model_remove_valM(m, idx);
                    where[idx]=0;
                } else {
                    unsigned idx = m->a[xrnd()%m->n];
                    RIX_LIST_REMOVE(h, g_nodes, NODEP(idx), link);
                    model_remove_valM(m, idx);
                    where[idx]=0;
                }
                check_integrity_against_model(h,m,"fuzz_remove");
            }
        } else {
            unsigned long sum=0, expected=0;
            for (struct node *it2 = RIX_LIST_FIRST(h, g_nodes);
                 it2; it2 = RIX_LIST_NEXT(it2, g_nodes, link))
                sum += RIX_IDX_FROM_PTR(g_nodes, it2);
            for (size_t i=0;i<m->n;i++) expected += m->a[i];
            if (sum!=expected) FAIL("fuzz foreach sum mismatch");
            check_integrity_against_model(h,m,"fuzz_walk_check");
        }
    }

    free(where);
    vec_free(&mA); vec_free(&mB);
}

/* ===== main ===== */
int main(int argc, char **argv){
    unsigned seed = 0x13572468u;
    unsigned N    = 128;
    unsigned ops  = 200000;

    if (argc>=2) seed = (unsigned)strtoul(argv[1],NULL,10);
    if (argc>=3) N    = (unsigned)strtoul(argv[2],NULL,10);
    if (argc>=4) ops  = (unsigned)strtoul(argv[3],NULL,10);

    g_cap = N;
    g_nodes = (struct node*)calloc((size_t)N, sizeof(struct node)); /* 0..N-1 <- idx=1..N */
    if (!g_nodes){ perror("calloc g_nodes"); return 1; }
    for (unsigned i=0;i<N;i++){
        g_nodes[i].val = (int)(i+1);
        g_nodes[i].link.rle_next = RIX_NIL;
        g_nodes[i].link.rle_prev = RIX_NIL;
    }

    test_init_empty();
    test_insert_remove();
    test_foreach_safe();
    test_swap();
    test_fuzz(seed, N, ops);

    free(g_nodes);
    printf("ALL RIX_LIST TESTS PASSED \n");
    return 0;
}
