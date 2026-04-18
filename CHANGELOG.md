# Changelog

All notable changes to this project will be documented in this file.

The format is inspired by Keep a Changelog.

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
