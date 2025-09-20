/* test_rix_slist.c
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
    RIX_SLIST_ENTRY(node) link;
};

static struct node *g_nodes = NULL;
static unsigned int g_cap = 0;

#define NODEP(i1) (&g_nodes[(i1)-1])

typedef struct {
    unsigned *a;
    size_t n, cap;
} vec_t;

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
static ssize_t vec_find(const vec_t *v, unsigned x){
    for (size_t i=0;i<v->n;i++) if (v->a[i]==x) return (ssize_t)i;
    return -1;
}
static void vec_insert_at(vec_t *v, size_t pos, unsigned x){
    assert(pos<=v->n);
    vec_reserve(v, v->n+1);
    memmove(v->a+pos+1, v->a+pos, (v->n-pos)*sizeof(unsigned));
    v->a[pos]=x; v->n++;
}
static int vec_remove_val(vec_t *v, unsigned x){
    ssize_t p = vec_find(v,x);
    if (p<0) return 0;
    memmove(v->a+p, v->a+p+1, (v->n-p-1)*sizeof(unsigned));
    v->n--;
    return 1;
}
static void dump_vec(const vec_t *v, const char *name){
    fprintf(stderr, "%s:", name);
    for (size_t i=0;i<v->n;i++) fprintf(stderr," %u", v->a[i]);
    fputc('\n', stderr);
}

RIX_SLIST_HEAD(qhead, node);

static void extract_forward(struct qhead *h, vec_t *out){
    vec_clear(out);
    for (struct node *it = RIX_SLIST_FIRST(h, g_nodes);
         it; it = RIX_SLIST_NEXT(it, g_nodes, link)) {
        vec_push_back(out, RIX_IDX_FROM_PTR(g_nodes, it));
    }
}

static void check_integrity_against_model(struct qhead *h,
                                          const vec_t *model,
                                          const char *tag)
{
    if (RIX_SLIST_EMPTY(h)) {
        if (h->rslh_first != RIX_NIL) FAILF("EMPTY but first not NIL tag=%s", tag);
    } else {
        if (h->rslh_first == RIX_NIL) FAILF("NON-EMPTY but first is NIL tag=%s", tag);
    }

    vec_t fw; vec_init(&fw); extract_forward(h, &fw);
    if (fw.n != model->n) {
        dump_vec(&fw, "forward"); dump_vec(model, "model");
        FAILF("length mismatch (forward) tag=%s", tag);
    }
    for (size_t i=0;i<fw.n;i++){
        if (fw.a[i] != model->a[i]) {
            dump_vec(&fw, "forward"); dump_vec(model, "model");
            FAILF("order mismatch (forward) at i=%zu tag=%s", i, tag);
        }
    }

    if (fw.n != 0){
        unsigned first = fw.a[0], last = fw.a[fw.n-1];
        if (h->rslh_first != first) FAILF("head first mismatch tag=%s", tag);
        if (NODEP(last)->link.rsle_next != RIX_NIL) FAILF("last.next != NIL tag=%s", tag);
    }

    for (size_t i=0;i<fw.n;i++){
        unsigned cur  = fw.a[i];
        unsigned next = (i+1<fw.n)? fw.a[i+1] : RIX_NIL;
        if (NODEP(cur)->link.rsle_next != next)
            FAILF("next link broken at idx=%u tag=%s", cur, tag);

        if (RIX_PTR_FROM_IDX(g_nodes, RIX_NIL) != NULL) FAIL("PTR_FROM_IDX(NIL) must be NULL");
        if (RIX_IDX_FROM_PTR(g_nodes, (struct node*)NULL) != RIX_NIL) FAIL("IDX_FROM_PTR(NULL) must be NIL");
        if (RIX_IDX_FROM_PTR(g_nodes, NODEP(cur)) != cur) FAIL("IDX<->PTR roundtrip failed");
    }

    vec_free(&fw);
}

static void model_insert_head(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_front(m,x); }
static void model_insert_after(vec_t *m, unsigned base, unsigned x){
    ssize_t p = vec_find(m, base); assert(p>=0); assert(vec_find(m,x)<0);
    vec_insert_at(m, (size_t)p+1, x);
}
static void model_remove_head(vec_t *m){
    if (m->n){ memmove(m->a, m->a+1, (m->n-1)*sizeof(unsigned)); m->n--; }
}
static void model_remove_after(vec_t *m, unsigned base){
    ssize_t p = vec_find(m, base); assert(p>=0);
    if ((size_t)p+1 < m->n){
        memmove(m->a+p+1, m->a+p+2, (m->n-p-2)*sizeof(unsigned));
        m->n--;
    }
}
static void model_remove_valM(vec_t *m, unsigned x){ int ok=vec_remove_val(m,x); assert(ok); }

static void slist_push_tail(struct qhead *h, unsigned idx){
    if (RIX_SLIST_EMPTY(h)){
        RIX_SLIST_INSERT_HEAD(h, g_nodes, NODEP(idx), link);
        return;
    }
    struct node *it = RIX_SLIST_FIRST(h, g_nodes);
    while (RIX_SLIST_NEXT(it, g_nodes, link)) it = RIX_SLIST_NEXT(it, g_nodes, link);
    RIX_SLIST_INSERT_AFTER(g_nodes, it, NODEP(idx), link);
}

static unsigned xrnd_state = 0x12345678u;
static unsigned xrnd(void){
    unsigned x = xrnd_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xrnd_state = x;
    return x;
}
static unsigned rnd_in(unsigned lo, unsigned hi){
    return lo + (xrnd() % (hi - lo + 1));
}

static void test_init_empty(void){
    printf("[T] init/empty\n");
    struct qhead h = RIX_SLIST_HEAD_INITIALIZER(h);
    if (!RIX_SLIST_EMPTY(&h)) FAIL("HEAD_INITIALIZER not empty");
    if (h.rslh_first != RIX_NIL) FAIL("HEAD_INITIALIZER first not NIL");
    RIX_SLIST_INIT(&h);
    if (!RIX_SLIST_EMPTY(&h)) FAIL("INIT not empty");
    if (RIX_SLIST_FIRST(&h, g_nodes) != NULL) FAIL("FIRST must be NULL on empty");
}

static void test_insert_remove_basic(void){
    printf("[T] insert/remove basic\n");
    struct qhead h; RIX_SLIST_INIT(&h);
    vec_t m; vec_init(&m);

    unsigned a=1,b=2,c=3,d=4,e=5;

    RIX_SLIST_INSERT_HEAD(&h, g_nodes, NODEP(a), link); model_insert_head(&m, a);
    check_integrity_against_model(&h,&m,"ins_head_a");

    RIX_SLIST_INSERT_AFTER(g_nodes, NODEP(a), NODEP(b), link); model_insert_after(&m, a, b);
    check_integrity_against_model(&h,&m,"after_a_b");

    RIX_SLIST_INSERT_HEAD(&h, g_nodes, NODEP(c), link); model_insert_head(&m, c);
    check_integrity_against_model(&h,&m,"ins_head_c");

    RIX_SLIST_INSERT_AFTER(g_nodes, NODEP(b), NODEP(d), link); model_insert_after(&m, b, d);
    check_integrity_against_model(&h,&m,"after_b_d");

    slist_push_tail(&h, e); model_insert_after(&m, d, e);
    check_integrity_against_model(&h,&m,"push_tail_e");

    RIX_SLIST_REMOVE_HEAD(&h, g_nodes, link); model_remove_head(&m);
    check_integrity_against_model(&h,&m,"rm_head");

    RIX_SLIST_REMOVE_AFTER(g_nodes, NODEP(a), link); model_remove_after(&m, a);
    check_integrity_against_model(&h,&m,"rm_after_a");

    RIX_SLIST_REMOVE(&h, g_nodes, NODEP(d), node, link); model_remove_valM(&m, d);
    check_integrity_against_model(&h,&m,"rm_val_d");

    vec_free(&m);
}

static void test_foreach_safe_and_previndex(void){
    printf("[T] foreach/safe/previndex\n");
    struct qhead h; RIX_SLIST_INIT(&h);
    vec_t m; vec_init(&m);

    for (unsigned i=1;i<=16;i++){ slist_push_tail(&h, i); vec_push_back(&m, i); }
    check_integrity_against_model(&h,&m,"fill_1_16");

    struct node *it, *tmp;
    RIX_SLIST_FOREACH_SAFE(it, &h, g_nodes, link, tmp){
        unsigned idx = RIX_IDX_FROM_PTR(g_nodes, it);
        if ((idx & 1u)==0){
            RIX_SLIST_REMOVE(&h, g_nodes, it, node, link);
            model_remove_valM(&m, idx);
        }
    }
    check_integrity_against_model(&h,&m,"safe_remove_evens");

    {
        unsigned *px = &h.rslh_first;
        while (*px != RIX_NIL) {
            struct node *cur = RIX_PTR_FROM_IDX(g_nodes, *px);
            *px = cur->link.rsle_next;
        }
    }
    vec_clear(&m);
    check_integrity_against_model(&h,&m,"previndex_clear_all");

    vec_free(&m);
}

static void test_fuzz(unsigned seed, unsigned N, unsigned ops){
    printf("[T] fuzz seed=%u N=%u ops=%u\n", seed, N, ops);
    xrnd_state = seed ? seed : 0xC0FFEE11u;

    struct qhead h; RIX_SLIST_INIT(&h);
    vec_t m; vec_init(&m);

    for (unsigned step=0; step<ops; step++){
        unsigned op = xrnd()%100;

        if (op < 45){
            unsigned idx = rnd_in(1,N);
            ssize_t pos = vec_find(&m, idx);
            if (pos >= 0){
                RIX_SLIST_REMOVE(&h, g_nodes, NODEP(idx), node, link);
                model_remove_valM(&m, idx);
            }
            if (m.n==0 || (xrnd()%2)==0){
                RIX_SLIST_INSERT_HEAD(&h, g_nodes, NODEP(idx), link); model_insert_head(&m, idx);
            } else {
                unsigned base = m.a[xrnd()%m.n];
                RIX_SLIST_INSERT_AFTER(g_nodes, NODEP(base), NODEP(idx), link);
                model_insert_after(&m, base, idx);
            }
            check_integrity_against_model(&h,&m,"fuzz_insert");
        } else {
            if (m.n==0) continue;
            unsigned mode = xrnd()%3;
            if (mode==0){
                RIX_SLIST_REMOVE_HEAD(&h, g_nodes, link);
                model_remove_head(&m);
            } else if (mode==1 && m.n>=2){
                size_t p = (size_t)(xrnd()%(m.n-1));
                unsigned base = m.a[p];
                RIX_SLIST_REMOVE_AFTER(g_nodes, NODEP(base), link);
                model_remove_after(&m, base);
            } else {
                unsigned idx = m.a[xrnd()%m.n];
                RIX_SLIST_REMOVE(&h, g_nodes, NODEP(idx), node, link);
                model_remove_valM(&m, idx);
            }
            check_integrity_against_model(&h,&m,"fuzz_remove");
        }
    }

    vec_free(&m);
}

/* ===== main ===== */
int main(int argc, char **argv){
    unsigned seed = 0x1234ABCDu;
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
        g_nodes[i].link.rsle_next = RIX_NIL;
    }

    test_init_empty();
    test_insert_remove_basic();
    test_foreach_safe_and_previndex();
    test_fuzz(seed, N, ops);

    free(g_nodes);
    printf("ALL RIX_SLIST TESTS PASSED \n");
    return 0;
}
