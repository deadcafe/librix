# Flow Table

`samples/ftable/` is a flow-table prototype built on librix. It is related
to `samples/fcache/`, but its purpose is different:

- no `findadd`
- no reclaim or timeout-driven eviction
- stable caller-owned record array
- explicit `find`, `add`, and `del`
- bucket-table growth without moving records

The current implementation is intentionally narrow in scope:

- `flow4` only
- runtime arch dispatch (`gen`, `sse4.2`, `avx2`, `avx512f`)
- bucket hash table grows by `2x`
- automatic shrink is not implemented

## Quick Start

```sh
# Build the flow table library
make -C samples/ftable static

# Run functional tests
make -C samples/ftable/test test

# Run the current benchmark set
make -C samples/ftable/test bench

# Or use the top-level delegating targets
make -C samples ftable
make -C samples ftable-test
make -C samples ftable-bench
```

## 1. Design Goals

The design target is a permanent-style flow table rather than a cache.
Entries are expected to live until the caller deletes them.

The table therefore prefers:

- stable entry indices
- explicit growth points
- no implicit victim eviction
- caller-controlled memory allocation for bucket storage

This differs from `fcache`, which is optimized around reclaim and
`findadd`.

## 2. Current Scope

The current prototype implements:

- `ft_flow4_table`
- scalar and bulk `find`, `add`, and `del`
- intrusive `init_ex()` with caller-defined record layout
- runtime arch dispatch with unsuffixed public APIs
- `need_grow`, `grow_2x`, and `reserve`
- bucket-table-only resize

Not implemented yet:

- `flow6`
- `flowu`
- shrink
- background or incremental resize
- benchmark text and published performance tables

## 3. Storage Model

The caller owns the record array. The table stores records in stable
storage and never relocates them during resize.

Resize affects only the bucket hash table.

This means:

- `entry_idx` remains stable across `grow_2x()`
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

- `hash0 != 0`
- `hash1 != 0`
- for the fixed `start_mask`,
  `(hash0 & start_mask) != (hash1 & start_mask)`
- therefore, for every later active table mask `mask = 2^n - 1`,
  `(hash0 & mask) != (hash1 & mask)`

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

This is why bucket-table-only growth is feasible.

When the bucket table grows:

1. Allocate a new bucket array.
2. For each live entry, recompute `bk0/bk1` from saved `hash0/hash1` and
   the new mask.
3. Reinsert the entry into the new bucket table.
4. Free the old bucket array.

The current policy is:

- bucket count is always a power of two
- default minimum bucket count is `16384`
- default maximum bucket count is `1048576`
- default grow watermark is `60%`
- growth factor is `2x`
- shrink is not performed

### 4.4 Current `grow_2x()` logic

The current `grow_2x()` implementation is intentionally structured as a
staged rebuild pipeline rather than a naive:

1. scan one old entry
2. compute one new bucket pair
3. insert immediately

The reason is that growth is dominated by memory latency. The rebuild must
touch three different objects:

- the old bucket table
- the stable entry array
- the new bucket table

The current implementation overlaps those accesses with a small ring and a
short ahead distance.

The grow path currently does the following:

1. Walk the old bucket table in bucket order.
2. Prefetch only the old bucket `idx[]` cache line.
   The old `hash[]` line is not needed during rebuild, because grow only
   harvests live entry indices from the old table.
3. Prefetch the corresponding entry lines for occupied old slots.
4. Read the saved `hash0/hash1` from the entry and derive `new_bk0/new_bk1`
   from the new mask.
5. Eagerly prefetch only `new_bk0.hash[]`.
   Since `grow_2x()` rebuilds into a table that is roughly half as full, most
   inserts are expected to succeed in `bk0`.
6. Defer the actual insert through a small staging ring.
7. Reinsert with a dedicated no-duplicate path.

The rehash insert path is intentionally different from normal `add()`:

- it does not perform duplicate-key checks
- it does not treat the old table as part of the lookup path
- it only needs to find an empty destination slot and perform kickout if
  necessary

This is valid because uniqueness has already been guaranteed by the old
table. During grow, the implementation is relocating known-live unique
entries, not admitting new keys.

Two negative choices are also intentional:

- `bk1` is not eagerly prefetched during grow
- the new bucket `idx[]` line is not eagerly prefetched

