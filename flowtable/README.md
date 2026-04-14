# Flow Table

`flowtable/` is a flow-table library built on librix.

- no `findadd`
- no implicit reclaim or timeout-driven eviction on datapath add
- stable caller-owned record array
- explicit `find`, `add`, and `del`
- bucket-table resize without moving records

The implementation supports three flow key families:

- `flow4` (IPv4 5-tuple)
- `flow6` (IPv6 5-tuple)
- `flowu` (unified / protocol-independent)

with runtime arch dispatch (`gen`, `sse4.2`, `avx2`, `avx512f`).

## Quick Start

```sh
# Build the flow table library
make -C flowtable static

# Run functional tests
make -C flowtable/test test

# Run benchmarks
make -C flowtable/test bench

# Or use the top-level delegating targets
make flowtable
make flowtable-test
make flowtable-bench
```

Build artifacts are written under `flowtable/build/`:

- `build/lib/libftable.a`
- `build/obj/*.o`
- `build/bin/ft_test`
- `build/bin/ft_bench`

## 1. Design Goals

The design target is a permanent-style flow table rather than a cache.
Entries are expected to live until the caller deletes them.

The table therefore prefers:

- stable entry indices
- explicit resize points
- no implicit victim eviction
- caller-controlled memory allocation for both record and bucket storage

This differs from `fcache`, which is optimized around reclaim and
`findadd`. `ftable` has explicit timeout maintenance, but it is
caller-invoked and leaves reclaim ownership to the caller.

## 2. Current Scope

The current implementation provides:

- `ft_flow4_table`, `ft_flow6_table`, `ft_flowu_table`
- scalar and bulk `find`, `add_idx`, `del_idx`, `del_key`
- intrusive `init()` with caller-defined record layout (stride + offset)
- runtime arch dispatch with unsuffixed public APIs
- `migrate()` for grow / shrink / rehash
- explicit timeout maintenance (`maintain`, `maintain_idx_bulk`)
- bucket-table-only resize (records are never relocated)

### 2.1 Timeout Maintenance

`ftable` does not reclaim on `add`. Expire is a separate maintenance step.

- `maintain(...)`
  - incremental bucket sweep
  - caller controls `start_bk`, `expire_tsc`, `max_expired`, and `min_bk_entries`
  - returns expired entry indices and `next_bk`
- `maintain_idx_bulk(...)`
  - local maintenance for buckets reached from recently hit `entry_idx[]`
  - intended for post-`find`/post-`add` maintenance when the bucket is
    likely still hot
  - caller can override expire time per call via `expire_tsc`

Both APIs return expired entry indices to the caller. Reclaiming those
indices back to a free list is caller responsibility. Expire time is chosen
per API call and is not taken from table state.

Permanent entries are supported by setting the stored timestamp to zero.

- `ft_flowX_table_set_permanent_idx(ft, idx)`
  - marks a linked entry as permanent
  - `maintain` and `maintain_idx_bulk` never expire it
  - `find` and duplicate `add` keep the zero timestamp unchanged

## 3. Storage Model

The caller owns the record array. The table stores records in stable
storage and never relocates them during resize.

Resize affects only the bucket hash table.

This means:

- `entry_idx` remains stable across `migrate()`
- intrusive record layouts are practical
- resize cost is concentrated in bucket rebuild, not record movement

## 4. Hash and Resize Model

At `add()` time the implementation computes and stores a stable hash pair:

- `hash0`
- `hash1`

These values are the core contract of `ftable`.

`hash0/hash1` are size-independent. They are computed once from the key using
the table's fixed `start_mask`, then stored in the entry. They are not
recomputed from the key during resize.

What changes during resize is only the bucket selection:

- `bk0 = hash0 & mask`
- `bk1 = hash1 & mask`

where `mask = nb_bk - 1` and `nb_bk` is always a power of two.

In other words:

- `key -> hash0/hash1` is stable state
- `hash0/hash1 -> bk0/bk1` is derived state

The table must preserve that distinction. Persisting old `bk0/bk1` across
resize would be incorrect.

`hash1` is not constrained by the current bucket-table size. The constraint is
applied against the fixed `start_mask` that was chosen when the table was
created. That is the resize-safe contract:

- `key -> hash0/hash1` uses the table's fixed `start_mask`
- `hash0/hash1 -> bk0/bk1` uses the current `mask`

### 4.1 Required invariants

For the saved hash pair, the intended invariants are:

