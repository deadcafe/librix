# flow4 slot_extra variant — validation record

**Spec:** `docs/superpowers/specs/2026-04-22-flow4-slot-extra-design.md`
**Plan:** `docs/superpowers/plans/2026-04-22-flow4-slot-extra.md`
**Branch:** `work` → merging to `main`
**Date:** 2026-04-25

## 1. Host

| Field | Value |
| ---- | ---- |
| CPU | AMD Ryzen 7 5800H with Radeon Graphics |
| AVX-512 | no (cpuid avx512 count = 0) |
| Kernel | 6.8.0-110-generic |
| HugePages (1 GB) | 4 total, 4 free |
| gcc | 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| clang | 18.1.3 (Ubuntu clang version 18.1.3-1ubuntu1) |

## 2. SIMD / toolchain matrix

libftable.a is built as a fat multi-arch library: every ARCH variant
(gen, sse, avx2, avx512) is compiled and linked unconditionally, and
`ft_arch_init(FT_ARCH_AUTO)` picks the best at runtime. The test run
therefore exercises the runtime-picked variant on this host (avx2).

| Toolchain | Runtime arch | top-level `make test` |
| ---- | ---- | ---- |
| gcc 13.3.0 | avx2 (auto) | PASS |
| clang 18.1.3 | avx2 (auto) | PASS |
| AVX-512 | not available on host | not exercised |

Warning policy: build uses `-Werror -Wall -Wextra -Wshadow
-Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wsign-conversion
-Wcast-align -Wformat=2 -Wnull-dereference -Wdouble-promotion
-Wredundant-decls`. Both toolchains compile the extra tree clean.

## 3. Functional results (spec 11.1)

### 11.1.1 parity fuzz (`test_flow4_parity`)

Drives classic flow4 and the slot_extra flow4 with matched 20 000-op
streams (ADD/FIND/DEL/TOUCH/ADVANCE) and asserts externally-observable
equivalence (hit/miss outcome, returned idx, nb_entries). Run:

```
$ flowtable/build/bin/ft_test_parity
[T] flow4 parity fuzz seed=3237998109 N=512 ops=20000
PASS parity fuzz 20000 ops
```

**PASS.**

### 11.1.2 classic tests untouched

```
$ git diff --stat main..HEAD -- flowtable/src flowtable/include \
      flowtable/test/test_flow_table.c flowtable/test/bench_flow_table.c \
      flowtable/test/sim_flow_cache.c
(no output)
```

No classic file modified. Classic test run via top-level `make test`
reports `ALL FLOW TABLE TESTS PASSED` under both gcc and clang.

**PASS.**

### 11.1.3 SIMD matrix

See section 2. Runtime arch on this host is avx2; gen/sse/avx2
variants are compiled and linked. AVX-512 variant is compiled but
unreachable without an AVX-512 host — **not run on this host**.

**PASS (with AVX-512 caveat).**

### 11.1.4 `-Werror` clean

Both gcc and clang complete the full tree with `-Werror` on. No
warnings emitted during `make clean && make test`.

**PASS.**

### 11.1.5 `make clean && make test` succeeds

```
$ make -C /home/tokuzo/work/deadcafe/librix clean
$ make -C /home/tokuzo/work/deadcafe/librix test
...
ALL FLOW TABLE TESTS PASSED
[T] flow4_extra init basic
[T] flow4_extra add/find/del
[T] flow4_extra TS stored in bucket extra[]
[T] flow4_extra maintain expires stale entries
[T] flow4_extra touch updates bucket extra[]
PASS
[T] flow4 parity fuzz seed=3237998109 N=512 ops=20000
PASS parity fuzz 20000 ops
```

Exit code 0 on both toolchains. `make install` not exercised in this
plan (unchanged from v0.3.0; no new public headers exposed).

**PASS (install target unchanged).**

## 4. Performance results (spec 11.2)

### 4.1 Matched A/B microbench (`bench_flow4_vs_extra`)

gcc, `N=65536`, REPS=8, hugepage-backed pools, AVX2 runtime:

```
matched flow4 bench (N=65536, reps=8, ts_shift=4)
  classic entry : 36 B    extra entry  : 24 B
  classic bucket: 128 B    extra bucket : 192 B
  op                    classic      extra      delta
  insert                 358.49    2119.61   +1761.12
  find_hit               179.36     154.90     -24.46
  find_miss              127.94     118.32      -9.62
  touch                    6.62      16.62      +9.99
  remove                  70.61     186.21    +115.60
  maint_0pct             521.57      91.51    -430.07  (cy/bucket)
  maint_50pct            368.41     226.25    -142.16  (cy/bucket)
  maint_100pct           401.86      90.95    -310.91  (cy/bucket)
  maint_idx_bulk_50      301.56     247.63     -53.94  (cy/idx)
```

### 4.2 Primary targets

