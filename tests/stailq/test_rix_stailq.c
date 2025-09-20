/* test_rix_stailq.c
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
    RIX_STAILQ_ENTRY(node) link;
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
static void vec_remove_head(vec_t *v){
    if (v->n==0) return;
    memmove(v->a, v->a+1, (v->n-1)*sizeof(unsigned int));
    v->n--;
}
static void vec_remove_after_pos(vec_t *v, size_t pos){
    assert(pos < v->n);
    if (pos+1>=v->n) return;
    memmove(v->a+pos+1, v->a+pos+2, (v->n-pos-2)*sizeof(unsigned int));
    v->n--;
}
static void vec_remove_head_until(vec_t *v, unsigned last){
    ssize_t p = vec_find(v,last);
    assert(p>=0);
    size_t cut = (size_t)p + 1;
    memmove(v->a, v->a+cut, (v->n-cut)*sizeof(unsigned int));
    v->n -= cut;
}

static void dump_vec(const vec_t *v, const char *name){
    fprintf(stderr, "%s:", name);
    for (size_t i=0;i<v->n;i++) fprintf(stderr, " %u", v->a[i]);
    fputc('\n', stderr);
}

RIX_STAILQ_HEAD(qhead, node);

static void extract_forward(struct qhead *h, vec_t *out){
    vec_clear(out);
    for (struct node *it = RIX_STAILQ_FIRST(h, g_nodes);
         it; it = RIX_STAILQ_NEXT(h, g_nodes, it, link)) {
        vec_push_back(out, RIX_IDX_FROM_PTR(g_nodes, it));
    }
}

static void check_integrity_against_model(struct qhead *h,
                                          const vec_t *model,
                                          const char *tag)
{
    if (RIX_STAILQ_EMPTY(h)) {
        if (h->rsqh_first != RIX_NIL || h->rsqh_last != RIX_NIL)
            FAILF("EMPTY but first/last not NIL tag=%s", tag);
    } else {
        if (h->rsqh_first == RIX_NIL || h->rsqh_last == RIX_NIL)
            FAILF("NON-EMPTY but first/last is NIL tag=%s", tag);
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
        if (h->rsqh_first != first) FAILF("head first mismatch tag=%s", tag);
        if (h->rsqh_last  != last ) FAILF("head last  mismatch tag=%s", tag);
        if (NODEP(last)->link.rsqe_next != RIX_NIL) FAILF("last.next != NIL tag=%s", tag);
    }

    for (size_t i=0; i<fw.n; i++){
        unsigned cur = fw.a[i];
        unsigned next_expected = (i+1<fw.n) ? fw.a[i+1] : RIX_NIL;
        if (NODEP(cur)->link.rsqe_next != next_expected)
            FAILF("next link broken at idx=%u tag=%s", cur, tag);

        if (RIX_PTR_FROM_IDX(g_nodes, RIX_NIL) != NULL) FAIL("PTR_FROM_IDX(NIL) must be NULL");
        if (RIX_IDX_FROM_PTR(g_nodes, (struct node*)NULL) != RIX_NIL) FAIL("IDX_FROM_PTR(NULL) must be NIL");
        if (RIX_IDX_FROM_PTR(g_nodes, NODEP(cur)) != cur) FAIL("IDX<->PTR roundtrip failed");
    }

    vec_free(&fw);
}

static void model_insert_head(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_front(m,x); }
static void model_insert_tail(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_back(m,x); }
static void model_insert_after(vec_t *m, unsigned base, unsigned x){
    ssize_t p = vec_find(m,base); assert(p>=0); assert(vec_find(m,x)<0);
    vec_insert_at(m, (size_t)p+1, x);
}
static void model_remove_head(vec_t *m){ vec_remove_head(m); }
static void model_remove_after(vec_t *m, unsigned base){
    ssize_t p = vec_find(m,base); assert(p>=0);
    vec_remove_after_pos(m, (size_t)p);
}
static void model_remove_valM(vec_t *m, unsigned x){ int ok=vec_remove_val(m,x); assert(ok); }
static void model_remove_head_until(vec_t *m, unsigned last){ vec_remove_head_until(m, last); }
static void model_concat(vec_t *a, vec_t *b){ for (size_t i=0;i<b->n;i++) vec_push_back(a,b->a[i]); b->n=0; }
static void model_swap(vec_t *a, vec_t *b){ vec_t t=*a; *a=*b; *b=t; }

static void test_init_empty(void){
    printf("[T] init/empty\n");
    struct qhead h = RIX_STAILQ_HEAD_INITIALIZER(h);
    if (!RIX_STAILQ_EMPTY(&h)) FAIL("HEAD_INITIALIZER not empty");
    if (h.rsqh_first!=RIX_NIL || h.rsqh_last!=RIX_NIL) FAIL("HEAD_INITIALIZER first/last not NIL");
    RIX_STAILQ_INIT(&h);
    if (!RIX_STAILQ_EMPTY(&h)) FAIL("INIT not empty");
    if (RIX_STAILQ_FIRST(&h, g_nodes)!=NULL || RIX_STAILQ_LAST(&h, g_nodes)!=NULL)
        FAIL("FIRST/LAST must be NULL on empty");
}

static void test_insert_remove(void){
    printf("[T] insert/remove scenarios\n");
    struct qhead h; RIX_STAILQ_INIT(&h);
    vec_t m; vec_init(&m);

    unsigned a=1,b=2,c=3,d=4,e=5;

    /* head, tail, after */
    RIX_STAILQ_INSERT_HEAD(&h, g_nodes, NODEP(a), link); model_insert_head(&m,a);
    check_integrity_against_model(&h,&m,"ins_head_a");

    RIX_STAILQ_INSERT_TAIL(&h, g_nodes, NODEP(b), link); model_insert_tail(&m,b);
    check_integrity_against_model(&h,&m,"ins_tail_b");

    RIX_STAILQ_INSERT_AFTER(&h, g_nodes, NODEP(a), NODEP(c), link); model_insert_after(&m,a,c);
    check_integrity_against_model(&h,&m,"after_a_c");

    RIX_STAILQ_INSERT_AFTER(&h, g_nodes, NODEP(c), NODEP(d), link); model_insert_after(&m,c,d);
    check_integrity_against_model(&h,&m,"after_c_d");

    RIX_STAILQ_INSERT_HEAD(&h, g_nodes, NODEP(e), link); model_insert_head(&m,e);
    check_integrity_against_model(&h,&m,"ins_head_e");

    /* remove head / after / val / until */
    RIX_STAILQ_REMOVE_HEAD(&h, g_nodes, link); model_remove_head(&m);
    check_integrity_against_model(&h,&m,"rm_head");

    RIX_STAILQ_REMOVE_AFTER(&h, g_nodes, NODEP(a), link); model_remove_after(&m,a);
    check_integrity_against_model(&h,&m,"rm_after_a");

    RIX_STAILQ_REMOVE(&h, g_nodes, NODEP(b), node, link); model_remove_valM(&m,b);
    check_integrity_against_model(&h,&m,"rm_val_b");

    if (m.n >= 2){
        unsigned last_take = m.a[0];
        RIX_STAILQ_REMOVE_HEAD_UNTIL(&h, g_nodes, NODEP(last_take), link);
        model_remove_head_until(&m, last_take);
        check_integrity_against_model(&h,&m,"rm_until_first");
    }

    vec_free(&m);
}

