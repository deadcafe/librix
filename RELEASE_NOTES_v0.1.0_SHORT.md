# librix v0.1.0

First public release of `librix`.

`librix` provides index-based queue, tree, and hash-table implementations for
shared-memory and relocatable layouts. The repository also includes
`samples/fcache`, a practical flow-cache sample with `flow4`, `flow6`, and
`flowu` variants.

## Highlights

- Shared-memory-friendly index-based data structures
- SIMD-friendly cuckoo hash implementations
- Slot-aware remove support
- Bulk flow-cache APIs with fixed in-flight scratch
- Size-query based initialization for allocator-independent layouts
- Built-in tests and benchmark programs

## Validation status

- Generic scalar path: tested
- AVX2 path: tested
- AVX-512 path: implemented, but not yet validated on AVX-512 hardware

## Notes

Most tuning so far has focused on `flow4`. `flow6` and `flowu` are implemented
and covered by the functional test suite, but have seen less performance
tuning so far.
