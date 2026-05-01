TOPDIR  := $(shell cd ../.. && pwd)
FTDIR   := $(shell cd .. && pwd)

CC_IS_CLANG := $(shell $(CC) --version 2>&1 | grep -c "clang")

WARN_COMMON  = -Wall -Wextra \
               -Wshadow \
               -Wstrict-prototypes \
               -Wmissing-prototypes \
               -Wvla \
               -Wcast-qual \
               -Wconversion \
               -Wsign-conversion \
               -Wundef \
               -Wformat=2 \
               -Wnull-dereference \
               -Wdouble-promotion \
               -Wredundant-decls \
               -Werror

ifeq ($(CC_IS_CLANG),0)
WARN_EXTRA   = -Wcast-align=strict -Wstrict-aliasing=2
else
WARN_EXTRA   = -Wcast-align -Wstrict-aliasing
endif

WARN         = $(WARN_COMMON) $(WARN_EXTRA)
OPTLEVEL    ?= 3
EXTRA_CFLAGS ?=
BENCH_FT_OPTS ?= --pin-core 2 --raw-repeat 3 --keep-n 1
BENCH_QUERIES := 1 8 32 256
BENCH_FULL_CORES ?= auto
BENCH_FULL_CAT_LOGS ?= 1
BENCH_FULL_NAME ?= bench-full
BENCH_FULL_OPS ?=
BENCH_LIGHT_FAMILY := flow4
BENCH_LIGHT_FILL := 60
BENCH_GROW_FILL := 60
BENCH_MAINT_FILL := 50
BENCH_EXTRA_FULL_ENTRIES ?= $(BENCH_FULL_ENTRIES)
BENCH_FULL_SERIAL_PIN ?= 2
BENCH_FULL_LOGDIR ?= $(BUILDDIR)/bench-full-logs
BENCH_FULL_RUN_EXTRA ?= 1
BENCH_FULL_RUN_MAINT ?= 1
BENCH_FULL_RUN_PURE_MAINT ?= $(BENCH_FULL_RUN_MAINT)
BENCH_FULL_RUN_EXTRA_MAINT ?= $(BENCH_FULL_RUN_MAINT)
BENCH_FULL_RUN_GROW ?= 1

BENCH_DEV_ARCHES ?= auto
BENCH_DEV_FAMILIES ?= flow4
BENCH_DEV_EXTRA_FAMILIES ?= $(addsuffix _extra,$(BENCH_DEV_FAMILIES))
BENCH_DEV_OPS ?= find_hit add_idx
BENCH_DEV_QUERIES ?= 256
BENCH_DEV_FILLS ?= 75
BENCH_DEV_ENTRIES ?= 262144
BENCH_DEV_FT_OPTS ?= --raw-repeat 3 --keep-n 1
BENCH_DEV_EXTRA_REPS ?= 3
BENCH_DEV_CAT_LOGS ?= 1
BENCH_DEV_RUN_PURE_MAINT ?= 0
BENCH_DEV_RUN_EXTRA_MAINT ?= 1

BENCH_RELEASE_ARCHES ?= auto
BENCH_RELEASE_FAMILIES ?= flow4 flow6 flowu
BENCH_RELEASE_EXTRA_FAMILIES ?= $(addsuffix _extra,$(BENCH_RELEASE_FAMILIES))
BENCH_RELEASE_OPS ?=
BENCH_RELEASE_QUERIES ?= 32 256
BENCH_RELEASE_FILLS ?= 75 95
BENCH_RELEASE_ENTRIES ?= 1048576
BENCH_RELEASE_FT_OPTS ?= --raw-repeat 5 --keep-n 3
BENCH_RELEASE_EXTRA_REPS ?= 3
BENCH_RELEASE_RUN_PURE_MAINT ?= 0
BENCH_RELEASE_RUN_EXTRA_MAINT ?= 1

BENCH_RELEASE_FULL_ARCHES ?= gen sse avx2 avx512
BENCH_RELEASE_FULL_FAMILIES ?= flow4 flow6 flowu
BENCH_RELEASE_FULL_EXTRA_FAMILIES ?= $(addsuffix _extra,$(BENCH_RELEASE_FULL_FAMILIES))
BENCH_RELEASE_FULL_OPS ?=
BENCH_RELEASE_FULL_QUERIES ?= 1 32 256
BENCH_RELEASE_FULL_FILLS ?= 75 95
BENCH_RELEASE_FULL_ENTRIES ?= 1048576
BENCH_RELEASE_FULL_FT_OPTS ?= --raw-repeat 7 --keep-n 5
BENCH_RELEASE_FULL_EXTRA_REPS ?= 5
BENCH_RELEASE_FULL_RUN_PURE_MAINT ?= 1
BENCH_RELEASE_FULL_RUN_EXTRA_MAINT ?= 1