static void test_foreach_safe(void){
    printf("[T] foreach/safe\n");
    struct qhead h; RIX_STAILQ_INIT(&h);
    vec_t m; vec_init(&m);

    for (unsigned i=1;i<=16;i++){
        RIX_STAILQ_INSERT_TAIL(&h, g_nodes, NODEP(i), link);
        model_insert_tail(&m, i);
    }
    check_integrity_against_model(&h,&m,"fill_1_16");

    struct node *it,*tmp;
    RIX_STAILQ_FOREACH_SAFE(it, &h, g_nodes, link, tmp){
        unsigned idx = RIX_IDX_FROM_PTR(g_nodes, it);
        if ((idx & 1u)==0){
            RIX_STAILQ_REMOVE(&h, g_nodes, it, node, link);
            model_remove_valM(&m, idx);
        }
    }
    check_integrity_against_model(&h,&m,"remove_evens_safe");

    unsigned long sum=0, expected=0;
    for (it = RIX_STAILQ_FIRST(&h, g_nodes); it; it = RIX_STAILQ_NEXT(&h, g_nodes, it, link))
        sum += RIX_IDX_FROM_PTR(g_nodes, it);
    for (size_t i=0;i<m.n;i++) expected += m.a[i];
    if (sum!=expected) FAIL("foreach sum mismatch");

    vec_free(&m);
}

