# flow4 slot_extra variant — Design

> Status: approved 2026-04-22. Branch: `work`. Successor to the rix_hash
> slot_extra work merged to main on 2026-04-18 (spec
> `2026-04-18-rix-hash-slot-extra-design.md`).

## 1. Goal

Apply the already-merged `RIX_HASH_GENERATE_SLOT_EXTRA_EX` rix_hash
variant to the flow4 flow cache. Relocate the per-entry expiry
timestamp from `flow_entry_meta.timestamp` into the bucket's third
cacheline (`extra[slot]`), so that maintenance sweeps can decide
expiry by touching only bucket memory, never entry memory.

The implementation is delivered as a **parallel** API
(`ft_table_extra_*`, `flow4_extra_table_*`) living in its own
subdirectory `flowtable/extra/`. Classic `ft_table_*` / `flow4_table_*`
is untouched. The two implementations coexist in one process so a
matched AB benchmark can run both under identical N/BK/reps/hugepage
conditions.

## 2. Scope and non-scope

### In scope

- flow4 variant only.
- Timestamp relocation from entry meta to bucket `extra[]`.
- New public API surface mirroring classic 1:1.
- Cross-check fuzz: classic and extra driven by the same op stream,
  asserted equivalent.
- Matched benchmark binaries: full AB + maintain sweep.

### Out of scope (explicit non-goals)

- flow6 / flowu extra variants (horizontal rollout after flow4 is
  validated, in a separate spec).
- Removing `meta` entirely by stashing `cur_hash` / `slot` also in
  bucket memory. Reverse lookup still uses these, so the minimum move
  is timestamp only.
- SIMD-accelerated `maintain` scan (the first cut is scalar; a SIMD
  lane is a followup task after AB numbers are in).
- `perf_event_open` syscalls inside the benchmark binary. External
  `perf stat -e ...` is the intended measurement path.
- Adding extra to `make install`. extra ships as an experimental
  subtree until classic is retired.

## 3. Architecture

### 3.1 Cacheline layout

| Item              | classic      | extra                                  |
|-------------------|--------------|----------------------------------------|
| bucket size       | 128 B (2 cl) | 192 B (3 cl)                           |
| cl0               | `hash[16]`   | `hash[16]`                             |
| cl1               | `idx[16]`    | `idx[16]`                              |
| cl2               | n/a          | `extra[16]` = `u32 encoded_timestamp`  |
| `flow_entry_meta` | 12 B         | 8 B (timestamp field removed)          |
| flow4 entry       | ~28 B        | ~24 B                                  |

### 3.2 Key invariant

`bk->idx[slot] != RIX_NIL`  implies  `bk->extra[slot]` holds the
encoded expiry timestamp of the entry at `bk->idx[slot]`. Empty
slots may hold stale bits in `extra[]`; consumers must gate on
`idx[slot] != RIX_NIL` before reading `extra[slot]`.

Kickouts (entry migrates to the alternate bucket during insert) must
carry `extra[old_slot]` into `extra[new_slot]`. This is provided by
the rix_hash layer's `_extra_save` / `_extra_restore` mechanism and
is covered by tests merged on 2026-04-18 — the flow4 layer only has
to keep passing the encoded timestamp through the 5-argument insert
entry point.

## 4. Data types

### 4.1 flow_extra_key.h

```c
struct flow_entry_meta_extra {
    u32 cur_hash;   /* hash_field */
    u16 slot;       /* slot_field */
    u16 reserved0;  /* padding, reserved for future */
};
/* sizeof == 8. Compile-time asserted. */

/* flow4_extra_key layout is byte-identical to classic flow4_key. */
struct flow4_extra_key {
    u8  family;
    u8  proto;
    u16 src_port;
    u16 dst_port;
    u16 pad;
    u32 src_addr;
    u32 dst_addr;
};

struct flow4_extra_entry {
    struct flow4_extra_key       key;   /* +0  : 16 B */
    struct flow_entry_meta_extra meta;  /* +16 :  8 B */
    /* caller-defined fields follow */
};
```

