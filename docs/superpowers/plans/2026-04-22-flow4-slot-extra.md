# flow4 slot_extra variant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a parallel `flowtable/extra/` variant of flow4 in which the per-entry expiry timestamp lives in the bucket's third cacheline (`rix_hash_bucket_extra_s::extra[]`) instead of embedded in `flow_entry_meta`, so that `ft_table_extra_maintain` can decide expiry without touching entry memory.

**Architecture:** Copy classic's template engine (`flow_core.h`, `flow_table_generate.h`, `ft_dispatch.c`, `ft_maintain.c`) into `flowtable/extra/`, rename symbols, switch to `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX` with a 192 B bucket, override the `FCORE_INIT/TOUCH/CLEAR_TIMESTAMP` hooks so timestamps round-trip through `bk->extra[slot]`, and rewrite `ft_maintain_extra_scan_bucket_` as a scalar scan of `extra[]` with no entry-side loads.

**Tech Stack:** C11 (`-std=gnu11`), librix templated hash (`rix_hash_slot_extra.h` merged 2026-04-18), rix_queue, `mmap`/`MADV_HUGEPAGE`, gcc + clang, SIMD variants GEN/SSE/AVX2/AVX512 dispatched via the existing `FT_ARCH_*` mechanism.

**Reference spec:** `docs/superpowers/specs/2026-04-22-flow4-slot-extra-design.md` (committed 2026-04-22, 924045e). The spec is the source of truth for data layout, API naming, algorithm, and success criteria. This plan implements it.

**Out of scope (per spec §2):** flow6/flowu extra variants, removing meta entirely, SIMD-accelerated maintain scan, perf_event integration in the bench binary, installing extra headers.

---

## File Inventory

Created under `flowtable/extra/`:

```
flowtable/extra/
├── README.md                          Task 1
├── include/
│   ├── flow_extra_common.h            Task 2
│   ├── flow_extra_key.h               Task 3
│   ├── flow_extra_table.h             Task 4
│   └── flow4_extra_table.h            Task 4
└── src/
    ├── flow_hash_extra.h              Task 5
    ├── flow_dispatch_extra.h          Task 5
    ├── flow_core_extra.h              Task 6
    ├── flow_table_generate_extra.h    Task 7
    ├── flow4_extra.c                  Task 8
    ├── ft_dispatch_extra.c            Task 9
    └── ft_maintain_extra.c            Task 10
```

Created under `flowtable/test/`:

```
flowtable/test/
├── test_flow4_extra.c                 Task 12
├── test_flow4_parity.c                Task 13
├── bench_flow4_vs_extra.c             Task 14
└── bench_flow4_maint_sweep.c          Task 15
```

Modified:

```
flowtable/Makefile                     Tasks 1, 11
flowtable/test/Makefile                Tasks 12, 13, 14, 15
Makefile (top-level)                   Task 1 (if needed)
```

---

## Task 1: Scaffold directory and Makefile wiring

**Files:**
- Create: `flowtable/extra/README.md`
- Create: `flowtable/extra/include/.gitkeep`
- Create: `flowtable/extra/src/.gitkeep`
- Modify: `flowtable/Makefile` (add extra source discovery but no new object files yet — pure scaffolding)

**Pre-read:** `flowtable/Makefile` (current build structure). Note the `SRCS` / `OBJS` patterns and how `FT_ARCH_SUFFIX` multi-arch builds work.

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p flowtable/extra/include flowtable/extra/src
touch flowtable/extra/include/.gitkeep flowtable/extra/src/.gitkeep
```

- [ ] **Step 2: Write `flowtable/extra/README.md`**

```markdown
# flowtable/extra/

Parallel implementation of the flow cache using the
`RIX_HASH_GENERATE_SLOT_EXTRA_EX` (192 B bucket) rix_hash variant.

The per-entry expiry timestamp is relocated from
`flow_entry_meta.timestamp` into the bucket's third cacheline
(`rix_hash_bucket_extra_s::extra[slot]`), which lets
`ft_table_extra_maintain` decide expiry without touching entry
memory.

Status: experimental. API parallels `flowtable/src/` (classic) but
uses `ft_table_extra_*`, `flow4_extra_table_*` symbol names so both
variants coexist in one binary for AB benchmarking.

Consumers that want to try extra include
`<rix/flow_extra_table.h>` explicitly and add
`-I<repo>/flowtable/extra/include` to their build. Not part of
`make install` until classic is retired.

See `docs/superpowers/specs/2026-04-22-flow4-slot-extra-design.md`
for design rationale and success criteria.
```

- [ ] **Step 3: Inspect classic Makefile to plan additions**

Run: `cat flowtable/Makefile | head -80`
Expected: see how `SRCS`, `OBJS`, `FT_ARCH_SUFFIX`, SIMD arch-variants are wired.

- [ ] **Step 4: Commit the empty scaffold**

```bash
git add flowtable/extra/
git commit -m "Scaffold flowtable/extra/ directory tree"
```

Expected output: `[work ...] Scaffold flowtable/extra/ directory tree`

---

## Task 2: Introduce `flow_extra_common.h` with `ft_table_extra`, config, enums, stats

**Files:**
- Create: `flowtable/extra/include/flow_extra_common.h`

**Pre-read:** `flowtable/include/flow_common.h:260-355` (classic `ft_table_stats`, `ft_table`, `ft_maint_ctx`). The extra variant mirrors these but swaps the bucket pointer type and drops `pool_base` / `pool_stride` / `pool_entry_offset` / `meta_off` / `max_entries` from the maint ctx.

- [ ] **Step 1: Write the header**

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_extra_common.h - protocol-independent types for the flowtable
 * slot_extra variant. Mirrors flow_common.h but uses the 192 B bucket
 * (rix_hash_bucket_extra_s) and a slimmed maintenance context.
 */

#ifndef _FLOW_EXTRA_COMMON_H_
#define _FLOW_EXTRA_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>
#include <rix/rix_queue.h>

#include "flow_common.h"   /* reuse FT_ARCH_*, ft_add_policy, flow_stats,
                              flow_status, ft_table_variant */

#ifndef FT_TABLE_EXTRA_BUCKET_SIZE
#define FT_TABLE_EXTRA_BUCKET_SIZE 192u
#endif

struct ft_table_extra_config {
    unsigned ts_shift;
};

struct ft_table_extra_stats {
    struct flow_stats core;
    u64 grow_execs;
    u64 grow_failures;
    u64 maint_calls;
    u64 maint_bucket_checks;
    u64 maint_evictions;
    u64 maint_extras_loaded;   /* EXTRA: bucket extra[] reads in maintain */
};

RIX_HASH_HEAD(ft_table_extra_ht);

struct ft_table_extra {
    struct rix_hash_bucket_extra_s *buckets;
    unsigned char                  *pool_base;
    size_t                          pool_stride;
    size_t                          pool_entry_offset;
    struct ft_table_extra_ht        ht_head;
    unsigned                        start_mask;
    unsigned                        nb_bk;
    unsigned                        max_entries;
    u8                              variant;
    u8                              ts_shift;
    struct ft_table_extra_stats     stats;
    struct flow_status              status;
};

/* Maintenance context: no pool_base/pool_stride/meta_off/max_entries. */
struct ft_maint_extra_ctx {
    struct rix_hash_bucket_extra_s *buckets;
    unsigned                       *rhh_nb;
    struct ft_table_extra_stats    *stats;
    unsigned                        rhh_mask;
    u8                              ts_shift;
};

int ft_table_extra_init(struct ft_table_extra *ft,
                        enum ft_table_variant variant,
                        void *array,
                        unsigned max_entries,
                        size_t stride,
                        size_t entry_offset,
                        void *buckets,
                        size_t bucket_size,
                        const struct ft_table_extra_config *cfg);

void ft_table_extra_destroy(struct ft_table_extra *ft);
void ft_table_extra_flush(struct ft_table_extra *ft);
unsigned ft_table_extra_nb_entries(const struct ft_table_extra *ft);
unsigned ft_table_extra_nb_bk(const struct ft_table_extra *ft);
void ft_table_extra_stats_get(const struct ft_table_extra *ft,
                              struct ft_table_extra_stats *out);
void ft_table_extra_status_get(const struct ft_table_extra *ft,
                               struct flow_status *out);

u32 ft_table_extra_add_idx(struct ft_table_extra *ft, u32 entry_idx,
                           u64 now);
unsigned ft_table_extra_add_idx_bulk(struct ft_table_extra *ft,
                                     u32 *entry_idxv, unsigned nb_keys,
                                     enum ft_add_policy policy, u64 now,
                                     u32 *unused_idxv);
unsigned ft_table_extra_add_idx_bulk_maint(struct ft_table_extra *ft,
                                           u32 *entry_idxv,
                                           unsigned nb_keys,
                                           enum ft_add_policy policy,
                                           u64 now, u64 timeout,
                                           u32 *unused_idxv,
                                           unsigned max_unused,
                                           unsigned min_bk_used);
u32 ft_table_extra_del_idx(struct ft_table_extra *ft, u32 entry_idx);
unsigned ft_table_extra_del_idx_bulk(struct ft_table_extra *ft,
                                     const u32 *entry_idxv,
                                     unsigned nb_keys,
                                     u32 *unused_idxv);
int ft_table_extra_walk(struct ft_table_extra *ft,
                        int (*cb)(u32 entry_idx, void *arg),
                        void *arg);
int ft_table_extra_migrate(struct ft_table_extra *ft,
                           void *new_buckets, size_t new_bucket_size);

void ft_table_extra_touch(struct ft_table_extra *ft, u32 entry_idx,
                          u64 now);

unsigned ft_table_extra_maintain(const struct ft_maint_extra_ctx *ctx,
                                 unsigned start_bk,
                                 u64 now, u64 expire_tsc,
                                 u32 *expired_idxv, unsigned max_expired,
                                 unsigned min_bk_entries,
                                 unsigned *next_bk);

unsigned ft_table_extra_maintain_idx_bulk(const struct ft_maint_extra_ctx *ctx,
                                          const u32 *entry_idxv,
                                          unsigned nb_idx,
                                          u64 now, u64 expire_tsc,
                                          u32 *expired_idxv,
                                          unsigned max_expired,
                                          unsigned min_bk_entries,
                                          int enable_filter);

size_t ft_table_extra_bucket_size(unsigned max_entries);
size_t ft_table_extra_bucket_mem_size(unsigned nb_bk);

void ft_arch_extra_init(unsigned arch_enable);

#define FT_TABLE_EXTRA_INIT_TYPED(ft, variant, array, max_entries, type,     \
                                  member, buckets, bucket_size, cfg)         \
    ft_table_extra_init((ft), (variant), (array), (max_entries),            \
                        sizeof(type), offsetof(type, member),               \
                        (buckets), (bucket_size), (cfg))

#endif /* _FLOW_EXTRA_COMMON_H_ */
```

