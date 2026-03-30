#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/Makefile - Top-level delegating Makefile
#
# Targets:
#   all    - build library, run fcache tests, and run bench-short (default)
#   static - build libfcache.a only
#   shared - build libfcache.so only
#   test   - build and run functional tests
#   bench  - build and run the representative benchmark suite
#   bench-full - build and run the full long-running benchmark suite
#   clean  - clean all subdirectories
#   install PREFIX=/path - install library and headers
#

.PHONY: all static shared test bench bench-full clean install

BENCH_EXTRA_CFLAGS ?= -DNDEBUG
BENCH_FC_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7
all:
	$(MAKE) -C fcache all

static:
	$(MAKE) -C fcache static

shared:
	$(MAKE) -C fcache shared

test: all
	$(MAKE) -C fcache/test ndebug-check BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)"
	$(MAKE) -C fcache/test test

bench:
	$(MAKE) -C fcache/test bench-short BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)" \
	    BENCH_FC_OPTS="$(BENCH_FC_OPTS)"

bench-full:
	$(MAKE) -C fcache/test bench-full BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)" \
	    BENCH_FC_OPTS="$(BENCH_FC_OPTS)"

clean:
	$(MAKE) -C fcache clean
	$(MAKE) -C fcache/test clean

install:
	$(MAKE) -C fcache install PREFIX=$(PREFIX)
