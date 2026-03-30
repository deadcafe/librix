# Changelog

All notable changes to this project will be documented in this file.

The format is inspired by Keep a Changelog.

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
