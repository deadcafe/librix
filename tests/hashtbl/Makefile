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
DEPFLAGS     = -MMD -MP
DEPS         = $(TEST_TARGET).d $(BENCH_TARGET).d $(SLOT_SWEEP_TARGET).d

TEST_TARGET  = hash_test
TEST_SRC     = test_rix_hash.c

BENCH_TARGET = hash_bench
BENCH_SRC    = bench_rix_hash.c
SLOT_SWEEP_TARGET = slot_query_sweep
SLOT_SWEEP_SRC = bench_slot_query_sweep.c

.PHONY: all clean test bench
all: $(TEST_TARGET) $(BENCH_TARGET) $(SLOT_SWEEP_TARGET)

$(TEST_TARGET): $(TEST_SRC) rix_hash.h
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(TEST_SRC)

$(BENCH_TARGET): $(BENCH_SRC) rix_hash.h
	$(CC) $(BENCH_CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_SRC)

$(SLOT_SWEEP_TARGET): $(SLOT_SWEEP_SRC) rix_hash.h
	$(CC) $(BENCH_CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(SLOT_SWEEP_SRC)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(BENCH_TARGET)
	./$(BENCH_TARGET)

clean:
	rm -f $(TEST_TARGET) $(BENCH_TARGET) $(SLOT_SWEEP_TARGET) $(DEPS) *~ core core.*

-include $(DEPS)