- [ ] **Step 2: Verify it parses standalone**

Run:
```bash
gcc -std=gnu11 -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include -Iinclude \
    -c -x c -o /tmp/hdrtest.o - <<'EOF'
#include "flow_extra_common.h"
int main(void) { return 0; }
EOF
```
Expected: compiles cleanly, no output beyond the command itself.

- [ ] **Step 3: Commit**

```bash
git add flowtable/extra/include/flow_extra_common.h
git commit -m "Add flow_extra_common.h with ft_table_extra types"
```

---

## Task 3: Add `flow_extra_key.h` with `meta_extra` and TS accessors

**Files:**
- Create: `flowtable/extra/include/flow_extra_key.h`

**Pre-read:** `flowtable/include/flow_key.h` (classic meta + TS helpers). Extra drops `meta.timestamp` and exposes `(bk, slot)`-addressed helpers.

- [ ] **Step 1: Write the header**

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_EXTRA_KEY_H_
#define _FLOW_EXTRA_KEY_H_

#include <stdint.h>

#include <rix/rix_hash_slot_extra.h>

#include "flow_key.h"   /* reuse FLOW_TIMESTAMP_* encoding, flowX_key structs */

struct flow_entry_meta_extra {
    u32 cur_hash;   /* hash_field */
    u16 slot;       /* slot_field */
    u16 reserved0;
};

_Static_assert(sizeof(struct flow_entry_meta_extra) == 8u,
               "flow_entry_meta_extra must be 8 bytes");

/* flow4_extra_key is byte-identical to flow4_key. */
struct flow4_extra_key {
    u8  family;
    u8  proto;
    u16 src_port;
    u16 dst_port;
    u16 pad;
    u32 src_addr;
    u32 dst_addr;
};

_Static_assert(sizeof(struct flow4_extra_key) == sizeof(struct flow4_key),
               "flow4_extra_key must match flow4_key size");

struct flow4_extra_entry {
    struct flow4_extra_key       key;   /* +0  : 16 B */
    struct flow_entry_meta_extra meta;  /* +16 :  8 B */
};

_Static_assert(offsetof(struct flow4_extra_entry, meta) == 16u,
               "flow4_extra_entry.meta must be at offset 16");

static inline u32
flow_extra_ts_get(const struct rix_hash_bucket_extra_s *bk, unsigned slot)
{
    return bk->extra[slot];
}

static inline void
flow_extra_ts_set(struct rix_hash_bucket_extra_s *bk, unsigned slot,
                  u32 encoded)
{
    bk->extra[slot] = encoded;
}

static inline int
flow_extra_ts_is_permanent(u32 encoded)
{
    return encoded == 0u;
}

/* Encode/decode re-use classic's flow_timestamp_encode /
 * flow_timestamp_is_expired_raw so classic <-> extra comparisons are
 * bit-exact. */

#endif /* _FLOW_EXTRA_KEY_H_ */
```

- [ ] **Step 2: Verify it parses**

Run:
```bash
gcc -std=gnu11 -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include -Iinclude \
    -c -x c -o /tmp/hdrtest.o - <<'EOF'
#include "flow_extra_key.h"
int main(void) { return 0; }
EOF
```
Expected: compiles cleanly.

- [ ] **Step 3: Commit**

```bash
git add flowtable/extra/include/flow_extra_key.h
git commit -m "Add flow_extra_key.h with meta_extra and TS accessors"
```

---

## Task 4: Add public API headers `flow_extra_table.h` and `flow4_extra_table.h`

**Files:**
- Create: `flowtable/extra/include/flow_extra_table.h`
- Create: `flowtable/extra/include/flow4_extra_table.h`

**Pre-read:** `flowtable/include/flow_table.h`, `flowtable/include/flow4_table.h`. These are the classic equivalents.

- [ ] **Step 1: Write `flow_extra_table.h`** (thin wrapper)

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_EXTRA_TABLE_H_
#define _FLOW_EXTRA_TABLE_H_

#include "flow_extra_common.h"
#include "flow_extra_key.h"
#include "flow4_extra_table.h"

#endif /* _FLOW_EXTRA_TABLE_H_ */
```

- [ ] **Step 2: Write `flow4_extra_table.h`**

Structure follows `flowtable/include/flow4_table.h`. Public-facing entry-lookup helpers and the flow4-specific init wrapper.

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_EXTRA_TABLE_H_
#define _FLOW4_EXTRA_TABLE_H_

#include "flow_extra_common.h"
#include "flow_extra_key.h"

static inline int
flow4_extra_table_init(struct ft_table_extra *ft,
                       struct flow4_extra_entry *entry_array,
                       unsigned max_entries,
                       void *buckets, size_t bucket_size,
                       const struct ft_table_extra_config *cfg)
{
    return FT_TABLE_EXTRA_INIT_TYPED(ft, FT_TABLE_VARIANT_FLOW4,
                                     entry_array, max_entries,
                                     struct flow4_extra_entry, meta,
                                     buckets, bucket_size, cfg);
}

static inline struct flow4_extra_entry *
flow4_extra_table_entry_ptr(const struct ft_table_extra *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset,
                                struct flow4_extra_entry);
}

u32 flow4_extra_table_add(struct ft_table_extra *ft,
                          struct flow4_extra_entry *entry, u64 now);
u32 flow4_extra_table_find(const struct ft_table_extra *ft,
                           const struct flow4_extra_key *key);
u32 flow4_extra_table_del(struct ft_table_extra *ft,
                          const struct flow4_extra_key *key);

unsigned flow4_extra_table_find_bulk(struct ft_table_extra *ft,
                                     const struct flow4_extra_key *keys,
                                     unsigned nb_keys, u64 now,
                                     u32 *results);
unsigned flow4_extra_table_del_bulk(struct ft_table_extra *ft,
                                    const struct flow4_extra_key *keys,
                                    unsigned nb_keys, u32 *idx_out);

size_t flow4_extra_table_bucket_size(unsigned max_entries);

#endif /* _FLOW4_EXTRA_TABLE_H_ */
```

- [ ] **Step 3: Verify headers compile**

```bash
gcc -std=gnu11 -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include -Iinclude \
    -c -x c -o /tmp/hdrtest.o - <<'EOF'
#include "flow_extra_table.h"
int main(void) { return 0; }
EOF
```
Expected: compiles cleanly.

- [ ] **Step 4: Commit**

```bash
git add flowtable/extra/include/flow_extra_table.h \
        flowtable/extra/include/flow4_extra_table.h
git commit -m "Add public extra headers (flow_extra_table.h, flow4_extra_table.h)"
```

---

## Task 5: Copy `flow_hash.h` and `flow_dispatch.h` into extra with symbol renames

**Files:**
- Create: `flowtable/extra/src/flow_hash_extra.h` (copy of `flowtable/src/flow_hash.h`)
- Create: `flowtable/extra/src/flow_dispatch_extra.h` (copy of `flowtable/src/flow_dispatch.h`)

**Pre-read:** `flowtable/src/flow_hash.h`, `flowtable/src/flow_dispatch.h`.

`flow_hash.h` only defines key-hash helpers (independent of bucket layout). `flow_dispatch.h` defines the per-arch function table macros.

- [ ] **Step 1: Copy flow_hash.h**

```bash
cp flowtable/src/flow_hash.h flowtable/extra/src/flow_hash_extra.h
```

- [ ] **Step 2: Rename the include guard**

Edit `flowtable/extra/src/flow_hash_extra.h`: change the include guard from `_FLOW_HASH_H_` to `_FLOW_HASH_EXTRA_H_`.

- [ ] **Step 3: Verify it compiles**

```bash
gcc -std=gnu11 -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    -c -x c -o /tmp/hdrtest.o - <<'EOF'
#include "flow_hash_extra.h"
int main(void) { return 0; }
EOF
```
Expected: compiles cleanly.

- [ ] **Step 4: Copy flow_dispatch.h**

```bash
cp flowtable/src/flow_dispatch.h flowtable/extra/src/flow_dispatch_extra.h
```

- [ ] **Step 5: Rename include guard and macro prefixes**

Edit `flowtable/extra/src/flow_dispatch_extra.h`:

- Change include guard from `_FLOW_DISPATCH_H_` to `_FLOW_DISPATCH_EXTRA_H_`.
- Change macro `FT_OPS_TABLE` to `FT_OPS_TABLE_EXTRA` (and every internal helper prefixed with `FT_OPS_` or `_FT_OPS_` similarly with `_EXTRA` appended to the top-level name only — internal concat helpers can keep their names, but rename the public one to avoid collisions).

Run `grep -n '^#define\|^FT_OPS_\|FT_OPS_TABLE' flowtable/extra/src/flow_dispatch_extra.h` and rename each public-looking macro.

- [ ] **Step 6: Verify compile**

```bash
gcc -std=gnu11 -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    -c -x c -o /tmp/hdrtest.o - <<'EOF'
#include "flow_dispatch_extra.h"
int main(void) { return 0; }
EOF
```
Expected: compiles cleanly.

- [ ] **Step 7: Commit**

```bash
git add flowtable/extra/src/flow_hash_extra.h \
        flowtable/extra/src/flow_dispatch_extra.h
