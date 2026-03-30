include mk/cc.mk
export CC

TESTDIRS := tests/slist tests/list tests/stailq tests/tailq tests/circleq \
            tests/rbtree tests/hashtbl tests/hashtbl32 tests/hashtbl64
BENCHDIRS := samples
SUBDIRS  := $(TESTDIRS) samples

HTAGS_PORT   ?= 8000
HTAGS_BIND   ?= 127.0.0.1

PREFIX       ?= /usr/local

RIX_PUB_HDRS := $(filter-out %_private.h, $(wildcard include/rix/*.h))

.PHONY: all build test bench bench-full run-tests run-bench run-bench-full clean install htags htags-serve help
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

build:
	@for d in $(SUBDIRS); do \
	  echo "[BUILD] $$d"; \
	  $(MAKE) -C $$d; \
	done

test: build run-tests

run-tests:
	@for d in $(SUBDIRS); do \
	  echo "[TEST] $$d"; \
	  $(MAKE) -C $$d test; \
	done

bench: build run-bench

run-bench:
	@for d in $(BENCHDIRS); do \
	  echo "[BENCH] $$d"; \
	  $(MAKE) -C $$d bench; \
	done

bench-full: build run-bench-full

run-bench-full:
	@for d in tests/hashtbl tests/hashtbl32 tests/hashtbl64; do \
	  echo "[BENCH] $$d"; \
	  $(MAKE) -C $$d bench; \
	done
	@echo "[BENCH] samples"
	@$(MAKE) -C samples bench-full

clean:
	@for d in $(SUBDIRS); do \
	  echo "[CLEAN] $$d"; \
	  $(MAKE) -C $$d clean; \
	done
	rm -rf HTML

install:
	install -d $(PREFIX)/include/rix $(PREFIX)/lib
	install -m 644 include/librix.h   $(PREFIX)/include/
	install -m 644 $(RIX_PUB_HDRS)   $(PREFIX)/include/rix/
	$(MAKE) -C samples/fcache install PREFIX=$(PREFIX)

htags:
	mkdir -p HTML
	gtags HTML
	htags -aDfnosF -d HTML
	printf '..' > HTML/GTAGSROOT

htags-serve: htags
	@echo "Serving htags HTML at http://$(HTAGS_BIND):$(HTAGS_PORT)/ (no-cache)"
	cd HTML && python3 ../scripts/nohttpserver.py $(HTAGS_PORT) $(HTAGS_BIND)
