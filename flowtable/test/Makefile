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
BENCH_FT_FULL_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7
BENCH_QUERIES := 1 8 32 256
BENCH_FULL_ARCHES ?= gen sse avx2 avx512
BENCH_LIGHT_FAMILY := flow4
BENCH_FULL_FAMILIES := flow4 flow6 flowu
BENCH_LIGHT_FILL := 60
BENCH_FULL_FILLS := 40 60 75 80 90
BENCH_GROW_FILL := 60
BENCH_MAINT_FILL := 50
BENCH_EXTRA_FULL_ENTRIES ?= 1048576
BENCH_EXTRA_FULL_REPS ?= 7

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

.PHONY: all help test test-pure test-extra test-extra-arch test-parity test-ctrl bench bench-light bench-full bench-extra bench-extra-full bench-sweep bench-zoned bench-ctrl bench-ctrl-gen bench-ctrl-sse bench-ctrl-avx2 bench-ctrl-avx512 bench-ctrl-build-all sample sim clean libft FORCE

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
	  '  bench-full   Run the full benchmark sweep (flow4/6/u query, maintain, grow)' \
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
		for family in flow4_extra flow6_extra flowu_extra; do \
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

bench-full: $(BENCH) $(BENCH_EXTRA_FULL)
	@echo "== full ftable bench (all supported arch variants, query sweep + maintain + grow) =="
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
		for family in $(BENCH_FULL_FAMILIES); do \
			for fill in $(BENCH_FULL_FILLS); do \
				for q in $(BENCH_QUERIES); do \
					echo "== full ftable bench ($$arch, $$family, q=$$q, 1M @ fill=$$fill%) =="; \
					$(BENCH) $(BENCH_FT_FULL_OPTS) --arch $$arch --query $$q $$family 1048576 $$fill; \
				done; \
			done; \
			echo "== full ftable maintain bench ($$arch, $$family, 1M @ fill=$(BENCH_MAINT_FILL)%) =="; \
			$(BENCH) $(BENCH_FT_FULL_OPTS) --arch $$arch --maint $$family 1048576 $(BENCH_MAINT_FILL); \
			echo "== full ftable grow bench ($$arch, $$family, 1M @ fill=$(BENCH_GROW_FILL)%) =="; \
			$(BENCH) $(BENCH_FT_FULL_OPTS) --arch $$arch --grow $$family 1048576 $(BENCH_GROW_FILL); \
		done; \
	done
	@$(MAKE) bench-extra-full

clean:
	rm -rf $(BINDIR) *~ core core.*

-include $(DEPS)