static void test_concat_swap(void){
    printf("[T] concat/swap\n");
    struct qhead h1, h2; RIX_STAILQ_INIT(&h1); RIX_STAILQ_INIT(&h2);
    vec_t m1, m2; vec_init(&m1); vec_init(&m2);

    for (unsigned i=1;i<=5;i++){ RIX_STAILQ_INSERT_TAIL(&h1, g_nodes, NODEP(i), link); model_insert_tail(&m1,i); }
    for (unsigned i=6;i<=10;i++){ RIX_STAILQ_INSERT_TAIL(&h2, g_nodes, NODEP(i), link); model_insert_tail(&m2,i); }
    check_integrity_against_model(&h1,&m1,"h1_init");
    check_integrity_against_model(&h2,&m2,"h2_init");

    RIX_STAILQ_CONCAT(&h1, &h2, g_nodes, link); model_concat(&m1,&m2);
    check_integrity_against_model(&h1,&m1,"concat");
    check_integrity_against_model(&h2,&m2,"concat_dst_empty");

    RIX_STAILQ_SWAP(&h1, &h2, g_nodes); model_swap(&m1,&m2);
    check_integrity_against_model(&h1,&m1,"swap1");
    check_integrity_against_model(&h2,&m2,"swap2");

    for (unsigned i=11;i<=15;i++){ RIX_STAILQ_INSERT_TAIL(&h1, g_nodes, NODEP(i), link); model_insert_tail(&m1,i); }
    for (unsigned i=16;i<=20;i++){ RIX_STAILQ_INSERT_HEAD(&h2, g_nodes, NODEP(i), link); model_insert_head(&m2,i); }
    check_integrity_against_model(&h1,&m1,"pre_swapA");
    check_integrity_against_model(&h2,&m2,"pre_swapB");

    RIX_STAILQ_SWAP(&h1, &h2, g_nodes); model_swap(&m1,&m2);
    check_integrity_against_model(&h1,&m1,"swapA");
    check_integrity_against_model(&h2,&m2,"swapB");

    vec_free(&m1); vec_free(&m2);
}

static unsigned xrnd_state = 0x12345678u;
static unsigned xrnd(void){ unsigned x=xrnd_state; x^=x<<13; x^=x>>17; x^=x<<5; xrnd_state=x; return x; }
static unsigned rnd_in(unsigned lo, unsigned hi){ return lo + (xrnd() % (hi - lo + 1)); }

