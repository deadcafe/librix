#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/Makefile - Top-level delegating Makefile
#
# Targets:
#   all    - build library (static + shared) and test binary (default)
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
BENCH_FC_OPTS ?= --pin-core 0 --raw-repeat 7 --keep-n 5

all:
	$(MAKE) -C fcache all
	$(MAKE) -C test   all

static:
	$(MAKE) -C fcache static

shared:
	$(MAKE) -C fcache shared

test: all
	$(MAKE) -C test ndebug-check BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)"
	$(MAKE) -C test test

bench:
	$(MAKE) -C test bench BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)" \
	    BENCH_FC_OPTS="$(BENCH_FC_OPTS)"

bench-full:
	$(MAKE) -C test bench-full BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)" \
	    BENCH_FC_OPTS="$(BENCH_FC_OPTS)"

clean:
	$(MAKE) -C fcache clean
	$(MAKE) -C test   clean

install:
	$(MAKE) -C fcache install PREFIX=$(PREFIX)
