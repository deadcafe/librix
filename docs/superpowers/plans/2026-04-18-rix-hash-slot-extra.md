# rix_hash slot_extra variant - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a 192B `rix_hash_bucket_extra_s` / `RIX_HASH_GENERATE_SLOT_EXTRA` variant that mirrors the existing slot variant but carries a per-slot `u32 extra[]` value through insert / kickout / remove, with validation at the rix_hash layer only. flowtable adoption is out of scope.

**Architecture:** New opt-in header `include/rix/rix_hash_slot_extra.h` that redefines the bucket struct with a third cache line for `extra[]`, plus a parallel find-context type. The GENERATE macro is a transformed copy of `RIX_HASH_GENERATE_SLOT_INTERNAL` from `rix_hash_slot.h`: bucket type swapped, `extra` parameter added to insert / insert_hashed_idx, flipflop carries `extra[old] -> extra[new]`, remove_at clears `extra[slot] = 0`. Umbrella `rix_hash.h` does **not** include the new header (explicit opt-in).

**Tech Stack:** C11, GNU extensions, BSD queue-style macros, SSE4.2 / AVX2 / AVX-512 runtime dispatch via `rix_hash_arch_init`, macro-generated per-name functions with token pasting.

---

## Reference Files

The primary source to clone and transform is `include/rix/rix_hash_slot.h`. Read it once before starting.

Related:
- `include/rix/rix_hash_common.h` — existing bucket struct, find_ctx, helpers
- `include/rix/rix_hash_arch.h` — `RIX_HASH_BUCKET_ENTRY_SZ`, SIMD ops
- `include/rix/rix_hash.h` — umbrella (do **not** modify)
- `tests/hashtbl/test_rix_hash.c` — reference test style
- `tests/hashtbl/bench_rix_hash.c` — reference bench style
- `tests/hashtbl/Makefile`, `tests/hashtbl/rix_hash.h` — reference build layout
- `mk/simd.mk` — SIMD build flags
- `Makefile` (top-level) — where to register `tests/hashtbl_extra`

## File Structure

To be created:
- `include/rix/rix_hash_slot_extra.h` — types, PROTOTYPE / GENERATE macros, convenience macro, inline helpers
- `tests/hashtbl_extra/Makefile` — build targets for test + bench
- `tests/hashtbl_extra/rix_hash.h` — compatibility shim (mirrors `tests/hashtbl/rix_hash.h`)
- `tests/hashtbl_extra/test_rix_hash_extra.c` — functional tests
- `tests/hashtbl_extra/bench_rix_hash_extra.c` — micro-benchmarks

To be modified:
- `Makefile` (top-level) — add `tests/hashtbl_extra` to `TESTDIRS` and `BENCH_FULL_DIRS`

Do **not** modify `include/rix/rix_hash.h`, `include/rix/rix_hash_slot.h`, `include/rix/rix_hash_common.h`.

## Macro Rename Table

When porting macro text from `rix_hash_slot.h` to `rix_hash_slot_extra.h`, apply these string substitutions to macro bodies **inside** `RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL`:

| Classic | Extra |
|---|---|
| `struct rix_hash_bucket_s` | `struct rix_hash_bucket_extra_s` |
| `struct rix_hash_find_ctx_s` | `struct rix_hash_find_ctx_extra_s` |
| `rix_hash_prefetch_bucket_of(` | `rix_hash_prefetch_extra_bucket_of(` |
| `rix_hash_prefetch_bucket_hashes_of(` | `rix_hash_prefetch_extra_bucket_hashes_of(` |
| `rix_hash_prefetch_bucket_indices_of(` | `rix_hash_prefetch_extra_bucket_indices_of(` |
| `rix_hash_bucket_of_idx(` | `rix_hash_extra_bucket_of_idx(` |
| `rix_hash_bucket_hashes_of_idx(` | `rix_hash_extra_bucket_hashes_of_idx(` |
| `rix_hash_bucket_indices_of_idx(` | `rix_hash_extra_bucket_indices_of_idx(` |
| `rix_hash_prefetch_bucket_hashes_of_idx(` | `rix_hash_prefetch_extra_bucket_hashes_of_idx(` |
| `rix_hash_prefetch_bucket_indices_of_idx(` | `rix_hash_prefetch_extra_bucket_indices_of_idx(` |

Top-level macro names:

| Classic | Extra |
|---|---|
| `RIX_HASH_PROTOTYPE_SLOT` | `RIX_HASH_PROTOTYPE_SLOT_EXTRA` |
| `RIX_HASH_PROTOTYPE_SLOT_EX` | `RIX_HASH_PROTOTYPE_SLOT_EXTRA_EX` |
| `RIX_HASH_PROTOTYPE_STATIC_SLOT` | `RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA` |
| `RIX_HASH_PROTOTYPE_STATIC_SLOT_EX` | `RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA_EX` |
| `RIX_HASH_GENERATE_SLOT` | `RIX_HASH_GENERATE_SLOT_EXTRA` |
| `RIX_HASH_GENERATE_SLOT_EX` | `RIX_HASH_GENERATE_SLOT_EXTRA_EX` |
| `RIX_HASH_GENERATE_STATIC_SLOT` | `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA` |
| `RIX_HASH_GENERATE_STATIC_SLOT_EX` | `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX` |
| `RIX_HASH_GENERATE_SLOT_INTERNAL` | `RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL` |
| `RIX_HASH_SLOT_DEFINE_INDEXERS` | `RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS` |

`RIX_HASH_PROTOTYPE_INTERNAL`, `RIX_HASH_HEAD`, `RIX_HASH_KEY_TYPE`, `RIX_HASH_SLOT_TYPE`, `RIX_HASH_DEFINE_DEFAULT_HASH_FN`, `RIX_HASH_DEFAULT_HASH_FN_NAME`, `rix_hash_fp`, `RIX_HASH_FIND_U32X16`, `RIX_HASH_FIND_U32X16_2`, `rix_hash_prefetch_entry_of_idx` are **reused as-is** from common headers.

## Functional Deltas from Classic

Compared to `RIX_HASH_GENERATE_SLOT_INTERNAL` in `include/rix/rix_hash_slot.h`:

