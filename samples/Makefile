#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/Makefile - Top-level delegating Makefile
#
# Targets:
#   all    - build ftable library, run tests, and run bench-light (default)
#   static - build libftable.a only
#   test   - build and run functional tests
#   bench  - build and run the representative benchmark suite
#   bench-full - build and run the full long-running benchmark suite
#   clean  - clean all subdirectories
#
# fcache targets (legacy, not built by default):
#   fcache        - build and test fcache
#   fcache-bench  - run fcache benchmarks
#   fcache-clean  - clean fcache
#

.DEFAULT_GOAL := build

.PHONY: all build static test bench bench-full clean
.PHONY: ftable ftable-test ftable-bench ftable-clean
.PHONY: fcache fcache-bench fcache-clean

BENCH_EXTRA_CFLAGS ?= -DNDEBUG
BENCH_FC_OPTS ?= --pin-core 2 --raw-repeat 2 --keep-n 1
BENCH_FT_OPTS ?= --pin-core 2 --raw-repeat 2 --keep-n 1
BENCH_FT_FULL_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7

build: static

all:
	$(MAKE) -C ftable/test all

static:
	$(MAKE) -C ftable static

test:
	$(MAKE) -C ftable/test test

ftable:
	$(MAKE) -C ftable static
	$(MAKE) -C ftable/test test

ftable-test:
	$(MAKE) -C ftable/test test

ftable-bench:
	$(MAKE) -C ftable/test bench-light BENCH_FT_OPTS="$(BENCH_FT_OPTS)"

ftable-clean:
	$(MAKE) -C ftable clean
	$(MAKE) -C ftable/test clean

bench:
	$(MAKE) -C ftable/test bench-light BENCH_FT_OPTS="$(BENCH_FT_OPTS)"

bench-full:
	$(MAKE) -C ftable/test bench-full BENCH_FT_FULL_OPTS="$(BENCH_FT_FULL_OPTS)"

clean:
	$(MAKE) -C ftable clean
	$(MAKE) -C ftable/test clean

# --- fcache (legacy, explicit only) ---

fcache:
	$(MAKE) -C fcache all
	$(MAKE) -C fcache/test ndebug-check BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)"
	$(MAKE) -C fcache/test test

fcache-bench:
	$(MAKE) -C fcache/test bench-short BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)" \
	    BENCH_FC_OPTS="$(BENCH_FC_OPTS)"

fcache-clean:
	$(MAKE) -C fcache clean
	$(MAKE) -C fcache/test clean
