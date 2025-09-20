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

all:
	$(MAKE) -C fcache all
	$(MAKE) -C test   all

static:
	$(MAKE) -C fcache static

shared:
	$(MAKE) -C fcache shared

test: all
	$(MAKE) -C test test

bench: all
	$(MAKE) -C test bench

bench-full: all
	$(MAKE) -C test bench-full

clean:
	$(MAKE) -C fcache clean
	$(MAKE) -C test   clean

install:
	$(MAKE) -C fcache install PREFIX=$(PREFIX)