1. **`name##_insert` signature** adds a trailing `u32 extra` parameter.
2. **`name##_insert_hashed` / `name##_insert_hashed_idx`** add the same trailing `u32 extra` parameter and thread it to the final write site.
3. **`name##_insert_hashed_idx`** writes `_bk->extra[_slot] = extra` alongside `_bk->hash[_slot] = _fp` at every placement site (fast path x2 and kickout slow path).
4. **`name##_flipflop`** reads `_bk->extra[slot]` before clearing, writes `_alt->extra[_slot] = _extra_save`, then sets `_bk->extra[slot] = 0u`.
5. **`name##_remove_at`** adds `_b->extra[slot] = 0u` alongside existing `hash[slot] = 0` / `idx[slot] = RIX_NIL`.
6. **All bucket pointer types** use `struct rix_hash_bucket_extra_s` instead of `struct rix_hash_bucket_s`.
7. **All staged-find ctx** uses `struct rix_hash_find_ctx_extra_s`.
8. **find / walk / staged find** otherwise unchanged (do not read or write `extra[]`).

Convenience macro added:
```c
#define RIX_HASH_INSERT_EXTRA(name, head, buckets, base, elm, extra)           \
    name##_insert(head, buckets, base, elm, extra)
```

---

## Task 1: Scaffold new header and test directory

**Files:**
- Create: `include/rix/rix_hash_slot_extra.h`
- Create: `tests/hashtbl_extra/Makefile`
- Create: `tests/hashtbl_extra/rix_hash.h`
- Create: `tests/hashtbl_extra/test_rix_hash_extra.c`

The scaffold header defines only the bucket / ctx types and inline helpers; it has NO GENERATE macro yet. A minimal test verifies the header compiles and types are the right size.

- [ ] **Step 1.1: Create `include/rix/rix_hash_slot_extra.h` skeleton**

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_slot_extra.h - slot variant with a third per-slot u32 (extra[]).
 *
 * Bucket layout (192B, 3 cache lines):
 *   hash [16]  (64B, cl0) - fingerprints
 *   idx  [16]  (64B, cl1) - 1-origin node idx
 *   extra[16]  (64B, cl2) - user-defined u32
 *
 * Opt-in only. Not included from rix_hash.h umbrella.
 */

#ifndef _RIX_HASH_SLOT_EXTRA_H_
#  define _RIX_HASH_SLOT_EXTRA_H_

#  include "rix_hash_common.h"

struct rix_hash_bucket_extra_s {
    u32 hash [RIX_HASH_BUCKET_ENTRY_SZ];
    u32 idx  [RIX_HASH_BUCKET_ENTRY_SZ];
    u32 extra[RIX_HASH_BUCKET_ENTRY_SZ];
} __attribute__((aligned(64)));

struct rix_hash_find_ctx_extra_s {
    union rix_hash_hash_u            hash;
    struct rix_hash_bucket_extra_s  *bk[2];
    const void                      *key;
    u32  fp;
    u32  fp_hits[2];
    u32  empties[2];
};

/* ---- accessor / prefetch helpers --------------------------------------- */

static RIX_FORCE_INLINE struct rix_hash_bucket_extra_s *
rix_hash_extra_bucket_of_idx(struct rix_hash_bucket_extra_s *buckets,
                             unsigned bk_idx)
{
    return &buckets[bk_idx];
}

static RIX_FORCE_INLINE u32 *
rix_hash_extra_bucket_hashes_of_idx(struct rix_hash_bucket_extra_s *buckets,
                                    unsigned bk_idx)
{
    return buckets[bk_idx].hash;
}

static RIX_FORCE_INLINE u32 *
rix_hash_extra_bucket_indices_of_idx(struct rix_hash_bucket_extra_s *buckets,
                                     unsigned bk_idx)
{
    return buckets[bk_idx].idx;
}

static RIX_FORCE_INLINE u32 *
rix_hash_extra_bucket_extras_of_idx(struct rix_hash_bucket_extra_s *buckets,
                                    unsigned bk_idx)
{
    return buckets[bk_idx].extra;
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_hashes_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->hash[0], 0, 1);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_indices_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->idx[0], 0, 1);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_extras_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->extra[0], 0, 1);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    rix_hash_prefetch_extra_bucket_hashes_of(bucket);
    rix_hash_prefetch_extra_bucket_indices_of(bucket);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_full_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    rix_hash_prefetch_extra_bucket_hashes_of(bucket);
    rix_hash_prefetch_extra_bucket_indices_of(bucket);
    rix_hash_prefetch_extra_bucket_extras_of(bucket);
}

#  define rix_hash_prefetch_extra_bucket_hashes_of_idx(buckets, bk_idx)       \
    rix_hash_prefetch_extra_bucket_hashes_of(&(buckets)[(unsigned)(bk_idx)])

#  define rix_hash_prefetch_extra_bucket_indices_of_idx(buckets, bk_idx)      \
    rix_hash_prefetch_extra_bucket_indices_of(&(buckets)[(unsigned)(bk_idx)])

#  define rix_hash_prefetch_extra_bucket_extras_of_idx(buckets, bk_idx)       \
    rix_hash_prefetch_extra_bucket_extras_of(&(buckets)[(unsigned)(bk_idx)])

/* PROTOTYPE / GENERATE macros are added in Task 2. */

#endif /* _RIX_HASH_SLOT_EXTRA_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
```

- [ ] **Step 1.2: Create `tests/hashtbl_extra/rix_hash.h` compatibility shim**

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Compatibility shim for tests/hashtbl_extra. Pulls in the slot_extra
 * variant in addition to the umbrella. The umbrella does NOT include
 * rix_hash_slot_extra.h, so tests must include it explicitly.
 */
#include "../../include/rix/rix_hash.h"
#include "../../include/rix/rix_hash_slot_extra.h"
```

- [ ] **Step 1.3: Create `tests/hashtbl_extra/Makefile`**

```make
#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#

CURDIR := $(PWD)
TOPDIR := $(shell cd ../.. && pwd)
include $(TOPDIR)/mk/simd.mk

EXTRA_CFLAGS ?=
BENCH_EXTRA_CFLAGS ?= $(EXTRA_CFLAGS) -DNDEBUG
CFLAGS       = -std=gnu11 -g -O2 $(SIMD_FLAGS) -Wall -Wextra -Wvla -Werror -I$(CURDIR) -I../../include $(EXTRA_CFLAGS)
BENCH_CFLAGS = -std=gnu11 -O3 $(SIMD_FLAGS) -Wall -Wextra -Wvla -Werror -I$(CURDIR) -I../../include $(BENCH_EXTRA_CFLAGS)
DEPFLAGS     = -MMD -MP
DEPS         = $(TEST_TARGET).d $(BENCH_TARGET).d

TEST_TARGET  = hash_extra_test
TEST_SRC     = test_rix_hash_extra.c

BENCH_TARGET = hash_extra_bench
BENCH_SRC    = bench_rix_hash_extra.c

.PHONY: all clean test bench
all: $(TEST_TARGET) $(BENCH_TARGET)

$(TEST_TARGET): $(TEST_SRC) rix_hash.h
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(TEST_SRC)

$(BENCH_TARGET): $(BENCH_SRC) rix_hash.h
	$(CC) $(BENCH_CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_SRC)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(BENCH_TARGET)
	./$(BENCH_TARGET)

clean:
	rm -f $(TEST_TARGET) $(BENCH_TARGET) $(DEPS) *~ core core.*

-include $(DEPS)
```

