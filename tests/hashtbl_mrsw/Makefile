#
# Copyright (c) 2026 deadcafe.beef@gmail.com
#

CURDIR := $(PWD)
TOPDIR := $(shell cd ../.. && pwd)
include $(TOPDIR)/mk/simd.mk

EXTRA_CFLAGS ?=
CFLAGS       = -std=gnu11 -g -O2 $(SIMD_FLAGS) -Wall -Wextra -Wvla -Werror -I../../include $(EXTRA_CFLAGS) -pthread
BENCH_CFLAGS = -std=gnu11 -g -O3 $(SIMD_FLAGS) -Wall -Wextra -Wvla -Werror -I../../include $(EXTRA_CFLAGS) -pthread
DEPFLAGS     = -MMD -MP
DEPS         = $(TEST_TARGET).d $(BENCH_TARGET).d

TEST_TARGET  = hash_mrsw_test
TEST_SRC     = test_rix_hash_mrsw.c

BENCH_TARGET = bench_pure_vs_mrsw
BENCH_SRC    = bench_pure_vs_mrsw.c

.PHONY: all clean test bench avx512
all: $(TEST_TARGET) $(BENCH_TARGET)

$(TEST_TARGET): $(TEST_SRC) ../../include/rix/rix_hash_mr.h
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(TEST_SRC)

$(BENCH_TARGET): $(BENCH_SRC) ../../include/rix/rix_hash_mr.h ../../include/rix/rix_hash.h
	$(CC) $(BENCH_CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_SRC)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

bench: $(BENCH_TARGET)
	./$(BENCH_TARGET)

avx512:
	$(MAKE) SIMD=avx512 all

clean:
	rm -f $(TEST_TARGET) $(BENCH_TARGET) $(DEPS) *~ core core.*

-include $(DEPS)