static void test_fuzz(unsigned seed, unsigned N, unsigned ops){
    printf("[T] fuzz seed=%u N=%u ops=%u\n", seed, N, ops);
    xrnd_state = seed ? seed : 0xCAFEBABEu;

    struct qhead hA, hB; RIX_STAILQ_INIT(&hA); RIX_STAILQ_INIT(&hB);
    vec_t mA, mB; vec_init(&mA); vec_init(&mB);
    unsigned char *where = (unsigned char*)calloc((size_t)g_cap+1,1);

    for (unsigned step=0; step<ops; step++){
        unsigned op = xrnd()%100;

        if (op<3){
            RIX_STAILQ_CONCAT(&hA,&hB,g_nodes,link); model_concat(&mA,&mB);
            memset(where,0,(size_t)g_cap+1); for (size_t i=0;i<mA.n;i++) where[mA.a[i]]=1;
            check_integrity_against_model(&hA,&mA,"fuzz_concat_A");
            check_integrity_against_model(&hB,&mB,"fuzz_concat_B");
            continue;
        } else if (op<6){
            RIX_STAILQ_SWAP(&hA,&hB,g_nodes); model_swap(&mA,&mB);
            memset(where,0,(size_t)g_cap+1); for (size_t i=0;i<mA.n;i++) where[mA.a[i]]=1; for (size_t i=0;i<mB.n;i++) where[mB.a[i]]=2;
            check_integrity_against_model(&hA,&mA,"fuzz_swap_A");
            check_integrity_against_model(&hB,&mB,"fuzz_swap_B");
            continue;
        }

        struct qhead *h = (xrnd()&1) ? &hA : &hB;
        vec_t *m        = (h==&hA) ? &mA : &mB;
        unsigned char myid = (h==&hA) ? 1 : 2;

        if (op<35){
            unsigned idx = rnd_in(1, N);
            if (where[idx]!=0){
                if (where[idx]==1){ RIX_STAILQ_REMOVE(&hA, g_nodes, NODEP(idx), node, link); model_remove_valM(&mA, idx); }
                else              { RIX_STAILQ_REMOVE(&hB, g_nodes, NODEP(idx), node, link); model_remove_valM(&mB, idx); }
                where[idx]=0;
            }
            unsigned which = xrnd()%3;
            if (which==0 || m->n==0){
                RIX_STAILQ_INSERT_HEAD(h, g_nodes, NODEP(idx), link); model_insert_head(m, idx);
            } else if (which==1){
                RIX_STAILQ_INSERT_TAIL(h, g_nodes, NODEP(idx), link); model_insert_tail(m, idx);
            } else {
                unsigned base = m->a[xrnd()%m->n];
                RIX_STAILQ_INSERT_AFTER(h, g_nodes, NODEP(base), NODEP(idx), link);
                model_insert_after(m, base, idx);
            }
            where[idx]=myid;
            check_integrity_against_model(h,m,"fuzz_insert");
        } else if (op<65){
            if (m->n>0){
                unsigned mode = xrnd()%3;
                if (mode==0){
                    RIX_STAILQ_REMOVE_HEAD(h, g_nodes, link);
                    unsigned removed = m->a[0];
                    model_remove_head(m);
                    where[removed]=0;
                } else if (mode==1 && m->n>=2){
                    size_t pos = xrnd()%(m->n-1);
                    unsigned base = m->a[pos];
                    unsigned removed = m->a[pos+1];
                    RIX_STAILQ_REMOVE_AFTER(h, g_nodes, NODEP(base), link);
                    model_remove_after(m, base);
                    where[removed]=0;
                } else {
                    size_t pos = xrnd()%m->n;
                    unsigned idx = m->a[pos];
                    RIX_STAILQ_REMOVE(h, g_nodes, NODEP(idx), node, link);
                    model_remove_valM(m, idx);
                    where[idx]=0;
                }
                check_integrity_against_model(h,m,"fuzz_remove");
            }
        } else if (op<80){
            if (m->n>0){
                size_t pos = xrnd()%m->n;
                unsigned last = m->a[pos];
                RIX_STAILQ_REMOVE_HEAD_UNTIL(h, g_nodes, NODEP(last), link);
                for (size_t i=0;i<=pos;i++) where[m->a[i]]=0;
                model_remove_head_until(m, last);
                check_integrity_against_model(h,m,"fuzz_rm_until");
            }
        } else {
            unsigned long sum=0, expected=0;
            for (struct node *it2 = RIX_STAILQ_FIRST(h, g_nodes);
                 it2; it2 = RIX_STAILQ_NEXT(h, g_nodes, it2, link))
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
    unsigned seed = 0x24681357u;
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
        g_nodes[i].link.rsqe_next = RIX_NIL;
    }

    test_init_empty();
    test_insert_remove();
    test_foreach_safe();
    test_concat_swap();
    test_fuzz(seed, N, ops);

    free(g_nodes);
    printf("ALL STAILQ TESTS PASSED \n");
    return 0;
}