- [ ] **Step 1.4: Create `tests/hashtbl_extra/test_rix_hash_extra.c` (smoke test only)**

```c
/* test_rix_hash_extra.c - slot_extra variant tests */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    test_bucket_layout();
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 1.5: Stub bench file so Makefile builds**

Create `tests/hashtbl_extra/bench_rix_hash_extra.c`:

```c
/* bench_rix_hash_extra.c - placeholder, real bench added in Task 10 */
#include <stdio.h>
int main(void) { printf("bench stub\n"); return 0; }
```

- [ ] **Step 1.6: Verify build**

Run:

```
cd tests/hashtbl_extra && make clean && make
```

Expected: `hash_extra_test` and `hash_extra_bench` produced. No warnings.

- [ ] **Step 1.7: Verify smoke test**

Run:

```
cd tests/hashtbl_extra && ./hash_extra_test
```

Expected output:

```
[T] bucket layout
PASS
```

- [ ] **Step 1.8: Commit**

```
git add include/rix/rix_hash_slot_extra.h tests/hashtbl_extra/
git commit -m "Add rix_hash_slot_extra scaffold (types + helpers)"
```

---

## Task 2: Add GENERATE_SLOT_EXTRA macros with full semantics

Append to `include/rix/rix_hash_slot_extra.h`. Port `RIX_HASH_GENERATE_SLOT_INTERNAL` from `rix_hash_slot.h` applying the rename table and functional deltas from the plan header.

**Files:**
- Modify: `include/rix/rix_hash_slot_extra.h` (append PROTOTYPE / GENERATE / convenience macros)
- Modify: `tests/hashtbl_extra/test_rix_hash_extra.c` (add first round-trip test)

- [ ] **Step 2.1: Append PROTOTYPE macros**

Add before the `#endif` line of `rix_hash_slot_extra.h`:

```c
#  define RIX_HASH_PROTOTYPE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_GENERATE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn, hash_fn, )

#  define RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn, hash_fn,        \
                                          RIX_UNUSED static)

#  define RIX_HASH_GENERATE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                     \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn,                  \
                                          RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_GENERATE_STATIC_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                     \
    RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field,  \
                                          slot_field, cmp_fn,                  \
                                          RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                          RIX_UNUSED static)

#  ifndef RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS
#    define RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS(name, type)                    \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p) {                        \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i) {                                  \
    return (struct type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);    \
}
#  endif
```

- [ ] **Step 2.2: Append `RIX_HASH_GENERATE_SLOT_EXTRA_INTERNAL` macro body**

This is a verbatim clone of `RIX_HASH_GENERATE_SLOT_INTERNAL` from `rix_hash_slot.h` (lines 68-550) with these precise transformations applied to the macro body:

1. Wherever `struct rix_hash_bucket_s` appears, change to `struct rix_hash_bucket_extra_s`.
2. Wherever `struct rix_hash_find_ctx_s` appears, change to `struct rix_hash_find_ctx_extra_s`.
3. Every `rix_hash_prefetch_bucket_of(...)` call site -> `rix_hash_prefetch_extra_bucket_of(...)`.
4. The line `RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)` -> `RIX_HASH_SLOT_EXTRA_DEFINE_INDEXERS(name, type)`.
5. `name##_insert_hashed_idx` signature adds a trailing `u32 extra` parameter. Inside its body, at every slot-write site, add `_bk->extra[_slot] = extra;` (fast-path loop, and kickout slow-path final placement). See step 2.3 for exact sites.
6. `name##_insert_hashed` signature adds a trailing `u32 extra` parameter; forwards to `name##_insert_hashed_idx`.
7. `name##_insert` signature adds a trailing `u32 extra` parameter; forwards to `name##_insert_hashed`.
8. `name##_flipflop` preserves `extra`: snapshot `_bk->extra[slot]` before touching anything else; write `_alt->extra[_slot] = _extra_save`; clear `_bk->extra[slot] = 0u` at the end. See step 2.4 for exact change.
9. `name##_remove_at` adds `_b->extra[slot] = 0u` right after `_b->idx[slot] = (u32)RIX_NIL`.

Write the full macro. Sections 2.3-2.5 below list the precise line-level deltas you need to get right; everything else is a mechanical copy.

- [ ] **Step 2.3: `insert_hashed_idx` delta (in the generated macro body)**

Final body shape (based on `rix_hash_slot.h` lines 381-458 transformed):

