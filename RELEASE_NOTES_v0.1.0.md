# librix v0.1.0

## Suggested GitHub repository description

Index-based shared-memory data structures in C, with SIMD-friendly cuckoo hash
tables and a practical IPv4/IPv6 flow-cache sample.

## Suggested GitHub topics

`c`, `shared-memory`, `mmap`, `data-structures`, `hash-table`, `cuckoo-hash`,
`simd`, `avx2`, `networking`, `flow-cache`

## Release summary

This is the first public release of `librix`.

`librix` provides index-based queue, tree, and hash-table implementations
designed for shared-memory and relocatable layouts. Instead of embedding raw
pointers into data structures, it stores indices, which makes the same memory
region usable across processes and after remapping.

This release also includes `samples/fcache`, a practical flow-cache
implementation built on top of `librix`. The sample currently includes three
variants:

- `flow4` for IPv4 5-tuple keys
- `flow6` for IPv6 5-tuple keys
- `flowu` for unified IPv4/IPv6 operation

## Highlights

- Relative-index queue and tree primitives
- SIMD-friendly cuckoo hash implementations
- Slot-aware remove support
- Bulk flow-cache APIs with fixed in-flight context storage
- Size-query based initialization for allocator-independent layouts
- Built-in test and benchmark programs

## Validation status

- Generic scalar path: tested
- AVX2 path: tested
- AVX-512 path: implemented, but not yet validated on AVX-512 hardware

## Notes

Most performance analysis and tuning so far has focused on the `flow4`
variant. `flow6` and `flowu` are implemented and covered by the functional
test suite, but have seen less performance tuning at this stage.