BENCH_FT_FULL_OPTS ?= $(BENCH_RELEASE_FT_OPTS)
BENCH_FULL_ARCHES ?= $(BENCH_RELEASE_ARCHES)
BENCH_FULL_ENTRIES ?= $(BENCH_RELEASE_ENTRIES)
BENCH_FULL_FAMILIES ?= $(BENCH_RELEASE_FAMILIES)
BENCH_FULL_EXTRA_FAMILIES ?= $(addsuffix _extra,$(BENCH_FULL_FAMILIES))
BENCH_FULL_FILLS ?= $(BENCH_RELEASE_FILLS)
BENCH_EXTRA_FULL_REPS ?= $(BENCH_RELEASE_EXTRA_REPS)

CFLAGS_COMMON = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include \
               -I$(FTDIR)/include \
               $(EXTRA_CFLAGS)
ARCH_CFLAGS_gen :=
ARCH_CFLAGS_sse := -msse4.2
ARCH_CFLAGS_avx2 := -mavx2 -msse4.2
ARCH_CFLAGS_avx512 := -mavx512f -mavx2 -msse4.2
CFLAGS       = $(CFLAGS_COMMON) $(ARCH_CFLAGS_avx2)
DEPFLAGS     = -MMD -MP

BUILDDIR     ?= $(FTDIR)/build
BINDIR       ?= $(BUILDDIR)/bin
TARGET_NAME   = ft_test
BENCH_NAME    = ft_bench
SIM_NAME      = ft_sim_cache
TEST_EXTRA_NAME = ft_test_extra
TEST_PARITY_NAME = ft_test_parity
TEST_CTRL_NAME = ft_test_ctrl
BENCH_EXTRA_NAME = ft_bench_extra
BENCH_EXTRA_FULL_NAME = ft_bench_extra_full
BENCH_SWEEP_NAME = ft_bench_sweep
BENCH_ZONED_NAME = ft_bench_zoned
BENCH_CTRL_NAME  = ft_bench_ctrl
SAMPLE_NAME      = ft_usage_sample
TARGET        = $(BINDIR)/$(TARGET_NAME)
BENCH         = $(BINDIR)/$(BENCH_NAME)
SIM           = $(BINDIR)/$(SIM_NAME)
TEST_EXTRA    = $(BINDIR)/$(TEST_EXTRA_NAME)
TEST_PARITY   = $(BINDIR)/$(TEST_PARITY_NAME)
TEST_CTRL     = $(BINDIR)/$(TEST_CTRL_NAME)
BENCH_EXTRA   = $(BINDIR)/$(BENCH_EXTRA_NAME)
BENCH_EXTRA_FULL = $(BINDIR)/$(BENCH_EXTRA_FULL_NAME)
BENCH_SWEEP   = $(BINDIR)/$(BENCH_SWEEP_NAME)
BENCH_ZONED   = $(BINDIR)/$(BENCH_ZONED_NAME)
BENCH_CTRL    = $(BINDIR)/$(BENCH_CTRL_NAME)
SAMPLE        = $(BINDIR)/$(SAMPLE_NAME)
BENCH_CTRL_GEN    = $(BINDIR)/$(BENCH_CTRL_NAME)_gen
BENCH_CTRL_SSE    = $(BINDIR)/$(BENCH_CTRL_NAME)_sse
BENCH_CTRL_AVX2   = $(BINDIR)/$(BENCH_CTRL_NAME)_avx2
BENCH_CTRL_AVX512 = $(BINDIR)/$(BENCH_CTRL_NAME)_avx512
SRC          = test_flow_table.c
BENCH_SRC    = bench_flow_table.c
SIM_SRC      = sim_flow_cache.c
TEST_EXTRA_SRC = test_flow4_extra.c
TEST_PARITY_SRC = test_flow4_parity.c
TEST_CTRL_SRC = test_fill_ctrl.c
BENCH_EXTRA_SRC = bench_flow4_vs_extra.c
BENCH_EXTRA_FULL_SRC = bench_flow_extra_table.c
BENCH_SWEEP_SRC = bench_flow4_maint_sweep.c
BENCH_ZONED_SRC = bench_flow4_zoned.c
BENCH_CTRL_SRC  = bench_fill_ctrl.c
SAMPLE_SRC      = usage_flowtable.c
LIBFT        = $(FTDIR)/build/lib/libftable.a
DEPS         = $(TARGET).d $(BENCH).d $(SIM).d $(TEST_EXTRA).d $(TEST_PARITY).d $(TEST_CTRL).d $(BENCH_EXTRA).d $(BENCH_EXTRA_FULL).d $(BENCH_SWEEP).d $(BENCH_ZONED).d $(BENCH_CTRL).d $(BENCH_CTRL_GEN).d $(BENCH_CTRL_SSE).d $(BENCH_CTRL_AVX2).d $(BENCH_CTRL_AVX512).d $(SAMPLE).d
BENCH_FULL_RUNNER = run_bench_full.sh