```c
static RIX_UNUSED RIX_FORCE_INLINE u32                                   \
name##_insert_hashed_idx(struct name *head,                                   \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         struct type *base,                                   \
                         struct type *elm,                                    \
                         union rix_hash_hash_u _h,                            \
                         u32 extra)                                           \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned _bk0, _bk1;                                                      \
    u32 _elm_idx = name##_hidx(base, elm);                                    \
    u32 _fp;                                                                  \
    u32 _hits_fp[2];                                                          \
    u32 _hits_zero[2];                                                        \
    _fp = rix_hash_fp(_h, mask, &_bk0, &_bk1);                                \
    elm->hash_field = _h.val32[0];                                            \
    rix_hash_prefetch_extra_bucket_of(buckets + _bk0);                        \
    rix_hash_prefetch_extra_bucket_of(buckets + _bk1);                        \
                                                                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_extra_s *_bk =                                 \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        RIX_HASH_FIND_U32X16_2(_bk->hash, _fp, 0u,                            \
                                &_hits_fp[_i], &_hits_zero[_i]);              \
    }                                                                         \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_extra_s *_bk =                                 \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        u32 _hits = _hits_fp[_i];                                             \
        while (_hits) {                                                       \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            _hits &= _hits - 1u;                                              \
            u32 _node_idx = _bk->idx[_bit];                                   \
            struct type *_node = name##_hptr(base, _node_idx);                \
            RIX_ASSUME_NONNULL(_node);                                        \
            if (cmp_fn(&elm->key_field, &_node->key_field) == 0)              \
                return _node_idx;                                             \
        }                                                                     \
    }                                                                         \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_extra_s *_bk = buckets + _bki;                 \
        u32 _nilm = _hits_zero[_i];                                           \
        if (_nilm) {                                                          \
            unsigned _slot = (unsigned)__builtin_ctz(_nilm);                  \
            _bk->hash [_slot] = _fp;                                          \
            _bk->idx  [_slot] = _elm_idx;                                     \
            _bk->extra[_slot] = extra;                                        \
            if (_i == 1)                                                      \
                elm->hash_field = _h.val32[1];                                \
            elm->slot_field = (RIX_HASH_SLOT_TYPE(type, slot_field))_slot;    \
            head->rhh_nb++;                                                   \
            return 0u;                                                        \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Slow path: kickout */                                                  \
    {                                                                         \
        int _pos;                                                             \
        unsigned _bki;                                                        \
        _pos = name##_kickout(buckets, base, mask, _bk0,                      \
                              RIX_HASH_FOLLOW_DEPTH);                         \
        if (_pos >= 0) {                                                      \
            _bki = _bk0;                                                      \
        } else {                                                              \
            _pos = name##_kickout(buckets, base, mask, _bk1,                  \
                                  RIX_HASH_FOLLOW_DEPTH);                     \
            if (_pos < 0)                                                     \
                return _elm_idx;                                              \
            _bki = _bk1;                                                      \
            elm->hash_field = _h.val32[1];                                    \
        }                                                                     \
        struct rix_hash_bucket_extra_s *_bk = buckets + _bki;                 \
        _bk->hash [_pos] = _fp;                                               \
        _bk->idx  [_pos] = _elm_idx;                                          \
        _bk->extra[_pos] = extra;                                             \
        elm->slot_field = (RIX_HASH_SLOT_TYPE(type, slot_field))_pos;         \
        head->rhh_nb++;                                                       \
        return 0u;                                                            \
    }                                                                         \
}
```

Forwarders:

```c
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_insert_hashed(struct name *head,                                       \
                     struct rix_hash_bucket_extra_s *buckets,                 \
                     struct type *base,                                       \
                     struct type *elm,                                        \
                     union rix_hash_hash_u _h,                                \
                     u32 extra)                                               \
{                                                                             \
    u32 _ret_idx =                                                            \
        name##_insert_hashed_idx(head, buckets, base, elm, _h, extra);        \
    return (_ret_idx == 0u) ? NULL : name##_hptr(base, _ret_idx);             \
}                                                                             \
                                                                              \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_extra_s *buckets,                        \
              struct type *base,                                              \
              struct type *elm,                                               \
              u32 extra)                                                      \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field,  \
                mask);                                                        \
    return name##_insert_hashed(head, buckets, base, elm, _h, extra);         \
}
```

- [ ] **Step 2.4: `flipflop` delta**

```c
static RIX_UNUSED int                                                         \
name##_flipflop(struct rix_hash_bucket_extra_s *buckets,                      \
                struct type *base,                                            \
                unsigned mask,                                                \
                unsigned bk_idx,                                              \
                unsigned slot)                                                \
{                                                                             \
    struct rix_hash_bucket_extra_s *_bk = buckets + bk_idx;                   \
    u32      _fp         = _bk->hash [slot];                                  \
    unsigned _idx        = _bk->idx  [slot];                                  \
    u32      _extra_save = _bk->extra[slot];                                  \
    struct type *_nd = name##_hptr(base, _idx);                               \
    if (!_nd) return -1;                                                      \
    u32 _h  = _nd->hash_field;                                                \
    unsigned _ab = (_fp ^ _h) & mask;                                         \
    int _slot = name##_find_empty(buckets, _ab);                              \
    if (_slot < 0) return -1;                                                 \
    struct rix_hash_bucket_extra_s *_alt = buckets + _ab;                     \
    _alt->hash [_slot] = _fp;                                                 \
    _alt->idx  [_slot] = _idx;                                                \
    _alt->extra[_slot] = _extra_save;                                         \
    _nd->hash_field = _fp ^ _h;                                               \
    _nd->slot_field = (RIX_HASH_SLOT_TYPE(type, slot_field))_slot;            \
    _bk->hash [slot] = 0u;                                                    \
    _bk->idx  [slot] = (u32)RIX_NIL;                                          \
    _bk->extra[slot] = 0u;                                                    \
    return (int)slot;                                                         \
}
```

- [ ] **Step 2.5: `remove_at` delta**

```c
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_extra_s *buckets,                     \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    struct rix_hash_bucket_extra_s *_b = buckets + bk;                        \
    unsigned _idx;                                                            \
    RIX_ASSERT(slot < RIX_HASH_BUCKET_ENTRY_SZ);                              \
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                     \
        return (unsigned)RIX_NIL;                                             \
    _idx = _b->idx[slot];                                                     \
    if (_idx == (unsigned)RIX_NIL)                                            \
        return (unsigned)RIX_NIL;                                             \
    _b->hash [slot] = 0u;                                                     \
    _b->idx  [slot] = (u32)RIX_NIL;                                           \
    _b->extra[slot] = 0u;                                                     \
    head->rhh_nb--;                                                           \
    return _idx;                                                              \
}
```

- [ ] **Step 2.6: Append convenience macro**

```c
#  define RIX_HASH_INSERT_EXTRA(name, head, buckets, base, elm, extra)         \
    name##_insert(head, buckets, base, elm, extra)
```

- [ ] **Step 2.7: Add first roundtrip test**

Append to `test_rix_hash_extra.c` before `main()`:

```c
struct ek {
    u64 hi;
    u64 lo;
};

struct enode {
    u32  cur_hash;  /* hash_field */
    u16  slot;      /* slot_field */
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
```

And add the call in `main`:

```c
int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    test_bucket_layout();
    test_insert_find_remove_basic();
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2.8: Verify build + test**

Run:

```
cd tests/hashtbl_extra && make clean && make && ./hash_extra_test
```

Expected: both tests PASS. No warnings.

- [ ] **Step 2.9: Commit**

```
git add include/rix/rix_hash_slot_extra.h tests/hashtbl_extra/test_rix_hash_extra.c
git commit -m "Add RIX_HASH_GENERATE_SLOT_EXTRA macro with extra[] semantics"
```

---

## Task 3: Test extra preservation across kickout

**Files:**
- Modify: `tests/hashtbl_extra/test_rix_hash_extra.c`

- [ ] **Step 3.1: Add kickout test**

Append:

```c
#define NB_KICK    256u
#define NB_BK_KICK  32u  /* 32 * 16 = 512 slots -> ~50% fill */