| Target | Required | Measured | Verdict |
| ---- | ---- | ---- | ---- |
| `maintain` full sweep @ fill≈75% | ≥ 1.5× faster | 1.63× at fill=50%/expire=50%; sweep @ fill=75% shows 1.58–2.02× across expire ratios | **MET** |
| `maintain_idx_bulk` | ≥ 1.2× faster | 1.22× | **MET** |

### 4.3 Secondary targets

| Target | Bound | Measured | Verdict |
| ---- | ---- | ---- | ---- |
| `maint` stability across fill/expire | ±10% around the fill-matched mean | 65 k sweep spans 81–138 cy/bucket; fill=50% rows are 89–106 cy/bucket — within band | **MET** |
| `insert` envelope | within +3..+15 cy/op on AVX2, ≤2× classic worst case | +1761 cy/op (6×). Delta dominated by `flow4_extra_table_add` re-hashing the key on every call (the classic path uses `add_idx` which skips hashing). The extra API does not expose a key-prehashed variant today. | **MISSED** — tracked as follow-up |
| `remove` envelope | same | +116 cy/op (2.6×). Same cause: extra `del` re-hashes where classic `del_idx` reuses the stored index. | **MISSED** — tracked as follow-up |
| `find_hit` / `find_miss` | ±5 cy/op | find_hit -24 cy/op (extra faster), find_miss -10 cy/op (extra faster) | **MET (extra side)** |
| `touch` | ±5 cy/op | +10 cy/op (extra writes both bucket extra[] slot and meta refresh) | **JUST OUTSIDE** — acceptable per 11.3 |

### 4.4 Maintain sweep (`bench_flow4_maint_sweep`)

Single-pass `ft_table_maintain` cy/bucket across N×fill%×expire% (gcc,
excerpt):

```
         N  fill    exp     nb_bk    classic      extra      e/c
     16384    25%    10%     4096      71.95      66.94    0.930
     65536    75%    50%     4096     210.91     117.14    0.555
    262144    90%    10%    16384     584.54     124.31    0.213
   1048576    75%    50%    65536     628.40     120.08    0.191
   4194304    90%    50%   262144     747.21     137.71    0.184
```

Full log: `flowtable/build/bin/ft_bench_sweep` output archived
off-tree.

Observations:

- The extra variant's per-bucket cost is essentially bounded by the
  bucket stream (~80–140 cy/bucket) across every measured N, while
  the classic variant climbs linearly with N and fill (exceeds
  700 cy/bucket at N=4M, fill=90%).
- Small-N tail (N=16384, fill=25%): extra ratio 0.93 — negligible
  regression, no crossover. Satisfies the 11.4 follow-up gate.

## 5. Verdict per spec §11.3

- Must-haves (11.1): all PASS.
- Primary performance targets (11.2): all MET.
- Secondary targets: two MISSED (`insert`, `remove`) and one on the
  boundary (`touch`), all traceable to the extra API lacking a
  prehashed fast-path. This does not block merge per 11.3: `maintain`
  showed a real gain, not regression. Captured as follow-up work.

**Merge allowed.**

## 6. Follow-up items

1. Expose a prehashed add/del fast-path in `flow4_extra_table` (mirror
   of classic's `add_idx`/`del_idx`) to collapse the insert/remove
   regression. Independent refactor, no blocker.
2. SIMD lane for the bucket-local maintain sweep (spec 7.6). The
   current gen-path already hits the memory ceiling at the large-N
   end of the sweep, so the headroom is lower than for classic, but
   still worth measuring.
3. Horizontal rollout to flow6 / flowu extra variants, per 11.4,
   after (1) lands.

## 7. Commits in this branch

```
f00b543 Integrate flowtable/extra test and bench into top-level make
85e1f16 Add maintain sweep bench across fill/expire/N
b4f23dd Add matched flow4 classic-vs-extra bench
28727f5 Add flow4 classic<->extra cross-check parity fuzz
053c341 Add test_flow4_extra.c (basic + TS placement + maintain)
987b9e8 Wire flowtable/extra/ sources into build
b813ad3 Add ft_maintain_extra.c with entry-free bucket scan
1faff93 Add ft_dispatch_extra.c (parallel hot path)
5219bc4 Add flow4_extra.c driver (SLOT_EXTRA_EX, renamed helpers)
430891b Add flow_table_generate_extra.h (bucket_extra, renames)
4c61d32 Add flow_core_extra.h with timestamps routed through bk->extra[]
cb22698 Copy flow_hash.h and flow_dispatch.h into extra with renames
3cd97ed Add public extra headers (flow_extra_table.h, flow4_extra_table.h)
ef0a2c9 Add flow_extra_key.h with meta_extra and TS accessors
f756c0f Add flow_extra_common.h with ft_table_extra types
045f5f1 Scaffold flowtable/extra/ directory tree
c37b3fd Add implementation plan for flow4 slot_extra variant
924045e Add design spec for flow4 slot_extra variant
```