Both are touched lazily only when the insert path actually needs them.
At the current grow watermark (`60%`) and `2x` expansion policy, the rebuilt
table lands near `30%` fill, so speculative `bk1` traffic is usually wasted.

The current prefetch depths are conservative defaults, not part of the API
contract. They may still be tuned per machine, but the more important design
choice is the staging itself: old bucket `idx[]`, then entry, then new
`bk0.hash[]`, then commit.

### 4.5 Design consequence

Most of `ftable` is intentionally similar to `fcache`:

- fixed-width flow keys
- intrusive `init_ex()` record layout
- stable `entry_idx`
- scalar and bulk APIs

The new design problem is not only the surrounding API shape. The central
problem is still the resize-safe hash contract described above, and the grow
path is the concrete place where that contract becomes performance-critical.

## 5. Allocation Model

The table does not assume `malloc`, `realloc`, or any specific allocator.
Bucket memory is provided through allocator callbacks in
`struct ft_bucket_allocator`.

The current contract is:

- allocate new bucket storage with `(size, align, arg)`
- rebuild into the new bucket array
- free the old bucket storage with `(ptr, size, align, arg)`

This is intended to fit hugepage-backed or custom control-plane allocators.

## 6. API Overview

Public headers:

- `samples/ftable/include/flow_table.h`
- `samples/ftable/include/flow4_table.h`

Important APIs:

- `ft_flow4_table_init()`
- `ft_flow4_table_init_ex()`
- `ft_flow4_table_destroy()`
- `ft_flow4_table_flush()`
- `ft_flow4_table_find()`
- `ft_flow4_table_add_entry_idx()`
- `ft_flow4_table_del_key()`
- `ft_flow4_table_del_entry_idx()`
- `ft_flow4_table_find_bulk()`
- `ft_flow4_table_add_idx_bulk()`
- `ft_flow4_table_del_entry_idx_bulk()`
- `ft_flow4_table_need_grow()`
- `ft_flow4_table_grow_2x()`
- `ft_flow4_table_reserve()`
- `ft_arch_init()`

Architecture selection follows the same pattern as `fcache`.

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
- `add_entry()` is a typed convenience wrapper over `add_idx()`
- `add()` remains available as a compatibility wrapper that allocates from the
  internal free-entry list

The behavior is intentionally simple:

- `find()` returns `0` on miss
- `add_idx()` and `add_entry()` return an existing `entry_idx` for duplicate
  insert
- `add_idx()` and `add_entry()` return `0` on failure
- `add()` returns an existing `entry_idx` for duplicate insert
- `add()` returns `0` on allocation or full failure
- `del()` returns `0` on miss

Typical caller-owned registration:

```c
struct my_flow4_record {
    struct flow4_entry entry;
    unsigned char body[128];
};

uint32_t idx = 17;

records[idx - 1].entry.key = key;
ft_flow4_table_add_idx(&ft, idx);
```

## 7. Intrusive Layout with `init_ex()`

The `_ex` API supports caller-defined fixed-stride records.

Example:

```c
struct my_flow4_record {
    unsigned char pad[64];
    struct flow4_entry entry;
    unsigned char body[128];
};

struct ft_flow4_config cfg = { 0 };

FT_FLOW4_TABLE_INIT_TYPED(&ft, records, max_entries,
                          struct my_flow4_record, entry, &cfg);
```

This keeps the record layout under caller control while still allowing the
table to map:

- `entry_idx -> record`
- `entry_idx -> embedded entry`
- `entry -> containing record`

When `init_ex()` is used, `flush()` and `del()` clear table metadata but do not
destroy caller-owned record payload. The caller may reuse the same record and
register it again through `add_idx()` or `add_entry()`.

Helper APIs and macros:

- `ft_flow4_table_record_ptr()`
- `ft_flow4_table_record_cptr()`
- `ft_flow4_table_entry_ptr()`
- `ft_flow4_table_entry_cptr()`
- `FT_FLOW4_TABLE_RECORD_PTR_AS(...)`
- `FT_FLOW4_TABLE_RECORD_FROM_ENTRY(...)`
- `FT_FLOW4_TABLE_ENTRY_FROM_RECORD(...)`

## 8. Layout Guidelines

`init_ex()` allows arbitrary record layouts, but layout quality still
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

## 9. Growth Policy

The intended operating model is:

- run with a conservative fill target, typically `50%` to `60%`
- mark `need_grow` when the watermark is crossed
- execute `grow_2x()` at a caller-selected time

