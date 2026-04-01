# Samples

This directory contains two separate sample families built on librix.

- `samples/fcache/`
  High-performance flow cache implementations with `findadd`, reclaim,
  pressure relief, timeout handling, and SIMD-dispatched datapath code.
- `samples/ftable/`
  Flow table prototype for permanent-style entries. `find` and `add` are
  separate operations. Records stay in stable caller-owned storage while
  only the bucket hash table grows.

## Layout

```text
samples/
  README.md
  README_JP.md
  Makefile
  fcache/
  ftable/
  test/
```

## Common Commands

```sh
# Build the flow cache library and sample test binary
make -C samples all

# Run the flow cache functional tests
make -C samples test

# Run the representative flow cache benchmark suite
make -C samples bench

# Build the flow table library
make -C samples ftable

# Run the flow table functional tests
make -C samples ftable-test
```

## Documents

- Flow cache design, API, tests, and benchmark notes:
  `samples/fcache/README.md`
- Flow table design, API, tests, and benchmark plan:
  `samples/ftable/README.md`
- Forward-looking design note for shared entry abstractions across flow cache
  and a future flow table:
  `samples/ENTRY_UNIFICATION_DESIGN_JP.md`

The top-level `samples/` README files are only an index. Detailed design
notes stay with each sample family so that `fcache` and `ftable` can evolve
independently.