### 4.2 Timestamp accessors

Classic `flow_timestamp_*(meta, ...)` inlines are not reusable
because `meta_extra` has no timestamp field. Replacement helpers
take `(bucket_ptr, slot)` explicitly so calls to the wrong variant
fail to compile:

```c
/* in flow_extra_key.h */
static inline u32
flow_extra_ts_get(const struct rix_hash_bucket_extra_s *bk, unsigned slot);

static inline void
flow_extra_ts_set(struct rix_hash_bucket_extra_s *bk, unsigned slot,
                  u32 encoded);

static inline int
flow_extra_ts_is_permanent(u32 encoded); /* encoded == 0 */
```

Encoding (`FLOW_TIMESTAMP_MASK`, `ts_shift`) matches classic exactly
so cross-check fuzz can compare encoded values directly.

## 5. API surface

### 5.1 Naming

- Protocol-independent: `ft_table_extra`, `ft_table_extra_config`,
  `ft_maint_extra_ctx`, `ft_table_extra_stats`.
- flow4-specific: `flow4_extra_key`, `flow4_extra_entry`,
  `flow4_extra_table_*`.
- Dispatch: `ft_table_extra_add_idx`, `ft_table_extra_del_idx`,
  `ft_table_extra_maintain`, `ft_table_extra_maintain_idx_bulk`.
- Arch init: `ft_arch_extra_init(unsigned arch_enable)`. Its internal
  arch dispatch table is independent of classic's `ft_arch_init`, so
  both can be called in one process.

### 5.2 Public headers

```
flowtable/extra/include/
    flow_extra_table.h      /* protocol-independent API */
    flow_extra_common.h     /* ft_table_extra, config, enums */
    flow_extra_key.h        /* meta_extra, ts helpers, flowX_extra_key */
    flow4_extra_table.h     /* flow4-specific API */
```

The classic wrapper header `flow_table.h` is not modified. A consumer
that wants extra includes `<rix/flow_extra_table.h>` explicitly.

### 5.3 Functions

For each classic `flow4_table_*` / `ft_table_*` function there is a
`flow4_extra_table_*` / `ft_table_extra_*` counterpart with identical
signature (modulo types being the `_extra` versions):

```
flow4_extra_table_bucket_size, _init, _destroy
flow4_extra_table_add, _find, _del
flow4_extra_table_add_idx, _del_idx
flow4_extra_table_walk, _flush, _migrate, _nb_entries, _nb_bk
ft_table_extra_add_idx_bulk, _add_idx_bulk_maint, _del_idx_bulk
ft_table_extra_maintain, _maintain_idx_bulk
ft_table_extra_stats, _status
```

### 5.4 Timestamp touch API (structural difference)

Classic lets callers touch `entry->meta.timestamp` directly through
the `flow_timestamp_touch` helper. In extra, the timestamp does not
live on the entry. The replacement is a library-provided function:

```c
void ft_table_extra_touch(struct ft_table_extra *ft, u32 entry_idx,
                          u64 now);
```

It reads `entry->meta.{cur_hash, slot}` to find the bucket, then
writes the encoded timestamp to `bk->extra[slot]`. This costs one
entry cacheline load (same as classic) plus one bucket cacheline
store; it is intentionally symmetric with classic's
`flow_timestamp_touch` from a call-site cost perspective.

### 5.5 Stats

`struct ft_table_extra_stats` keeps all fields of `ft_table_stats`
at the same names and offsets, so diff reporting is trivial. Extra
adds one counter specific to the variant:

```c
u64 maint_extras_loaded;   /* # of buckets whose extra[] was consulted */
```

A debug-only invariant asserts `maint_entry_touches == 0` in
`_maintain` (not `_maintain_idx_bulk`, which still needs one entry
load per input index).

