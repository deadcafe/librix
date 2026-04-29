# Changelog

All notable changes to this project will be documented in this file.

The format is inspired by Keep a Changelog.

## [0.5.0] - 2026-04-29

Patch release focused on public header brush-up and release document cleanup.

### Changed

- Flowtable support headers now live under `flowtable/include/flowtable/`.
  Normal users include `flow_table.h`; family-specific headers remain
  available as `flowtable/*.h` for narrow advanced use.
- Public flowtable headers now document the normal include path, generic
  facade exceptions, and per-API usage for common and family-specific entry
  points.
- Flowtable C/H files now consistently carry the BSD 4-space soft-tab
  coding-style footer.
- Added `flowtable/test/usage_flowtable.c`, a readable API usage sample that
  is built by `make -C flowtable/test sample` and is intentionally separate
  from test/benchmark targets.
- Public flowtable key/entry layouts now have static size, alignment, and
  metadata-offset assertions for pure and slot-extra variants.
- Public `include/rix/` hash bucket, staged-find context, dispatch, and ring
  structs now have explicit cache-line/layout assertions and consistent
  coding-style footers where they were missing.
- Release notes are consolidated into `RELEASE_NOTES.md`, with new release
  entries added at the head of the file.

### Validation status

- `git diff --check`: passed
- `make -C flowtable static CC=gcc`: passed
- `make -C flowtable/test test-extra-arch CC=gcc`: passed
- `make -C flowtable/test test-extra CC=clang BUILDDIR=/tmp/librix-flowtable-clang-v050-extra LIBFT=/tmp/librix-flowtable-clang-v050-extra/lib/libftable.a`: passed
- `make -C flowtable/test test-extra CC=clang BUILDDIR=/tmp/librix-flowtable-clang-v050-api-doc LIBFT=/tmp/librix-flowtable-clang-v050-api-doc/lib/libftable.a`: passed
- `make -C flowtable/test sample CC=gcc`: passed
- `make -C flowtable/test sample CC=clang BUILDDIR=/tmp/librix-flowtable-clang-sample LIBFT=/tmp/librix-flowtable-clang-sample/lib/libftable.a`: passed
- `ft_usage_sample` GCC and Clang binaries: passed
- `make -C flowtable/test test-extra CC=gcc`: passed
- `make -C flowtable/test test-extra CC=clang BUILDDIR=/tmp/librix-flowtable-clang-align-extra LIBFT=/tmp/librix-flowtable-clang-align-extra/lib/libftable.a`: passed
- `make build CC=gcc`: passed
- `make -B build CC=clang`: passed
- rix unit tests under `tests/slist`, `tests/list`, `tests/stailq`,
  `tests/tailq`, `tests/circleq`, `tests/rbtree`, `tests/hashtbl`,
  `tests/hashtbl_extra`, `tests/hashtbl32`, and `tests/hashtbl64`: passed
- `make -C flowtable/test test CC=gcc`: stopped after `ft_test` produced no
  output for more than 90 seconds

## [0.4.0] - 2026-04-28

Minor release focused on integrating the slot-extra flow-table variant into
the normal flowtable build and extending it beyond IPv4.

### Added

- `flow6_extra` and `flowu_extra` public APIs, entry/key layouts, dispatch
  wrappers, tests, and benchmark coverage.
- `FT_TABLE_*` generic facade macros over pure `struct ft_table` and
  slot-extra `struct ft_table_extra` for shared operations, including
  maintenance facade helpers.
- `bench-extra-full`, a full slot-extra benchmark sweep for
  `flow4_extra`, `flow6_extra`, and `flowu_extra` across datapath,
  maintenance, grow, fill levels, query sizes, and supported arch variants.

### Changed

- The former `flowtable/extra/` subtree is merged into the normal
  `flowtable/include/` and `flowtable/src/` layout; `libftable.a` now carries
  pure and slot-extra implementations together.
- Extra arch builds are generated for `gen`, `sse`, `avx2`, and `avx512` in
  the same cross-product style as pure `flow4/flow6/flowu`.
- `make bench` keeps the representative best-supported-arch run, while
  `bench-full` runs pure and slot-extra full sweeps over CPU-supported
  arch variants.

### Fixed

- Slot-extra touch and maintenance helpers now resolve metadata offsets per
  variant instead of assuming the `flow4_extra` entry layout.
- Documentation now describes the pure/extra API boundary, the generic
  facade exceptions, and the intended single-owner worker-local flow-cache
  model.

### Validation status

- `git diff --check`: passed
- `make -C flowtable/test test-extra-arch CC=gcc`: passed
- `make -C flowtable/test test-extra CC=clang BUILDDIR=/tmp/librix-flowtable-clang-extra-all LIBFT=/tmp/librix-flowtable-clang-extra-all/lib/libftable.a`: passed
- `ft_bench_extra_full` GCC and Clang builds: passed
- Small `bench-extra-full` run over `flow4_extra`, `flow6_extra`, and
  `flowu_extra`: passed
- AVX-512 objects build on this machine; AVX-512 execution was skipped because
  the local CPU does not advertise AVX512F.

## [0.3.0] - 2026-04-18

Minor release focused on reusable index-ring primitives, flow-entry allocator
cleanup, and more realistic flow-cache simulation under burst load.

### Added

- New `rix_ring` public API for `u32` index FIFO/LIFO operation with burst
  enqueue/dequeue and push/pop helpers.
- New release notes for `v0.3.0`.
- `add_idx_bulk_maint` APIs for `flow4`, `flow6`, and `flowu` so add and
  maintenance work can be combined in one call.

