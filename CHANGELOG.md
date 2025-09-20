# Changelog

All notable changes to this project will be documented in this file.

The format is inspired by Keep a Changelog.

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
- Test and benchmark runners under `samples/test/`.

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
