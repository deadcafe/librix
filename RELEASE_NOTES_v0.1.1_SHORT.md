# librix v0.1.1

Maintenance release focused on portability, validation, and benchmark
usability.

`v0.1.1` fixes older GCC release-build false positives in the flow-cache
paths without weakening the warning policy, adds a fast `-DNDEBUG` flow-cache
build check to `make test`, adds CI coverage for both `SIMD=avx2` and
`SIMD=gen`, and improves `fc_bench archcmp ...` so it prints an explicit
`auto` versus `avx2` winner summary.

## Validation status

- Generic scalar path: tested, including CI coverage
- AVX2 path: tested
- AVX-512 path: tested on AMD Zen 4 and Intel Cascade Lake hardware
- Remote GCC 11.5 build/test: confirmed

## Notes

- AVX-512 is supported and validated, but it is still workload- and
  CPU-dependent rather than universally better than AVX2.