.PHONY: all help test test-pure test-extra test-extra-arch test-parity test-ctrl bench bench-light bench-dev bench-release bench-release-full bench-full bench-full-serial bench-extra bench-extra-full bench-sweep bench-zoned bench-ctrl bench-ctrl-gen bench-ctrl-sse bench-ctrl-avx2 bench-ctrl-avx512 bench-ctrl-build-all sample sim clean libft FORCE

all: test

help:
	@printf '%s\n' \
	  'flowtable/test targets:' \
	  '  help         Show this help' \
	  '  test         Build and run functional tests' \
	  '  test-pure    Build and run the pure flowtable functional tests' \
	  '  test-extra-arch  Run ft_test_extra for each CPU-supported arch' \
	  '  bench        Run the light benchmark set (flow4 only, q=1/8/32/256 @ fill=60%)' \
	  '  bench-light  Same as bench' \
	  '  bench-dev    Run a short development benchmark profile' \
	  '  bench-release  Run the standard release benchmark profile in parallel' \
	  '  bench-release-full  Run the exhaustive release benchmark sweep' \
	  '  bench-full   Compatibility target using release defaults' \
	  '  bench-full-serial  Run the release profile on one pinned core' \
	  '  bench-extra  Run focused flow4 pure-vs-slot-extra comparison @ 75% fill' \
	  '  bench-extra-full  Run full slot-extra sweep (flow4/6/u extra)' \
	  '  bench-sweep  Run pure-vs-slot-extra maintain fill/expire matrix' \
	  '  bench-zoned  Run zone-based flowcache control benchmark' \
	  '  bench-ctrl   Run adaptive fill-controller benchmark' \
	  '  sample       Build the readable API usage sample (not a test/bench)' \
	  '  sim          Build and run the flow cache simulation' \
	  '  clean        Remove generated binaries and dependency files' \
	  '' \
	  'Useful variables:' \
	  '  BENCH_FT_OPTS       Options for bench / bench-light' \
	  '  BENCH_FT_FULL_OPTS  Options for bench-full' \
	  '  BENCH_FULL_ARCHES   Arch variants for bench-full' \
	  '  BENCH_FULL_CORES    Physical CPUs for parallel bench-full (auto or list)' \
	  '  BENCH_FULL_EXTRA_FAMILIES  Slot-extra families for bench-full' \
	  '' \
	  'Fill-controller bench arch variants (optimized binaries):' \
	  '  bench-ctrl-gen      Build/run ft_bench_ctrl_gen with GEN arch' \
	  '  bench-ctrl-sse      Build/run ft_bench_ctrl_sse with SSE arch' \
	  '  bench-ctrl-avx2     Build/run ft_bench_ctrl_avx2 with AVX2 arch' \
	  '  bench-ctrl-avx512   Build ft_bench_ctrl_avx512; run only on AVX512 CPU' \
	  '  bench-ctrl-build-all  Build all ft_bench_ctrl_* arch binaries'

$(BINDIR):
	mkdir -p $@

