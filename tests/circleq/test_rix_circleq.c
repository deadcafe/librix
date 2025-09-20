/* test_rix_circleq.c
 *  RIX_CIRCLEQ true circular, single-head, 1-origin index, NIL=0, array[index-1]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    int val;
    RIX_CIRCLEQ_ENTRY(node) link; /* rce_next / rce_prev: 1-origin index, NIL=0 */
};

static struct node *g_nodes = NULL;
static unsigned int g_cap = 0;

#define NODEP(i1) (&g_nodes[(i1)-1])

typedef struct { unsigned *a; size_t n, cap; } vec_t;
static void vec_init(vec_t *v){ v->a=NULL; v->n=0; v->cap=0; }
static void vec_reserve(vec_t *v, size_t cap){
    if (v->cap>=cap) return;
    size_t nc = v->cap ? v->cap : 8;
    while (nc < cap) nc <<= 1;
    v->a = (unsigned*)realloc(v->a, nc*sizeof(unsigned));
    v->cap = nc;
}
static void vec_free(vec_t *v){ free(v->a); v->a=NULL; v->n=v->cap=0; }
static void vec_clear(vec_t *v){ v->n=0; }
static void vec_push_back(vec_t *v, unsigned x){ vec_reserve(v, v->n+1); v->a[v->n++]=x; }
static void vec_push_front(vec_t *v, unsigned x){
    vec_reserve(v, v->n+1);
    memmove(v->a+1, v->a, v->n*sizeof(unsigned));
    v->a[0]=x; v->n++;
}
static ssize_t vec_find(const vec_t *v, unsigned x){ for (size_t i=0;i<v->n;i++) if (v->a[i]==x) return (ssize_t)i; return -1; }
static void vec_insert_at(vec_t *v, size_t pos, unsigned x){
    assert(pos<=v->n);
    vec_reserve(v, v->n+1);
    memmove(v->a+pos+1, v->a+pos, (v->n-pos)*sizeof(unsigned));
    v->a[pos]=x; v->n++;
}
static int vec_remove_val(vec_t *v, unsigned x){
    ssize_t p = vec_find(v,x); if (p<0) return 0;
    memmove(v->a+p, v->a+p+1, (v->n-p-1)*sizeof(unsigned)); v->n--; return 1;
}
static void dump_vec(const vec_t *v, const char *name){
    fprintf(stderr, "%s:", name);
    for (size_t i=0;i<v->n;i++) fprintf(stderr," %u", v->a[i]);
    fputc('\n', stderr);
}

RIX_CIRCLEQ_HEAD(qhead, node);

static void extract_forward(struct qhead *h, vec_t *out){
    vec_clear(out);
    struct node *start = RIX_CIRCLEQ_FIRST(h, g_nodes);
    if (!start) return;
    struct node *it = start;
    do {
        vec_push_back(out, RIX_IDX_FROM_PTR(g_nodes, it));
        it = RIX_CIRCLEQ_NEXT(it, g_nodes, link);
    } while (it != start);
}

static void extract_reverse(struct qhead *h, vec_t *out){
    vec_clear(out);
    struct node *start = RIX_CIRCLEQ_LAST(h, g_nodes, link);
    if (!start) return;
    struct node *it = start;
    do {
        vec_push_back(out, RIX_IDX_FROM_PTR(g_nodes, it));
        it = RIX_CIRCLEQ_PREV(it, g_nodes, link);
    } while (it != start);
}