static struct enode                     k_pool[NB_KICK];
static struct rix_hash_bucket_extra_s   k_bk[NB_BK_KICK]
    __attribute__((aligned(64)));
static struct eht                       k_head;

static void
test_kickout_preserves_extra(void)
{
    printf("[T] kickout preserves extra\n");
    memset(k_pool, 0, sizeof(k_pool));
    memset(k_bk,   0, sizeof(k_bk));
    RIX_HASH_INIT(eht, &k_head, NB_BK_KICK);

    for (unsigned i = 0; i < NB_KICK; i++) {
        k_pool[i].key.hi = (u64)(0xA5A5A500u + i);
        k_pool[i].key.lo = (u64)(0x5A5A5A00u + i) * 17ULL;
    }

    for (unsigned i = 0; i < NB_KICK; i++) {
        u32 expected_extra = 0xC0DE0000u | i;
        struct enode *dup = eht_insert(&k_head, k_bk, k_pool,
                                       &k_pool[i], expected_extra);
        if (dup != NULL)
            FAILF("kick insert[%u] unexpected dup %p", i, (void *)dup);
    }
    if (k_head.rhh_nb != NB_KICK)
        FAILF("kick rhh_nb expected %u got %u", NB_KICK, k_head.rhh_nb);

    for (unsigned i = 0; i < NB_KICK; i++) {
        struct enode *f = eht_find(&k_head, k_bk, k_pool, &k_pool[i].key);
        if (f != &k_pool[i])
            FAILF("kick find[%u] wrong ptr", i);
        unsigned bk   = f->cur_hash & k_head.rhh_mask;
        unsigned slot = f->slot;
        u32 expected  = 0xC0DE0000u | i;
        if (k_bk[bk].extra[slot] != expected)
            FAILF("kick extra[%u]: expected 0x%x got 0x%x at bk=%u slot=%u",
                  i, expected, k_bk[bk].extra[slot], bk, slot);
    }
}
```

And call from `main`:

```c
    test_kickout_preserves_extra();
```

- [ ] **Step 3.2: Run**

```
cd tests/hashtbl_extra && make && ./hash_extra_test
```

Expected: `[T] kickout preserves extra` passes.

- [ ] **Step 3.3: Commit**

```
git add tests/hashtbl_extra/test_rix_hash_extra.c
git commit -m "Test extra[] preservation across kickouts"
```

---

## Task 4: Test remove clears extra

**Files:**
- Modify: `tests/hashtbl_extra/test_rix_hash_extra.c`

- [ ] **Step 4.1: Add remove clear test**

```c
static void
test_remove_clears_extra(void)
{
    printf("[T] remove clears extra\n");
    e_basic_init();

    for (unsigned i = 0; i < NB_BASIC; i++)
        eht_insert(&e_head, e_bk, e_basic, &e_basic[i], 0xABCD0000u | i);

    /* Snapshot bk/slot before removal */
    unsigned saved_bk[NB_BASIC];
    unsigned saved_slot[NB_BASIC];
    for (unsigned i = 0; i < NB_BASIC; i++) {
        saved_bk[i]   = e_basic[i].cur_hash & e_head.rhh_mask;
        saved_slot[i] = e_basic[i].slot;
    }

    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct enode *r = eht_remove(&e_head, e_bk, e_basic, &e_basic[i]);
        if (r != &e_basic[i])
            FAILF("remove[%u] wrong ptr", i);
        if (e_bk[saved_bk[i]].extra[saved_slot[i]] != 0u)
            FAILF("remove[%u] did not clear extra at bk=%u slot=%u (got 0x%x)",
                  i, saved_bk[i], saved_slot[i],
                  e_bk[saved_bk[i]].extra[saved_slot[i]]);
        if (e_bk[saved_bk[i]].idx[saved_slot[i]] != (u32)RIX_NIL)
            FAILF("remove[%u] did not clear idx at bk=%u slot=%u",
                  i, saved_bk[i], saved_slot[i]);
        if (e_bk[saved_bk[i]].hash[saved_slot[i]] != 0u)
            FAILF("remove[%u] did not clear hash at bk=%u slot=%u",
                  i, saved_bk[i], saved_slot[i]);
    }
    if (e_head.rhh_nb != 0u)
        FAILF("rhh_nb expected 0 after full remove, got %u", e_head.rhh_nb);
}
```

And call from `main`. Verify extras are per-entry unique so saved_bk/slot stays valid through iterations (kickouts only occur during insert, not remove — so saved snapshots remain correct).

- [ ] **Step 4.2: Run + commit**

```
cd tests/hashtbl_extra && make && ./hash_extra_test
git add tests/hashtbl_extra/test_rix_hash_extra.c
git commit -m "Test remove clears bucket extra[]"
```

---

## Task 5: Test staged find x1 parity with single-shot find

**Files:**
- Modify: `tests/hashtbl_extra/test_rix_hash_extra.c`

- [ ] **Step 5.1: Add staged find test**

```c
static void
test_staged_find_x1(void)
{
    printf("[T] staged find x1\n");
    e_basic_init();
    for (unsigned i = 0; i < NB_BASIC; i++)
        eht_insert(&e_head, e_bk, e_basic, &e_basic[i], 0xE1E1u + i);

    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct rix_hash_find_ctx_extra_s ctx;
        RIX_HASH_HASH_KEY(eht, &ctx, &e_head, e_bk, &e_basic[i].key);
        RIX_HASH_SCAN_BK (eht, &ctx, &e_head, e_bk);
        RIX_HASH_PREFETCH_NODE(eht, &ctx, e_basic);
        struct enode *n = RIX_HASH_CMP_KEY(eht, &ctx, e_basic);
        if (n != &e_basic[i])
            FAILF("staged find[%u] expected %p got %p",
                  i, (void *)&e_basic[i], (void *)n);
    }
}