$(TARGET): $(SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(SRC) $(LIBFT)

$(BENCH): $(BENCH_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_SRC) $(LIBFT)

$(SIM): $(SIM_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(SIM_SRC) $(LIBFT) -lm

$(TEST_EXTRA): $(TEST_EXTRA_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(TEST_EXTRA_SRC) $(LIBFT)

$(TEST_PARITY): $(TEST_PARITY_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(TEST_PARITY_SRC) $(LIBFT)

$(TEST_CTRL): $(TEST_CTRL_SRC) | $(BINDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(TEST_CTRL_SRC)

$(BENCH_EXTRA): $(BENCH_EXTRA_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_EXTRA_SRC) $(LIBFT)

$(BENCH_EXTRA_FULL): $(BENCH_EXTRA_FULL_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_EXTRA_FULL_SRC) $(LIBFT)

$(BENCH_SWEEP): $(BENCH_SWEEP_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_SWEEP_SRC) $(LIBFT)

$(BENCH_ZONED): $(BENCH_ZONED_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_ZONED_SRC) $(LIBFT)

$(BENCH_CTRL): $(BENCH_CTRL_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_CTRL_SRC) $(LIBFT)

$(SAMPLE): $(SAMPLE_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(SAMPLE_SRC) $(LIBFT)

define BENCH_CTRL_ARCH_RULE
$(BINDIR)/$(BENCH_CTRL_NAME)_$(1): $(BENCH_CTRL_SRC) $(LIBFT) | $(BINDIR)
	$$(CC) $$(CFLAGS_COMMON) $$(ARCH_CFLAGS_$(1)) -DNDEBUG $$(DEPFLAGS) \
	    -MF $$@.d -MT $$@ -o $$@ $$(BENCH_CTRL_SRC) $$(LIBFT)
endef

$(foreach arch,gen sse avx2 avx512,$(eval $(call BENCH_CTRL_ARCH_RULE,$(arch))))

# Always enter the ftable sub-make so its own .d files can decide whether
# header changes require rebuilding the library objects/archive.
$(LIBFT): FORCE
	$(MAKE) -C $(FTDIR) static

FORCE:

libft: $(LIBFT)

test: test-pure test-extra test-extra-arch test-parity test-ctrl

test-pure: $(TARGET)
	$(TARGET)

test-extra: $(TEST_EXTRA)
	$(TEST_EXTRA)

test-extra-arch: $(TEST_EXTRA)
	$(TEST_EXTRA) gen
	@if grep -qw sse4_2 /proc/cpuinfo; then \
		$(TEST_EXTRA) sse; \
	else \
		echo "skip: CPU does not support SSE4.2"; \
	fi
	@if grep -qw avx2 /proc/cpuinfo; then \
		$(TEST_EXTRA) avx2; \
	else \
		echo "skip: CPU does not support AVX2"; \
	fi
	@if grep -qw avx512f /proc/cpuinfo; then \
		$(TEST_EXTRA) avx512; \
	else \
		echo "skip: CPU does not support AVX512F"; \
	fi

test-parity: $(TEST_PARITY)
	$(TEST_PARITY)

test-ctrl: $(TEST_CTRL)
	$(TEST_CTRL)

bench-light: $(BENCH)
	@for q in $(BENCH_QUERIES); do \
		echo "== light ftable bench (auto, $(BENCH_LIGHT_FAMILY), q=$$q, 1M @ fill=$(BENCH_LIGHT_FILL)%) =="; \
		$(BENCH) $(BENCH_FT_OPTS) --arch auto --query $$q $(BENCH_LIGHT_FAMILY) 1048576 $(BENCH_LIGHT_FILL); \
	done

bench: bench-light

bench-extra: $(BENCH_EXTRA)
	$(BENCH_EXTRA)

bench-extra-full: $(BENCH_EXTRA_FULL)
	@for arch in $(BENCH_FULL_ARCHES); do \
		case "$$arch" in \
		gen) supported=1 ;; \
		sse) grep -qw sse4_2 /proc/cpuinfo && supported=1 || supported=0 ;; \
		avx2) grep -qw avx2 /proc/cpuinfo && supported=1 || supported=0 ;; \
		avx512) grep -qw avx512f /proc/cpuinfo && supported=1 || supported=0 ;; \
		*) echo "unknown arch: $$arch"; exit 1 ;; \
		esac; \
		if [ "$$supported" != 1 ]; then \
			echo "skip: CPU does not support $$arch"; \
			continue; \
		fi; \
		for family in $(BENCH_FULL_EXTRA_FAMILIES); do \
			for fill in $(BENCH_FULL_FILLS); do \
				for q in $(BENCH_QUERIES); do \
					echo "== full extra bench ($$arch, $$family, q=$$q, $(BENCH_EXTRA_FULL_ENTRIES) @ fill=$$fill%) =="; \
					$(BENCH_EXTRA_FULL) --arch $$arch --query $$q --reps $(BENCH_EXTRA_FULL_REPS) $$family $(BENCH_EXTRA_FULL_ENTRIES) $$fill; \
				done; \
			done; \
			echo "== full extra maintain bench ($$arch, $$family, $(BENCH_EXTRA_FULL_ENTRIES) @ fill=$(BENCH_MAINT_FILL)%) =="; \
			$(BENCH_EXTRA_FULL) --arch $$arch --maint --reps $(BENCH_EXTRA_FULL_REPS) $$family $(BENCH_EXTRA_FULL_ENTRIES) $(BENCH_MAINT_FILL); \
			echo "== full extra grow bench ($$arch, $$family, $(BENCH_EXTRA_FULL_ENTRIES) @ fill=$(BENCH_GROW_FILL)%) =="; \
			$(BENCH_EXTRA_FULL) --arch $$arch --grow --reps $(BENCH_EXTRA_FULL_REPS) $$family $(BENCH_EXTRA_FULL_ENTRIES) $(BENCH_GROW_FILL); \
		done; \
	done

bench-sweep: $(BENCH_SWEEP)
	$(BENCH_SWEEP)

bench-zoned: $(BENCH_ZONED)
	$(BENCH_ZONED)

bench-ctrl: $(BENCH_CTRL)
	$(BENCH_CTRL)

bench-ctrl-build-all: $(BENCH_CTRL_GEN) $(BENCH_CTRL_SSE) $(BENCH_CTRL_AVX2) $(BENCH_CTRL_AVX512)

sample: $(SAMPLE)

bench-ctrl-gen: $(BENCH_CTRL_GEN)
	$(BENCH_CTRL_GEN) gen

bench-ctrl-sse: $(BENCH_CTRL_SSE)
	$(BENCH_CTRL_SSE) sse

bench-ctrl-avx2: $(BENCH_CTRL_AVX2)
	$(BENCH_CTRL_AVX2) avx2

bench-ctrl-avx512: $(BENCH_CTRL_AVX512)
	@if grep -qw avx512f /proc/cpuinfo; then \
		$(BENCH_CTRL_AVX512) avx512; \
	else \
		echo "skip: CPU does not support AVX512F; built $(BENCH_CTRL_AVX512)"; \
	fi

sim: $(SIM)
	$(SIM)

define RUN_BENCH_FULL
	@BENCH="$(BENCH)" \
	BENCH_EXTRA_FULL="$(BENCH_EXTRA_FULL)" \
	BENCH_FT_FULL_OPTS="$(BENCH_FT_FULL_OPTS)" \
	BENCH_FULL_NAME="$(BENCH_FULL_NAME)" \
	BENCH_FULL_ARCHES="$(BENCH_FULL_ARCHES)" \
	BENCH_FULL_CORES="$(BENCH_FULL_CORES)" \
	BENCH_FULL_CAT_LOGS="$(BENCH_FULL_CAT_LOGS)" \
	BENCH_FULL_ENTRIES="$(BENCH_FULL_ENTRIES)" \
	BENCH_FULL_FAMILIES="$(BENCH_FULL_FAMILIES)" \
	BENCH_FULL_EXTRA_FAMILIES="$(BENCH_FULL_EXTRA_FAMILIES)" \
	BENCH_FULL_OPS="$(BENCH_FULL_OPS)" \
	BENCH_FULL_FILLS="$(BENCH_FULL_FILLS)" \
	BENCH_FULL_LOGDIR="$(BENCH_FULL_LOGDIR)" \
	BENCH_FULL_RUN_EXTRA="$(BENCH_FULL_RUN_EXTRA)" \
	BENCH_FULL_RUN_MAINT="$(BENCH_FULL_RUN_MAINT)" \
	BENCH_FULL_RUN_PURE_MAINT="$(BENCH_FULL_RUN_PURE_MAINT)" \
	BENCH_FULL_RUN_EXTRA_MAINT="$(BENCH_FULL_RUN_EXTRA_MAINT)" \
	BENCH_FULL_RUN_GROW="$(BENCH_FULL_RUN_GROW)" \
	BENCH_GROW_FILL="$(BENCH_GROW_FILL)" \
	BENCH_MAINT_FILL="$(BENCH_MAINT_FILL)" \
	BENCH_QUERIES="$(BENCH_QUERIES)" \
	BENCH_EXTRA_FULL_ENTRIES="$(BENCH_EXTRA_FULL_ENTRIES)" \
	BENCH_EXTRA_FULL_REPS="$(BENCH_EXTRA_FULL_REPS)" \
	bash "$(BENCH_FULL_RUNNER)"
endef

bench-dev: BENCH_FT_FULL_OPTS = $(BENCH_DEV_FT_OPTS)
bench-dev: BENCH_FULL_NAME = bench-dev
bench-dev: BENCH_FULL_ARCHES = $(BENCH_DEV_ARCHES)
bench-dev: BENCH_FULL_CAT_LOGS = $(BENCH_DEV_CAT_LOGS)
bench-dev: BENCH_FULL_ENTRIES = $(BENCH_DEV_ENTRIES)
bench-dev: BENCH_FULL_FAMILIES = $(BENCH_DEV_FAMILIES)
bench-dev: BENCH_FULL_EXTRA_FAMILIES = $(BENCH_DEV_EXTRA_FAMILIES)
bench-dev: BENCH_FULL_OPS = $(BENCH_DEV_OPS)
bench-dev: BENCH_FULL_FILLS = $(BENCH_DEV_FILLS)
bench-dev: BENCH_FULL_RUN_PURE_MAINT = $(BENCH_DEV_RUN_PURE_MAINT)
bench-dev: BENCH_FULL_RUN_EXTRA_MAINT = $(BENCH_DEV_RUN_EXTRA_MAINT)
bench-dev: BENCH_QUERIES = $(BENCH_DEV_QUERIES)
bench-dev: BENCH_EXTRA_FULL_ENTRIES = $(BENCH_DEV_ENTRIES)
bench-dev: BENCH_EXTRA_FULL_REPS = $(BENCH_DEV_EXTRA_REPS)
bench-dev: $(BENCH) $(BENCH_EXTRA_FULL)
	$(RUN_BENCH_FULL)

bench-release: BENCH_FT_FULL_OPTS = $(BENCH_RELEASE_FT_OPTS)
bench-release: BENCH_FULL_NAME = bench-release
bench-release: BENCH_FULL_ARCHES = $(BENCH_RELEASE_ARCHES)
bench-release: BENCH_FULL_ENTRIES = $(BENCH_RELEASE_ENTRIES)
bench-release: BENCH_FULL_FAMILIES = $(BENCH_RELEASE_FAMILIES)
bench-release: BENCH_FULL_EXTRA_FAMILIES = $(BENCH_RELEASE_EXTRA_FAMILIES)
bench-release: BENCH_FULL_OPS = $(BENCH_RELEASE_OPS)
bench-release: BENCH_FULL_FILLS = $(BENCH_RELEASE_FILLS)
bench-release: BENCH_FULL_RUN_PURE_MAINT = $(BENCH_RELEASE_RUN_PURE_MAINT)
bench-release: BENCH_FULL_RUN_EXTRA_MAINT = $(BENCH_RELEASE_RUN_EXTRA_MAINT)
bench-release: BENCH_QUERIES = $(BENCH_RELEASE_QUERIES)
bench-release: BENCH_EXTRA_FULL_ENTRIES = $(BENCH_RELEASE_ENTRIES)
bench-release: BENCH_EXTRA_FULL_REPS = $(BENCH_RELEASE_EXTRA_REPS)
bench-release: $(BENCH) $(BENCH_EXTRA_FULL)
	$(RUN_BENCH_FULL)

bench-release-full: BENCH_FT_FULL_OPTS = $(BENCH_RELEASE_FULL_FT_OPTS)
bench-release-full: BENCH_FULL_NAME = bench-release-full
bench-release-full: BENCH_FULL_ARCHES = $(BENCH_RELEASE_FULL_ARCHES)
bench-release-full: BENCH_FULL_ENTRIES = $(BENCH_RELEASE_FULL_ENTRIES)
bench-release-full: BENCH_FULL_FAMILIES = $(BENCH_RELEASE_FULL_FAMILIES)
bench-release-full: BENCH_FULL_EXTRA_FAMILIES = $(BENCH_RELEASE_FULL_EXTRA_FAMILIES)
bench-release-full: BENCH_FULL_OPS = $(BENCH_RELEASE_FULL_OPS)
bench-release-full: BENCH_FULL_FILLS = $(BENCH_RELEASE_FULL_FILLS)
bench-release-full: BENCH_FULL_RUN_PURE_MAINT = $(BENCH_RELEASE_FULL_RUN_PURE_MAINT)
bench-release-full: BENCH_FULL_RUN_EXTRA_MAINT = $(BENCH_RELEASE_FULL_RUN_EXTRA_MAINT)
bench-release-full: BENCH_QUERIES = $(BENCH_RELEASE_FULL_QUERIES)
bench-release-full: BENCH_EXTRA_FULL_ENTRIES = $(BENCH_RELEASE_FULL_ENTRIES)
bench-release-full: BENCH_EXTRA_FULL_REPS = $(BENCH_RELEASE_FULL_EXTRA_REPS)
bench-release-full: $(BENCH) $(BENCH_EXTRA_FULL)
	$(RUN_BENCH_FULL)

bench-full: BENCH_FT_FULL_OPTS = $(BENCH_RELEASE_FT_OPTS)
bench-full: BENCH_FULL_NAME = bench-full
bench-full: BENCH_FULL_ARCHES = $(BENCH_RELEASE_ARCHES)
bench-full: BENCH_FULL_ENTRIES = $(BENCH_RELEASE_ENTRIES)
bench-full: BENCH_FULL_FAMILIES = $(BENCH_RELEASE_FAMILIES)
bench-full: BENCH_FULL_EXTRA_FAMILIES = $(BENCH_RELEASE_EXTRA_FAMILIES)
bench-full: BENCH_FULL_OPS = $(BENCH_RELEASE_OPS)
bench-full: BENCH_FULL_FILLS = $(BENCH_RELEASE_FILLS)
bench-full: BENCH_FULL_RUN_PURE_MAINT = $(BENCH_RELEASE_RUN_PURE_MAINT)
bench-full: BENCH_FULL_RUN_EXTRA_MAINT = $(BENCH_RELEASE_RUN_EXTRA_MAINT)
bench-full: BENCH_QUERIES = $(BENCH_RELEASE_QUERIES)
bench-full: BENCH_EXTRA_FULL_ENTRIES = $(BENCH_RELEASE_ENTRIES)
bench-full: BENCH_EXTRA_FULL_REPS = $(BENCH_RELEASE_EXTRA_REPS)
bench-full: $(BENCH) $(BENCH_EXTRA_FULL)
	$(RUN_BENCH_FULL)

bench-full-serial: $(BENCH) $(BENCH_EXTRA_FULL)
bench-full-serial: BENCH_FT_FULL_OPTS = $(BENCH_RELEASE_FT_OPTS)
bench-full-serial: BENCH_FULL_NAME = bench-full-serial
bench-full-serial: BENCH_FULL_ARCHES = $(BENCH_RELEASE_ARCHES)
bench-full-serial: BENCH_FULL_CORES = $(BENCH_FULL_SERIAL_PIN)
bench-full-serial: BENCH_FULL_ENTRIES = $(BENCH_RELEASE_ENTRIES)
bench-full-serial: BENCH_FULL_FAMILIES = $(BENCH_RELEASE_FAMILIES)
bench-full-serial: BENCH_FULL_EXTRA_FAMILIES = $(BENCH_RELEASE_EXTRA_FAMILIES)
bench-full-serial: BENCH_FULL_OPS = $(BENCH_RELEASE_OPS)
bench-full-serial: BENCH_FULL_FILLS = $(BENCH_RELEASE_FILLS)
bench-full-serial: BENCH_FULL_RUN_PURE_MAINT = $(BENCH_RELEASE_RUN_PURE_MAINT)
bench-full-serial: BENCH_FULL_RUN_EXTRA_MAINT = $(BENCH_RELEASE_RUN_EXTRA_MAINT)
bench-full-serial: BENCH_QUERIES = $(BENCH_RELEASE_QUERIES)
bench-full-serial: BENCH_EXTRA_FULL_ENTRIES = $(BENCH_RELEASE_ENTRIES)
bench-full-serial: BENCH_EXTRA_FULL_REPS = $(BENCH_RELEASE_EXTRA_REPS)
bench-full-serial:
	$(RUN_BENCH_FULL)

clean:
	rm -rf $(BINDIR) *~ core core.*

-include $(DEPS)