git commit -m "Copy flow_hash.h and flow_dispatch.h into extra with renames"
```

---

## Task 6: Create `flow_core_extra.h` by copying classic and applying the extra transformations

**Files:**
- Create: `flowtable/extra/src/flow_core_extra.h`

**Pre-read:** All of `flowtable/src/flow_core.h` (1210 lines). Note in particular:
- Trait hook defaults at lines 140-175 (`FCORE_ON_HIT`, `FCORE_INIT_TIMESTAMP`, `FCORE_TOUCH_TIMESTAMP`, `FCORE_CLEAR_TIMESTAMP`).
- `FCORE_GENERATE` body references `struct rix_hash_bucket_s *buckets` many times (lines 241, 332, etc.).
- Hash ctx scan-bucket calls like `_FCORE_HT(ht, hash_key_2bk_masked)`.
- The 4-arg classic insert call.

The transformation is mechanical but large. This task does the copy + renames + type swap in a single commit.

- [ ] **Step 1: Copy the file**

```bash
cp flowtable/src/flow_core.h flowtable/extra/src/flow_core_extra.h
```

- [ ] **Step 2: Rename the include guard**

Edit: change `#ifndef _FLOW_CORE_H_` / `#define _FLOW_CORE_H_` / matching `#endif` to `_FLOW_CORE_EXTRA_H_`.

- [ ] **Step 3: Update the include of `flow_key.h`**

In `flow_core_extra.h`, replace `#include "flow_key.h"` with `#include "flow_extra_key.h"`.

- [ ] **Step 4: Mass-rename the `FCORE_` macros to `FCORE_EXTRA_`**

Apply the following renames across the file (use an editor search/replace — not a shell sed to avoid rewriting comments incorrectly). All identifiers that start with `FCORE_` become `FCORE_EXTRA_`:

- `FCORE_RECORD_MEMBER_PTR_NONNULL_ALIGNED` → `FCORE_EXTRA_RECORD_MEMBER_PTR_NONNULL_ALIGNED`
- `FCORE_LAYOUT_HASH_BASE` → `FCORE_EXTRA_LAYOUT_HASH_BASE`
- `FCORE_LAYOUT_ENTRY_PTR` → `FCORE_EXTRA_LAYOUT_ENTRY_PTR`
- `FCORE_LAYOUT_ENTRY_INDEX` → `FCORE_EXTRA_LAYOUT_ENTRY_INDEX`
- `FCORE_ON_HIT` → `FCORE_EXTRA_ON_HIT`
- `FCORE_TIMESTAMP_SHIFT` → `FCORE_EXTRA_TIMESTAMP_SHIFT`
- `FCORE_INIT_TIMESTAMP` → `FCORE_EXTRA_INIT_TIMESTAMP`
- `FCORE_TOUCH_TIMESTAMP` → `FCORE_EXTRA_TOUCH_TIMESTAMP`
- `FCORE_CLEAR_TIMESTAMP` → `FCORE_EXTRA_CLEAR_TIMESTAMP`
- `FCORE_HASH_MASK` → `FCORE_EXTRA_HASH_MASK`
- `FCORE_ON_FINDADD_MISS` → `FCORE_EXTRA_ON_FINDADD_MISS`
- `FCORE_GENERATE` → `FCORE_EXTRA_GENERATE`
- `FCORE_RECORD_MEMBER_PTR_NONNULL_ALIGNED` (any remaining) → `FCORE_EXTRA_RECORD_MEMBER_PTR_NONNULL_ALIGNED`

Any internal concat helpers named `_FCORE_INT`, `_FCORE_HT`, `_FCORE_OT`, `_FCORE_ENTRY_T`, `_FCORE_KEY_T`, `_FCORE_HT_T`, `_FCORE_HT_HEAD`: rename them to `_FCORE_EXTRA_INT`, `_FCORE_EXTRA_HT`, etc. to keep internal symbols disjoint.

After rename, run `grep -n 'FCORE_\(EXTRA_\)\?' flowtable/extra/src/flow_core_extra.h | grep -v FCORE_EXTRA_` — this should return **no matches** (every `FCORE_` reference should now be `FCORE_EXTRA_`).

- [ ] **Step 5: Replace bucket type references**

Across `flow_core_extra.h`, replace `struct rix_hash_bucket_s` with `struct rix_hash_bucket_extra_s`. Verify with:

```bash
grep -n 'rix_hash_bucket_s\b' flowtable/extra/src/flow_core_extra.h
```
Expected: no matches (all should be `rix_hash_bucket_extra_s`).

- [ ] **Step 6: Override the timestamp hooks**

In the default-hook section of `flow_core_extra.h` (around line 155-175 in the original), replace the three `FCORE_EXTRA_{INIT,TOUCH,CLEAR}_TIMESTAMP` default bodies. Default bodies must write to `bk->extra[slot]`, not `entry->meta`. Because `bk` and `slot` are not available directly in the current call sites (they were written to expect `entry->meta`), the hooks must be invoked from new call sites that already have `(bk, slot)` in scope.

Strategy: **Do not attempt to rewrite the hooks to take `(bk, slot)` at this task**. Instead:

1. Keep the hooks' names but change their default body to be a **no-op**:

```c
#ifndef FCORE_EXTRA_INIT_TIMESTAMP
#define FCORE_EXTRA_INIT_TIMESTAMP(owner, entry, now)  ((void)(owner), (void)(entry), (void)(now))
#endif

#ifndef FCORE_EXTRA_TOUCH_TIMESTAMP
#define FCORE_EXTRA_TOUCH_TIMESTAMP(owner, entry, now) ((void)(owner), (void)(entry), (void)(now))
#endif

#ifndef FCORE_EXTRA_CLEAR_TIMESTAMP
#define FCORE_EXTRA_CLEAR_TIMESTAMP(entry)             ((void)(entry))
#endif
```

2. Record in a **header comment** at the top of `flow_core_extra.h`:

```c
/*
 * TIMESTAMP SEMANTICS (differs from classic flow_core.h):
 *
 * Classic FCORE_INIT/TOUCH_TIMESTAMP wrote to entry->meta.timestamp.
 * In the extra variant the timestamp lives at bk->extra[slot], and
 * writing it requires (bk, slot) context that is local to the insert /
 * touch call site. The generated hot path therefore performs the
 * timestamp store itself (see the 5-arg insert call and the explicit
 * bk->extra[slot] = encoded(now) assignment in find_key_bulk and
 * add_idx_small_). The FCORE_EXTRA_*_TIMESTAMP hooks remain as no-ops
 * so variant .c files can plug extra bookkeeping in if needed.
 */
```

3. Patch each affected call site in `FCORE_EXTRA_GENERATE`:

- Every call to `_FCORE_EXTRA_HT(ht, insert)(head, buckets, hash_base, entry)` (the 4-arg form) must become `_FCORE_EXTRA_HT(ht, insert)(head, buckets, hash_base, entry, flow_timestamp_encode(now, FCORE_EXTRA_TIMESTAMP_SHIFT(owner)))` (the 5-arg form). The 5th argument is the encoded timestamp.
- Every call site that previously relied on `FCORE_INIT_TIMESTAMP(owner, entry, now)` no longer needs a separate assignment because the insert call now stores the timestamp.
- `FCORE_TOUCH_TIMESTAMP(owner, entry, now)` on find-hit must be replaced with an explicit `(bk, slot)` write:

```c
/* find_key_bulk: after a successful cmp_key hit, the find_ctx carries
 * bk_idx and slot; use them to update extra[]. */
if (RIX_LIKELY(entry != NULL)) {
    u32 eidx = FCORE_EXTRA_LAYOUT_ENTRY_INDEX(owner, entry);
    u32 bk_idx = ctx[idx & ctx_mask].bk_idx;       /* bucket the match was in */
    unsigned slot = ctx[idx & ctx_mask].slot;       /* slot within that bucket */
    if (now != 0u)
        buckets[bk_idx].extra[slot] =
            flow_timestamp_encode(now,
                                  FCORE_EXTRA_TIMESTAMP_SHIFT(owner));
    FCORE_EXTRA_ON_HIT(owner, entry, eidx);
    results[idx] = eidx;
    hit_count++;
}
```

Verify that `struct rix_hash_find_ctx_s` exposes the matched `bk_idx` and `slot` from `rix_hash_slot_extra.h`. If not, use the helper functions generated by `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX` (e.g., `<ht>_ctx_bk_idx(ctx)` / `<ht>_ctx_slot(ctx)`) — consult `include/rix/rix_hash_slot_extra.h` to find the exact accessor names.

For `FCORE_EXTRA_CLEAR_TIMESTAMP(entry)` call sites (on delete): either leave them as no-ops (bucket extra is overwritten on next insert) or follow the bucket-style cleanup pattern already used in the delete path — the insert/kick rix_hash layer already zeroes `bk->extra[slot]` in the extra variant's remove path, so no-op is correct here.

- [ ] **Step 7: Compile-check the header**

Create a temporary `.c` that expands `FCORE_EXTRA_GENERATE` for `flow4`:

```bash
cat > /tmp/fc_extra_smoke.c <<'EOF'
#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>

#include "flow4_extra_table.h"

#define FCORE_EXTRA_LAYOUT_HASH_BASE(owner) \
    ((struct flow4_extra_entry *)(void *)(owner))
#define FCORE_EXTRA_LAYOUT_ENTRY_PTR(owner, idx) \
    flow4_extra_table_entry_ptr((owner), (idx))
#define FCORE_EXTRA_LAYOUT_ENTRY_INDEX(owner, entry) \
    ft_record_index_from_member_ptr((owner)->pool_base, \
                                    (owner)->pool_stride, \
                                    (owner)->pool_entry_offset, (entry))
static inline union rix_hash_hash_u
flow4x_hash(const struct flow4_extra_key *k, u32 mask)
{ (void)mask; union rix_hash_hash_u h = { .u = { (u32)k->src_addr, (u32)k->dst_addr } }; return h; }
static inline int
flow4x_cmp(const struct flow4_extra_key *a, const struct flow4_extra_key *b)
{ return __builtin_memcmp(a, b, sizeof(*a)) ? 1 : 0; }

RIX_HASH_HEAD(fc_flow4x_ht);
RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(fc_flow4x_ht, flow4_extra_entry,
    key, meta.cur_hash, meta.slot, flow4x_cmp, flow4x_hash)

#include "flow_core_extra.h"
FCORE_EXTRA_GENERATE(flow4, ft_table_extra, fc_flow4x_ht,
                     flow4x_hash, flow4x_cmp)
int main(void) { return 0; }
EOF
gcc -std=gnu11 -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    -c -o /tmp/fc_extra_smoke.o /tmp/fc_extra_smoke.c
```
Expected: compiles cleanly. **If it does not compile**, fix the identified mismatch before committing — this smoke file is diagnostic, not a permanent test.