- for the fixed `start_mask`,
  `(hash0 & start_mask) != (hash1 & start_mask)`
- therefore, for every later active table mask `mask = 2^n - 1`,
  `(hash0 & mask) != (hash1 & mask)`
- therefore `fp = hash0 ^ hash1` is also non-zero for every active mask

`hash0` and `hash1` themselves may be zero. What matters is that the masked
bucket pair stays distinct, so the derived fingerprint used in the bucket
array is never zero.

The last condition is what makes bucket-table-only resize possible. The
entry carries enough information to derive a valid candidate bucket pair for
both the old and new table sizes.

### 4.2 Why `hash0/hash1` are saved

This is the essential difference between `ftable` and `fcache`.

`fcache` can treat bucket placement as an ephemeral cache detail because the
structure is not designed around stable resize. `ftable` cannot. It needs a
canonical, resize-safe hash identity per entry.

That is why `ftable` stores:

- the key
- `hash0`
- `hash1`

and treats bucket indices as temporary derived values only.

### 4.3 Resize behavior

Resize must not rebuild `hash1` from the old mask, old bucket, or old slot.
It must not rehash from the key either unless the implementation explicitly
changes the hash algorithm or seed.

The normal resize path is:

1. Keep the stored `hash0/hash1` unchanged.
2. Allocate a new bucket array.
3. Compute `new_bk0 = hash0 & new_mask`.
4. Compute `new_bk1 = hash1 & new_mask`.
5. Reinsert the live entry into the new bucket table.
6. Free the old bucket array.

This is why bucket-table-only resize is feasible.

### 4.4 Resize policy

- bucket count is always a power of two
- minimum bucket count is `4096` (`FT_TABLE_MIN_NB_BK`)
- `migrate()` accepts grow, shrink, and same-size rehash
- constraint: new bucket count must be >= init-time bucket count
  (`new_nb_bk > start_mask`)
- shrink below init-time bucket count is rejected
- the caller decides when and how much to resize

### 4.5 Current `migrate()` logic

The current `migrate()` is a staged rebuild pipeline rather than a naive
one-entry-at-a-time reinsertion. Migration is dominated by memory latency.
The rebuild must touch three different objects:

- the old bucket table
- the stable entry array
- the new bucket table

The current implementation overlaps those accesses with a small ring and a
short ahead distance.

The grow path currently does the following:

1. Walk the old bucket table in bucket order.
2. Prefetch only the old bucket `idx[]` cache line.
3. Prefetch the corresponding entry lines for occupied old slots.
4. Read the saved `hash0/hash1` from the entry and derive `new_bk0/new_bk1`
   from the new mask.
5. Eagerly prefetch only `new_bk0.hash[]`.
6. Defer the actual insert through a small staging ring.
7. Reinsert with a dedicated no-duplicate path.

### 4.6 Design consequence

Most of `ftable` is intentionally similar to `fcache`:

- fixed-width flow keys
- intrusive record layout with caller-defined stride/offset
- stable `entry_idx`
- scalar and bulk APIs

The central design problem is the resize-safe hash contract described above,
and the migrate path is the concrete place where that contract becomes
performance-critical.

## 5. Allocation Model

The table does not assume `malloc`, `realloc`, or any specific allocator.
All memory is provided by the caller:

- **Record array**: caller allocates, owns, and frees. The table indexes
  into it by stride and offset.
- **Bucket memory**: caller allocates a raw buffer (no alignment requirement)
  and passes it to `init()` or `migrate()`. The library carves out the
  largest power-of-2 aligned bucket region internally via
  `ft_table_bucket_carve()`.

Helper functions:

- `ft_table_bucket_size(max_entries)` — compute the recommended bucket
  allocation size for init (minimum 4096 buckets = 512 KiB)
- `ft_table_bucket_mem_size(nb_bk)` — compute bucket memory size from
  a known bucket count (useful for grow/shrink: multiply by 2 or divide
  by 2)

This model fits hugepage-backed, NUMA-aware, or custom control-plane
allocators. The library never calls `malloc` or `free` internally.

## 6. API Overview

Public headers:

- `flowtable/include/flow_table.h` — umbrella header
- `flowtable/include/flow4_table.h`
- `flowtable/include/flow6_table.h`
- `flowtable/include/flowu_table.h`
- `flowtable/include/flow_common.h` — shared types and helpers

Private implementation headers live under `flowtable/src/`, and bench-only
helpers live under `flowtable/test/`.

