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
BENCH_FT_OPTS ?= --pin-core 2 --raw-repeat 11 --keep-n 7

CFLAGS       = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(FTDIR)/include \
               -mavx2 -msse4.2 $(EXTRA_CFLAGS)

TARGET       = ft_test
BENCH        = ft_bench
SRC          = test_flow_table.c
BENCH_SRC    = bench_flow_table.c
LIBFT        = $(FTDIR)/lib/libftable.a

.PHONY: all test bench clean libft

all: $(TARGET) $(BENCH)

$(TARGET): $(SRC) libft
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBFT)

$(BENCH): $(BENCH_SRC) libft
	$(CC) $(CFLAGS) -DNDEBUG -o $@ $(BENCH_SRC) $(LIBFT)

libft:
	$(MAKE) -C $(FTDIR) static

test: $(TARGET)
	./$(TARGET)

bench: $(BENCH)
	./$(BENCH) $(BENCH_FT_OPTS) datapath
	./$(BENCH) $(BENCH_FT_OPTS) grow

clean:
	rm -f $(TARGET) $(BENCH) *.o *~ core core.*