- [ ] **Step 8: Commit**

```bash
git add flowtable/extra/src/flow_core_extra.h
git commit -m "Add flow_core_extra.h with timestamps routed through bk->extra[]"
```

---

## Task 7: Create `flow_table_generate_extra.h`

**Files:**
- Create: `flowtable/extra/src/flow_table_generate_extra.h`

**Pre-read:** `flowtable/src/flow_table_generate.h` (646 lines). Observe that it references `struct rix_hash_bucket_s` in several places and calls `_FCORE_*` helpers generated by `flow_core.h`.

- [ ] **Step 1: Copy the file**

```bash
cp flowtable/src/flow_table_generate.h flowtable/extra/src/flow_table_generate_extra.h
```

- [ ] **Step 2: Rename include guard**

`_FLOW_TABLE_GENERATE_H_` → `_FLOW_TABLE_GENERATE_EXTRA_H_`.

- [ ] **Step 3: Mass-rename identifiers**

Apply these renames (editor search/replace):

- `FT_TABLE_BULK_STEP_KEYS` / `FT_TABLE_BULK_AHEAD_STEPS` / `FT_TABLE_BULK_AHEAD_KEYS` / `FT_TABLE_BULK_CTX_RING` / `FT_TABLE_GROW_OLD_BK_AHEAD` / `FT_TABLE_GROW_REINSERT_AHEAD` / `FT_TABLE_GROW_CTX_RING` / `FT_TABLE_FIND_BULK_MIN_KEYS` → append `_EXTRA` before the numeric value suffix (e.g., `FT_TABLE_EXTRA_BULK_STEP_KEYS`).
- `FT_TABLE_GENERATE` → `FT_TABLE_EXTRA_GENERATE`
- `FT_TABLE_INIT_TYPED` → `FT_TABLE_EXTRA_INIT_TYPED` (already defined in `flow_extra_common.h`, so inside this file rename to avoid collision and then delete the duplicate definition if present).

Any `_FT_TABLE_*` internal helpers similarly become `_FT_TABLE_EXTRA_*`.

After rename run:
```bash
grep -n '\bFT_TABLE_\(EXTRA_\)\?' flowtable/extra/src/flow_table_generate_extra.h | grep -v FT_TABLE_EXTRA_
```
Expected: no matches.

- [ ] **Step 4: Replace bucket type references**

`struct rix_hash_bucket_s` → `struct rix_hash_bucket_extra_s`. Verify:
```bash
grep -n 'rix_hash_bucket_s\b' flowtable/extra/src/flow_table_generate_extra.h
```
Expected: no matches.

- [ ] **Step 5: Replace owner type references**

Any `struct ft_table ` (with trailing space, to avoid touching `ft_table_extra`) → `struct ft_table_extra `. Any `struct ft_maint_ctx` → `struct ft_maint_extra_ctx`. Any `ft_table_stats` → `ft_table_extra_stats`.

- [ ] **Step 6: Compile-check**

Run:
```bash
gcc -std=gnu11 -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    -c -x c -o /tmp/hdrtest.o - <<'EOF'
#include "flow_table_generate_extra.h"
int main(void) { return 0; }
EOF
```
Expected: compiles cleanly.

- [ ] **Step 7: Commit**

```bash
git add flowtable/extra/src/flow_table_generate_extra.h
git commit -m "Add flow_table_generate_extra.h (bucket_extra, renames)"
```

---

## Task 8: Create `flow4_extra.c` driver

**Files:**
- Create: `flowtable/extra/src/flow4_extra.c`

**Pre-read:** `flowtable/src/flow4.c` (171 lines, full listing above).

- [ ] **Step 1: Copy the file**

```bash
cp flowtable/src/flow4.c flowtable/extra/src/flow4_extra.c
```

- [ ] **Step 2: Apply the following transformations**

Using an editor (not sed, to respect comments and whitespace):

1. Include changes:
   - `<rix/rix_hash_slot.h>` → `<rix/rix_hash_slot_extra.h>`
   - `"flow4_table.h"` → `"flow4_extra_table.h"`
   - `"flow_hash.h"` → `"flow_hash_extra.h"`
   - `"flow_core.h"` → `"flow_core_extra.h"`
   - `"flow_table_generate.h"` → `"flow_table_generate_extra.h"`
   - `"flow_dispatch.h"` → `"flow_dispatch_extra.h"`

2. Local-helper renames (prefix `ft_flow4_` → `ft_flow4_extra_`, prefix `fcore_flow4_` → `fcore_flow4_extra_`).

3. Type renames in local helpers and elsewhere:
   - `struct flow4_entry` → `struct flow4_extra_entry`
   - `struct ft_table` → `struct ft_table_extra`

4. Macro references in the body:
   - `ft_flow4_hash_fn` → `ft_flow4_extra_hash_fn`
   - `ft_flow4_cmp` → `ft_flow4_extra_cmp`
   - `RIX_HASH_GENERATE_STATIC_SLOT_EX` → `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX`
   - `FCORE_GENERATE` → `FCORE_EXTRA_GENERATE`
   - `FT_TABLE_GENERATE` → `FT_TABLE_EXTRA_GENERATE`
   - `FCORE_LAYOUT_*` macros → `FCORE_EXTRA_LAYOUT_*`
   - `FCORE_HASH_MASK` → `FCORE_EXTRA_HASH_MASK`
   - `FLOW_STATS` → itself (not renamed — the macro is defined locally; just ensure the local `#define FLOW_STATS(owner) ((owner)->stats.core)` still refers to `ft_table_extra::stats.core`).
   - `FTG_LAYOUT_*` → `FTG_EXTRA_LAYOUT_*`
   - `FTG_ENTRY_TYPE` → `FTG_EXTRA_ENTRY_TYPE`
   - `FT_OPS_TABLE(...)` → `FT_OPS_TABLE_EXTRA(...)`

5. The `hash_field` / `slot_field` arguments passed to `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX` stay as `meta.cur_hash, meta.slot` since `flow4_extra_entry.meta` is `flow_entry_meta_extra` which still has these fields.

6. Ensure final file uses `flow4_extra_entry` as the `type` argument in both `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(fcore_flow4_extra_ht, flow4_extra_entry, ...)` and `RIX_HASH_GENERATE_STATIC_SLOT_EXTRA_EX(ft_flow4_extra_ht, flow4_extra_entry, ...)`.

- [ ] **Step 3: Write a build-only smoke test**

Create `/tmp/flow4x_build.sh`:

```bash
cat > /tmp/flow4x_build.sh <<'EOF'
#!/bin/bash
set -euo pipefail
gcc -std=gnu11 -g -O2 -mavx2 -msse4.2 \
    -Wall -Wextra -Wvla -Werror \
    -DFT_ARCH_SUFFIX=_avx2 \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    -c flowtable/extra/src/flow4_extra.c -o /tmp/flow4_extra.o
echo BUILD_OK
EOF
chmod +x /tmp/flow4x_build.sh
/tmp/flow4x_build.sh
```
Expected: `BUILD_OK`. If not, iterate on Step 2 transformations until compilation succeeds.

- [ ] **Step 4: Commit**

```bash
git add flowtable/extra/src/flow4_extra.c
git commit -m "Add flow4_extra.c driver (SLOT_EXTRA_EX, renamed helpers)"
```

---

## Task 9: Create `ft_dispatch_extra.c`

**Files:**
- Create: `flowtable/extra/src/ft_dispatch_extra.c`

**Pre-read:** `flowtable/src/ft_dispatch.c` (661 lines). This is the arch-dispatched hot-path multiplexer.

- [ ] **Step 1: Copy and rename**

```bash
cp flowtable/src/ft_dispatch.c flowtable/extra/src/ft_dispatch_extra.c
```

Apply via editor:

- Include `"flow_extra_common.h"` in place of `"flow_common.h"`, and `"flow_extra_key.h"` in place of `"flow_key.h"`.
- Everywhere `struct ft_table` appears (ensure `ft_table_extra` is not already matched), replace with `struct ft_table_extra`.
- `struct ft_maint_ctx` → `struct ft_maint_extra_ctx`.
- `struct ft_table_stats` → `struct ft_table_extra_stats`.
- `struct rix_hash_bucket_s` → `struct rix_hash_bucket_extra_s`.
- Every public function name `ft_table_*` (or nested `flow_table_*` symbols) → `ft_table_extra_*`.
- `ft_arch_init` → `ft_arch_extra_init`.
- `ft_arch_ops` → `ft_arch_extra_ops`.
- `FT_OPS_TABLE(flow4, ...)` already lives in `flow4.c` not here, but any reference to `flow4_ops_*` structs → `flow4_extra_ops_*`.

- [ ] **Step 2: Compile-check single-arch**

```bash
gcc -std=gnu11 -g -O2 -mavx2 -msse4.2 \
    -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    -c flowtable/extra/src/ft_dispatch_extra.c -o /tmp/ft_dispatch_extra.o
```
Expected: compiles cleanly.

- [ ] **Step 3: Commit**

```bash
git add flowtable/extra/src/ft_dispatch_extra.c
git commit -m "Add ft_dispatch_extra.c (parallel hot path)"
```

---

## Task 10: Write `ft_maintain_extra.c` from scratch (new algorithm)

**Files:**
- Create: `flowtable/extra/src/ft_maintain_extra.c`

**Pre-read:** `flowtable/src/ft_maintain.c` for the structure of the compiled-per-arch file (prototype forward-decls, arch-suffix macros, `ft_maint_scan_bucket_`, `ft_table_maintain`, `ft_table_maintain_idx_bulk`). The extra variant keeps the outer structure but replaces the scan body.

- [ ] **Step 1: Write the file**

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * ft_maintain_extra.c - bucket maintenance using bk->extra[] timestamps.
 *
 * EXTRA: no entry-side cacheline loads in ft_table_extra_maintain.
 * maintain_idx_bulk still needs one entry load per input to resolve
 * (bucket, slot) from entry->meta; it then uses the extra scan for
 * the expire decision.
 *
 * Compiled once per arch variant (gen/sse/avx2/avx512) with FT_ARCH_SUFFIX.
 */

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>
#include <rix/rix_defs_private.h>

