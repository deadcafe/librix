# librix v0.2.0

Release focused on consolidating the tree around `flowtable/` and tightening
the public/private header boundary.

`v0.2.0` removes the old `samples/` split in favor of a dedicated
`flowtable/` subtree, updates release-facing documentation to match the
current layout, moves private generator/dispatch/hash headers out of the
public include tree, and simplifies the flow-table generator layer by
removing unused hooks and wrapper macros. It also improves small-query add
behavior by routing `q=2/3 add_idx` through the small-query path.

## Validation status

- `make -C flowtable/test test`: passed
- Generic scalar path: tested
- AVX2 path: tested
- AVX-512 path: tested on AVX-512-capable hardware

## Notes

- AVX-512 remains workload- and CPU-dependent rather than universally faster
  than AVX2.
- Most tuning still focuses on `flow4`; `flow6` and `flowu` are functionally
  covered but less tuned.
