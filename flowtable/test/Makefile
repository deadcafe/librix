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
BENCH_LIGHT_FAMILY := flow4
BENCH_FULL_FAMILIES := flow4 flow6 flowu
BENCH_LIGHT_FILL := 60
BENCH_FULL_FILLS := 40 50 60 80 90
BENCH_GROW_FILL := 60
BENCH_MAINT_FILL := 50

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

.PHONY: all help test bench bench-light bench-full clean libft FORCE

all: test

help:
	@printf '%s\n' \
	  'flowtable/test targets:' \
	  '  help         Show this help' \
	  '  test         Build and run functional tests' \
	  '  bench        Run the light benchmark set (flow4 only, q=1/8/32/256 @ fill=60%)' \
	  '  bench-light  Same as bench' \
	  '  bench-full   Run the full benchmark sweep (flow4/6/u query, maintain, grow)' \
	  '  clean        Remove generated binaries and dependency files' \
	  '' \
	  'Useful variables:' \
	  '  BENCH_FT_OPTS       Options for bench / bench-light' \
	  '  BENCH_FT_FULL_OPTS  Options for bench-full'

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
	@for q in $(BENCH_QUERIES); do \
		echo "== light ftable bench ($(BENCH_LIGHT_FAMILY), q=$$q, 1M @ fill=$(BENCH_LIGHT_FILL)%) =="; \
		$(BENCH) $(BENCH_FT_OPTS) --query $$q $(BENCH_LIGHT_FAMILY) 1048576 $(BENCH_LIGHT_FILL); \
	done

bench: bench-light

bench-full: $(BENCH)
	@echo "== full ftable bench (query sweep + maintain + grow) =="
	@for family in $(BENCH_FULL_FAMILIES); do \
		for fill in $(BENCH_FULL_FILLS); do \
			for q in $(BENCH_QUERIES); do \
				echo "== full ftable bench ($$family, q=$$q, 1M @ fill=$$fill%) =="; \
				$(BENCH) $(BENCH_FT_FULL_OPTS) --query $$q $$family 1048576 $$fill; \
			done; \
		done; \
		echo "== full ftable maintain bench ($$family, 1M @ fill=$(BENCH_MAINT_FILL)%) =="; \
		$(BENCH) $(BENCH_FT_FULL_OPTS) --maint $$family 1048576 $(BENCH_MAINT_FILL); \
		echo "== full ftable grow bench ($$family, 1M @ fill=$(BENCH_GROW_FILL)%) =="; \
		$(BENCH) $(BENCH_FT_FULL_OPTS) --grow $$family 1048576 $(BENCH_GROW_FILL); \
	done

clean:
	rm -rf $(BINDIR) *~ core core.*

-include $(DEPS)
