include mk/cc.mk
export CC

TESTDIRS := tests/slist tests/list tests/stailq tests/tailq tests/circleq \
            tests/rbtree tests/hashtbl tests/hashtbl_extra tests/hashtbl32 tests/hashtbl64
BENCHDIRS := flowtable
SUBDIRS  := $(TESTDIRS) flowtable

HTAGS_PORT   ?= 8000
HTAGS_BIND   ?= 127.0.0.1

PREFIX       ?= /usr/local

RIX_PUB_HDRS := $(filter-out %_private.h, $(wildcard include/rix/*.h))

.PHONY: all build test bench bench-full run-tests run-bench run-bench-full clean install htags htags-serve help ftable flowtable flowtable-test flowtable-bench bench-flowtable-extra bench-flowtable-sweep
all: build run-tests run-bench

help:
	@printf '%s\n' \
	  'Usage: make <target> [VAR=value]' \
	  '' \
	  'Targets:' \
	  '  all          Build and run the full test suite (default)' \
	  '  build        Build all tests and samples without running them' \
	  '  test         Build and run all tests' \
	  '  bench        Build and run the representative benchmark suite' \
	  '  bench-full   Build and run the full long-running benchmark suite' \
	  '  flowtable    Build flowtable library and run flowtable tests' \
	  '  ftable       Alias for flowtable' \
	  '  clean        Remove build artifacts and generated HTML' \
	  '  install      Install headers and sample library under PREFIX' \
	  '  htags        Generate HTML source browsing output under HTML/' \
	  '  htags-serve  Generate htags output and serve it locally' \
	  '  help         Show this help' \
	  '' \
	  'Variables:' \
	  '  PREFIX=/usr/local   Install prefix for make install' \
	  '  HTAGS_BIND=127.0.0.1  Bind address for htags-serve' \
	  '  HTAGS_PORT=8000       Port for htags-serve'

BUILD_TARGETS := $(addprefix build-,$(SUBDIRS))
TEST_TARGETS  := $(addprefix test-,$(TESTDIRS))
BENCH_TARGETS :=

build: $(BUILD_TARGETS)

$(BUILD_TARGETS): build-%:
	@echo "[BUILD] $*"
	@$(MAKE) -C $*

test: build run-tests

run-tests: $(TEST_TARGETS) test-flowtable

$(TEST_TARGETS): test-%: build-%
	@echo "[TEST] $*"
	@$(MAKE) -C $* test

test-flowtable: build-flowtable
	@echo "[TEST] flowtable"
	@$(MAKE) -C flowtable/test test

bench: build run-bench

run-bench: bench-flowtable bench-flowtable-extra

bench-flowtable: build-flowtable
	@echo "[BENCH] flowtable"
	@$(MAKE) -C flowtable/test bench

bench-flowtable-extra: build-flowtable
	@echo "[BENCH] flowtable extra"
	@$(MAKE) -C flowtable/test bench-extra

$(BENCH_TARGETS): bench-%: build-%
	@echo "[BENCH] $*"
	@$(MAKE) -C $* bench

BENCH_FULL_DIRS := tests/hashtbl tests/hashtbl_extra tests/hashtbl32 tests/hashtbl64
BENCH_FULL_TARGETS := $(addprefix bench-full-,$(BENCH_FULL_DIRS))
CLEAN_TARGETS := $(addprefix clean-,$(SUBDIRS))

bench-full: build run-bench-full

run-bench-full: $(BENCH_FULL_TARGETS) bench-full-flowtable bench-flowtable-sweep

bench-full-flowtable: build-flowtable
	@echo "[BENCH] flowtable"
	@$(MAKE) -C flowtable/test bench-full

bench-flowtable-sweep: build-flowtable
	@echo "[BENCH] flowtable/sweep"
	@$(MAKE) -C flowtable/test bench-sweep

$(BENCH_FULL_TARGETS): bench-full-%: build-%
	@echo "[BENCH] $*"
	@$(MAKE) -C $* bench

clean: $(CLEAN_TARGETS)
	rm -rf HTML

$(CLEAN_TARGETS): clean-%:
	@echo "[CLEAN] $*"
	@$(MAKE) -C $* clean

flowtable:
	@$(MAKE) -C flowtable/test all

ftable: flowtable

flowtable-test:
	@$(MAKE) -C flowtable/test test

flowtable-bench:
	@$(MAKE) -C flowtable/test bench

install:
	install -d $(PREFIX)/include/rix $(PREFIX)/lib
	install -m 644 include/librix.h   $(PREFIX)/include/
	install -m 644 $(RIX_PUB_HDRS)   $(PREFIX)/include/rix/
	@echo "[SKIP] flowtable sample install is not provided"

htags:
	mkdir -p HTML
	gtags HTML
	htags -aDfnosF -d HTML
	printf '..' > HTML/GTAGSROOT

htags-serve: htags
	@echo "Serving htags HTML at http://$(HTAGS_BIND):$(HTAGS_PORT)/ (no-cache)"
	cd HTML && python3 ../scripts/nohttpserver.py $(HTAGS_PORT) $(HTAGS_BIND)
