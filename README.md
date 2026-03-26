# librix -- Relative Index Library

Index-based data structures for shared memory, plus a practical flow-cache
implementation built on top of them.

librix provides relative (index-pointing) implementations of classic BSD data
structures -- SLIST, LIST, STAILQ, TAILQ, CIRCLEQ, a Red-Black tree, and
high-performance cuckoo hash tables -- so they can live directly in shared
memory or mmapped files without raw pointers. The repository also includes
`samples/fcache`, a flow-cache implementation for IPv4, IPv6, and unified
operation, intended as both a real sample application and a performance
reference.

## Why this project?

Most in-memory data structures assume process-local pointers and heap
allocation. That is convenient inside a single process, but awkward when the
same data must be shared across processes, remapped at different virtual
addresses, or stored in file-backed regions.

librix takes a different approach:

- it stores indices instead of pointers
- it keeps data structures relocatable by construction
- it supports SIMD-friendly cuckoo hash implementations
- it ships with a non-trivial flow-cache sample, not just container macros

The goal is to make shared-memory-friendly data structures practical without
giving up performance.

## Table of Contents

- [Why this project?](#why-this-project)
- [Why indices?](#why-indices)
- [Design](#design)
- [Repository layout](#repository-layout)
- [Getting started](#getting-started)
- [Common helpers](#common-helpers)
- [Queue structures](#queue-structures)
  - [RIX_SLIST](#rix_slist)
  - [RIX_LIST](#rix_list)
  - [RIX_STAILQ](#rix_stailq)
  - [RIX_TAILQ](#rix_tailq)
  - [RIX_CIRCLEQ](#rix_circleq)
- [Red-Black tree -- RIX_RB](#red-black-tree--rix_rb)
- [Cuckoo hash tables](#cuckoo-hash-tables)
  - [Variant comparison](#variant-comparison)
  - [RIX_HASH (fingerprint, variable-length key)](#rix_hash-fingerprint-variable-length-key)
  - [RIX_HASH32 (uint32_t key)](#rix_hash32-uint32_t-key)
  - [RIX_HASH64 (uint64_t key)](#rix_hash64-uint64_t-key)
  - [Hash table test coverage matrix](#hash-table-test-coverage-matrix)
- [Samples](#samples)
- [Build](#build)
- [Concurrency](#concurrency)
- [Testing](#testing)
- [License](#license)

---

## Why indices?

Raw pointers are process-local and non-relocatable.  By storing **unsigned
indices** instead:

- Structures are relocatable across processes and after remapping.
- They live naturally in shared memory and file-backed regions.
- Zero-initialization makes every head/node empty by construction.
- Mixed 32/64-bit processes mapping the same region avoid pointer-size mismatches.

---

## Design

| Concept | Detail |
|---------|--------|
| Nil sentinel | `RIX_NIL = 0` -- the zero value means "no element" |
| 1-origin mapping | valid indices are `1 ... UINT_MAX-1`; `pool[i]` <-> index `i+1` |
| No stored pointers | heads and link fields contain indices only |
| Transient pointers | conversion is done at call time by passing `base` (your element array) |
| Standard | C11; no external dependencies; single-header per subsystem |

Index <-> pointer conversion macros:

```c
RIX_IDX_FROM_PTR(base, p)   /* (p - base) + 1  (NULL -> RIX_NIL) */
RIX_PTR_FROM_IDX(base, i)   /* base + (i-1)    (0 -> NULL)        */
```

---

## Repository layout

```
include/
  librix.h          umbrella header (includes all subsystems)
  rix/
    rix_defs_private.h  common macros, index helpers  (internal; auto-included)
    rix_hash_arch.h     arch dispatch, SIMD helpers   (internal; auto-included)
    rix_queue.h     SLIST / LIST / STAILQ / TAILQ / CIRCLEQ
    rix_tree.h      Red-Black tree
    rix_hash_common.h  cuckoo hash -- shared types and helpers (internal; auto-included)
    rix_hash_fp.h      cuckoo hash -- fingerprint variant
    rix_hash_slot.h    cuckoo hash -- slot variant (hash_field + slot_field)
    rix_hash_keyonly.h cuckoo hash -- key-only variant (no auxiliary fields)
    rix_hash.h      cuckoo hash umbrella (includes fp, slot, keyonly, hash32, hash64)
    rix_hash32.h    cuckoo hash -- uint32_t key variant
    rix_hash64.h    cuckoo hash -- uint64_t key variant
    rix_hash_key.h  cuckoo hash -- uint32_t and uint64_t variants combined
samples/              flow cache sample application (see samples/README.md)
```

---

## Getting started

```c
#include "librix.h"      /* queue + tree + all hash variants */
```

Or include only what you need:

```c
#include "rix/rix_queue.h"   /* queue structures only              */
#include "rix/rix_tree.h"    /* Red-Black tree only                */
#include "rix/rix_hash.h"    /* cuckoo hash (fp)                   */
#include "rix/rix_hash32.h"  /* cuckoo hash (u32 key)              */
#include "rix/rix_hash64.h"  /* cuckoo hash (u64 key)              */
#include "rix/rix_hash_key.h"/* cuckoo hash (u32 + u64, combined)  */
```

### Queue quick start

```c
struct node {
    int value;
    RIX_TAILQ_ENTRY(node) link;
};

RIX_TAILQ_HEAD(qhead);
struct qhead h;
struct node *base;   /* your element array in shared memory */

RIX_TAILQ_INIT(&h);

RIX_TAILQ_INSERT_TAIL(&h, base, &base[0], link);
RIX_TAILQ_INSERT_TAIL(&h, base, &base[1], link);

struct node *it;
RIX_TAILQ_FOREACH(it, &h, base, link) {
    /* use it->value */
}

RIX_TAILQ_REMOVE(&h, base, &base[0], link);
```

---

## Common helpers

Available via any rix public header (defined internally in `rix_defs_private.h`):

```c
RIX_NIL                          /* 0 -- null index */
RIX_IDX_FROM_PTR(base, p)        /* pointer -> index */
RIX_PTR_FROM_IDX(base, i)        /* index -> pointer (NULL if i==0) */
RIX_IDX_IS_NIL(i)                /* i == RIX_NIL */
RIX_IDX_IS_VALID(i, cap)         /* 1 <= i <= cap */

RIX_MIN(a, b)
RIX_MAX(a, b)
RIX_COUNT_OF(arr)
RIX_OFFSET_OF(type, field)
RIX_CONTAINER_OF(ptr, type, field)
RIX_SWAP(a, b)
RIX_CLAMP(v, lo, hi)
RIX_ASSERT(expr)
RIX_STATIC_ASSERT(expr, msg)
```

---

## Queue structures

In all macros below:
- `type` -- your element struct (bare name, no `struct` keyword)
- `field` -- the embedded link field inside `type`
- `base` -- `type *` pointer to the element array
- `head` -- pointer to the container head

### RIX_SLIST

Singly-linked list.  O(1) insert-head, O(n) remove.

```c
/* Declarations */
RIX_SLIST_ENTRY(type)                   /* link field inside struct */
RIX_SLIST_HEAD(name)                    /* declare head type */
RIX_SLIST_HEAD_INITIALIZER(var)         /* static initializer */

/* Init & query */
RIX_SLIST_INIT(head)
RIX_SLIST_EMPTY(head)                   /* 1 if empty */
RIX_SLIST_FIRST(head, base)             /* first element or NULL */
RIX_SLIST_NEXT(elm, base, field)        /* next element or NULL */

/* Modifiers */
RIX_SLIST_INSERT_HEAD(head, base, elm, field)
RIX_SLIST_INSERT_AFTER(base, slistelm, elm, field)
RIX_SLIST_REMOVE_HEAD(head, base, field)
RIX_SLIST_REMOVE_AFTER(base, elm, field)
RIX_SLIST_REMOVE(head, base, elm, type, field)   /* O(n) search */

/* Iteration */
RIX_SLIST_FOREACH(var, head, base, field)
RIX_SLIST_FOREACH_SAFE(var, head, base, field, tvar)
RIX_SLIST_FOREACH_PREVINDEX(var, varidxp, head, base, field)
```

### RIX_LIST

Doubly-linked list with O(1) insert/remove anywhere.

```c
/* Declarations */
RIX_LIST_ENTRY(type)
RIX_LIST_HEAD(name)
RIX_LIST_HEAD_INITIALIZER(var)

/* Init & query */
RIX_LIST_INIT(head)
RIX_LIST_EMPTY(head)
RIX_LIST_FIRST(head, base)
RIX_LIST_NEXT(elm, base, field)

/* Modifiers */
RIX_LIST_INSERT_HEAD(head, base, elm, field)
RIX_LIST_INSERT_AFTER(head, base, listelm, elm, field)
RIX_LIST_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_LIST_REMOVE(head, base, elm, field)
RIX_LIST_SWAP(head1, head2, base, type, field)

/* Iteration */
RIX_LIST_FOREACH(var, head, base, field)
RIX_LIST_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_STAILQ

Singly-linked tail queue.  O(1) insert-head and insert-tail.

```c
/* Declarations */
RIX_STAILQ_ENTRY(type)
RIX_STAILQ_HEAD(name)
RIX_STAILQ_HEAD_INITIALIZER(var)

/* Init & query */
RIX_STAILQ_INIT(head)
RIX_STAILQ_EMPTY(head)
RIX_STAILQ_FIRST(head, base)
RIX_STAILQ_LAST(head, base)
RIX_STAILQ_NEXT(head, base, elm, field)

/* Modifiers */
RIX_STAILQ_INSERT_HEAD(head, base, elm, field)
RIX_STAILQ_INSERT_TAIL(head, base, elm, field)
RIX_STAILQ_INSERT_AFTER(head, base, tqelm, elm, field)
RIX_STAILQ_REMOVE_HEAD(head, base, field)
RIX_STAILQ_REMOVE_AFTER(head, base, elm, field)
RIX_STAILQ_REMOVE(head, base, elm, type, field)   /* O(n) search */
RIX_STAILQ_REMOVE_HEAD_UNTIL(head, base, elm, field)
RIX_STAILQ_CONCAT(head1, head2, base, field)
RIX_STAILQ_SWAP(head1, head2, base)

/* Iteration */
RIX_STAILQ_FOREACH(var, head, base, field)
RIX_STAILQ_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_TAILQ

Doubly-linked tail queue.  O(1) insert/remove at head, tail, or any position.

```c
/* Declarations */
RIX_TAILQ_ENTRY(type)
RIX_TAILQ_HEAD(name)
RIX_TAILQ_HEAD_INITIALIZER(var)

/* Init & query */
RIX_TAILQ_INIT(head)
RIX_TAILQ_RESET(head)           /* alias of INIT */
RIX_TAILQ_EMPTY(head)
RIX_TAILQ_FIRST(head, base)
RIX_TAILQ_LAST(head, base)
RIX_TAILQ_NEXT(head, base, elm, field)
RIX_TAILQ_PREV(head, base, elm, field)

/* Modifiers */
RIX_TAILQ_INSERT_HEAD(head, base, elm, field)
RIX_TAILQ_INSERT_TAIL(head, base, elm, field)
RIX_TAILQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_TAILQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_TAILQ_REMOVE(head, base, elm, field)
RIX_TAILQ_CONCAT(head1, head2, base, field)
RIX_TAILQ_SWAP(head1, head2, base)

/* Iteration */
RIX_TAILQ_FOREACH(var, head, base, field)
RIX_TAILQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_TAILQ_FOREACH_REVERSE(var, head, base, field)
```

### RIX_CIRCLEQ

Circular doubly-linked list.  FIRST wraps to LAST; traversal is cyclic.

```c
/* Declarations */
RIX_CIRCLEQ_ENTRY(type)
RIX_CIRCLEQ_HEAD(name)
RIX_CIRCLEQ_HEAD_INITIALIZER(var)

/* Init & query */
RIX_CIRCLEQ_INIT(head)
RIX_CIRCLEQ_EMPTY(head)
RIX_CIRCLEQ_FIRST(head, base)
RIX_CIRCLEQ_LAST(head, base, field)
RIX_CIRCLEQ_NEXT(elm, base, field)
RIX_CIRCLEQ_PREV(elm, base, field)

/* Modifiers */
RIX_CIRCLEQ_INSERT_HEAD(head, base, elm, field)
RIX_CIRCLEQ_INSERT_TAIL(head, base, elm, field)
RIX_CIRCLEQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_CIRCLEQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_CIRCLEQ_REMOVE(head, base, elm, field)

/* Iteration (one full lap) */
RIX_CIRCLEQ_FOREACH(var, head, base, field)
RIX_CIRCLEQ_FOREACH_REVERSE(var, head, base, field)
RIX_CIRCLEQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_CIRCLEQ_FOREACH_REVERSE_SAFE(var, head, base, field, tvar)
```

---

## Red-Black tree -- RIX_RB

Self-balancing BST.  O(log n) insert, remove, find.

### Quick start

```c
struct rbnode {
    int key;
    RIX_RB_ENTRY(rbnode) rb;
};

static int rb_cmp(const struct rbnode *a, const struct rbnode *b) {
    return (a->key > b->key) - (a->key < b->key);
}

RIX_RB_HEAD(rbtree);
RIX_RB_PROTOTYPE(rbt, rbnode, rb, rb_cmp)
RIX_RB_GENERATE (rbt, rbnode, rb, rb_cmp)

void demo(struct rbtree *rh, struct rbnode *base) {
    RIX_RB_INIT(rh);

    base[0].key = 42;
    RIX_RB_INSERT(rbt, rh, base, &base[0]);

    struct rbnode probe = { .key = 42 };
    struct rbnode *hit = RIX_RB_FIND(rbt, rh, base, &probe);

    struct rbnode *it;
    RIX_RB_FOREACH(it, rbt, rh, base) { /* ascending */ }
}
```

### API reference

```c
/* Declarations & codegen */
RIX_RB_ENTRY(type)
RIX_RB_HEAD(name)
RIX_RB_HEAD_INITIALIZER(var)
RIX_RB_INIT(head)
RIX_RB_PROTOTYPE(name, type, field, cmp)     /* extern declaration */
RIX_RB_GENERATE (name, type, field, cmp)     /* full implementation */

/* Operations */
RIX_RB_INSERT(name, head, base, elm)   /* NULL -> inserted; non-NULL -> duplicate */
RIX_RB_REMOVE(name, head, base, elm)   /* returns removed elm */
RIX_RB_FIND  (name, head, base, key)   /* exact match or NULL */
RIX_RB_NFIND (name, head, base, key)   /* lower bound (first node >= key) */
RIX_RB_MIN   (name, head, base)
RIX_RB_MAX   (name, head, base)
RIX_RB_NEXT  (name, base, elm)
RIX_RB_PREV  (name, base, elm)

/* Iteration */
RIX_RB_FOREACH        (var, name, head, base)   /* ascending  */
RIX_RB_FOREACH_REVERSE(var, name, head, base)   /* descending */
```

Comparator signature: `int cmp(const type *a, const type *b)` -- strict weak ordering.

---

## Cuckoo hash tables

Five header-only, index-based cuckoo hash variants.  All share:

- **16 slots per bucket** (SIMD-parallel slot scan)
- **Runtime SIMD dispatch** -- Generic / SSE4.2 / AVX2 / AVX-512 selected per source file via `rix_hash_arch_init(enable)`
- **Two candidate buckets per key** via XOR symmetry -- O(1) remove, no rehash
- **N-ahead pipelined lookup** API -- hides DRAM latency across multiple requests
- **1-origin index storage** -- `RIX_NIL = 0` marks empty slots; no raw pointers

### Variant comparison

| Variant | Header | Key storage | Node aux fields | Bucket size | Best for |
|---------|--------|------------|-----------------|-------------|----------|
| fp      | `rix_hash_fp.h`      | fingerprint in bucket, full key in node | `hash_field`                | 128 B (2 CL) | Variable-length keys, general purpose |
| slot    | `rix_hash_slot.h`    | fingerprint in bucket, full key in node | `hash_field` + `slot_field` | 128 B (2 CL) | Variable-length keys, fastest remove |
| keyonly | `rix_hash_keyonly.h` | fingerprint in bucket, full key in node | (none)                      | 128 B (2 CL) | Variable-length keys, smallest node |
| hash32  | `rix_hash32.h`       | `uint32_t` key in bucket                | (none)                      | 128 B (2 CL) | 32-bit integer keys |
| hash64  | `rix_hash64.h`       | `uint64_t` key in bucket                | (none)                      | 192 B (3 CL) | 64-bit integer keys |

All fp/slot/keyonly variants share the same bucket layout and staged-find pipeline.
`rix_hash.h` is the umbrella header that includes all five variants.

#### Find performance (DRAM-cold, pipelined, avg cycles/op)

| Pattern | fp | keyonly | slot | hash32 | hash64 |
|---------|---:|-------:|-----:|-------:|-------:|
| single (no pipeline) | 377 | 406 | 458 | 152 | 141 |
| x1 staged            | 266 | 220 | 291 |  75 | 122 |
| x4 staged            | 231 | 249 | 248 |  90 | 137 |
| x6 pipeline          | 153 | 113 | 153 |  75 | 117 |
| x8 pipeline          | 159 | 122 | 142 |  73 | 107 |

Conditions: 100 M entries (fp/slot/keyonly), 10 M entries (hash32/hash64),
74.5%/59.6% fill, sequential keys, AVX2, bucket memory >> L3 cache.
MSHR theoretical minimum: 45 cycles/op (20 MSHRs, 300 cy latency).

#### Insert / remove performance (avg cycles/op)

| Operation | fp | keyonly | slot | hash32 | hash64 |
|-----------|---:|-------:|-----:|-------:|-------:|
| insert    | 80 |  57    |  51  |  37    |  51    |
| remove    | 31 |  37    |  12  |  27    |  27    |

slot remove is fastest (12 cy/op) due to O(1) slot lookup via `slot_field`.
keyonly remove is slowest (37 cy/op) because it requires re-hashing the key.

#### Maximum fill rate

| Variant | max_fill |
|---------|----------|
| fp / slot / keyonly | 95.0% |
| hash32 / hash64     | 100%  |

hash32/hash64 achieve higher fill because key comparison completes within
the bucket (no node access required).

#### Bucket scan performance (`find_u32x16`, 16 slots/bucket, L2-warm)

The innermost hot path is the fingerprint / key scan across 16 slots per bucket.
Measured on a single core with 128 buckets resident in L2 cache:

| Build flags | Runtime level | cy/bucket | Notes |
|-------------|---------------|-----------|-------|
| `-msse4.2`       | GEN (force 0) | 36.0 | pure scalar loop |
| `-msse4.2`       | **SSE4.2**    |  6.3 | XMM 128-bit — ×5.7 vs GEN |
| `-mavx2 -msse4.2`| SSE4.2        |  6.0 | XMM 128-bit |
| `-mavx2 -msse4.2`| **AVX2**      |  3.3 | YMM 256-bit — ×1.8 vs SSE4.2 |
| `-mavx2 -msse4.2`| GEN (force 0) |  3.8 | compiler auto-vectorizes to AVX2 * |

\* With `-mavx2` the compiler applies AVX2 auto-vectorization to the GEN scalar
loop itself, so forcing `enable=0` still executes AVX2 instructions.

This table currently stops at GEN / SSE4.2 / AVX2. AVX-512 reference
measurements are documented separately in [samples/README.md](samples/README.md),
because the published AVX-512 rerun was captured later on `AMD Ryzen 9 8945HS`
(Zen 4) and should be treated as CPU-specific reference data.

**Takeaway:** The SSE4.2 tier is most beneficial on CPUs with SSE4.2 but
without AVX2 (Sandy Bridge / Ivy Bridge, 2011–2012). On AVX2 or later CPUs
the manual AVX2 path is the right choice.

---

### RIX_HASH (fingerprint, variable-length key)

Node struct must include a `hash_field` integer that stores the current-bucket
hash. `SLOT` variants additionally store the current slot in a caller-defined
integer `slot_field`.

```c
#include "rix/rix_hash.h"

/* 1. typedef required -- macro uses the bare identifier */
typedef struct mynode mynode;
struct mynode {
    uint8_t  key[16];    /* variable-length key field */
    uint32_t cur_hash;   /* current-bucket hash (any name) */
    uint32_t value;
};

/* 2. Declare head and generate the API */
RIX_HASH_HEAD(myht);
RIX_HASH_GENERATE(myht, mynode, key, cur_hash, my_cmp_fn)
/* Optional typed hash hook:
 * RIX_HASH_GENERATE_EX(myht, mynode, key, cur_hash,
 *                      my_cmp_fn, my_hash_fn)
 */

typedef struct mynode_slot mynode_slot;
struct mynode_slot {
    uint8_t  key[16];
    uint32_t cur_hash;
    uint16_t slot;
    uint16_t value;
};

RIX_HASH_HEAD(myht_slot);
RIX_HASH_GENERATE_SLOT(myht_slot, mynode_slot, key, cur_hash, slot, my_cmp_fn)

/* 3. Optional: enable SIMD in this source file */
rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

/* 4. Allocate 64-byte-aligned bucket array */
struct rix_hash_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));   /* 0 = all slots empty */
mynode *pool = calloc(N, sizeof(*pool));         /* 1-origin: pool[0] = index 1 */

struct myht head;
RIX_HASH_INIT(&head, NB_BK);   /* NB_BK must be power of 2 */
```

#### Single-shot operations

```c
/* Insert: NULL -> success; other ptr -> duplicate; elm itself -> table full */
mynode *dup = myht_insert(&head, buckets, pool, &pool[i]);

/* Find by key pointer */
mynode *hit = myht_find(&head, buckets, pool, key_ptr);

/* Remove by node pointer */
mynode *rem = myht_remove(&head, buckets, pool, &pool[i]);

/* Walk all entries: cb returns 0 to continue, non-zero to stop */
myht_walk(&head, buckets, pool, cb, arg);
```

#### Pipelined (staged) find

Issue multiple lookups in flight to hide DRAM latency:

```c
struct rix_hash_find_ctx_s ctx[4];
const void *keys[4] = { k0, k1, k2, k3 };
mynode *results[4];

/* Stage 1: hash + bucket prefetch */
myht_hash_key4(ctx, &head, buckets, keys);
/* Stage 2: fingerprint scan */
myht_scan_bk4 (ctx, &head, buckets);
/* Stage 3: full key comparison */
myht_cmp_key4 (ctx, pool, results);
```

Bulk variants: `_key1` / `_key2` / `_key4` / `_key8` (suffix = count).

#### `RIX_HASH_GENERATE` options

| Variant | Macro |
|---------|-------|
| External linkage | `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)` |
| External linkage + custom hash | `RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| External linkage + slot-aware remove | `RIX_HASH_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| External linkage + slot-aware remove + custom hash | `RIX_HASH_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |
| `static` linkage | `RIX_HASH_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn)` |
| `static` linkage + custom hash | `RIX_HASH_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| `static` linkage + slot-aware remove | `RIX_HASH_GENERATE_STATIC_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| `static` linkage + slot-aware remove + custom hash | `RIX_HASH_GENERATE_STATIC_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |

`cmp_fn` signature: `int cmp_fn(const type *a, const type *b)` -- returns 0 if equal.
`hash_fn` signature: `union rix_hash_hash_u hash_fn(const key_type *key, uint32_t mask)`.
`slot_field` may be any caller-defined integer type that can represent
`[0, RIX_HASH_BUCKET_ENTRY_SZ - 1]`.

SLOT variants maintain:

- `node->hash_field & mask == current_bucket`
- `buckets[current_bucket].idx[node->slot_field] == node_idx`

This makes `remove()` direct-slot and avoids the `idx[16]` scan used by the
non-SLOT variants.

---

### RIX_HASH32 (uint32_t key)

No `hash_field` required in the node struct.  The key itself is stored in the
bucket, so `scan_bk` performs exact 32-bit comparison.

```c
#include "rix/rix_hash32.h"

typedef struct entry32 entry32;
struct entry32 {
    uint32_t key;    /* key field -- any name */
    uint32_t value;
};

RIX_HASH32_HEAD(ht32);
RIX_HASH32_GENERATE(ht32, entry32, key)

rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

struct rix_hash32_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));
entry32 *pool = calloc(N, sizeof(*pool));

struct ht32 head;
RIX_HASH32_INIT(&head, NB_BK);
```

#### API

```c
entry32 *ht32_insert(&head, buckets, pool, &pool[i]);
entry32 *ht32_find  (&head, buckets, pool, key_value);   /* key by value */
entry32 *ht32_remove(&head, buckets, pool, key_value);
int      ht32_walk  (&head, buckets, pool, cb, arg);

/* Pipelined find (same stage pattern as rix_hash) */
struct rix_hash32_find_ctx_s ctx[4];
uint32_t keys[4] = { k0, k1, k2, k3 };
entry32 *results[4];

ht32_hash_key4(ctx, &head, buckets, keys);
ht32_scan_bk4 (ctx, &head, buckets);
ht32_cmp_key4 (ctx, pool, results);
```

---

### RIX_HASH64 (uint64_t key)

Same interface as RIX_HASH32 with `uint64_t` keys.  Bucket is 192 B (3 cache lines)
instead of 128 B.

```c
#include "rix/rix_hash64.h"

typedef struct entry64 entry64;
struct entry64 {
    uint64_t key;
    uint32_t value;
};

RIX_HASH64_HEAD(ht64);
RIX_HASH64_GENERATE(ht64, entry64, key)

struct rix_hash64_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));

struct ht64 head;
RIX_HASH64_INIT(&head, NB_BK);

entry64 *ht64_insert(&head, buckets, pool, &pool[i]);
entry64 *ht64_find  (&head, buckets, pool, key_value);
entry64 *ht64_remove(&head, buckets, pool, key_value);
```

Pipelined stages follow the same pattern: `ht64_hash_key4`, `ht64_scan_bk4`,
`ht64_cmp_key4`.

#### Important notes (all hash variants)

- `rix_hash_arch_init(enable)` is optional. Without it, each source file
  stays on the Generic path by default.
- For SIMD acceleration, call `rix_hash_arch_init(enable)` in each source file
  that uses hash operations. Pass `RIX_HASH_ARCH_AUTO` to use the best
  available SIMD level (recommended).
  Pass `RIX_HASH_ARCH_SSE` to cap at the SSE4.2 XMM tier (no AVX2).
  Pass `RIX_HASH_ARCH_AVX2` to cap at AVX2 even if AVX-512 is present.
  Pass `0` to force Generic (scalar) — useful for benchmarking.
- Bucket arrays must be **64-byte aligned** (`aligned_alloc(64, ...)` or `posix_memalign`).
- `NB_BK` must be a **power of 2** and at least 2.
- `insert` return values:
  - `NULL` -- success
  - other pointer -- duplicate (key already present)
  - `elm` itself -- table full (kickout depth exhausted)
- Operating guidance: do not plan around `100%` fill.
  Keep steady-state fill **<= 90%** as a hard upper bound, and prefer
  **<= 75%** when datapath performance matters. The sample flow-cache does
  this by running maintenance/reclaim periodically.

---

## Samples

`samples/` provides a flow-cache implementation built on top of librix.
The current tree includes three variants:

- `flow4`  : IPv4 5-tuple cache
- `flow6`  : IPv6 5-tuple cache
- `flowu`  : unified IPv4/IPv6 cache

The flow-cache code is not just a toy sample; it is the main performance
showcase in this tree. In the latest rerun on `AMD Ryzen 9 8945HS`
(`avx512`, March 23, 2026), `flow4` reached `25.94 cyc/key` at `32K` entries
and `26.60 cyc/key` at `1024K` entries on `findadd_hit`, while `flowu`
reached `341.42 cyc/key` on the larger `1024K findadd_miss` case.

There is an `avx512` dispatch tier and it has been validated on real
hardware. In `samples/fcache`, the `avx512` tier now uses dedicated
AVX-512 bucket-scan helpers, while the `avx2` tier keeps the AVX2 helpers.
The AVX2-versus-AVX-512 balance is still CPU-specific: on this Zen 4
machine, forced `avx512` improved some datapaths and regressed others, so
the published numbers are reference measurements rather than universal
expectations for all AVX-512 CPUs.

The flow-cache variants also support an allocator-independent, hugepage-friendly
size-query initialization flow via `fc_*_cache_size_query()` and
`fc_cache_size_bind()`.

See [samples/README.md](samples/README.md) for the actual API walkthrough,
detailed benchmark tables, backend-specific validation commands, and design
notes. For release-facing text, see
[CHANGELOG.md](CHANGELOG.md) and
[RELEASE_NOTES_v0.1.1.md](RELEASE_NOTES_v0.1.1.md). For GitHub metadata and
release preparation, see
[GITHUB_ABOUT.md](GITHUB_ABOUT.md),
[RELEASE_NOTES_v0.1.1_SHORT.md](RELEASE_NOTES_v0.1.1_SHORT.md), and
[PUBLIC_RELEASE_CHECKLIST.md](PUBLIC_RELEASE_CHECKLIST.md).

---

## Build

C11 required.  Suggested flags:

```sh
# AVX2 (recommended default)
cc -std=gnu11 -O3 -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror  \
   -I/path/to/librix/include       \
   your_sources.c

# AVX-512
cc -std=gnu11 -O3 -mavx512f -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror             \
   -I/path/to/librix/include                  \
   your_sources.c

# Generic scalar only (no SIMD search; CRC32C hash retained)
cc -std=gnu11 -O3 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror \
   -I/path/to/librix/include      \
   your_sources.c
```

When using the bundled test/benchmark suite, the `SIMD=` make variable
selects the SIMD level for all sub-targets:

```sh
make SIMD=avx2    # default
make SIMD=avx512
make SIMD=gen
```

Compiler and optimisation level are also overridable:

```sh
make CC=gcc   OPTLEVEL=3
make CC=clang OPTLEVEL=3
```

The current tree is expected to build with both GCC and Clang in this mode.

For `samples/fcache`, the `avx512` tier uses AVX-512 bucket-scan helpers.
The detailed measured AVX2-versus-AVX-512 numbers in
[samples/README.md](samples/README.md) are reference data from
`AMD Ryzen 9 8945HS` (Zen 4), not a blanket claim for every AVX-512 system.

For `samples/fcache`, the lookup pipeline constants default to
`FLOW_CACHE_LOOKUP_STEP_KEYS=8`,
`FLOW_CACHE_LOOKUP_AHEAD_STEPS=4`, and
`FLOW_CACHE_LOOKUP_AHEAD_KEYS=32`.
`AHEAD_KEYS` is the software-pipeline stage distance, not a count of
hardware prefetch requests.
You can override them for tuning runs via `EXTRA_CFLAGS`:

```sh
make -C samples/fcache static CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
make -C samples/test all CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
```

For address/UB sanitizers during development:

```sh
-fsanitize=address,undefined -fno-omit-frame-pointer
```

---

## Concurrency

librix has no internal synchronization.  All structures are suitable for
single-threaded use or for multi-reader/single-writer access under your own
locking (futex, pthread mutex, process-shared primitives, ...).

Lock-free / RCU operation is out of scope and would require additional design.

---

## Testing

```sh
# Full library test pass
make test

# Top-level representative benchmark rerun
make bench

# Full long-running benchmark rerun
make bench-full
```

For flow-cache-specific validation and benchmark commands, see
[samples/README.md](samples/README.md).

Test coverage includes:

- Empty / singleton / multi-element transitions for every operation
- All insert/remove variants
- Safe iteration while removing elements
- Red-Black invariants (root black, no red-red, equal black height)
- Hash table: duplicate detection, staged pipeline correctness, walk count
- Fuzz: random insert/find/remove verified against a reference model
- Flow cache: find, remove, flush, expire, batch lookup, insert exhaustion

### Hash table test coverage matrix

All five cuckoo hash variants pass the full test suite:

| Test | fp | slot | keyonly | hash32 | hash64 |
|------|:--:|:----:|:------:|:------:|:------:|
| init/empty           | PASS | PASS | PASS | PASS | PASS |
| insert/find/remove   | PASS | PASS | PASS | PASS | PASS |
| duplicate insert     | PASS | PASS | PASS | PASS | PASS |
| remove miss          | PASS | PASS | PASS | PASS | PASS |
| staged find (x1/x2/x4) | PASS | -  | -    | PASS | PASS |
| walk                 | PASS | -    | -    | PASS | PASS |
| high_fill (90%+)     | 93.8% | 93.8% | 93.8% | 93.8% | 93.8% |
| max_fill             | 95.0% | 95.0% | 95.0% | 100%  | 100%  |
| kickout_safety       | PASS | PASS | PASS | PASS | PASS |
| fuzz (200K ops)      | PASS | PASS | PASS | PASS | PASS |
| fuzz (500K ops)      | PASS | PASS | PASS | PASS | PASS |

Key observations:

- **Kickout safety**: All variants show zero element loss during cuckoo
  kickout chains, confirming the non-destructive recursive kickout
  implementation.
- **Fill rate**: fp/slot/keyonly reach 95% of total slots.  hash32/hash64
  reach 100% because key comparison is bucket-local.
- **Fuzz testing**: Random insert/find/remove sequences (up to 500K ops)
  verified against an in-memory reference set for all variants.

---

## License

BSD 3-Clause.  See [LICENSE](LICENSE).

---

## Design Lineage

Queue and tree APIs mirror the BSD `sys/queue.h` / `sys/tree.h` interfaces,
replacing raw pointers with indices to enable robust shared-memory deployments.

The cuckoo hash table follows the XOR-based two-bucket scheme for O(1)
amortised insert with prefetch-driven staged lookup.
