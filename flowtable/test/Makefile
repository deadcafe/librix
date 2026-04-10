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
BENCH_FT_SMALL_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7
BENCH_FT_SMALL_PERF_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7 --perf-cycles
BENCH_FT_SMALL_DEBUG_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7 --show-samples
BENCH_FT_FULL_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7

CFLAGS       = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include \
               -I$(FTDIR)/include \
               -mavx2 -msse4.2 $(EXTRA_CFLAGS)
DEPFLAGS     = -MMD -MP

BUILDDIR     ?= $(FTDIR)/build
BINDIR       ?= $(BUILDDIR)/bin
TARGET_NAME   = ft_test
BENCH_NAME    = ft_bench
TARGET        = $(BINDIR)/$(TARGET_NAME)
BENCH         = $(BINDIR)/$(BENCH_NAME)
SRC          = test_flow_table.c
BENCH_SRC    = bench_flow_table.c
LIBFT        = $(FTDIR)/build/lib/libftable.a
DEPS         = $(TARGET).d $(BENCH).d

.PHONY: all test bench bench-light bench-small bench-small-debug bench-small-perf bench-full bench-maint clean libft FORCE

all: test bench-light

$(BINDIR):
	mkdir -p $@

$(TARGET): $(SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(SRC) $(LIBFT)

$(BENCH): $(BENCH_SRC) $(LIBFT) | $(BINDIR)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_SRC) $(LIBFT)

# Always enter the ftable sub-make so its own .d files can decide whether
# header changes require rebuilding the library objects/archive.
$(LIBFT): FORCE
	$(MAKE) -C $(FTDIR) static

FORCE:

libft: $(LIBFT)

test: $(TARGET)
	$(TARGET)

bench-light: $(BENCH)
	@echo "== light ftable bench (flow4, q=1, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 1 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=2, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 2 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=4, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 4 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=8, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 8 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=16, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 16 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=32, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 32 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=64, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 64 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=128, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 128 flow4 1048576 60
	@echo "== light ftable bench (flow4, q=256, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_OPTS) --query 256 flow4 1048576 60

bench-small: $(BENCH)
	@echo "== small ftable bench (flow4, q=1, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 1 flow4 1048576 60
	@echo "== small ftable bench (flow4, q=2, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 2 flow4 1048576 60
	@echo "== small ftable bench (flow4, q=3, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 3 flow4 1048576 60
	@echo "== small ftable bench (flow4, q=4, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 4 flow4 1048576 60
	@echo "== small ftable bench (flow4, q=6, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 6 flow4 1048576 60
	@echo "== small ftable bench (flow4, q=8, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 8 flow4 1048576 60
	@echo "== small ftable bench (flow4, q=12, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 12 flow4 1048576 60
	@echo "== small ftable bench (flow4, q=16, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_OPTS) --query 16 flow4 1048576 60

bench-small-debug: $(BENCH)
	@echo "== small debug ftable bench (flow4, q=1, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_DEBUG_OPTS) --query 1 flow4 1048576 60
	@echo "== small debug ftable bench (flow4, q=2, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_DEBUG_OPTS) --query 2 flow4 1048576 60
	@echo "== small debug ftable bench (flow4, q=4, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_DEBUG_OPTS) --query 4 flow4 1048576 60
	@echo "== small debug ftable bench (flow4, q=8, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_DEBUG_OPTS) --query 8 flow4 1048576 60
	@echo "== small debug ftable bench (flow4, q=16, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_DEBUG_OPTS) --query 16 flow4 1048576 60

bench-small-perf: $(BENCH)
	@echo "== small perf ftable bench (flow4, q=1, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_PERF_OPTS) --query 1 flow4 1048576 60
	@echo "== small perf ftable bench (flow4, q=2, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_PERF_OPTS) --query 2 flow4 1048576 60
	@echo "== small perf ftable bench (flow4, q=4, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_PERF_OPTS) --query 4 flow4 1048576 60
	@echo "== small perf ftable bench (flow4, q=8, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_PERF_OPTS) --query 8 flow4 1048576 60
	@echo "== small perf ftable bench (flow4, q=16, 1M @ fill=60%) =="
	$(BENCH) $(BENCH_FT_SMALL_PERF_OPTS) --query 16 flow4 1048576 60

bench: bench-light

bench-maint: $(BENCH)
	@echo "== ftable maintain bench (1M, fill=50%, perf metrics) =="
	$(BENCH) $(BENCH_FT_OPTS) --maint flow4 1048576 50
	$(BENCH) $(BENCH_FT_OPTS) --maint flow6 1048576 50
	$(BENCH) $(BENCH_FT_OPTS) --maint flowu 1048576 50

bench-full: $(BENCH)
	@echo "== full ftable bench (1M baseline, cold bulk datapath sweep + grow@60%) =="
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --grow flow4 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --grow flow6 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 40
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 50
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 60
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 80
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 90
	$(BENCH) $(BENCH_FT_FULL_OPTS) --grow flowu 1048576 60

clean:
	rm -rf $(BINDIR) *~ core core.*

-include $(DEPS)