## 6. Internal layout (non-public)

```
flowtable/extra/src/
    flow_core_extra.h              /* template, mirrors flow_core.h */
    flow_table_generate_extra.h    /* public-type/function generator */
    flow_dispatch_extra.h          /* SIMD dispatch helpers */
    flow_hash_extra.h              /* hash fn (identical body) */
    flow4_extra.c                  /* template expansion for flow4 */
    ft_dispatch_extra.c            /* protocol-indep hot path */
    ft_maintain_extra.c            /* maintain / maintain_idx_bulk */
```

Copy-paste from classic (not shared `#ifdef`) on purpose:

- Both variants must link in one binary for AB benchmarking.
- Classic is expected to be deleted once extra is adopted, so shared
  machinery would have a short life.
- Differences are localized to a handful of call sites; diffing the
  two trees is clearer than reading conditional compilation.

Each divergence from classic is flagged by a `/* EXTRA: ... */`
comment citing what was changed and why.

## 7. Maintain path (the core change)

### 7.1 Classic cost profile

`ft_table_maintain` currently runs a 4-deep prefetch pipeline to hide
the per-entry cacheline miss: each bucket visit touches up to 16
entry cachelines just to read the 4-byte timestamp on each one.
In large working sets this is the dominant DRAM-miss source.

### 7.2 Extra algorithm

```c
static unsigned
ft_maint_extra_scan_bucket_(const struct ft_maint_extra_ctx *ctx,
                            unsigned bk_idx, u64 now_ts, u64 timeout_ts,
                            u32 *expired_idxv, unsigned out_pos,
                            unsigned max_expired, unsigned min_bk_entries,
                            unsigned *out_used_count)
{
    struct rix_hash_bucket_extra_s *bk = ctx->buckets + bk_idx;
    u32 used_bits = 0u;
    u32 expired_bits = 0u;

    for (unsigned slot = 0; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++)
        if (bk->hash[slot] != 0u)
            used_bits |= (1u << slot);

    if (out_used_count)
        *out_used_count = (unsigned)__builtin_popcount(used_bits);
    if (min_bk_entries > 1u &&
        (unsigned)__builtin_popcount(used_bits) < min_bk_entries)
        return out_pos;

    u32 u = used_bits;
    while (u != 0u) {
        unsigned slot = (unsigned)__builtin_ctz(u);
        u &= u - 1u;
        u32 ts = bk->extra[slot];
        if (flow_timestamp_is_expired_raw(ts, now_ts, timeout_ts))
            expired_bits |= (1u << slot);
    }

    while (expired_bits != 0u && out_pos < max_expired) {
        unsigned slot = (unsigned)__builtin_ctz(expired_bits);
        unsigned idx = bk->idx[slot];
        expired_bits &= expired_bits - 1u;

        bk->hash[slot]  = 0u;
        bk->idx[slot]   = (u32)RIX_NIL;
        bk->extra[slot] = 0u;
        (*ctx->rhh_nb)--;
        expired_idxv[out_pos++] = (u32)idx;
    }
    return out_pos;
}
```

Entry memory is never touched. `metas[]`, pool pointer arithmetic,
and the staged prefetch vanish.

### 7.3 Prefetch

Next-bucket prefetch uses
`rix_hash_prefetch_bucket_full_of(buckets, next_bk)` which covers
all three cachelines of a 192 B bucket. This helper was merged with
the slot_extra rix_hash work and is already tested.

### 7.4 `ft_maint_extra_ctx`

```c
struct ft_maint_extra_ctx {
    struct rix_hash_bucket_extra_s *buckets;
    unsigned                       *rhh_nb;
    struct ft_table_extra_stats    *stats;
    unsigned                        rhh_mask;
    u8                              ts_shift;
};
```

No `pool_base`, `pool_stride`, `meta_off`, `max_entries`. These were
only needed for the entry-side dereference that extra eliminates.

### 7.5 `maintain_idx_bulk`

