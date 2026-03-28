TOPDIR := $(shell cd ../.. && pwd)

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

CFLAGS_BASE  = -std=gnu11 -O$(OPTLEVEL) $(WARN) \
               -I$(TOPDIR)/include -I$(CURDIR)/include -I$(CURDIR)/src \
               -fPIC $(EXTRA_CFLAGS)

LIBDIR      ?= lib
SRCDIR       = src
INCDIR       = include
DISPATCH_SRC  = $(SRCDIR)/ft_dispatch.c

PUB_HDRS     = $(INCDIR)/flow_table.h \
               $(INCDIR)/flow4_table.h \
               $(INCDIR)/ft_table_common.h

OBJS         = $(LIBDIR)/flow4_gen.o \
               $(LIBDIR)/flow4_sse.o \
               $(LIBDIR)/flow4_avx2.o \
               $(LIBDIR)/flow4_avx512.o \
               $(LIBDIR)/ft_dispatch.o
LIB_STATIC   = $(LIBDIR)/libftable.a

.PHONY: all static clean

all: static
static: $(LIB_STATIC)

$(LIBDIR):
	mkdir -p $@

$(LIBDIR)/flow4_gen.o: $(SRCDIR)/flow4.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -DFT_ARCH_SUFFIX=_gen -c -o $@ $<

$(LIBDIR)/flow4_sse.o: $(SRCDIR)/flow4.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFT_ARCH_SUFFIX=_sse -c -o $@ $<

$(LIBDIR)/flow4_avx2.o: $(SRCDIR)/flow4.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx2 -c -o $@ $<

$(LIBDIR)/flow4_avx512.o: $(SRCDIR)/flow4.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx512 -c -o $@ $<

$(LIBDIR)/ft_dispatch.o: $(DISPATCH_SRC) | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -c -o $@ $<

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

clean:
	rm -f $(OBJS) $(LIB_STATIC) *~ core core.*