static void check_integrity_against_model(struct qhead *h,
                                          const vec_t *model,
                                          const char *tag)
{
    if (RIX_CIRCLEQ_EMPTY(h)) {
        if (h->rch_first != RIX_NIL) FAILF("EMPTY but head not NIL tag=%s", tag);
    } else {
        if (h->rch_first == RIX_NIL)  FAILF("NON-EMPTY but first is NIL tag=%s", tag);
    }

    vec_t fw; vec_init(&fw); extract_forward(h, &fw);
    if (fw.n != model->n) { dump_vec(&fw,"forward"); dump_vec(model,"model"); FAILF("length mismatch (forward) tag=%s", tag); }
    for (size_t i=0;i<fw.n;i++){
        if (fw.a[i] != model->a[i]) { dump_vec(&fw,"forward"); dump_vec(model,"model"); FAILF("order mismatch (forward) at %zu tag=%s", i, tag); }
    }

    vec_t rv; vec_init(&rv); extract_reverse(h, &rv);
    if (rv.n != model->n) { dump_vec(&rv,"reverse"); dump_vec(model,"model"); FAILF("length mismatch (reverse) tag=%s", tag); }
    for (size_t i=0;i<rv.n;i++){
        if (rv.a[i] != model->a[(model->n-1)-i]) { dump_vec(&rv,"reverse"); dump_vec(model,"model"); FAILF("order mismatch (reverse) at %zu tag=%s", i, tag); }
    }

    if (fw.n == 0){ vec_free(&fw); vec_free(&rv); return; }

    unsigned first = fw.a[0], last = fw.a[fw.n-1];
    struct node *pfirst = RIX_CIRCLEQ_FIRST(h, g_nodes);
    struct node *plast  = RIX_CIRCLEQ_LAST(h, g_nodes, link);
    if (RIX_IDX_FROM_PTR(g_nodes, pfirst) != first) FAILF("first ptr mismatch tag=%s", tag);
    if (RIX_IDX_FROM_PTR(g_nodes, plast ) != last ) FAILF("last  ptr mismatch tag=%s", tag);
    if (NODEP(last)->link.rce_next != first)  FAILF("ring broken: last->next != first tag=%s", tag);
    if (NODEP(first)->link.rce_prev != last)  FAILF("ring broken: first->prev != last tag=%s", tag);

    for (size_t i=0;i<fw.n;i++){
        unsigned cur  = fw.a[i];
        unsigned prev = fw.a[(i + fw.n - 1) % fw.n];
        unsigned next = fw.a[(i + 1) % fw.n];
        if (NODEP(cur)->link.rce_prev != prev) FAILF("prev link broken at %u tag=%s", cur, tag);
        if (NODEP(cur)->link.rce_next != next) FAILF("next link broken at %u tag=%s", cur, tag);
        if (RIX_PTR_FROM_IDX(g_nodes, RIX_NIL) != NULL) FAIL("PTR_FROM_IDX(NIL) must be NULL");
        if (RIX_IDX_FROM_PTR(g_nodes, (struct node*)NULL) != RIX_NIL) FAIL("IDX_FROM_PTR(NULL) must be NIL");
        if (RIX_IDX_FROM_PTR(g_nodes, NODEP(cur)) != cur) FAIL("IDX<->PTR roundtrip failed");
    }

    vec_free(&fw); vec_free(&rv);
}

static void model_insert_head(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_front(m,x); }
static void model_insert_tail(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_back(m,x); }
static void model_insert_after(vec_t *m, unsigned base, unsigned x){
    ssize_t p = vec_find(m, base); assert(p>=0); assert(vec_find(m,x)<0); vec_insert_at(m, (size_t)p+1, x);
}
static void model_insert_before(vec_t *m, unsigned base, unsigned x){
    ssize_t p = vec_find(m, base); assert(p>=0); assert(vec_find(m,x)<0); vec_insert_at(m, (size_t)p, x);
}
static void model_remove_valM(vec_t *m, unsigned x){ int ok = vec_remove_val(m, x); assert(ok); }

static unsigned xrnd_state = 0x12345678u;
static unsigned xrnd(void){ unsigned x=xrnd_state; x^=x<<13; x^=x>>17; x^=x<<5; xrnd_state=x; return x; }
static unsigned rnd_in(unsigned lo, unsigned hi){ return lo + (xrnd() % (hi - lo + 1)); }

static void cq_push_tail(struct qhead *h, unsigned idx){
    if (RIX_CIRCLEQ_EMPTY(h)) RIX_CIRCLEQ_INSERT_HEAD(h, g_nodes, NODEP(idx), link);
    else                      RIX_CIRCLEQ_INSERT_TAIL(h, g_nodes, NODEP(idx), link);
}

static void test_init_singleton(void){
    printf("[T] init/singleton\n");
    struct qhead h = RIX_CIRCLEQ_HEAD_INITIALIZER(h);
    if (!RIX_CIRCLEQ_EMPTY(&h)) FAIL("HEAD_INITIALIZER not empty");
    RIX_CIRCLEQ_INIT(&h);
    if (!RIX_CIRCLEQ_EMPTY(&h)) FAIL("INIT not empty");
    if (RIX_CIRCLEQ_FIRST(&h, g_nodes) != NULL) FAIL("FIRST must be NULL on empty");
    if (RIX_CIRCLEQ_LAST(&h, g_nodes, link)  != NULL) FAIL("LAST must be NULL on empty");

    unsigned a = 1;
    RIX_CIRCLEQ_INSERT_HEAD(&h, g_nodes, NODEP(a), link);
    vec_t m; vec_init(&m); model_insert_head(&m, a);
    check_integrity_against_model(&h,&m,"singleton_insert_head");
    struct node *pa = RIX_CIRCLEQ_FIRST(&h, g_nodes);
    if (RIX_CIRCLEQ_NEXT(pa, g_nodes, link) != pa) FAIL("singleton next not self");
    if (RIX_CIRCLEQ_PREV(pa, g_nodes, link) != pa) FAIL("singleton prev not self");
    RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(a), link); model_remove_valM(&m, a);
    check_integrity_against_model(&h,&m,"singleton_remove");
    vec_free(&m);
}