static void
test_staged_find_xN(void)
{
    printf("[T] staged find xN\n");
    e_basic_init();
    for (unsigned i = 0; i < NB_BASIC; i++)
        eht_insert(&e_head, e_bk, e_basic, &e_basic[i], 0xFEFEu + i);

    enum { N = 4 };
    struct rix_hash_find_ctx_extra_s ctx[N];
    const struct ek *keys[N];
    struct enode    *res[N];

    for (unsigned base = 0; base + N <= NB_BASIC; base += N) {
        for (unsigned i = 0; i < N; i++)
            keys[i] = &e_basic[base + i].key;
        RIX_HASH_HASH_KEY_N(eht, ctx, N, &e_head, e_bk, keys);
        RIX_HASH_SCAN_BK_N (eht, ctx, N, &e_head, e_bk);
        RIX_HASH_PREFETCH_NODE_N(eht, ctx, N, e_basic);
        RIX_HASH_CMP_KEY_N (eht, ctx, N, e_basic, res);
        for (unsigned i = 0; i < N; i++) {
            if (res[i] != &e_basic[base + i])
                FAILF("staged_n[%u+%u] expected %p got %p",
                      base, i, (void *)&e_basic[base + i], (void *)res[i]);
        }
    }
}
```

Call both from `main`.

- [ ] **Step 5.2: Run + commit**

```
cd tests/hashtbl_extra && make && ./hash_extra_test
git add tests/hashtbl_extra/test_rix_hash_extra.c
git commit -m "Test staged find (x1 + xN) for slot_extra variant"
```

---

## Task 6: Test classic and extra variants coexisting in one TU

**Files:**
- Modify: `tests/hashtbl_extra/test_rix_hash_extra.c`

- [ ] **Step 6.1: Generate a classic slot table in the same TU and cross-check**

Append:

```c
/* Classic slot variant, distinct name prefix */
struct cnode {
    u32 cur_hash;
    u16 slot;
    u16 _pad;
    struct ek key;
};

RIX_HASH_HEAD(cht);
RIX_HASH_GENERATE_SLOT(cht, cnode, key, cur_hash, slot, ek_cmp)

