#
# Copyright (c) 2025 deadcafe.beef@gmail.com
#

CURDIR := $(PWD)
TOPDIR := $(shell cd ../.. && pwd)
include $(TOPDIR)/mk/simd.mk

EXTRA_CFLAGS ?=
BENCH_EXTRA_CFLAGS ?= $(EXTRA_CFLAGS) -DNDEBUG
CFLAGS       = -std=gnu11 -g -O2 $(SIMD_FLAGS) -Wall -Wextra -Wvla -Werror -I$(CURDIR) -I../../include $(EXTRA_CFLAGS)
BENCH_CFLAGS = -std=gnu11 -O3 $(SIMD_FLAGS) -Wall -Wextra -Wvla -Werror -I$(CURDIR) -I../../include $(BENCH_EXTRA_CFLAGS)
DEPENDS      = .depend

TEST_TARGET  = hash64_test
TEST_SRC     = test_rix_hash64.c

BENCH_TARGET = hash64_bench
BENCH_SRC    = bench_rix_hash64.c

.PHONY: all clean depend test bench
all: $(TEST_TARGET) $(BENCH_TARGET)

$(TEST_TARGET): $(TEST_SRC) rix_hash64.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC)

$(BENCH_TARGET): $(BENCH_SRC) rix_hash64.h
	$(CC) $(BENCH_CFLAGS) -o $@ $(BENCH_SRC)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(BENCH_TARGET)
	./$(BENCH_TARGET)

clean:
	rm -f $(TEST_TARGET) $(BENCH_TARGET) $(DEPENDS) *~ core core.*

depend: $(TEST_SRC) $(BENCH_SRC) Makefile
	-@ $(CC) $(CFLAGS) -MM -MG $(TEST_SRC) $(BENCH_SRC) > $(DEPENDS)

-include $(DEPENDS)
