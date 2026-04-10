# librix v0.2.0

## Suggested GitHub repository description

Index-based shared-memory data structures in C, with SIMD-friendly cuckoo hash
tables and an IPv4/IPv6 flow-table library.

## Suggested GitHub topics

`c`, `shared-memory`, `mmap`, `data-structures`, `hash-table`, `cuckoo-hash`,
`simd`, `avx2`, `networking`, `flow-table`

## Release summary

`v0.2.0` consolidates the repository around `flowtable/` and cleans up the
boundary between public API headers and private implementation headers.

The old `samples/` layout and the `fcache` split are gone. `flowtable/` is now
the single higher-level library in the tree, with a flatter public include
layout, private generator/dispatch/hash headers under `src/`, and bench-only
helpers under `test/`. The release also simplifies the flow-table generator
layer, reduces leftover dead hooks and wrapper macros, and updates release-
facing documentation so it matches the current tree.

On the datapath side, the most visible behavioral tuning in this release is
for small-query add batches: `q=2/3 add_idx` now uses the dedicated small-query
path instead of the previous bulk-style path, which materially improves the
small-batch regime without changing the public API.

## Highlights

- `flowtable/` replaces the old multi-sample layout as the primary sample
  library
- public headers are now clearly separated from private implementation headers
- `flow4/6/u` hash/cmp helpers are private, not part of the public header set
- generated flow-table code is simpler and carries fewer dead hooks/macros
- small-query add behavior is improved for `q=2/3`
- benchmark targets are simplified to `bench-light` and `bench-full`

## Validation status

- `make -C flowtable/test test`: passed
- Representative flowtable benchmark reruns: completed during tuning and
  release preparation
- Generic scalar path: tested
- AVX2 path: tested
- AVX-512 path: tested on AVX-512-capable hardware

## Notes

- AVX-512 support is validated, but the best dispatch tier is still
  workload- and CPU-dependent. `auto` is not always slower or faster than a
  forced `avx512` choice.
- Most performance tuning still targets `flow4`. `flow6` and `flowu` are
  functionally covered, but they have seen less datapath tuning than `flow4`.
- Small-query cold datapath behavior improved in this release, but the
  `q=1` regime is still dominated by exposed memory latency rather than by
  bulk pipeline hiding.