static void test_insert_remove_basic(void){
    printf("[T] insert/remove basic\n");
    struct qhead h; RIX_CIRCLEQ_INIT(&h);
    vec_t m; vec_init(&m);

    unsigned a=1,b=2,c=3,d=4,e=5;

    RIX_CIRCLEQ_INSERT_HEAD(&h, g_nodes, NODEP(a), link); model_insert_head(&m,a);
    check_integrity_against_model(&h,&m,"ins_head_a");

    RIX_CIRCLEQ_INSERT_TAIL(&h, g_nodes, NODEP(b), link); model_insert_tail(&m,b);
    check_integrity_against_model(&h,&m,"ins_tail_b");

    RIX_CIRCLEQ_INSERT_HEAD(&h, g_nodes, NODEP(c), link); model_insert_head(&m,c);
    check_integrity_against_model(&h,&m,"ins_head_c");

    RIX_CIRCLEQ_INSERT_AFTER(&h, g_nodes, NODEP(b), NODEP(d), link); model_insert_after(&m,b,d);
    check_integrity_against_model(&h,&m,"after_b_d");

    RIX_CIRCLEQ_INSERT_BEFORE(&h, g_nodes, NODEP(d), NODEP(e), link); model_insert_before(&m,d,e);
    check_integrity_against_model(&h,&m,"before_d_e");

    RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(c), link); model_remove_valM(&m,c);
    check_integrity_against_model(&h,&m,"rm_head_c");

    RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(d), link); model_remove_valM(&m,d);
    check_integrity_against_model(&h,&m,"rm_mid_d");

    RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(b), link); model_remove_valM(&m,b);
    check_integrity_against_model(&h,&m,"rm_tail_b");

    RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(e), link); model_remove_valM(&m,e);
    check_integrity_against_model(&h,&m,"rm_last_e");

    RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(a), link); model_remove_valM(&m,a);
    check_integrity_against_model(&h,&m,"rm_last_a");

    vec_free(&m);
}

static void test_foreach_and_safe(void){
    printf("[T] foreach/safe (+reverse)\n");
    struct qhead h; RIX_CIRCLEQ_INIT(&h);
    vec_t m; vec_init(&m);

    for (unsigned i=1;i<=16;i++){ cq_push_tail(&h,i); model_insert_tail(&m,i); }
    check_integrity_against_model(&h,&m,"fill_1_16");

    unsigned long sum=0, expected=0, cnt=0;
    struct node *it;
    RIX_CIRCLEQ_FOREACH(it, &h, g_nodes, link){ sum += RIX_IDX_FROM_PTR(g_nodes,it); cnt++; }
    for (size_t i=0;i<m.n;i++) expected += m.a[i];
    if (cnt != m.n) FAIL("foreach count mismatch");
    if (sum != expected) FAIL("foreach sum mismatch");

    unsigned long rsum=0; cnt=0;
    RIX_CIRCLEQ_FOREACH_REVERSE(it, &h, g_nodes, link){ rsum += RIX_IDX_FROM_PTR(g_nodes,it); cnt++; }
    if (cnt != m.n) FAIL("reverse foreach count mismatch");
    if (rsum != expected) FAIL("reverse foreach sum mismatch");

    struct node *tmp;
    RIX_CIRCLEQ_FOREACH_SAFE(it, &h, g_nodes, link, tmp){
        unsigned idx = RIX_IDX_FROM_PTR(g_nodes, it);
        if ((idx & 1u)==0){
            RIX_CIRCLEQ_REMOVE(&h, g_nodes, it, link);
            model_remove_valM(&m, idx);
        }
    }
    (void)tmp;
    check_integrity_against_model(&h,&m,"safe_remove_current_evens");

    RIX_CIRCLEQ_FOREACH_SAFE(it, &h, g_nodes, link, tmp){
        struct node *nx = tmp;
        if (nx){
            unsigned idx = RIX_IDX_FROM_PTR(g_nodes, nx);
            if ((idx % 3u) == 0){
                RIX_CIRCLEQ_REMOVE(&h, g_nodes, nx, link);
                model_remove_valM(&m, idx);
                tmp = RIX_CIRCLEQ_NEXT(it, g_nodes, link);
            }
        }
    }
    (void)tmp;
    check_integrity_against_model(&h,&m,"safe_remove_next_multiples_of_3");

    struct node *rtmp;
    RIX_CIRCLEQ_FOREACH_REVERSE_SAFE(it, &h, g_nodes, link, rtmp){
        unsigned idx = RIX_IDX_FROM_PTR(g_nodes, it);
        if ((idx & 1u)==1){
            RIX_CIRCLEQ_REMOVE(&h, g_nodes, it, link);
            model_remove_valM(&m, idx);
        }
    }
    (void)rtmp;
    check_integrity_against_model(&h,&m,"safe_reverse_remove_current_odds");

    while (!RIX_CIRCLEQ_EMPTY(&h)){
        struct node *first = RIX_CIRCLEQ_FIRST(&h, g_nodes);
        unsigned idx = RIX_IDX_FROM_PTR(g_nodes, first);
        RIX_CIRCLEQ_REMOVE(&h, g_nodes, first, link);
        model_remove_valM(&m, idx);
    }
    check_integrity_against_model(&h,&m,"clear_all");

    vec_free(&m);
}