Primary APIs (shown for `flow4`; `flow6` and `flowu` are identical):

- `ft_flow4_table_init()` — initialise table with caller-provided
  record array and bucket memory
- `ft_flow4_table_destroy()` — zero the table struct (does not free memory)
- `ft_flow4_table_flush()` — remove all entries
- `ft_flow4_table_find()` / `ft_flow4_table_find_bulk()`
- `ft_flow4_table_add_idx()` / `ft_flow4_table_add_idx_bulk()`
- `ft_flow4_table_del_idx()` / `ft_flow4_table_del_idx_bulk()`
- `ft_flow4_table_del_key_bulk()`
- `ft_flow4_table_migrate()` — bucket resize (grow / shrink / rehash)
- `ft_flow4_table_maintain()` / `ft_flow4_table_maintain_idx_bulk()`
- `ft_flow4_table_walk()`
- `ft_arch_init()` — one-time CPU detection and SIMD dispatch

Architecture selection:

- `FT_ARCH_GEN`
- `FT_ARCH_SSE`
- `FT_ARCH_AVX2`
- `FT_ARCH_AVX512`
- `FT_ARCH_AUTO`

Typical startup:

```c
#include "flow_table.h"

ft_arch_init(FT_ARCH_AUTO);
```

The intended registration model is:

- caller-owned records remain stable
- the table indexes those records by `entry_idx`
- `add_idx()` is the primary registration API
- `find()` returns `0` on miss
- `add_idx()` returns an existing `entry_idx` for duplicate insert
- `add_idx()` returns `0` on failure
- `del_idx()` / `del_key_bulk()` return `0` on miss

Typical caller-owned registration:

```c
struct my_flow4_record {
    struct flow4_entry entry;
    unsigned char body[128];
};

u32 idx = 17;

records[idx - 1].entry.key = key;
ft_flow4_table_add_idx(&ft, idx, now);
```

## 7. Intrusive Layout

`init()` accepts caller-defined fixed-stride records via `stride` and
`entry_offset` parameters. The convenience macro `FT_FLOW4_TABLE_INIT_TYPED`
computes these from a struct type and member name.

Example:

```c
struct my_flow4_record {
    unsigned char pad[64];
    struct flow4_entry entry;
    unsigned char body[128];
};

size_t bk_size = ft_table_bucket_size(max_entries);
void *bk_mem = mmap(NULL, bk_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

FT_FLOW4_TABLE_INIT_TYPED(&ft, records, max_entries,
                          struct my_flow4_record, entry,
                          bk_mem, bk_size, NULL);
```

This keeps the record layout under caller control while still allowing the
table to map:

- `entry_idx -> record`
- `entry_idx -> embedded entry`
- `entry -> containing record`

`flush()` and `del()` clear table metadata but do not destroy caller-owned
record payload. The caller may reuse the same record and register it again
through `add_idx()`.

Helper APIs and macros:

- `ft_flow4_table_record_ptr()` / `ft_flow4_table_record_cptr()`
- `ft_flow4_table_entry_ptr()` / `ft_flow4_table_entry_cptr()`
- `FT_FLOW4_TABLE_RECORD_PTR_AS(...)`
- `FT_FLOW4_TABLE_RECORD_FROM_ENTRY(...)`
- `FT_FLOW4_TABLE_ENTRY_FROM_RECORD(...)`

## 8. Layout Guidelines

`init()` allows arbitrary record layouts, but layout quality still
matters for performance.

Recommended:

- place `entry` on a cache-line boundary
- keep record stride under control
- avoid making the record body larger than needed

Observed risks:

- large record bodies increase cache and TLB pressure
- misaligned embedded entries can hurt lookup performance significantly
- performance-sensitive paths should treat layout as part of the API
  contract

## 9. Resize Policy

The intended operating model is:

- run with a conservative fill target, typically `50%` to `60%`
- decide whether to execute `migrate()` from stats and caller policy
- use `ft_table_bucket_mem_size(ft->nb_bk) * 2` to compute the grow
  allocation size

The library does not implement incremental migration. Resize is a single
rebuild step.

That is a deliberate tradeoff:

- simpler semantics
- stable lookup path
- explicit control over when the resize cost is paid

Shrink is supported: the caller can allocate a smaller bucket region and
call `migrate()`. The only constraint is that the new bucket count must
be >= the init-time bucket count.

## 10. Current Test Coverage