static void
test_coexist_classic_extra(void)
{
    printf("[T] classic + extra coexist in one TU\n");

    struct cnode c_pool[NB_BASIC];
    struct rix_hash_bucket_s c_bk[NB_BK_BASIC] __attribute__((aligned(64)));
    struct cht c_head;

    memset(c_pool, 0, sizeof(c_pool));
    memset(c_bk,   0, sizeof(c_bk));
    RIX_HASH_INIT(cht, &c_head, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        c_pool[i].key.hi = (u64)(i + 1);
        c_pool[i].key.lo = 0xCAFEBABEu;
    }
    for (unsigned i = 0; i < NB_BASIC; i++)
        if (cht_insert(&c_head, c_bk, c_pool, &c_pool[i]) != NULL)
            FAILF("classic insert[%u] unexpected dup", i);

    e_basic_init();
    for (unsigned i = 0; i < NB_BASIC; i++)
        if (eht_insert(&e_head, e_bk, e_basic, &e_basic[i], i) != NULL)
            FAILF("extra insert[%u] unexpected dup", i);

    /* Both tables must find their own entries */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        if (cht_find(&c_head, c_bk, c_pool, &c_pool[i].key) != &c_pool[i])
            FAILF("classic find[%u] wrong", i);
        if (eht_find(&e_head, e_bk, e_basic, &e_basic[i].key) != &e_basic[i])
            FAILF("extra find[%u] wrong", i);
    }

    /* Classic bucket type is 128B, extra type is 192B -
       independence check. */
    if (sizeof(struct rix_hash_bucket_s) !=
            2u * RIX_HASH_BUCKET_ENTRY_SZ * sizeof(u32))
        FAIL("classic bucket size wrong");
    if (sizeof(struct rix_hash_bucket_extra_s) !=
            3u * RIX_HASH_BUCKET_ENTRY_SZ * sizeof(u32))
        FAIL("extra bucket size wrong");
}
```

Call from `main`.

- [ ] **Step 6.2: Run + commit**

```
cd tests/hashtbl_extra && make && ./hash_extra_test
git add tests/hashtbl_extra/test_rix_hash_extra.c
git commit -m "Test classic and slot_extra variants coexist in one TU"
```

---

## Task 7: Fuzz test for long-run extra consistency

**Files:**
- Modify: `tests/hashtbl_extra/test_rix_hash_extra.c`

- [ ] **Step 7.1: Add fuzz test**

```c
/* A tiny deterministic PRNG (xorshift32) for reproducibility. */
static u32
xs32(u32 *s)
{
    u32 x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

#define FUZZ_POOL   512u
#define FUZZ_BK      64u
#define FUZZ_OPS 20000u

static struct enode fuzz_pool[FUZZ_POOL];
static struct rix_hash_bucket_extra_s fuzz_bk[FUZZ_BK]
    __attribute__((aligned(64)));
static struct eht fuzz_head;
static u32 fuzz_expected[FUZZ_POOL]; /* 0 = not inserted */

static void
test_fuzz_extra_consistency(void)
{
    printf("[T] fuzz extra consistency\n");
    memset(fuzz_pool, 0, sizeof(fuzz_pool));
    memset(fuzz_bk,   0, sizeof(fuzz_bk));
    memset(fuzz_expected, 0, sizeof(fuzz_expected));
    RIX_HASH_INIT(eht, &fuzz_head, FUZZ_BK);

    for (unsigned i = 0; i < FUZZ_POOL; i++) {
        fuzz_pool[i].key.hi = (u64)(i + 1);
        fuzz_pool[i].key.lo = (u64)i * 0x9E3779B97F4A7C15ULL;
    }

    u32 rng = 0xC0FFEE00u;
    unsigned inserts = 0, removes = 0, updates = 0;
    for (unsigned op = 0; op < FUZZ_OPS; op++) {
        u32 r = xs32(&rng);
        unsigned i = r % FUZZ_POOL;
        unsigned action = (r >> 16) & 3u;  /* 0..3 */

        if (action == 0u || (action == 1u && fuzz_expected[i] == 0u)) {
            /* insert */
            if (fuzz_expected[i] != 0u)
                continue; /* already inserted; skip */
            u32 extra = (r | 1u); /* non-zero so we can detect 0 = clear */
            struct enode *dup = eht_insert(&fuzz_head, fuzz_bk, fuzz_pool,
                                           &fuzz_pool[i], extra);
            if (dup != NULL)
                FAILF("fuzz insert[%u] unexpected dup %p",
                      i, (void *)dup);
            fuzz_expected[i] = extra;
            inserts++;
        } else if (action == 1u) {
            /* remove */
            struct enode *rm = eht_remove(&fuzz_head, fuzz_bk, fuzz_pool,
                                          &fuzz_pool[i]);
            if (rm != &fuzz_pool[i])
                FAILF("fuzz remove[%u] expected %p got %p",
                      i, (void *)&fuzz_pool[i], (void *)rm);
            fuzz_expected[i] = 0u;
            removes++;
        } else {
            /* extra update (only if present) */
            if (fuzz_expected[i] == 0u)
                continue;
            unsigned bk   = fuzz_pool[i].cur_hash & fuzz_head.rhh_mask;
            unsigned slot = fuzz_pool[i].slot;
            u32 new_extra = (r ^ 0xABCD0000u) | 1u;
            fuzz_bk[bk].extra[slot] = new_extra;
            fuzz_expected[i] = new_extra;
            updates++;
        }

        /* Every 128 ops, validate a random 8 entries */
        if ((op & 127u) == 127u) {
            for (unsigned t = 0; t < 8u; t++) {
                unsigned j = xs32(&rng) % FUZZ_POOL;
                if (fuzz_expected[j] == 0u)
                    continue;
                struct enode *f = eht_find(&fuzz_head, fuzz_bk, fuzz_pool,
                                           &fuzz_pool[j].key);
                if (f != &fuzz_pool[j])
                    FAILF("fuzz find[%u] at op=%u returned %p expected %p",
                          j, op, (void *)f, (void *)&fuzz_pool[j]);
                unsigned bk   = f->cur_hash & fuzz_head.rhh_mask;
                unsigned slot = f->slot;
                if (fuzz_bk[bk].extra[slot] != fuzz_expected[j])
                    FAILF("fuzz extra[%u] at op=%u expected 0x%x got 0x%x",
                          j, op, fuzz_expected[j],
                          fuzz_bk[bk].extra[slot]);
            }
        }
    }
    printf("    fuzz: ins=%u rem=%u upd=%u\n", inserts, removes, updates);
}
```

Call from `main`.

- [ ] **Step 7.2: Run + commit**

```
cd tests/hashtbl_extra && make && ./hash_extra_test
git add tests/hashtbl_extra/test_rix_hash_extra.c
git commit -m "Fuzz test for long-run extra[] consistency under mixed ops"
```

---

## Task 8: Test `_EX` variant with explicit hash_fn

**Files:**
- Modify: `tests/hashtbl_extra/test_rix_hash_extra.c`

- [ ] **Step 8.1: Generate an _EX variant and test**

Append:

```c
static RIX_UNUSED RIX_FORCE_INLINE union rix_hash_hash_u
ek_hash(const struct ek *k, u32 mask)
{
    return rix_hash_bytes_fast((const void *)k, sizeof(*k), mask);
}

/* Distinct head + name prefix. */
struct xnode {
    u32 cur_hash;
    u16 slot;
    u16 _pad;
    struct ek key;
};

RIX_HASH_HEAD(xht);
RIX_HASH_GENERATE_SLOT_EXTRA_EX(xht, xnode, key, cur_hash, slot,
                                 ek_cmp, ek_hash)

static void
test_ex_variant(void)
{
    printf("[T] SLOT_EXTRA_EX (explicit hash_fn)\n");
    struct xnode pool[NB_BASIC];
    struct rix_hash_bucket_extra_s bk[NB_BK_BASIC]
        __attribute__((aligned(64)));
    struct xht head;

    memset(pool, 0, sizeof(pool));
    memset(bk,   0, sizeof(bk));
    RIX_HASH_INIT(xht, &head, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        pool[i].key.hi = (u64)(i + 1);
        pool[i].key.lo = 0x1234ABCDu;
    }
    for (unsigned i = 0; i < NB_BASIC; i++) {
        if (xht_insert(&head, bk, pool, &pool[i], 0x9000u + i) != NULL)
            FAILF("ex insert[%u] dup", i);
    }
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct xnode *f = xht_find(&head, bk, pool, &pool[i].key);
        if (f != &pool[i])
            FAILF("ex find[%u] wrong", i);
        unsigned b = f->cur_hash & head.rhh_mask;
        unsigned s = f->slot;
        if (bk[b].extra[s] != 0x9000u + i)
            FAILF("ex extra[%u] got 0x%x", i, bk[b].extra[s]);
    }
}
```

Call from `main`.

- [ ] **Step 8.2: Run + commit**

```
cd tests/hashtbl_extra && make && ./hash_extra_test
git add tests/hashtbl_extra/test_rix_hash_extra.c
git commit -m "Test RIX_HASH_GENERATE_SLOT_EXTRA_EX variant"
```

---

## Task 9: Bench harness (insert / find / remove throughput)

**Files:**
- Modify: `tests/hashtbl_extra/bench_rix_hash_extra.c` (replace stub)

The bench mirrors the classic `tests/hashtbl/bench_rix_hash.c` style but only for slot_extra. Goal: a usable micro-bench, not a full parity harness.

- [ ] **Step 9.1: Replace the stub with a usable bench**

Overwrite `bench_rix_hash_extra.c`:

```c
/* bench_rix_hash_extra.c - slot_extra variant micro-benchmark.
 *
 * Measures cycles/op for insert, find_hit, find_miss, remove
 * at a configurable fill. Compare against
 * tests/hashtbl/bench_rix_hash.c for classic slot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include "rix_hash.h"

struct bkey {
    u64 hi;
    u64 lo;
};

struct bnode {
    u32 cur_hash;
    u16 slot;
    u16 _pad;
    struct bkey key;
};

static RIX_FORCE_INLINE int
bkey_cmp(const struct bkey *a, const struct bkey *b)
{
    return (a->hi == b->hi && a->lo == b->lo) ? 0 : 1;
}

RIX_HASH_HEAD(bht);
RIX_HASH_GENERATE_SLOT_EXTRA(bht, bnode, key, cur_hash, slot, bkey_cmp)

static RIX_FORCE_INLINE u64
rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

#define N_KEYS   (1u << 16)       /* 65536 keys   */
#define N_BK     (1u << 14)       /* 16384 bkts   */
#define REPS      8u

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    static struct bnode *pool;
    static struct rix_hash_bucket_extra_s *bk;
    struct bht head;

    pool = calloc(N_KEYS, sizeof(*pool));
    bk   = aligned_alloc(64, (size_t)N_BK * sizeof(*bk));
    if (!pool || !bk) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    memset(bk, 0, (size_t)N_BK * sizeof(*bk));

    for (unsigned i = 0; i < N_KEYS; i++) {
        pool[i].key.hi = (u64)(i + 1);
        pool[i].key.lo = (u64)i * 0xC6BC279692B5C323ULL;
    }

    u64 ins_cy = 0, find_cy = 0, miss_cy = 0, rm_cy = 0;

    for (unsigned r = 0; r < REPS; r++) {
        memset(bk, 0, (size_t)N_BK * sizeof(*bk));
        RIX_HASH_INIT(bht, &head, N_BK);

        u64 t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)bht_insert(&head, bk, pool, &pool[i], 0xABCDu + i);
        u64 t1 = rdtsc();
        ins_cy += (t1 - t0);

        t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)bht_find(&head, bk, pool, &pool[i].key);
        t1 = rdtsc();
        find_cy += (t1 - t0);

        struct bkey miss;
        miss.hi = 0xDEADBEEFu;
        miss.lo = 0u;
        t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++) {
            miss.lo = i;
            (void)bht_find(&head, bk, pool, &miss);
        }
        t1 = rdtsc();
        miss_cy += (t1 - t0);

        t0 = rdtsc();
        for (unsigned i = 0; i < N_KEYS; i++)
            (void)bht_remove(&head, bk, pool, &pool[i]);
        t1 = rdtsc();
        rm_cy += (t1 - t0);
    }

    u64 total_ops = (u64)N_KEYS * REPS;
    printf("slot_extra bench (N=%u, BK=%u, reps=%u)\n",
           N_KEYS, N_BK, REPS);
    printf("  insert     : %5.2f cy/op\n", (double)ins_cy / (double)total_ops);
    printf("  find_hit   : %5.2f cy/op\n", (double)find_cy / (double)total_ops);
    printf("  find_miss  : %5.2f cy/op\n", (double)miss_cy / (double)total_ops);
    printf("  remove     : %5.2f cy/op\n", (double)rm_cy / (double)total_ops);

    free(pool);
    free(bk);
    return 0;
}
```

- [ ] **Step 9.2: Run**

```
cd tests/hashtbl_extra && make && ./hash_extra_bench
```

Expected: four lines with cycles/op. Values are informational, not asserted.

- [ ] **Step 9.3: Commit**

```
git add tests/hashtbl_extra/bench_rix_hash_extra.c
git commit -m "Add slot_extra micro-bench (insert/find/remove)"
```

---

## Task 10: Wire `tests/hashtbl_extra` into top-level Makefile

**Files:**
- Modify: `Makefile` (top-level)

- [ ] **Step 10.1: Read current Makefile `TESTDIRS` and `BENCH_FULL_DIRS`**

Currently:

```
TESTDIRS := tests/slist tests/list tests/stailq tests/tailq tests/circleq \
            tests/rbtree tests/hashtbl tests/hashtbl32 tests/hashtbl64