static void test_fuzz(unsigned seed, unsigned N, unsigned ops){
    printf("[T] fuzz seed=%u N=%u ops=%u\n", seed, N, ops);
    xrnd_state = seed ? seed : 0xA5A5A5A5u;

    struct qhead h; RIX_CIRCLEQ_INIT(&h);
    vec_t m; vec_init(&m);

    for (unsigned step=0; step<ops; step++){
        unsigned op = xrnd()%100;

        if (op < 55){
            unsigned idx = rnd_in(1,N);
            if (vec_find(&m,idx) >= 0){ RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(idx), link); model_remove_valM(&m,idx); }
            if (m.n==0 || (xrnd()%4)==0){
                RIX_CIRCLEQ_INSERT_HEAD(&h, g_nodes, NODEP(idx), link); model_insert_head(&m, idx);
            } else if ((xrnd()%3)==0){
                RIX_CIRCLEQ_INSERT_TAIL(&h, g_nodes, NODEP(idx), link); model_insert_tail(&m, idx);
            } else if ((xrnd()%2)==0){
                unsigned base = m.a[xrnd()%m.n];
                RIX_CIRCLEQ_INSERT_AFTER(&h, g_nodes, NODEP(base), NODEP(idx), link); model_insert_after(&m, base, idx);
            } else {
                unsigned base = m.a[xrnd()%m.n];
                RIX_CIRCLEQ_INSERT_BEFORE(&h, g_nodes, NODEP(base), NODEP(idx), link); model_insert_before(&m, base, idx);
            }
            check_integrity_against_model(&h,&m,"fuzz_insert");
        } else {
            if (m.n==0) continue;
            unsigned mode = xrnd()%3;
            if (mode==0){ unsigned idx = m.a[0];      RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(idx), link); model_remove_valM(&m, idx); }
            else if (mode==1){ unsigned idx = m.a[m.n-1]; RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(idx), link); model_remove_valM(&m, idx); }
            else { unsigned idx = m.a[xrnd()%m.n];   RIX_CIRCLEQ_REMOVE(&h, g_nodes, NODEP(idx), link); model_remove_valM(&m, idx); }
            check_integrity_against_model(&h,&m,"fuzz_remove");
        }

        if ((step & 1023u)==0u){
            check_integrity_against_model(&h,&m,"fuzz_periodic");
        }
    }

    check_integrity_against_model(&h,&m,"fuzz_final");
    vec_free(&m);
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
    g_nodes = (struct node*)calloc((size_t)N, sizeof(struct node));
    if (!g_nodes){ perror("calloc g_nodes"); return 1; }
    for (unsigned i=0;i<N;i++){
        g_nodes[i].val = (int)(i+1);
        g_nodes[i].link.rce_next = RIX_NIL;
        g_nodes[i].link.rce_prev = RIX_NIL;
    }

    test_init_singleton();
    test_insert_remove_basic();
    test_foreach_and_safe();
    test_fuzz(seed, N, ops);

    free(g_nodes);
    printf("ALL RIX_CIRCLEQ TESTS PASSED \n");
    return 0;
}