The input is a set of entry indices. To identify which bucket each
belongs to, the code still reads `entry->meta.cur_hash` and
`entry->meta.slot` — one entry cacheline per input. The saved cost
is that the subsequent bucket scan does not touch any entry. This is
a partial gain; fully eliminating the entry load would require the
caller to supply `(bucket, slot)` pairs instead of `entry_idx`, which
is a larger API change deferred to a later spec.

### 7.6 SIMD optimization (followup)

Slot occupancy and expiry checks operate on 16-wide u32 arrays in
two adjacent cachelines — a good fit for `_mm512_*_epu32` or
equivalent AVX2/SSE4.2 two-step reductions. The first cut is scalar
to keep the diff small and to validate the algorithmic gain.
Post-merge, a SIMD lane can be added as a separate task following
the same dispatch pattern as classic's `ft_arch_init`.

## 8. Testing

### 8.1 Files

```
flowtable/test/
    test_flow4_extra.c        /* basic functional tests, extra-only */
    test_flow4_parity.c       /* classic vs extra cross-check fuzz */
```

### 8.2 test_flow4_extra.c

Mirrors every flow4 case in the existing `test_flow_table.c` plus
extra-specific checks:

- init / init_ex mapping
- basic add / find / del
- bulk ops, stats correctness
- add_idx_bulk: duplicate ignore, mixed batch, policy, zero-extra skip
- add_idx_bulk_maint: duplicate reclaim, zero-extra skip
- timestamp update writes to `bk->extra[slot]`
- permanent timestamp (`extra == 0`) is skipped by `maintain`
- `maintain_idx_bulk`
- walk / flush / del_idx
- migrate preserves entries and doubles buckets, including
  `extra[]` preservation across the migration
