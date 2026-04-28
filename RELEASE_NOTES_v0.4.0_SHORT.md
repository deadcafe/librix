# librix v0.4.0

Release focused on the slot-extra flow-table model for worker-local flow
caches.

`v0.4.0` merges the former `flowtable/extra/` code into the normal
`flowtable/include` and `flowtable/src` layout, extends slot-extra support to
`flow6_extra` and `flowu_extra`, and adds full-family slot-extra tests and
benchmarks. It also introduces `FT_TABLE_*` generic facade macros for shared
classic/extra operations, including maintenance helpers.

## Validation status

- `git diff --check`: passed
- `make -C flowtable/test test-extra-arch CC=gcc`: passed
- `make -C flowtable/test test-extra CC=clang BUILDDIR=/tmp/librix-flowtable-clang-extra-all LIBFT=/tmp/librix-flowtable-clang-extra-all/lib/libftable.a`: passed
- `ft_bench_extra_full` GCC and Clang builds: passed
- small `bench-extra-full` run over all extra families: passed

## Notes

- The target model is VPP-style worker-local flow cache usage, not a
  multi-threaded shared hash table.
- AVX-512 objects build locally; AVX-512 execution was skipped because the
  local CPU does not support AVX512F.
