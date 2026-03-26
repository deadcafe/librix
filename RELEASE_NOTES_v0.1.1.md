# librix v0.1.1

## Suggested GitHub repository description

Index-based shared-memory data structures in C, with SIMD-friendly cuckoo hash
tables and a practical IPv4/IPv6 flow-cache sample.

## Suggested GitHub topics

`c`, `shared-memory`, `mmap`, `data-structures`, `hash-table`, `cuckoo-hash`,
`simd`, `avx2`, `networking`, `flow-cache`

## Release summary

`v0.1.1` is a maintenance release for `librix`.

The main focus of this release is making release builds and benchmark builds
behave consistently across a wider compiler and CPU range. In particular,
older GCC versions now build the flow-cache paths cleanly without weakening
warning policy, and `make test` catches `-DNDEBUG`-only issues without having
to run the full benchmark suite. The CI matrix now also checks both
`SIMD=avx2` and `SIMD=gen`, so the generic scalar path is validated in
automation rather than only by ad hoc local runs.

This release also improves benchmark usability. `fc_bench archcmp ...` now
prints an explicit `auto` versus `avx2` winner summary, which is useful on
machines where AVX-512 is available but not always the fastest choice.

## Highlights

- No warning suppression added; invariants are now expressed directly in code
- `make test` includes a fast `-DNDEBUG` flow-cache build check
- CI exercises both `SIMD=avx2` and `SIMD=gen`
- `fc_bench archcmp ...` prints winner summaries for `datapath` and
  `findadd_*` modes
- `add_bulk()` now prefetches both candidate buckets before duplicate-aware
  insertion

## Validation status

- Generic scalar path: tested, including CI coverage
- AVX2 path: tested
- AVX-512 path: tested on AMD Zen 4 and Intel Cascade Lake hardware
- Remote GCC 11.5 build/test: confirmed

## Notes

- AVX-512 remains CPU-dependent. Some machines benefit from the `auto`
  dispatch choice, while others may prefer `avx2` for specific workloads.
- `flow4` remains the most aggressively tuned path, but `flow6` and `flowu`
  are functionally covered and now better documented for cross-CPU comparison.