#include "flow_extra_common.h"
#include "flow_extra_key.h"

#define _FTM_EXTRA_CAT2(a, b) a##b
#define _FTM_EXTRA_CAT(a, b)  _FTM_EXTRA_CAT2(a, b)

#ifdef FT_ARCH_SUFFIX
#define _FTM_EXTRA_FN(name) _FTM_EXTRA_CAT(name, FT_ARCH_SUFFIX)
#else
#define _FTM_EXTRA_FN(name) name
#endif

unsigned _FTM_EXTRA_FN(ft_table_extra_maintain)(
    const struct ft_maint_extra_ctx *, unsigned, u64, u64,
    u32 *, unsigned, unsigned, unsigned *);
unsigned _FTM_EXTRA_FN(ft_table_extra_maintain_idx_bulk)(
    const struct ft_maint_extra_ctx *, const u32 *, unsigned,
    u64, u64, u32 *, unsigned, unsigned, int);

/*
 * EXTRA scan_bucket: reads hash[] for occupancy, extra[] for TS.
 * Never touches entry memory.
 */
static unsigned
ft_maint_extra_scan_bucket_(const struct ft_maint_extra_ctx *ctx,
                            unsigned bk_idx,
                            u64 now_ts, u64 timeout_ts,
                            u32 *expired_idxv, unsigned out_pos,
                            unsigned max_expired,
                            unsigned min_bk_entries,
                            unsigned *out_used_count)
{
    struct rix_hash_bucket_extra_s *bk = ctx->buckets + bk_idx;
    u32 used_bits = 0u;
    u32 expired_bits = 0u;
    unsigned used_count;

    for (unsigned slot = 0; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {
        if (bk->hash[slot] != 0u)
            used_bits |= (u32)(1u << slot);
    }

    used_count = (unsigned)__builtin_popcount(used_bits);
    if (out_used_count != NULL)
        *out_used_count = used_count;
    if (min_bk_entries > 1u && used_count < min_bk_entries)
        return out_pos;

    ctx->stats->maint_extras_loaded++;

    {
        u32 u = used_bits;
        while (u != 0u) {
            unsigned slot = (unsigned)__builtin_ctz(u);
            u &= u - 1u;
            u32 ts = bk->extra[slot];
            if (flow_timestamp_is_expired_raw(ts, now_ts, timeout_ts))
                expired_bits |= (u32)(1u << slot);
        }
    }

    while (expired_bits != 0u && out_pos < max_expired) {
        unsigned slot = (unsigned)__builtin_ctz(expired_bits);
        unsigned idx = bk->idx[slot];
        expired_bits &= expired_bits - 1u;
        RIX_ASSERT(idx != (unsigned)RIX_NIL);

        bk->hash[slot]  = 0u;
        bk->idx[slot]   = (u32)RIX_NIL;
        bk->extra[slot] = 0u;
        (*ctx->rhh_nb)--;

        expired_idxv[out_pos++] = (u32)idx;
    }
    return out_pos;
}

unsigned
_FTM_EXTRA_FN(ft_table_extra_maintain)(
    const struct ft_maint_extra_ctx *ctx,
    unsigned start_bk,
    u64 now, u64 expire_tsc,
    u32 *expired_idxv, unsigned max_expired,
    unsigned min_bk_entries, unsigned *next_bk)
{
    unsigned mask;
    unsigned cur_bk;
    unsigned nb_bk;
    unsigned count = 0u;
    u64 now_ts;
    u64 timeout_ts;

    if (ctx == NULL || max_expired == 0u || expired_idxv == NULL) {
        if (next_bk != NULL) *next_bk = start_bk;
        return 0u;
    }
    if (expire_tsc == 0u) {
        if (next_bk != NULL) *next_bk = start_bk;
        return 0u;
    }

    ctx->stats->maint_calls++;

    mask = ctx->rhh_mask;
    nb_bk = mask + 1u;
    cur_bk = start_bk & mask;

    now_ts = flow_timestamp_encode(now, ctx->ts_shift);
    timeout_ts = flow_timestamp_timeout_encode(expire_tsc, ctx->ts_shift);

    for (unsigned i = 0u; i < nb_bk; i++) {
        unsigned next = (cur_bk + 1u) & mask;
        rix_hash_prefetch_bucket_full_of(ctx->buckets, next);
        ctx->stats->maint_bucket_checks++;

        count = ft_maint_extra_scan_bucket_(ctx, cur_bk, now_ts, timeout_ts,
                                            expired_idxv, count, max_expired,
                                            min_bk_entries, NULL);
        cur_bk = next;
        if (count >= max_expired)
            break;
    }

    ctx->stats->maint_evictions += count;
    if (next_bk != NULL) *next_bk = cur_bk;
    return count;
}

/*
 * maintain_idx_bulk: resolves (bk, slot) from entry->meta, then uses
 * the extra scan. One entry cacheline load per input is unavoidable
 * at this API; the win is that the bucket scan does not then load
 * every other entry in the bucket.
 */
unsigned
_FTM_EXTRA_FN(ft_table_extra_maintain_idx_bulk)(
    const struct ft_maint_extra_ctx *ctx,
    const u32 *entry_idxv, unsigned nb_idx,
    u64 now, u64 expire_tsc,
    u32 *expired_idxv, unsigned max_expired,
    unsigned min_bk_entries, int enable_filter)
{
    /* For the first cut we reuse the classic control flow but drop
     * the per-slot entry prefetch inside the scan. A direct copy of
     * the classic three-stage pipeline with ft_maint_extra_scan_bucket_
     * as the scan primitive is sufficient. Reference:
     * flowtable/src/ft_maintain.c, the _idx_bulk_ function. */
    (void)ctx; (void)entry_idxv; (void)nb_idx; (void)now; (void)expire_tsc;
    (void)expired_idxv; (void)max_expired; (void)min_bk_entries;
    (void)enable_filter;

    /* Placeholder return; actual body is transcribed from
     * ft_maintain.c in the subagent implementer step. */
    return 0u;
}
```

Note: the `maintain_idx_bulk` body is written out fully by the implementer by copy-adapting from `flowtable/src/ft_maintain.c` (lines 238-386 in the classic file). Three changes from the classic version:

1. Replace `struct flow_entry_meta` with `struct flow_entry_meta_extra` and drop any `flow_timestamp_get(meta)` read at the end (the scan now reads bk->extra[slot] internally).
2. Drop the `metas[]` prefetch array and its staged pipeline inside the scan — that logic is now inside `ft_maint_extra_scan_bucket_`.
3. Call `ft_maint_extra_scan_bucket_` instead of `ft_maint_scan_bucket_`.

- [ ] **Step 2: Write a unit-level smoke test of the scan**

Create `flowtable/extra/src/test_ft_maintain_extra_scan.c` (staged file, removed after this task):

```c
#include <assert.h>
#include <string.h>
#include <rix/rix_hash_slot_extra.h>
#include "flow_extra_common.h"
#include "flow_extra_key.h"

/* Forward the internal scan for test access. */
extern unsigned ft_maint_extra_scan_bucket_(
    const struct ft_maint_extra_ctx *, unsigned, u64, u64,
    u32 *, unsigned, unsigned, unsigned, unsigned *);

int main(void)
{
    struct rix_hash_bucket_extra_s bk = { 0 };
    struct ft_table_extra_stats stats = { 0 };
    unsigned rhh_nb = 0;
    struct ft_maint_extra_ctx ctx = {
        .buckets = &bk, .rhh_nb = &rhh_nb, .stats = &stats,
        .rhh_mask = 0, .ts_shift = 0,
    };
    /* Populate slot 3 with a very old timestamp. */
    bk.hash[3]  = 0xAB;
    bk.idx[3]   = 17;
    bk.extra[3] = 0u + 1u; /* encoded ts = 1 (very old relative to now) */
    rhh_nb = 1u;
    u32 out[4] = {0};
    unsigned n = ft_maint_extra_scan_bucket_(&ctx, 0,
        /* now_ts  = */ 1000u,
        /* to_ts   = */ 10u,
        out, 0, 4, 0, NULL);
    assert(n == 1 && out[0] == 17);
    assert(bk.hash[3] == 0 && bk.idx[3] == (u32)RIX_NIL);
    assert(rhh_nb == 0);
    return 0;
}
```

Temporarily expose `ft_maint_extra_scan_bucket_` by removing `static` in `ft_maintain_extra.c` for this smoke test, build it, run it, then restore `static`.

```bash
# Temporarily drop 'static' from ft_maint_extra_scan_bucket_
sed -i 's/^static unsigned$/unsigned/' flowtable/extra/src/ft_maintain_extra.c
gcc -std=gnu11 -g -O2 -mavx2 -msse4.2 \
    -Wall -Wextra -Wvla -Werror \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    flowtable/extra/src/ft_maintain_extra.c \
    flowtable/extra/src/test_ft_maintain_extra_scan.c \
    -o /tmp/scan_smoke
/tmp/scan_smoke && echo SCAN_OK
# Restore 'static'
sed -i 's/^unsigned$/static unsigned/' flowtable/extra/src/ft_maintain_extra.c
rm flowtable/extra/src/test_ft_maintain_extra_scan.c
```

Expected: `SCAN_OK`. The sed is targeted at a very narrow pattern; if the implementer finds it rewrites too much, they should hand-edit instead.

- [ ] **Step 3: Transcribe `maintain_idx_bulk`**

Open `flowtable/src/ft_maintain.c` and copy the `ft_table_maintain_idx_bulk` body (and any helpers it calls that are still relevant) into `flow_table_extra_maintain_idx_bulk` in `ft_maintain_extra.c`. Apply:

- Rename `ft_maint_ctx` → `ft_maint_extra_ctx`.
- Rename `rix_hash_bucket_s` → `rix_hash_bucket_extra_s`.
- Replace `flow_entry_meta` → `flow_entry_meta_extra`.
- Replace all calls to `ft_maint_scan_bucket_` with `ft_maint_extra_scan_bucket_`.
- Drop any reference to `flow_timestamp_get(meta)` from the scan path (the scan reads bk->extra[slot] internally).
- Drop the prefetch of entry cachelines inside the scan (`__builtin_prefetch(metas[...])` calls). Keep the prefetch of bucket cachelines; use `rix_hash_prefetch_bucket_full_of` for the next bucket.

- [ ] **Step 4: Build whole-file**

```bash
gcc -std=gnu11 -g -O2 -mavx2 -msse4.2 \
    -Wall -Wextra -Wvla -Werror \
    -DFT_ARCH_SUFFIX=_avx2 \
    -Iflowtable/include -Iflowtable/extra/include \
    -Iflowtable/src -Iflowtable/extra/src -Iinclude \
    -c flowtable/extra/src/ft_maintain_extra.c -o /tmp/ft_maintain_extra.o
```
Expected: compiles cleanly.

- [ ] **Step 5: Commit**

```bash
git add flowtable/extra/src/ft_maintain_extra.c
git commit -m "Add ft_maintain_extra.c with entry-free bucket scan"
```

---

## Task 11: Wire extra sources into `flowtable/Makefile`

**Files:**
- Modify: `flowtable/Makefile`

**Pre-read:** Current `flowtable/Makefile`. Find the section that enumerates classic sources and per-arch `OBJS`. Extra sources follow the same pattern.

- [ ] **Step 1: Add extra SRCS**

After the classic `SRCS` definition, add:

```make
EXTRA_SRCS := extra/src/flow4_extra.c \
              extra/src/ft_dispatch_extra.c \
              extra/src/ft_maintain_extra.c

EXTRA_HDRS := extra/include/flow_extra_common.h \
              extra/include/flow_extra_key.h \
              extra/include/flow_extra_table.h \
              extra/include/flow4_extra_table.h \
              extra/src/flow_hash_extra.h \
              extra/src/flow_dispatch_extra.h \
              extra/src/flow_core_extra.h \
              extra/src/flow_table_generate_extra.h

CFLAGS += -Iextra/include -Iextra/src
```

Include `$(EXTRA_SRCS)` in whatever top-level source-list drives the static library, using the same per-arch compilation suffix as classic (`_gen`, `_sse`, `_avx2`, `_avx512`).

- [ ] **Step 2: Build all arch variants**

```bash
make -C flowtable clean
make -C flowtable all 2>&1 | tail -20
```
Expected: every `.o` lists includes from both `src/` and `extra/src/`. No `-Werror` failures.

- [ ] **Step 3: Build with clang**

```bash
make -C flowtable clean
CC=clang make -C flowtable all 2>&1 | tail -20
```
Expected: clean build.

- [ ] **Step 4: Build gen and sse variants (AVX-512 optional per host)**

```bash
for s in gen sse avx2; do
    make -C flowtable clean
    SIMD=$s make -C flowtable all 2>&1 | tail -3
done
```
Expected: all three pass. AVX-512 is attempted only if `grep -q avx512f /proc/cpuinfo` — otherwise log "AVX-512 not available on this host" and move on.

- [ ] **Step 5: Commit**

```bash
git add flowtable/Makefile
git commit -m "Wire flowtable/extra/ sources into build"
```

---

## Task 12: Add `test_flow4_extra.c` with full classic-parity basic tests

**Files:**
- Create: `flowtable/test/test_flow4_extra.c`
- Modify: `flowtable/test/Makefile`

**Pre-read:** `flowtable/test/test_flow_table.c` — specifically the flow4 test functions (`test_flow4_basic`, `test_flow4_bulk`, `test_flow4_maintain_idx_bulk`, etc.). The extra tests mirror these but use extra types.

- [ ] **Step 1: Write the test harness skeleton**

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * test_flow4_extra.c - Functional tests for the flow4 slot_extra variant.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>

#include "flow_extra_table.h"

#define N_MAX 1024u
#define N_BK  64u

static void *
hugealloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

static struct flow4_extra_key
mk_key(unsigned i)
{
    struct flow4_extra_key k = { 0 };
    k.family = 4;
    k.proto  = 6;
    k.src_addr = 0x0A000000u | i;
    k.dst_addr = 0x0B000000u | (i + 1u);
    k.src_port = (u16)(i * 7u);
    k.dst_port = (u16)(i * 11u);
    return k;
}

static void
test_init_basic(void)
{
    printf("[T] flow4_extra init basic\n");
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk = hugealloc(flow4_extra_table_bucket_size(N_MAX));
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    int rc = flow4_extra_table_init(&ft, pool, N_MAX, bk,
                                    flow4_extra_table_bucket_size(N_MAX),
                                    &cfg);
    assert(rc == 0);
    assert(ft_table_extra_nb_entries(&ft) == 0u);
    assert(ft_table_extra_nb_bk(&ft) > 0u);
    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk, flow4_extra_table_bucket_size(N_MAX));
}

static void
test_add_find_del(void)
{
    printf("[T] flow4_extra add/find/del\n");
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk = hugealloc(flow4_extra_table_bucket_size(N_MAX));
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk,
        flow4_extra_table_bucket_size(N_MAX), &cfg) == 0);

    for (unsigned i = 0; i < 100; i++)
        pool[i].key = mk_key(i);

    for (unsigned i = 0; i < 100; i++) {
        u32 idx = flow4_extra_table_add(&ft, &pool[i], /* now = */ 1000u);
        assert(idx != RIX_NIL);
    }
    assert(ft_table_extra_nb_entries(&ft) == 100u);
    for (unsigned i = 0; i < 100; i++) {
        struct flow4_extra_key k = mk_key(i);
        u32 idx = flow4_extra_table_find(&ft, &k);
        assert(idx != RIX_NIL);
    }
    for (unsigned i = 0; i < 100; i++) {
        struct flow4_extra_key k = mk_key(i);
        assert(flow4_extra_table_del(&ft, &k) != RIX_NIL);
    }
    assert(ft_table_extra_nb_entries(&ft) == 0u);

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk, flow4_extra_table_bucket_size(N_MAX));
}

static void
test_ts_in_bucket(void)
{
    printf("[T] flow4_extra TS stored in bucket extra[]\n");
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(flow4_extra_table_bucket_size(N_MAX));
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw,
        flow4_extra_table_bucket_size(N_MAX), &cfg) == 0);

    pool[0].key = mk_key(0);
    u32 idx = flow4_extra_table_add(&ft, &pool[0], /* now = */ 2048u);
    assert(idx != RIX_NIL);

    unsigned bk_idx = pool[0].meta.cur_hash & (ft_table_extra_nb_bk(&ft) - 1u);
    unsigned slot   = pool[0].meta.slot;
    u32 got = flow_extra_ts_get(&ft.buckets[bk_idx], slot);
    u32 want = flow_timestamp_encode(2048u, cfg.ts_shift);
    assert(got == want);

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, flow4_extra_table_bucket_size(N_MAX));
}

static void
test_maintain_expires(void)
{
    printf("[T] flow4_extra maintain expires stale entries\n");
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(flow4_extra_table_bucket_size(N_MAX));
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw,
        flow4_extra_table_bucket_size(N_MAX), &cfg) == 0);

    for (unsigned i = 0; i < 50; i++)
        pool[i].key = mk_key(i);
    /* Insert at t=1000 */
    for (unsigned i = 0; i < 50; i++)
        flow4_extra_table_add(&ft, &pool[i], /* now = */ 1000u);

    /* Call maintain at t = 1000 + timeout + 1 (all entries expire) */
    u32 expired[64];
    struct ft_maint_extra_ctx ctx = {
        .buckets = ft.buckets,
        .rhh_nb  = &ft.ht_head.rhh_nb,
        .stats   = &ft.stats,
        .rhh_mask = ft.ht_head.rhh_mask,
        .ts_shift = cfg.ts_shift,
    };
    unsigned next = 0;
    unsigned n = ft_table_extra_maintain(&ctx, 0,
        /* now       = */ 1000u + (1u << 20),
        /* timeout   = */ 1u << 18,
        expired, 64, 0, &next);
    assert(n >= 50u - 14u); /* at least enough, depending on eviction budget */
    (void)next;

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, flow4_extra_table_bucket_size(N_MAX));
}

static void
test_touch_updates_bucket_extra(void)
{
    printf("[T] flow4_extra touch updates bucket extra[]\n");
    struct flow4_extra_entry *pool = hugealloc(N_MAX * sizeof(*pool));
    void *bk_raw = hugealloc(flow4_extra_table_bucket_size(N_MAX));
    struct ft_table_extra ft;
    struct ft_table_extra_config cfg = { .ts_shift = 4u };
    assert(flow4_extra_table_init(&ft, pool, N_MAX, bk_raw,
        flow4_extra_table_bucket_size(N_MAX), &cfg) == 0);

    pool[0].key = mk_key(0);
    u32 idx = flow4_extra_table_add(&ft, &pool[0], 1000u);
    assert(idx != RIX_NIL);

    ft_table_extra_touch(&ft, idx, 5000u);

    unsigned bk_idx = pool[0].meta.cur_hash & (ft_table_extra_nb_bk(&ft) - 1u);
    unsigned slot = pool[0].meta.slot;
    assert(flow_extra_ts_get(&ft.buckets[bk_idx], slot) ==
           flow_timestamp_encode(5000u, cfg.ts_shift));

    ft_table_extra_destroy(&ft);
    munmap(pool, N_MAX * sizeof(*pool));
    munmap(bk_raw, flow4_extra_table_bucket_size(N_MAX));
}

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);
    test_init_basic();
    test_add_find_del();
    test_ts_in_bucket();
    test_maintain_expires();
    test_touch_updates_bucket_extra();
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Add the test to `flowtable/test/Makefile`**

Edit `flowtable/test/Makefile` to add a new binary target built from `test_flow4_extra.c` plus the `flowtable/extra/` object set plus `flowtable/src/` classic objects (both are linked in). The `test` target runs both the classic and the extra suite.

```make
TEST_EXTRA_TARGET := test_flow4_extra
TEST_EXTRA_SRC    := test_flow4_extra.c

$(TEST_EXTRA_TARGET): $(TEST_EXTRA_SRC) $(LIB_EXTRA_OBJS)
	$(CC) $(CFLAGS) -Iextra/include -o $@ $^

test: test-classic test-extra
test-extra: $(TEST_EXTRA_TARGET)
	./$(TEST_EXTRA_TARGET)
```

- [ ] **Step 3: Run**

```bash
make -C flowtable/test test-extra 2>&1 | tail -20
```
Expected: all `[T]` lines plus final `PASS`.

- [ ] **Step 4: Cross-toolchain check**

```bash
make -C flowtable/test clean
CC=clang make -C flowtable/test test-extra
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add flowtable/test/test_flow4_extra.c flowtable/test/Makefile
git commit -m "Add test_flow4_extra.c (basic + TS placement + maintain)"
```

---

## Task 13: Add `test_flow4_parity.c` — cross-check fuzz

**Files:**
- Create: `flowtable/test/test_flow4_parity.c`
- Modify: `flowtable/test/Makefile`

**Pre-read:** `tests/hashtbl_extra/test_rix_hash_extra.c` — the existing SLOT_EXTRA fuzz harness. Reuse its xorshift32, op-mix, and periodic-validation pattern.

- [ ] **Step 1: Write the harness**

```c
/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * test_flow4_parity.c - drive classic flow4 and extra flow4 with the
 * same op stream; assert externally observable equivalence.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>

#include "flow_table.h"        /* classic */
#include "flow_extra_table.h"  /* extra   */

#define FUZZ_POOL 512u
#define FUZZ_BK    64u
#define FUZZ_OPS  20000u
#define VAL_EVERY  128u

static u32 rng_state = 0xC0FFEE;
static inline u32
xs32(void)
{
    u32 x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

static void *
hugealloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    madvise(p, bytes, MADV_HUGEPAGE);
    memset(p, 0, bytes);
    return p;
}

struct flow_keys { u32 a; u32 b; };

static struct flow4_key
classic_key(unsigned pool_slot)
{
    struct flow4_key k = { 0 };
    k.family = 4; k.proto = 6;
    k.src_addr = 0x0A000000u | pool_slot;
    k.dst_addr = 0x0B000000u | (pool_slot + 1u);
    k.src_port = (u16)(pool_slot * 7u);
    k.dst_port = (u16)(pool_slot * 11u);
    return k;
}

static struct flow4_extra_key
extra_key(unsigned pool_slot)
{
    struct flow4_extra_key k = { 0 };
    k.family = 4; k.proto = 6;
    k.src_addr = 0x0A000000u | pool_slot;
    k.dst_addr = 0x0B000000u | (pool_slot + 1u);
    k.src_port = (u16)(pool_slot * 7u);
    k.dst_port = (u16)(pool_slot * 11u);
    return k;
}

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    ft_arch_init(FT_ARCH_AUTO);
    ft_arch_extra_init(FT_ARCH_AUTO);

    struct flow4_entry      *pool_c = hugealloc(FUZZ_POOL * sizeof(*pool_c));
    struct flow4_extra_entry *pool_e = hugealloc(FUZZ_POOL * sizeof(*pool_e));
    void *bk_c = hugealloc(flow4_table_bucket_size(FUZZ_POOL));
    void *bk_e = hugealloc(flow4_extra_table_bucket_size(FUZZ_POOL));
    struct ft_table ft_c;
    struct ft_table_extra ft_e;
    struct ft_table_config cfg_c = { .ts_shift = 4u };
    struct ft_table_extra_config cfg_e = { .ts_shift = 4u };
    assert(flow4_table_init(&ft_c, pool_c, FUZZ_POOL, bk_c,
        flow4_table_bucket_size(FUZZ_POOL), &cfg_c) == 0);
    assert(flow4_extra_table_init(&ft_e, pool_e, FUZZ_POOL, bk_e,
        flow4_extra_table_bucket_size(FUZZ_POOL), &cfg_e) == 0);

    /* Initialize the keys in both pools to identical data. */
    for (unsigned i = 0; i < FUZZ_POOL; i++) {
        pool_c[i].key = classic_key(i);
        pool_e[i].key = extra_key(i);
    }

    u64 now = 1000u;
    unsigned inserted_c = 0, inserted_e = 0;
    unsigned ops_performed = 0;

    for (unsigned op = 0; op < FUZZ_OPS; op++) {
        unsigned which = xs32() & 0x7u;
        unsigned i = xs32() % FUZZ_POOL;
        switch (which) {
        case 0: case 1: case 2: {  /* ADD */
            u32 rc = flow4_table_add(&ft_c, &pool_c[i], now);
            u32 re = flow4_extra_table_add(&ft_e, &pool_e[i], now);
            assert((rc == RIX_NIL) == (re == RIX_NIL));
            break;
        }
        case 3: case 4: {          /* FIND */
            struct flow4_key       kc = classic_key(i);
            struct flow4_extra_key ke = extra_key(i);
            u32 rc = flow4_table_find(&ft_c, &kc);
            u32 re = flow4_extra_table_find(&ft_e, &ke);
            assert((rc == RIX_NIL) == (re == RIX_NIL));
            break;
        }
        case 5: {                  /* DEL */
            struct flow4_key       kc = classic_key(i);
            struct flow4_extra_key ke = extra_key(i);
            u32 rc = flow4_table_del(&ft_c, &kc);
            u32 re = flow4_extra_table_del(&ft_e, &ke);
            assert((rc == RIX_NIL) == (re == RIX_NIL));
            break;
        }
        case 6: {                  /* TOUCH */
            if (ft_table_nb_entries(&ft_c) == 0u) break;
            u32 idx_c = xs32() % FUZZ_POOL + 1u;
            if (idx_c > ft.max_entries) break;
            flow_timestamp_touch(&pool_c[idx_c - 1].meta, now, cfg_c.ts_shift);
            ft_table_extra_touch(&ft_e, idx_c, now);
            break;
        }
        case 7: {                  /* ADVANCE TIME, occasional MAINT */
            now += (1u + (xs32() & 0xFFu));
            if ((xs32() & 0x3Fu) == 0u) {
                u32 ex_c[64], ex_e[64];
                struct ft_maint_ctx mc = { .buckets = ft_c.buckets,
                    .rhh_nb = &ft_c.ht_head.rhh_nb,
                    .pool_base = (void*)pool_c,
                    .stats = &ft_c.stats,
                    .pool_stride = sizeof(*pool_c),
                    .meta_off = offsetof(struct flow4_entry, meta),
                    .max_entries = FUZZ_POOL,
                    .rhh_mask = ft_c.ht_head.rhh_mask,
                    .ts_shift = cfg_c.ts_shift };
                struct ft_maint_extra_ctx me = { .buckets = ft_e.buckets,
                    .rhh_nb = &ft_e.ht_head.rhh_nb,
                    .stats = &ft_e.stats,
                    .rhh_mask = ft_e.ht_head.rhh_mask,
                    .ts_shift = cfg_e.ts_shift };
                unsigned next_c = 0, next_e = 0;
                unsigned nc = ft_table_maintain(&mc, 0, now,
                    /* expire_tsc = */ 1u << 16, ex_c, 64, 0, &next_c);
                unsigned ne = ft_table_extra_maintain(&me, 0, now,
                    /* expire_tsc = */ 1u << 16, ex_e, 64, 0, &next_e);
                assert(nc == ne);
                /* Sort both arrays and compare. */
                qsort(ex_c, nc, sizeof(u32),
                      int (*)(const void*, const void*))cmp_u32);
                qsort(ex_e, ne, sizeof(u32),
                      int (*)(const void*, const void*))cmp_u32);
                assert(memcmp(ex_c, ex_e, nc * sizeof(u32)) == 0);
            }
            break;
        }
        }
        ops_performed++;
        if (ops_performed % VAL_EVERY == 0u) {
            /* Count-level validation. */
            assert(ft_table_nb_entries(&ft_c) ==
                   ft_table_extra_nb_entries(&ft_e));
        }
    }

    ft_table_destroy(&ft_c);
    ft_table_extra_destroy(&ft_e);
    munmap(pool_c, FUZZ_POOL * sizeof(*pool_c));
    munmap(pool_e, FUZZ_POOL * sizeof(*pool_e));
    munmap(bk_c, flow4_table_bucket_size(FUZZ_POOL));
    munmap(bk_e, flow4_extra_table_bucket_size(FUZZ_POOL));
    printf("PASS parity fuzz %u ops\n", ops_performed);
    return 0;
}

