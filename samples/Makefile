#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/Makefile - Top-level delegating Makefile
#
# Targets:
#   all    - build library, run fcache tests, and run bench-light (default)
#   static - build libfcache.a only
#   shared - build libfcache.so only
#   test   - build and run functional tests
#   bench  - build and run the representative benchmark suite
#   bench-full - build and run the full long-running benchmark suite
#   clean  - clean all subdirectories
#   install PREFIX=/path - install library and headers
#

.DEFAULT_GOAL := build

.PHONY: all build static shared test bench bench-full clean install
.PHONY: ftable ftable-test ftable-bench ftable-clean

BENCH_EXTRA_CFLAGS ?= -DNDEBUG
BENCH_FC_OPTS ?= --pin-core 2 --raw-repeat 2 --keep-n 1
BENCH_FT_OPTS ?= --pin-core 2 --raw-repeat 2 --keep-n 1
BENCH_FT_FULL_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7

build: static shared
	$(MAKE) -C ftable static

all:
	$(MAKE) -C fcache all
	$(MAKE) -C ftable/test all

static:
	$(MAKE) -C fcache static

shared:
	$(MAKE) -C fcache shared

test: static shared
	$(MAKE) -C fcache/test ndebug-check BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)"
	$(MAKE) -C fcache/test test
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
	$(MAKE) -C fcache/test bench-short BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)" \
	    BENCH_FC_OPTS="$(BENCH_FC_OPTS)"
	$(MAKE) -C ftable/test bench-light BENCH_FT_OPTS="$(BENCH_FT_OPTS)"

bench-full:
	$(MAKE) -C fcache/test bench-full BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)" \
	    BENCH_FC_OPTS="$(BENCH_FC_OPTS)"
	$(MAKE) -C ftable/test bench-full BENCH_FT_FULL_OPTS="$(BENCH_FT_FULL_OPTS)"

clean:
	$(MAKE) -C fcache clean
	$(MAKE) -C fcache/test clean
	$(MAKE) -C ftable clean
	$(MAKE) -C ftable/test clean

install:
	$(MAKE) -C fcache install PREFIX=$(PREFIX)