The prototype does not implement incremental migration. Growth is a single
rebuild step.

That is a deliberate tradeoff:

- simpler semantics
- stable lookup path
- explicit control over when the resize cost is paid

## 10. Current Test Coverage

The current test program is `samples/ftable/test/test_flow_table.c`.

It covers:

- basic `add/find/del`
- duplicate `add` returning the existing entry
- delete miss accounting
- intrusive `init_ex()` record mapping
- `entry -> record` and `record -> entry` round-trip helpers
- bulk `find/add/del`
- `walk()` full scan and early-stop behavior
- hash-pair invariants used by resize
- manual `grow_2x()` preserving existing entries
- repeated `grow_2x()` preserving `hash0/hash1`
- failed `grow_2x()` leaving the old table intact
- `need_grow()` behavior after watermark crossing
- `reserve()` growing the bucket table ahead of time
- allocator failure on initial bucket allocation
- max-bucket limit handling
- config rounding and clamp behavior

Run:

```sh
make -C samples/ftable/test test
```

## 11. Current Benchmark Coverage

The current benchmark program is `samples/ftable/test/bench_flow_table.c`.
It builds `ft_bench`.

Run:

```sh
make -C samples/ftable/test bench
```

The current modes are:

- `./ft_bench datapath`
- `./ft_bench grow`

The current datapath benchmark reports:

- `find_hit`
- `find_miss`
- `add_only`
- `del_bulk`

for the current built-in table sizes:

- `32768`
- `1048576`

The current resize benchmark reports:

- `grow_2x()` cycles per live entry

### 11.1 Current measured grow status

The current `grow_2x()` path has already gone through a few structural
optimizations:

- resize-safe saved `hash0/hash1`
- staged rebuild instead of scalar reinsertion
- no-duplicate rehash insert path
- old-table prefetch limited to `idx[]`
- new-table eager prefetch limited to `bk0.hash[]`
- no eager `bk1` prefetch

With the current implementation, the representative measured point is:

- arch: `avx2`
- entries: `1048576`
- fill before grow: `60%`
- benchmark sampling: `raw_repeat=5`, `keep_n=3`, `pin-core=2`
- observed `grow_2x()`: about `118 cy/live-entry`

The current `perf stat` point for the same scenario is approximately:

- IPC: `0.91`
- cache miss rate: `13.55%`

This means the grow path is still primarily memory-bound, but it is no longer
dominated by avoidable duplicate checking or wasted `bk1` prefetch traffic.

The current grow defaults are intentionally conservative:

- `FT_FLOW4_GROW_OLD_BK_AHEAD = 2`
- `FT_FLOW4_GROW_REINSERT_AHEAD = 8`

These depths were tuned experimentally, but the results were noisy enough that
no single deeper setting was consistently better after full rebuild-and-run
validation. The current conclusion is:

- the staging structure matters more than fine-grained ahead tuning
- line selection (`old idx[]`, `entry`, `new bk0.hash[]`) matters more than
  speculative `bk1` traffic
- further improvement is still possible, but the remaining work is expected to
  be more expensive in code complexity

for:

- `1048576` entries

Current methodology:

- runtime arch dispatch via `ft_arch_init()`
- optional `--arch gen|sse|avx2|avx512|auto`
- optional `--raw-repeat N`, `--keep-n N`, and `--pin-core CPU`
- default `make bench` policy: `--pin-core 2 --raw-repeat 11 --keep-n 7`
- the tightest `keep_n` samples out of `raw_repeat` are kept and reported
- datapath prefill at a conservative `40%` live-entry target
- grow benchmark using bucket-table-only rebuild with stable record storage

The current benchmark is intentionally narrow. It exists to validate the
initial `flow4` table datapath and growth path before broader coverage is
added.

## 12. Performance Notes

Performance tables are intentionally omitted for now.

The current goal is to stabilize:

- API shape
- growth semantics
- intrusive layout support
- allocator contract

The benchmark binary is now in place, but the published performance text is
still deferred until:

- benchmark item coverage expands
- results are checked across more than one CPU generation
- numbers are stable enough to document as representative

## 13. File Structure

```text
samples/ftable/
  README.md
  README_JP.md
  Makefile
  include/
    flow_table.h
    flow4_table.h
    ft_table_common.h
  src/
    flow4.c
  test/
    Makefile
    bench_flow_table.c
    test_flow_table.c
```
