/* test_rix_tailq.c
 *  RIX_ TAILQ test(1-origin, NIL=0, array[index-1])
 *
 *  - INIT/INITIALIZER/RESET
 *  - EMPTY/FIRST/LAST/NEXT/PREV
 *  - INSERT_HEAD/TAIL/AFTER/BEFORE
 *  - REMOVE
 *  - FOREACH / FOREACH_SAFE / REVERSE
 *  - CONCAT / SWAP
 *  - IDX<->PTR
 *
 *    gcc -std=gnu11 -O2 -g -Werror -Wextra -Wall -Wstrict-aliasing -pedantic \
 *        -I. test_rix_tailq.c -o tailq_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>  /* ssize_t */
#include "../../include/librix.h"

#define FAIL(msg) do {							\
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort();								\
  } while (0)

#define FAILF(fmt, ...) do {						\
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort();								\
  } while (0)

struct node {
  int value;
  RIX_TAILQ_ENTRY(node) link;
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

RIX_TAILQ_HEAD(qhead, node);

static void dump_vec(const vec_t *v, const char *name){
  fprintf(stderr, "%s:", name);
  for (size_t i=0;i<v->n;i++) fprintf(stderr, " %u", v->a[i]);
  fputc('\n', stderr);
}

static void extract_forward(struct qhead *h, vec_t *out){
  vec_clear(out);
  for (struct node *it = RIX_TAILQ_FIRST(h, g_nodes);
       it; it = RIX_TAILQ_NEXT(h, g_nodes, it, link)) {
    vec_push_back(out, RIX_IDX_FROM_PTR(g_nodes, it));
  }
}
static void extract_reverse(struct qhead *h, vec_t *out){
  vec_clear(out);
  for (struct node *it = RIX_TAILQ_LAST(h, g_nodes);
       it; it = RIX_TAILQ_PREV(h, g_nodes, it, link)) {
    vec_push_back(out, RIX_IDX_FROM_PTR(g_nodes, it));
  }
}

static void check_integrity_against_model(struct qhead *h,
                                          const vec_t *model,
                                          const char *tag)
{
  if (RIX_TAILQ_EMPTY(h)) {
    if (h->rqh_first != RIX_NIL || h->rqh_last != RIX_NIL)
      FAIL("EMPTY but first/last not NIL");
  } else {
    if (h->rqh_first == RIX_NIL || h->rqh_last == RIX_NIL)
      FAIL("NON-EMPTY but first/last is NIL");
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

  vec_t rv; vec_init(&rv); extract_reverse(h, &rv);
  if (rv.n != fw.n) {
    dump_vec(&rv, "reverse"); dump_vec(&fw, "forward");
    FAILF("reverse length mismatch tag=%s", tag);
  }
  for (size_t i=0;i<rv.n;i++){
    if (rv.a[i] != fw.a[fw.n-1-i]) {
      dump_vec(&rv, "reverse"); dump_vec(&fw, "forward");
      FAILF("reverse order mismatch at i=%zu tag=%s", i, tag);
    }
  }

  if (fw.n != 0) {
    unsigned first = fw.a[0], last = fw.a[fw.n-1];
    if (h->rqh_first != first) FAIL("head first mismatch");
    if (h->rqh_last  != last ) FAIL("head last mismatch");
    if (NODEP(first)->link.rqe_prev != RIX_NIL) FAIL("first.prev must be NIL");
    if (NODEP(last )->link.rqe_next != RIX_NIL) FAIL("last.next must be NIL");
  }

  for (size_t i=0;i<fw.n;i++){
    unsigned cur  = fw.a[i];
    unsigned prev = NODEP(cur)->link.rqe_prev;
    unsigned next = NODEP(cur)->link.rqe_next;

    if (i==0){
      if (prev != RIX_NIL) FAIL("first.prev != NIL");
    } else {
      if (prev != fw.a[i-1]) FAIL("prev link broken");
      if (NODEP(prev)->link.rqe_next != cur) FAIL("prev->next != cur");
    }
    if (i==fw.n-1){
      if (next != RIX_NIL) FAIL("last.next != NIL");
    } else {
      if (next != fw.a[i+1]) FAIL("next link broken");
      if (NODEP(next)->link.rqe_prev != cur) FAIL("next->prev != cur");
    }

    if (RIX_PTR_FROM_IDX(g_nodes, RIX_NIL) != NULL) FAIL("PTR_FROM_IDX(NIL) must be NULL");
    if (RIX_IDX_FROM_PTR(g_nodes, (struct node*)NULL) != RIX_NIL) FAIL("IDX_FROM_PTR(NULL) must be NIL");
    if (RIX_IDX_FROM_PTR(g_nodes, NODEP(cur)) != cur) FAIL("IDX<->PTR roundtrip failed");
  }

  vec_free(&fw);
  vec_free(&rv);
}

static void model_insert_head(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_front(m,x); }
static void model_insert_tail(vec_t *m, unsigned x){ assert(vec_find(m,x)<0); vec_push_back(m,x); }
static void model_insert_after(vec_t *m, unsigned base, unsigned x){
  ssize_t p = vec_find(m,base); assert(p>=0); assert(vec_find(m,x)<0);
  vec_insert_at(m, (size_t)p+1, x);
}
static void model_insert_before(vec_t *m, unsigned base, unsigned x){
  ssize_t p = vec_find(m,base); assert(p>=0); assert(vec_find(m,x)<0);
  vec_insert_at(m, (size_t)p, x);
}
static void model_remove(vec_t *m, unsigned x){ int ok=vec_remove_val(m,x); assert(ok); }
static void model_concat(vec_t *a, vec_t *b){ for (size_t i=0;i<b->n;i++) vec_push_back(a,b->a[i]); b->n=0; }
static void model_swap(vec_t *a, vec_t *b){ vec_t t=*a; *a=*b; *b=t; }

static void test_init_empty(void){
  printf("[T] init/empty\n");
  struct qhead h = RIX_TAILQ_HEAD_INITIALIZER(h);
  if (!RIX_TAILQ_EMPTY(&h)) FAIL("HEAD_INITIALIZER not empty");
  if (h.rqh_first!=RIX_NIL || h.rqh_last!=RIX_NIL) FAIL("HEAD_INITIALIZER first/last not NIL");
  RIX_TAILQ_INIT(&h);
  if (!RIX_TAILQ_EMPTY(&h)) FAIL("INIT not empty");
  if (RIX_TAILQ_FIRST(&h, g_nodes)!=NULL || RIX_TAILQ_LAST(&h, g_nodes)!=NULL)
    FAIL("FIRST/LAST must be NULL on empty");
}

static void test_insert_remove(void){
  printf("[T] insert/remove scenarios\n");
  struct qhead h; RIX_TAILQ_INIT(&h);
  vec_t m; vec_init(&m);
  unsigned a=1,b=2,c=3,d=4,e=5;

  RIX_TAILQ_INSERT_HEAD(&h, g_nodes, NODEP(a), link); model_insert_head(&m,a);
  check_integrity_against_model(&h,&m,"ins_head_a");

  RIX_TAILQ_INSERT_TAIL(&h, g_nodes, NODEP(b), link); model_insert_tail(&m,b);
  check_integrity_against_model(&h,&m,"ins_tail_b");

  RIX_TAILQ_INSERT_AFTER(&h, g_nodes, NODEP(a), NODEP(c), link); model_insert_after(&m,a,c);
  check_integrity_against_model(&h,&m,"after_a_c");

  RIX_TAILQ_INSERT_BEFORE(&h, g_nodes, NODEP(c), NODEP(d), link); model_insert_before(&m,c,d);
  check_integrity_against_model(&h,&m,"before_c_d");

  RIX_TAILQ_INSERT_BEFORE(&h, g_nodes, NODEP(a), NODEP(e), link); model_insert_before(&m,a,e);
  check_integrity_against_model(&h,&m,"before_head_e");

  RIX_TAILQ_REMOVE(&h, g_nodes, NODEP(d), link); model_remove(&m,d);
  check_integrity_against_model(&h,&m,"rm_mid_d");

  RIX_TAILQ_REMOVE(&h, g_nodes, NODEP(b), link); model_remove(&m,b);
  check_integrity_against_model(&h,&m,"rm_tail_b");

  RIX_TAILQ_REMOVE(&h, g_nodes, NODEP(e), link); model_remove(&m,e);
  check_integrity_against_model(&h,&m,"rm_head_e");

  RIX_TAILQ_REMOVE(&h, g_nodes, NODEP(a), link); model_remove(&m,a);
  check_integrity_against_model(&h,&m,"rm_to_single");

  RIX_TAILQ_REMOVE(&h, g_nodes, NODEP(c), link); model_remove(&m,c);
  check_integrity_against_model(&h,&m,"rm_to_empty");

  RIX_TAILQ_RESET(&h);
  if (!RIX_TAILQ_EMPTY(&h)) FAIL("RESET should make it empty");

  vec_free(&m);
}

static void test_foreach_safe_reverse(void){
  printf("[T] foreach/safe/reverse\n");
  struct qhead h; RIX_TAILQ_INIT(&h);
  vec_t m; vec_init(&m);

  for (unsigned i=1;i<=16;i++){
    RIX_TAILQ_INSERT_TAIL(&h, g_nodes, NODEP(i), link);
    model_insert_tail(&m, i);
  }
  check_integrity_against_model(&h,&m,"fill_1_16");

  struct node *it,*tmp;
  RIX_TAILQ_FOREACH_SAFE(it, &h, g_nodes, link, tmp){
    unsigned idx = RIX_IDX_FROM_PTR(g_nodes, it);
    if ((idx & 1u)==0){
      RIX_TAILQ_REMOVE(&h, g_nodes, it, link);
      model_remove(&m, idx);
    }
  }
  check_integrity_against_model(&h,&m,"remove_evens");

  unsigned long sum=0, expected=0;
  RIX_TAILQ_FOREACH(it, &h, g_nodes, link){
    sum += RIX_IDX_FROM_PTR(g_nodes, it);
  }
  for (size_t i=0;i<m.n;i++) expected += m.a[i];
  if (sum!=expected) FAIL("foreach sum mismatch");

  unsigned long rsum=0;
  for (it = RIX_TAILQ_LAST(&h, g_nodes); it; it = RIX_TAILQ_PREV(&h, g_nodes, it, link))
    rsum += RIX_IDX_FROM_PTR(g_nodes, it);
  if (rsum!=sum) FAIL("reverse sum mismatch");

  vec_free(&m);
}

static void test_concat_swap(void){
  printf("[T] concat/swap\n");
  struct qhead h1, h2; RIX_TAILQ_INIT(&h1); RIX_TAILQ_INIT(&h2);
  vec_t m1, m2; vec_init(&m1); vec_init(&m2);

  for (unsigned i=1;i<=5;i++){ RIX_TAILQ_INSERT_TAIL(&h1, g_nodes, NODEP(i), link); model_insert_tail(&m1,i); }
  for (unsigned i=6;i<=10;i++){ RIX_TAILQ_INSERT_TAIL(&h2, g_nodes, NODEP(i), link); model_insert_tail(&m2,i); }
  check_integrity_against_model(&h1,&m1,"h1_init");
  check_integrity_against_model(&h2,&m2,"h2_init");

  RIX_TAILQ_CONCAT(&h1, &h2, g_nodes, link); model_concat(&m1,&m2);
  check_integrity_against_model(&h1,&m1,"concat");
  check_integrity_against_model(&h2,&m2,"concat_dst_empty");

  RIX_TAILQ_SWAP(&h1, &h2, g_nodes); model_swap(&m1,&m2);
  check_integrity_against_model(&h1,&m1,"swap1");
  check_integrity_against_model(&h2,&m2,"swap2");

  for (unsigned i=11;i<=15;i++){ RIX_TAILQ_INSERT_TAIL(&h1, g_nodes, NODEP(i), link); model_insert_tail(&m1,i); }
  for (unsigned i=16;i<=20;i++){ RIX_TAILQ_INSERT_HEAD(&h2, g_nodes, NODEP(i), link); model_insert_head(&m2,i); }
  check_integrity_against_model(&h1,&m1,"pre_swapA");
  check_integrity_against_model(&h2,&m2,"pre_swapB");

  RIX_TAILQ_SWAP(&h1, &h2, g_nodes); model_swap(&m1,&m2);
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

  struct qhead hA, hB; RIX_TAILQ_INIT(&hA); RIX_TAILQ_INIT(&hB);
  vec_t mA, mB; vec_init(&mA); vec_init(&mB);
  unsigned char *where = (unsigned char*)calloc((size_t)g_cap+1,1); /* 1-based: 0..g_cap */

  for (unsigned step=0; step<ops; step++){
    unsigned op = xrnd()%100;

    if (op<3){
      RIX_TAILQ_CONCAT(&hA,&hB,g_nodes,link); model_concat(&mA,&mB);
      memset(where,0,(size_t)g_cap+1); for (size_t i=0;i<mA.n;i++) where[mA.a[i]]=1;
      check_integrity_against_model(&hA,&mA,"fuzz_concat_A");
      check_integrity_against_model(&hB,&mB,"fuzz_concat_B");
      continue;
    } else if (op<6){
      RIX_TAILQ_SWAP(&hA,&hB,g_nodes); model_swap(&mA,&mB);
      memset(where,0,(size_t)g_cap+1); for (size_t i=0;i<mA.n;i++) where[mA.a[i]]=1; for (size_t i=0;i<mB.n;i++) where[mB.a[i]]=2;
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
	if (where[idx]==1){ RIX_TAILQ_REMOVE(&hA, g_nodes, NODEP(idx), link); model_remove(&mA, idx); }
	else              { RIX_TAILQ_REMOVE(&hB, g_nodes, NODEP(idx), link); model_remove(&mB, idx); }
	where[idx]=0;
      }
      unsigned which = xrnd()%4;
      if (which==0 || m->n==0){
	RIX_TAILQ_INSERT_HEAD(h, g_nodes, NODEP(idx), link); model_insert_head(m, idx);
      } else if (which==1){
	RIX_TAILQ_INSERT_TAIL(h, g_nodes, NODEP(idx), link); model_insert_tail(m, idx);
      } else if (which==2){
	unsigned base = m->a[xrnd()%m->n];
	RIX_TAILQ_INSERT_AFTER(h, g_nodes, NODEP(base), NODEP(idx), link);
	model_insert_after(m, base, idx);
      } else {
	unsigned base = m->a[xrnd()%m->n];
	RIX_TAILQ_INSERT_BEFORE(h, g_nodes, NODEP(base), NODEP(idx), link);
	model_insert_before(m, base, idx);
      }
      where[idx]=myid;
      check_integrity_against_model(h,m,"fuzz_insert");
    } else if (op<70){
      if (m->n>0){
	unsigned pos = xrnd()%m->n;
	unsigned idx = m->a[pos];
	RIX_TAILQ_REMOVE(h, g_nodes, NODEP(idx), link);
	model_remove(m, idx);
	where[idx]=0;
	check_integrity_against_model(h,m,"fuzz_remove");
      }
    } else if (op<85){
      struct node *it,*tmp;
      RIX_TAILQ_FOREACH_SAFE(it, h, g_nodes, link, tmp){
	unsigned idx = RIX_IDX_FROM_PTR(g_nodes, it);
	if ((idx & 1u)==0){
	  RIX_TAILQ_REMOVE(h, g_nodes, it, link);
	  model_remove(m, idx);
	  where[idx]=0;
	}
      }
      check_integrity_against_model(h,m,"fuzz_safe");
    } else {
      unsigned long sum=0, expected=0;
      for (struct node *it2 = RIX_TAILQ_FIRST(h, g_nodes);
	   it2; it2 = RIX_TAILQ_NEXT(h, g_nodes, it2, link))
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
  g_nodes = (struct node*)calloc((size_t)N, sizeof(struct node));
  if (!g_nodes){ perror("calloc g_nodes"); return 1; }
  for (unsigned i=0;i<N;i++){
    g_nodes[i].value = (int)(i+1);
    g_nodes[i].link.rqe_next = RIX_NIL;
    g_nodes[i].link.rqe_prev = RIX_NIL;
  }

  test_init_empty();
  test_insert_remove();
  test_foreach_safe_reverse();
  test_concat_swap();
  test_fuzz(seed, N, ops);

  free(g_nodes);
  printf("ALL TESTS PASSED \n");
  return 0;
}