static int cmp_u32(const void *a, const void *b) { u32 x = *(const u32*)a, y = *(const u32*)b; return (x>y)-(x<y); }
```

Note: `cmp_u32` is referenced inside the qsort call via an odd cast — rewrite to a conventional static function at file scope (move the declaration above `main` and fix the qsort call to `qsort(ex_c, nc, sizeof(u32), cmp_u32)`). The implementer should clean this up during write-up.

- [ ] **Step 2: Register the target**

In `flowtable/test/Makefile`:

```make
TEST_PARITY_TARGET := test_flow4_parity
TEST_PARITY_SRC    := test_flow4_parity.c

$(TEST_PARITY_TARGET): $(TEST_PARITY_SRC) $(LIB_CLASSIC_OBJS) $(LIB_EXTRA_OBJS)
	$(CC) $(CFLAGS) -Iextra/include -o $@ $^

test: test-classic test-extra test-parity
test-parity: $(TEST_PARITY_TARGET)
	./$(TEST_PARITY_TARGET)
```

- [ ] **Step 3: Run with gcc and clang across SIMD variants**

```bash
for cc in gcc clang; do
    for s in gen sse avx2; do
        make -C flowtable/test clean
        CC=$cc SIMD=$s make -C flowtable/test test-parity 2>&1 | tail -3
    done