### Changed

- Test, bench, and simulator flow-entry allocators now use a ring-backed LIFO
  instead of intrusive free-list links embedded in records.
- The flow-cache simulator now separates generator and receiver roles more
  clearly, with explicit batch metadata for traffic classification.
- Timeout control in the simulator now uses a near-target setpoint in normal
  operation and batch-level reclaim bands at `85%`, `90%`, and `95%` fill.

### Fixed

- The DDoS simulator no longer pins steady-state fill near `67%` by applying
  the DoS timeout outside the aggressive reclaim band.
- Burst handling now reclaims per batch before add work when fill enters the
  aggressive band, keeping tested burst scenarios below the `95%` guardrail.
- Flow-entry records used by tests and simulator support paths no longer carry
  allocator-only link/live-marker fields.

### Validation status

- `make test -C flowtable/test`: passed
- `make -C flowtable/test bench`: passed
- `make -C flowtable/test /home/tokuzo/work/deadcafe/librix/flowtable/build/bin/ft_sim_cache`: passed
- Hugepage `ft_sim_cache` scenarios validated at `1M entry / 1M pps / batch=256`

## [0.2.0] - 2026-04-10

Public release focused on consolidating the repository around `flowtable/`
and tightening the public/private boundary of the flow-table codebase.

### Added

- New release notes for `v0.2.0`.
- Root-level release metadata now reflects `flowtable/` as the primary sample
  library rather than the removed `fcache` tree.

### Changed

- The old `samples/` layout has been replaced by a dedicated `flowtable/`
  subtree with flat public headers under `flowtable/include/`.
- Build outputs for `flowtable/` now live under `flowtable/build/{lib,obj,bin}`.
- Public and private headers are now separated cleanly:
  - public API headers remain under `flowtable/include/`
  - private generator/dispatch/hash headers live under `flowtable/src/`
  - bench-only helpers live under `flowtable/test/`
- `q=2/3` `add_idx` now uses the small-query add path instead of the bulk
  `step=2/ahead=2` path, improving small-query behavior.
- The `flowtable/test` benchmark targets are simplified to `bench-light` and
  `bench-full`, with clearer top-level forwarding targets.
- Generator-side hooks and wrapper macros were simplified so the current
  implementation matches the actual flow-table behavior more directly.

### Fixed

- Older GCC builds that warned about possible null dereferences in generated
  `flow4/6/u add_idx_bulk` paths now build cleanly by using internal non-null
  record helpers rather than weakening warning policy.
- Public-facing documents no longer refer to removed `samples/fcache` paths as
  the current sample layout.

### Validation status

- `make -C flowtable/test test`: passed
- Representative flowtable benchmark reruns: completed during tuning and
  release preparation
- Generic scalar path: tested
- AVX2 path: tested
- AVX-512 path: tested on AVX-512-capable hardware; performance remains
  workload- and CPU-dependent

## [0.1.1] - 2026-03-26

Public maintenance release focused on portability, validation, and benchmark
usability.

### Added

- `make test` now includes a fast `-DNDEBUG` flow-cache build check so
  release-only compiler diagnostics are caught without running the full
  benchmark suite.
- CI now exercises both `SIMD=avx2` and `SIMD=gen` test builds.
- `fc_bench archcmp ...` now prints an explicit `auto` vs `avx2` winner
  summary for `datapath` and `findadd_*` modes.
- New release notes for `v0.1.1`.

### Changed

- Flow-cache generated code now carries compiler-visible non-null invariants
  in release builds, reducing `-Wnull-dereference` false positives on older
  GCC versions without disabling warnings.
- `add_bulk()` now prefetches both candidate buckets up front, matching its
  duplicate-check behavior.
- Release-facing README references now point at the current release notes.

### Fixed

- GCC `-Wnull-dereference` false positives in `samples/fcache` and generated
  hash helpers, including remote GCC 11.5 build failures.
- A regression where the earlier non-null helper change broke the nullable
  `rbtree` NIL-to-pointer path.

### Validation status

- Generic scalar path: tested, including CI coverage
- AVX2 path: tested
- AVX-512 path: tested on AMD Zen 4 and Intel Cascade Lake hardware
- Remote GCC 11.5 build/test: confirmed

## [0.1.0] - 2026-03-22

Initial public release.

### Added

- `librix` index-based queue, tree, and cuckoo-hash implementations for
  shared-memory and mmap-friendly layouts.
- `RIX_HASH`, `RIX_HASH32`, and `RIX_HASH64` variants, including slot-aware
  remove support.
- `samples/fcache` flow-cache implementations for:
  - `flow4`
  - `flow6`
  - `flowu`
- Bulk flow-cache APIs:
  - `find_bulk`
  - `findadd_bulk`
  - `add_bulk`
  - `del_bulk`
  - `del_idx_bulk`
- Maintenance and timeout-control APIs for the sample flow cache.
- Size-query based initialization:
  - `fc_*_cache_size_query()`
  - `fc_cache_size_bind()`
  - `fc_*_cache_init_attr()`
- Fixed in-flight scratch support for bulk APIs.
- Test and benchmark runners under `samples/fcache/test/`.

### Changed

- Bulk APIs no longer rely on VLA-sized `ctx[nb_keys]`; they now use fixed
  in-flight context rings.
- Maintenance-side VLA usage has been removed in favor of fixed-size bounded
  buffers.
- Bucket and entry prefetch paths were normalized through helper functions.

### Validation status

- Generic scalar path: tested
- AVX2 path: tested
- AVX-512 path: implemented, but not yet validated on AVX-512 hardware
