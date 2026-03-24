#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#
# samples/test/Makefile - fcache test, benchmark, and matrix runner
#

TOPDIR  := $(shell cd ../.. && pwd)
FCDIR   := $(shell cd ../fcache && pwd)

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
BENCH_EXTRA_CFLAGS ?= $(EXTRA_CFLAGS) -DNDEBUG
CFLAGS       = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(FCDIR)/include \
               $(EXTRA_CFLAGS)
BENCH_CFLAGS = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(FCDIR)/include \
               $(BENCH_EXTRA_CFLAGS)

VARIANT     ?= flow4

TARGET       = fc_test
BENCH        = fc_bench
SRC          = test_flow_cache.c
BENCH_SRC    = bench_fc.c
BENCH_HDRS   = bench_fc_common.h bench_fc_body.h
LIBFC        = $(FCDIR)/lib/libfcache.a

.PHONY: all test bench-build bench bench-full matrix matrix-full clean

all: $(TARGET) $(BENCH)

$(TARGET): $(SRC) $(LIBFC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBFC)

$(BENCH): $(BENCH_SRC) $(BENCH_HDRS) $(LIBFC)
	$(CC) $(BENCH_CFLAGS) -o $@ $(BENCH_SRC) $(LIBFC)

$(LIBFC):
	$(MAKE) -C $(FCDIR) static

test: $(TARGET)
	./$(TARGET)

bench-build:
	$(MAKE) -C $(FCDIR) clean static EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)"
	$(MAKE) clean
	$(MAKE) $(BENCH) BENCH_EXTRA_CFLAGS="$(BENCH_EXTRA_CFLAGS)"

bench: bench-build
	./$(BENCH) datapath
	./$(BENCH) maint_partial
	./run_fc_bench_matrix.sh all quick

bench-full: bench-build
	./$(BENCH) datapath
	./$(BENCH) maint_partial
	./run_fc_bench_matrix.sh all full

matrix: bench-build
	./run_fc_bench_matrix.sh $(VARIANT) quick

matrix-full: bench-build
	./run_fc_bench_matrix.sh $(VARIANT) full

clean:
	rm -f $(TARGET) $(BENCH) *.o *~ core core.*