done
```
Expected: every combination ends with `PASS parity fuzz 20000 ops`.

AVX-512: run only if the host supports it; otherwise log "AVX-512 not run on this host".

- [ ] **Step 4: Commit**

```bash
git add flowtable/test/test_flow4_parity.c flowtable/test/Makefile
git commit -m "Add flow4 classic<->extra cross-check parity fuzz"
```

---

## Task 14: Add `bench_flow4_vs_extra.c` — matched AB full bench

**Files:**
- Create: `flowtable/test/bench_flow4_vs_extra.c`
- Modify: `flowtable/test/Makefile`

**Pre-read:** `tests/hashtbl_extra/bench_vs_classic.c` — the rix_hash slot vs slot_extra matched bench merged to main on 2026-04-21. Use the same hugepage allocation, asymmetric TSC fences, and output format.

- [ ] **Step 1: Write the bench**

The bench has the same structural shape as `bench_vs_classic.c`:

1. Allocate two parallel pools (classic `flow4_entry` and extra `flow4_extra_entry`) via `mmap + MADV_HUGEPAGE`.
2. Allocate two parallel bucket buffers.
3. For each rep:
   - Reset both tables.
   - Measure each op (insert, find_hit, find_miss, touch, maintain(0%), maintain(50%), maintain(100%), maintain_idx_bulk, remove) separately, summing cycles.
4. Print the result table (`op`, `classic`, `extra`, `delta`) in cycles/op.

Key implementation notes (the implementer writes the full ~250-line body using this skeleton):

- Use `tsc_start()` (`lfence; rdtsc`) and `tsc_end()` (`rdtscp; lfence`) helpers identical to `bench_vs_classic.c`.
- For `maintain(R%)` generate a pool population where R% of entries have timestamps older than the expire threshold — write the raw encoded value to either `pool_c[i].meta.timestamp` (classic) or `bk_e[bk_idx].extra[slot]` (extra) after the fill phase. Benchmark-only privilege, not API.
- `N_ENTRIES = 1u << 16`, `N_BK = 1u << 12`, `REPS = 8` per spec §9.2.
- Print `sizeof(struct flow4_entry)` and `sizeof(struct flow4_extra_entry)` in the header for context.

- [ ] **Step 2: Build and run**

```bash
make -C flowtable/test bench-extra 2>&1 | tail -20
```
Expected: prints the cy/op matrix. Record the delta column values in the commit message for history.

- [ ] **Step 3: Sanity — compare with classic-only bench**

Run the classic bench (`flowtable/test/bench_flow_table`) and confirm that the `insert` / `find_hit` / `remove` cy/op of classic in the matched bench match (within ±10%) the classic-only bench at the same N/BK.

- [ ] **Step 4: Commit**

```bash
git add flowtable/test/bench_flow4_vs_extra.c flowtable/test/Makefile
git commit -m "Add matched flow4 classic-vs-extra bench"
```

---

## Task 15: Add `bench_flow4_maint_sweep.c` — maintain sweep matrix

**Files:**
- Create: `flowtable/test/bench_flow4_maint_sweep.c`
- Modify: `flowtable/test/Makefile`

**Pre-read:** Task 14's `bench_flow4_vs_extra.c` — this bench reuses its setup helpers but sweeps three dimensions.

- [ ] **Step 1: Write the bench**

The bench runs nested loops over `(N_ENTRIES, fill%, expire%)` per spec §9.3:

```c
const unsigned N_list[]      = { 16384, 65536, 262144, 1048576, 4194304 };
const unsigned fill_list[]   = { 25, 50, 75, 90 };
const unsigned expire_list[] = { 10, 50, 90 };
```

For each `(N, fill%, expire%)` combination:

- Allocate `N` classic entries + `N` extra entries + matching bucket tables. Bucket count chosen so that fill = `N / (bk_count * 16)` achieves the target %.
- Populate with `fill * N / 100` entries (classic and extra in lockstep).
- Select `expire% * fill_count / 100` entries uniformly at random and mark them stale (direct TS write).
- Run `ft_table_maintain` and `ft_table_extra_maintain` once each, timing cycles.
- Print a single row per combination: `N, fill%, expire%, classic_cy/bk, extra_cy/bk, ratio`.

Memory budget at top end: `4194304 * 24 B + 4194304 * 192 B ≈ 860 MB * 2 pools ≈ 1.7 GB`. Use hugepages.

- [ ] **Step 2: Build**

```bash
make -C flowtable/test bench-sweep 2>&1 | tail -20
```
Expected: binary built cleanly.

- [ ] **Step 3: Run**

Run the sweep (long-running; may take several minutes):

```bash
./flowtable/test/bench_flow4_maint_sweep | tee /tmp/maint_sweep_$(date +%Y%m%d_%H%M).log
```
Expected: full matrix prints. Archive the log file (git-ignore it).

- [ ] **Step 4: Commit**

```bash
git add flowtable/test/bench_flow4_maint_sweep.c flowtable/test/Makefile
git commit -m "Add maintain sweep bench across fill/expire/N"
```

---

## Task 16: Top-level Makefile integration and SIMD/toolchain matrix

**Files:**
- Modify: `Makefile` (top-level, if needed to expose `make test`/`make bench`/`make bench-full` paths for extra tests)

**Pre-read:** Top-level `Makefile`. The classic path already aggregates `flowtable/test/test` and `flowtable/test/bench`. Extend these so `make test` also runs the new extra and parity binaries, and `make bench` runs `bench-extra`, and `make bench-full` adds `bench-sweep`.

- [ ] **Step 1: Update the top-level targets**

Add:

```make
# in the test aggregator:
test: ... test-flowtable-extra test-flowtable-parity

