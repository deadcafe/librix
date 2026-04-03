TOPDIR  := $(shell cd ../../.. && pwd)
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

CFLAGS       = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(TOPDIR)/samples/include \
               -I$(FTDIR)/include \
               -mavx2 -msse4.2 $(EXTRA_CFLAGS)
DEPFLAGS     = -MMD -MP

TARGET       = ft_test
BENCH        = ft_bench
SRC          = test_flow_table.c
BENCH_SRC    = bench_flow_table.c
LIBFT        = $(FTDIR)/lib/libftable.a
DEPS         = $(TARGET).d $(BENCH).d

.PHONY: all test bench bench-light bench-full clean libft

all: test bench-light

$(TARGET): $(SRC) $(LIBFT)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(SRC) $(LIBFT)

$(BENCH): $(BENCH_SRC) $(LIBFT)
	$(CC) $(CFLAGS) -DNDEBUG $(DEPFLAGS) -MF $@.d -MT $@ -o $@ $(BENCH_SRC) $(LIBFT)

$(LIBFT):
	$(MAKE) -C $(FTDIR) static

libft: $(LIBFT)

test: $(TARGET)
	./$(TARGET)

bench: $(BENCH)

bench-light: $(BENCH)
	@echo "== representative light ftable bench (1M, query=256, cold bulk datapath @ fill=50%) =="
	./$(BENCH) $(BENCH_FT_OPTS) flow4 1048576 50
	./$(BENCH) $(BENCH_FT_OPTS) flow6 1048576 50
	./$(BENCH) $(BENCH_FT_OPTS) flowu 1048576 50

bench: bench-light

bench-full: $(BENCH)
	@echo "== full ftable bench (1M baseline, cold bulk datapath sweep + grow@60%) =="
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow4 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow4 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow4 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --grow flow4 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flow6 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flow6 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flow6 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --grow flow6 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 40
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 50
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 60
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 80
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 1 flowu 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 32 flowu 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --query 256 flowu 1048576 90
	./$(BENCH) $(BENCH_FT_FULL_OPTS) --grow flowu 1048576 60

clean:
	rm -f $(TARGET) $(BENCH) $(DEPS) *.o *~ core core.*

-include $(DEPS)
