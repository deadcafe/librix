# Release Notes

## librix v0.5.1

Patch release focused on `flowtable` implementation brush-up while preserving
the performance-oriented hot path.

### Summary

`v0.5.1` removes duplicate cold-path helper code in the pure and slot-extra
dispatch layers, tightens the slot-extra bulk lookup wrapper path, and updates
the public documentation to match the current allocation and `find_bulk`
contracts.

The most performance-relevant change is in slot-extra `find_bulk`: generated
lookup code now writes `u32` result indices directly, so public wrappers no
longer need a temporary `struct ft_table_result[64]` buffer plus copy loop.

### Highlights

- write slot-extra bulk lookup results directly as `u32` indices
- share `flow4_extra`, `flow6_extra`, and `flowu_extra` wrapper generation
- share pure/extra arch-enable and maintain selector code
- share pure/extra bucket-size calculation logic
- reuse one slot-extra bucket-carve helper in init and migrate paths
- update English and Japanese allocation-model documentation
- correct slot-extra `find_bulk` return-value docs
- correct the small `ft_bench_extra` add+inline-maint phase-1-only setup so it
  no longer measures an unintended 100% fill case
- fix `ft_bench_extra` delta formatting when extra is faster than pure

### Validation status

- `git diff --check`: passed
- `make -C flowtable static`: passed
- `make -C flowtable/test test-extra-arch`: passed
- `make -C flowtable/test test-parity`: passed
- `make -C flowtable/test bench-extra`: passed

### Notes

- AVX-512 objects build as part of `make -C flowtable static`; AVX-512
  execution was skipped on the local machine because the CPU does not
  advertise AVX512F.

## librix v0.5.0

Release focused on public API brush-up for `flowtable`.

### Summary

`v0.5.0` simplifies the programmer-facing flowtable header surface.  The
primary include remains `flow_table.h`, optional fill-rate control remains
`ft_fill_ctrl.h`, and family/common support headers move under the
`flowtable/` include namespace.  The goal is to make the normal user path
read as "include one header, then pick pure or extra table types" while
keeping advanced family headers available through explicit names such as
`flowtable/flow4_table.h`.

Release notes are also consolidated into this single file.  New release
entries should be added at the top.

### Highlights

- keep `flow_table.h` as the primary programmer-facing header
- keep `ft_fill_ctrl.h` as the optional controller header
- move support headers under `flowtable/`
- add API usage comments to common, family-specific, and generic facade
  headers
- add consistent coding-style footers to flowtable C/H files
- add a readable API usage sample under `flowtable/test/usage_flowtable.c`
- lock down public flowtable key/entry sizes, alignment, and metadata
  offsets with static assertions
- lock down public `include/rix/` hash bucket, staged-find context,
  dispatch, and ring layouts with static assertions
- update docs to describe the simplified include model
- consolidate release notes into `RELEASE_NOTES.md`

## librix v0.4.0

### Summary

`v0.4.0` integrates the slot-extra flow-table implementation into the normal
`flowtable/` build and extends it from `flow4_extra` to `flow6_extra` and
`flowu_extra`. The extra variant keeps the single-owner, worker-local model:
it is not a general multi-threaded hash table, and deliberately focuses on
flow-cache datapath performance, explicit reclaim, and caller-owned record
storage.

The slot-extra variant stores expiry timestamps in bucket-side `extra[]`
slots, allowing maintenance sweeps to decide expiry from bucket memory before
touching entry records. This release also adds generic `FT_TABLE_*` facade
macros for operations shared by pure and extra tables, including
maintenance helpers, while keeping key-specific APIs family-specific where the
types genuinely differ.

### Highlights

- `flow6_extra` and `flowu_extra` APIs, key/entry layouts, dispatch wrappers,
  tests, and benchmark coverage
- `flowtable/extra/` merged into the normal public/private flowtable layout
- `libftable.a` now builds pure and slot-extra objects together
- `gen`, `sse`, `avx2`, and `avx512` object builds for all extra families
- `FT_TABLE_*` generic facade over pure and slot-extra common operations
- `bench-extra-full` full-family slot-extra benchmark sweep
- documentation updated for pure/extra API boundaries and worker-local
  flow-cache usage

### Validation status

- `git diff --check`: passed
- `make -C flowtable/test test-extra-arch CC=gcc`: passed
- `make -C flowtable/test test-extra CC=clang BUILDDIR=/tmp/librix-flowtable-clang-extra-all LIBFT=/tmp/librix-flowtable-clang-extra-all/lib/libftable.a`: passed
- `ft_bench_extra_full` GCC and Clang builds: passed
- small `bench-extra-full` run over `flow4_extra`, `flow6_extra`, and
  `flowu_extra`: passed

### Notes

- The intended deployment model is a VPP-style worker-local flow cache.
  Multi-threaded shared-table semantics are intentionally outside this model.
- AVX-512 support is built, but execution was not validated on the local
  machine because the CPU does not advertise AVX512F.
- `bench-extra` remains the focused `flow4` pure-vs-extra comparison;
  `bench-extra-full` is the broad slot-extra family sweep.

## librix v0.3.0

### Summary

`v0.3.0` adds a reusable `rix_ring` container for `u32` index FIFO/LIFO use
and moves the flowtable support allocator paths onto that ring-backed model.
The release also exposes `add_idx_bulk_maint` for combined add-plus-maintain
work, which lets the flow-cache simulator exercise hot-bucket expiration while
new flows are being inserted.

The simulator side is the other major change in this release. Generator and
receiver responsibilities are now separated more cleanly, attack traffic is
carried explicitly as batch metadata instead of by positional convention, and
the timeout controller now distinguishes normal target tracking from
high-fill emergency reclaim. In tested hugepage scenarios at `1M entry /
1M pps / batch=256`, normal load settles near the configured target while
burst load is allowed to rise toward the aggressive band and is then pulled
back without crossing the `95%` guardrail.

### Highlights

- new public `rix_ring` API for reusable index FIFO/LIFO handling
- test/bench/sim flow allocators now use a ring-backed LIFO instead of
  intrusive free-list links
- `flow4/6/u add_idx_bulk_maint` combines add and maintenance work in one path
- flow-cache simulator separates generator and receiver concerns more clearly
- burst controller now applies aggressive reclaim in `85%/90%/95%` fill bands

### Validation status

- `make test -C flowtable/test`: passed
- `make -C flowtable/test bench`: passed
- `make -C flowtable/test /home/tokuzo/work/deadcafe/librix/flowtable/build/bin/ft_sim_cache`: passed
- Hugepage `ft_sim_cache` scenarios validated at `1M entry / 1M pps / batch=256`

### Notes

- The simulator controller is tuned to stay just below target in normal load,
  then spend headroom more gradually under burst before entering aggressive
  reclaim at high fill.
- AVX-512 support remains workload- and CPU-dependent rather than universally
  faster than AVX2.
- Most datapath tuning still focuses on `flow4`. `flow6` and `flowu` remain
  functionally covered, but have seen less tuning.

## librix v0.2.0

Public release focused on consolidating the repository around `flowtable/`
and tightening the public/private boundary of the flow-table codebase.

## librix v0.1.1

Public maintenance release focused on portability, validation, and benchmark
usability.

## librix v0.1.0

Initial public release.