The current test program is `flowtable/test/test_flow_table.c`.

It covers (for all three variants: flow4, flow6, flowu):

- basic `add/find/del`
- duplicate `add` returning the existing entry
- delete miss accounting
- intrusive record mapping
- `entry -> record` and `record -> entry` round-trip helpers
- bulk `find/add/del`
- `add_idx_bulk` duplicate ignore and update policies
- `walk()` full scan and early-stop behavior
- hash-pair invariants used by resize
- `migrate()` preserving existing entries
- `migrate()` doubling bucket count
- timestamp update on find/add
- permanent timestamp entries
- `maintain_idx_bulk` expiry
- fuzz testing with random operation sequences

Run:

```sh
make -C flowtable/test test
```

## 11. Benchmark

The benchmark program is `flowtable/test/bench_flow_table.c`,
producing the `ft_bench` binary.

```sh
make -C flowtable/test bench
```

- `bench` / `bench-light`: `flow4` only, `q=1/8/32/256`, fill `60%`
- `bench-full`: `flow4/6/u` query sweep plus `maint` and `grow`

### 11.1 Benchmark modes

- **Datapath** (default): cold bulk operations with cache-flush between rounds
- **Grow** (`--grow`): bucket-table-only `migrate()` (2x) with mmap allocation
- **Maintain** (`--maint`): bucket sweep and index-based maintenance

Options:

- `--arch gen|sse|avx2|avx512|auto`
- `--raw-repeat N`, `--keep-n N` — sampling control
- `--pin-core CPU` — core affinity
- `--no-hugepage` — disable 2 MiB hugepages (on by default)
- `--op OP` — run only a specific operation
- `--query N` — batch size (default 256)

Default `make bench` policy: `--pin-core 2 --raw-repeat 11 --keep-n 7`

Notes:

- In cold datapath `add_*` measurements, the query entry itself is warmed
  immediately before timing. This keeps the input record hot while bucket and
  resident-table state remain cold.
- `add_idx_bulk()` uses the small scalar add path for `--query < 4`; larger
  batches use the pipelined bulk path.

### 11.2 Representative Performance

Measured on a single x86-64 core with `--pin-core 2 --raw-repeat 3
--keep-n 1`, hugepages enabled, 1M entries at 60% fill.

**Datapath (cold bulk, 256-key batches):**

| Operation      | flow4 (cy/key) | flow6 (cy/key) |
|----------------|---------------:|---------------:|
| find_hit       |          72.66 |          79.84 |
| find_miss      |          60.47 |          67.19 |
| add_idx        |          75.39 |          87.27 |
| add_ignore     |          90.62 |         101.33 |
| add_update     |          94.22 |         107.34 |
| del_idx        |          51.02 |          55.62 |
| del_key        |          71.88 |          87.97 |
| find_del_idx   |          92.73 |         111.72 |

**Grow (migrate 2x):**

| Phase          | flow4 (cy/live-entry) |
|----------------|----------------------:|
| alloc (mmap)   |                 68.42 |
| migrate        |                227.33 |
| total          |                295.75 |

**Maintain:**

| Mode                | flow4 (cy/entry) |  IPC | cache-hit |
|---------------------|------------------:|-----:|----------:|
| maint_expire_dense  |             84.50 | 1.09 |    70.18% |
| maint_nohit_dense   |             86.49 | 1.05 |    70.21% |
| maint_idx_expire    |             22.62 | 0.44 |    43.74% |
| maint_idx_filtered  |             27.50 | 0.32 |    65.70% |

Notes:

- Datapath operations are measured cold (cache-flush between rounds)
  to reflect realistic packet-processing scenarios
- `del_idx` does not write to the entry; registration is bucket-only
- The grow path is primarily memory-bound; the staged prefetch pipeline
  hides most of the latency
- `maint_idx_expire` at ~23 cy/entry is efficient for post-hit
  local maintenance

## 12. File Structure

```text
flowtable/
  README.md
  README_JP.md
  TEST_SPEC_JP.md
  Makefile
  include/
    flow_table.h
    flow4_table.h
    flow6_table.h
    flowu_table.h
    flow_common.h
  src/
    flow4.c
    flow6.c
    flowu.c
    flow_core.h
    flow_hash.h
    ft_dispatch.c
    ft_maintain.c
    flow_dispatch.h
    flow_table_generate.h
  test/
    Makefile
    bench_flow_table.c
    bench_scope.h
    test_flow_table.c
```