- kickout preserves `extra[]` (forces high fill, then random
  inserts; asserts specific entries' timestamps unchanged)
- compile-time asserts: `sizeof(flow_entry_meta_extra) == 8`,
  `offsetof(flow4_extra_entry, meta) == 16`

### 8.3 test_flow4_parity.c

Drives classic and extra with the same xorshift32-seeded op stream
of add / find / del / touch / maintain / advance_time ops.

Equivalence predicates:

- Per op: return value / resulting entry idx match.
- After every op: the two tables hold the same live `{key -> idx}`
  map (compared via walk → sort → memcmp).
- After touch: classic's `meta.timestamp` equals extra's
  `bk->extra[slot]` bit-for-bit.
- After maintain: the set of evicted indices matches as sets
  (sorted equality).

Parameters (shared with `test_rix_hash_extra.c`'s existing fuzz):

- seed 0xC0FFEE
- POOL = 512
- NB_BK = 64 (high collision, forces kickouts)
- OPS = 20_000
- periodic validation every 128 ops

### 8.4 Build / platform matrix

- gcc and clang.
- `SIMD={gen, sse, avx2, avx512}` — avx512 optional per host.
- `-Wall -Wextra -Wvla -Werror`.
- Hugepage-backed allocations only (user-durable preference).

## 9. Benchmarks

### 9.1 Files

```
flowtable/test/
    bench_flow4_vs_extra.c        /* full AB: insert/find/touch/maint/remove */
    bench_flow4_maint_sweep.c     /* maintain-focused fill/expire sweep */
```

### 9.2 Full AB bench

Mirrors `tests/hashtbl_extra/bench_vs_classic.c`:

- `N_ENTRIES = 1u << 16`, `N_BK = 1u << 12`, `REPS = 8`.
- Hugepage `mmap` + `MADV_HUGEPAGE` for every buffer.
- Asymmetric TSC fences (`lfence;rdtsc` / `rdtscp;lfence`).
- `rix_hash_arch_init(RIX_HASH_ARCH_AUTO)` + `ft_arch_init(FT_ARCH_AUTO)`
  + `ft_arch_extra_init(FT_ARCH_AUTO)`.

Operations measured: insert, find_hit, find_miss, touch, maintain
(at expire_ratio 0% / 50% / 100%), maintain_idx_bulk, remove. Output
is a three-column table (classic, extra, delta) printed in
cycles/op.

Expire ratios are induced by direct-writing timestamps into either
classic `entry->meta.timestamp` or extra `bk->extra[slot]` after the
fill phase; this is a benchmark-only privilege, not a supported API.

### 9.3 Maintain sweep bench

Sweeps three axes and prints a matrix per axis cross-section:

- `N_ENTRIES ∈ {16K, 64K, 256K, 1M, 4M}`
- `fill ∈ {25%, 50%, 75%, 90%}`
- `expire ∈ {10%, 50%, 90%}`

Memory budget at the top end: 4M entries × ~24 B + 8M buckets × 192 B
≈ 2 GB, comfortably within hugepage-backed allocation on the target
host.

### 9.4 perf integration

No `perf_event_open` inside the binary. External measurement is via
`perf stat -e L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses ./bench_flow4_vs_extra`.
The bench exposes `--detail` to print min/median/max per op if
fine-grained timing is needed, and `--seed=N --reps=M` for
reproducibility sweeps.

### 9.5 Makefile targets

Added to `flowtable/test/Makefile`:

- `bench-extra` → `bench_flow4_vs_extra`
- `bench-sweep` → `bench_flow4_maint_sweep`
- `bench-all` → existing `bench` + `bench-extra`

Top-level `make bench` runs classic + extra full AB. `make
bench-full` adds the sweep.

## 10. Build integration

- `flowtable/extra/src/*.c` compiled into the same
  `libflowtable.a`-equivalent as classic.
- SIMD arch dispatch uses the same `FT_ARCH_SUFFIX` pattern.
- `make install` is unchanged: extra remains an experimental
  subtree, not part of the installed surface until classic is
  retired.
- `flowtable/extra/README.md` documents that extra is an
  evaluation-phase parallel API.

## 11. Success criteria

### 11.1 Must-have (merge blockers)

1. `test_flow4_parity.c` cross-check fuzz passes at 20K ops across
   multiple seeds with full state equivalence.
2. Classic tests stay green without modification.
3. gcc + clang × `{gen, sse, avx2}` all pass; avx512 verified if
   host permits, otherwise reported as "not run on this host".
4. `-Werror` clean under existing warning policy.
5. `make clean && make test` and `make install` both succeed.

### 11.2 Performance targets (merge-gating judgment)

Primary (hugepage, `N=65536`, `BK=4096`, `fill=75%`):

- extra `maintain` full sweep ≥ **1.5×** faster than classic in
  cycles/bucket (≥ 33% reduction).
- extra `maintain_idx_bulk` ≥ **1.2×** faster than classic in
  cycles/op.

Secondary:

- extra `maint_bucket_checks` cy/bk stable across fill and
  expire_ratio, within ±10%.
- `insert` / `remove` regressions stay within the slot-level
  envelope already measured (+3..+15 cy/op on AVX2; within 2×
  classic in the worst case).
- `find_hit` / `find_miss` within ±5 cy/op noise vs classic.
- `touch` within ±5 cy/op of classic.

### 11.3 Miss handling

- Functional targets missed: merge blocked; return to spec.
- Performance targets missed but functional targets met:
  - if `maintain` shows no gain at all, merge blocked; revisit
    design.
  - otherwise merge and carry follow-up optimization tasks in a
    new plan (most likely the SIMD lane in 7.6).
- Achieved targets recorded in `docs/superpowers/validation/` in
  the same format as the 2026-04-18 slot_extra v0.3.0 validation.

### 11.4 Follow-up gate

Horizontal rollout (flow6 / flowu extra variants) starts only after
all must-haves and the primary `maintain` target are met, and after
`bench_flow4_maint_sweep` shows no regression at the small-scale
(~10K entries) end of the sweep.
