# librix v0.4.0

## Suggested GitHub repository description

Index-based shared-memory data structures in C, with worker-local IPv4/IPv6
flow-table and slot-extra flow-cache variants.

## Suggested GitHub topics

`c`, `shared-memory`, `mmap`, `data-structures`, `hash-table`, `cuckoo-hash`,
`simd`, `avx2`, `networking`, `flow-table`, `flow-cache`, `vpp`

## Release summary

`v0.4.0` integrates the slot-extra flow-table implementation into the normal
`flowtable/` build and extends it from `flow4_extra` to `flow6_extra` and
`flowu_extra`. The extra variant keeps the single-owner, worker-local model:
it is not a general multi-threaded hash table, and deliberately focuses on
flow-cache datapath performance, explicit reclaim, and caller-owned record
storage.

The slot-extra variant stores expiry timestamps in bucket-side `extra[]`
slots, allowing maintenance sweeps to decide expiry from bucket memory before
touching entry records. This release also adds generic `FT_TABLE_*` facade
macros for operations shared by classic and extra tables, including
maintenance helpers, while keeping key-specific APIs family-specific where the
types genuinely differ.

## Highlights

- `flow6_extra` and `flowu_extra` APIs, key/entry layouts, dispatch wrappers,
  tests, and benchmark coverage
- `flowtable/extra/` merged into the normal public/private flowtable layout
- `libftable.a` now builds classic and slot-extra objects together
- `gen`, `sse`, `avx2`, and `avx512` object builds for all extra families
- `FT_TABLE_*` generic facade over classic and slot-extra common operations
- `bench-extra-full` full-family slot-extra benchmark sweep
- documentation updated for classic/extra API boundaries and worker-local
  flow-cache usage

## Validation status

- `git diff --check`: passed
- `make -C flowtable/test test-extra-arch CC=gcc`: passed
- `make -C flowtable/test test-extra CC=clang BUILDDIR=/tmp/librix-flowtable-clang-extra-all LIBFT=/tmp/librix-flowtable-clang-extra-all/lib/libftable.a`: passed
- `ft_bench_extra_full` GCC and Clang builds: passed
- small `bench-extra-full` run over `flow4_extra`, `flow6_extra`, and
  `flowu_extra`: passed

## Notes

- The intended deployment model is a VPP-style worker-local flow cache.
  Multi-threaded shared-table semantics are intentionally outside this model.
- AVX-512 support is built, but execution was not validated on the local
  machine because the CPU does not advertise AVX512F.
- `bench-extra` remains the focused `flow4` classic-vs-extra comparison;
  `bench-extra-full` is the broad slot-extra family sweep.