...
BENCH_FULL_DIRS := tests/hashtbl tests/hashtbl32 tests/hashtbl64
```

- [ ] **Step 10.2: Add `tests/hashtbl_extra` to both**

Edit `Makefile` lines 4-5:

```
TESTDIRS := tests/slist tests/list tests/stailq tests/tailq tests/circleq \
            tests/rbtree tests/hashtbl tests/hashtbl_extra tests/hashtbl32 tests/hashtbl64
```

Edit line 76:

```
BENCH_FULL_DIRS := tests/hashtbl tests/hashtbl_extra tests/hashtbl32 tests/hashtbl64
```

- [ ] **Step 10.3: Verify full build + test**

```
make clean && make test
```

Expected: `[TEST] tests/hashtbl_extra` appears and the new test passes. All other tests also pass.

- [ ] **Step 10.4: Commit**

```
git add Makefile
git commit -m "Register tests/hashtbl_extra in top-level Makefile"
```

---

## Task 11: Cross-compiler and SIMD matrix verification

Verify the new header compiles and tests pass under the full toolchain matrix defined in `CLAUDE.md` (gcc + clang, gen/sse/avx2/avx512).

- [ ] **Step 11.1: gcc default (avx2) test**

```
make -C tests/hashtbl_extra clean && CC=gcc make -C tests/hashtbl_extra test
```

Expected: PASS.

- [ ] **Step 11.2: clang default (avx2) test**

```
make -C tests/hashtbl_extra clean && CC=clang make -C tests/hashtbl_extra test
```

Expected: PASS.

- [ ] **Step 11.3: gcc SIMD=gen test**

```
make -C tests/hashtbl_extra clean && CC=gcc SIMD=gen make -C tests/hashtbl_extra test
```

Expected: PASS.

- [ ] **Step 11.4: clang SIMD=gen test**

```
make -C tests/hashtbl_extra clean && CC=clang SIMD=gen make -C tests/hashtbl_extra test
```

Expected: PASS.

- [ ] **Step 11.5: gcc SIMD=avx512 test (if CPU supports AVX-512)**

```
make -C tests/hashtbl_extra clean && CC=gcc SIMD=avx512 make -C tests/hashtbl_extra test
```

Expected: PASS if `cat /proc/cpuinfo | grep -m1 avx512f` is non-empty.
If CPU lacks AVX-512, report "skipped (no AVX-512 on this host)" and move on per `CLAUDE.md` line 5.

- [ ] **Step 11.6: clang SIMD=avx512 test (if CPU supports AVX-512)**

```
make -C tests/hashtbl_extra clean && CC=clang SIMD=avx512 make -C tests/hashtbl_extra test
```

Same skip rule.

- [ ] **Step 11.7: Bench sanity run**

```
make -C tests/hashtbl_extra clean && make -C tests/hashtbl_extra bench
```

Expected: bench runs and prints four cycle numbers. Record them for the spec's 6.2 acceptance criterion.

- [ ] **Step 11.8: Classic regression sanity**

```
make -C tests/hashtbl clean && make -C tests/hashtbl test && make -C tests/hashtbl bench
```

Expected: classic tests still PASS and bench numbers are materially unchanged vs. baseline (we did not modify classic code paths).

- [ ] **Step 11.9: Commit any Makefile tweaks discovered during matrix run**

If no changes: skip commit. If Makefile fixes were needed (e.g., missing include path under SIMD=gen):

```
git add <changed files>
git commit -m "Fix slot_extra build under SIMD=gen / clang"
```

---

## Acceptance Criteria (per spec section 6)

- All functional tests pass under gcc + clang, gen / avx2 (and avx512 when hardware allows).
- `hash_extra_bench` numbers are within a few % of `hash_bench` insert / find / remove for an equivalent configuration; the spec's 6.2 criterion can be validated by manual inspection after Task 11.
- `tests/hashtbl` test + bench show no regression vs. baseline.

After acceptance, flowtable adoption is a separate brainstorm + spec + plan cycle.