test-flowtable-extra: build-flowtable
	@$(MAKE) -C flowtable/test test-extra

test-flowtable-parity: build-flowtable
	@$(MAKE) -C flowtable/test test-parity

# in the bench aggregator:
bench: ... bench-flowtable-extra

bench-flowtable-extra: build-flowtable
	@$(MAKE) -C flowtable/test bench-extra

bench-full: ... bench-flowtable-sweep

bench-flowtable-sweep: build-flowtable
	@$(MAKE) -C flowtable/test bench-sweep
```

- [ ] **Step 2: Run the full matrix**

```bash
for cc in gcc clang; do
    for s in gen sse avx2; do
        make clean
        CC=$cc SIMD=$s make test 2>&1 | tail -5
    done
done
```
Expected: every combination reports success. Record which combinations were run and their results in the commit message.

AVX-512 runs are host-dependent; report separately.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "Integrate flowtable/extra test and bench into top-level make"
```

---

## Task 17: Record validation and merge to main

**Files:**
- Create: `docs/superpowers/validation/2026-04-22-flow4-slot-extra-validation.md`

**Pre-read:** `docs/superpowers/validation/*-slot-extra-validation.md` (or the v0.3.0 validation record referenced in the last slot_extra work). Match the format.

- [ ] **Step 1: Capture bench numbers**

Run the matched bench and the sweep on this host, capturing the full output:

```bash
make clean
make bench 2>&1 | tee /tmp/bench_extra.log
make bench-full 2>&1 | tee /tmp/bench_extra_sweep.log
```

- [ ] **Step 2: Write the validation doc**

Include:

1. Host info: `cat /proc/cpuinfo | head -30`, `lscpu | head`, hugepage status.
2. Toolchains used: gcc version, clang version.
3. SIMD matrix coverage: which variants were run; AVX-512 status.
4. Full-AB bench result table (paste from `/tmp/bench_extra.log`).
5. Sweep bench summary table (classic/extra ratio per N × fill × expire).
6. Pass/fail against spec §11 success criteria, item by item:
   - 11.1.1 parity fuzz — PASS / FAIL with evidence.
   - 11.1.2 classic tests untouched — confirm by `git diff --stat flowtable/src flowtable/include flowtable/test/test_flow_table.c`.
   - 11.1.3 SIMD matrix — list.
   - 11.1.4 `-Werror` — confirm.
   - 11.1.5 build/install — confirm.
   - 11.2 performance targets — rows filled with actual numbers; highlight any missed target.

- [ ] **Step 3: Commit the validation record**

```bash
git add docs/superpowers/validation/2026-04-22-flow4-slot-extra-validation.md
git commit -m "Record flow4 slot_extra validation results"
```

- [ ] **Step 4: Review and merge to main**

```bash
git log --oneline main..work
```
Expected: the full task chain (Task 1 through Task 16 commits plus this validation commit).

If the validation doc confirms spec §11.1 must-haves are met (spec §11.2 performance targets may be missed — follow spec §11.3 for merge decision):

```bash
git checkout main
git merge --no-ff work -m "Merge branch 'work': flow4 slot_extra variant"
make clean && make test
git branch -d work
```

- [ ] **Step 5: Push to origin**

Confirm with the user before pushing:

> "All tasks complete, validation recorded, merge to main done locally. Push `main` to `origin`?"

If yes:

```bash
git push origin main
```

Expected: remote updated. Report the new `origin/main` SHA to the user.

---

## Self-Review (for the plan author)

**Spec coverage check:**

- §1 goal — covered by Tasks 1-17.
- §2 scope (flow4 only, parallel API) — enforced by file layout in Task 1 and symbol naming in Tasks 2-4.
- §3 cacheline layout — Task 2 (ft_table_extra), Task 3 (meta_extra).
- §4 data types — Task 3.
- §4.2 TS accessors — Task 3 inline helpers.
- §5 API surface — Tasks 2, 4, 8, 9, 10.
- §5.4 touch — called out explicitly in Task 10/flow4_extra.c and tested in Task 12.
- §6 internal layout — Tasks 5-10.
- §7 maintain algorithm — Task 10 is the implementer.
- §8 testing — Task 12 (functional) + Task 13 (parity).
- §9 benchmarks — Task 14 (full AB) + Task 15 (sweep).
- §10 build integration — Tasks 11, 12, 13, 14, 15, 16.
- §11 success criteria — Task 17 validation record.

No spec section left uncovered.

**Placeholder scan:** Task 10's `maintain_idx_bulk` intentionally leaves the body to be transcribed from classic; Task 10 Step 3 gives the exact transcription rules. No `TBD`/`TODO` elsewhere.

**Type consistency check:**

- `struct ft_table_extra` — used consistently from Task 2 onwards.
- `struct flow4_extra_entry` / `struct flow4_extra_key` — Tasks 3, 4, 8, 12, 13, 14, 15.
- `struct rix_hash_bucket_extra_s` — from rix_hash_slot_extra.h, no local typedef.
- `struct ft_maint_extra_ctx` — Task 2, referenced Task 10, used Tasks 12, 13.
- `ft_table_extra_stats` — Task 2, used throughout.
- `ft_arch_extra_init` — declared Task 2, defined Task 9, called in tests Task 12/13 and benches Task 14/15.

Names match across tasks.
