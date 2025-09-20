TOPDIR := $(shell cd ../.. && pwd)

# Detect GCC vs Clang
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
               -I$(TOPDIR)/include -I$(CURDIR)/include -I$(CURDIR)/src -fPIC \
               $(EXTRA_CFLAGS)

SRCDIR       = src
INCDIR       = include
LIBDIR       = lib

VARIANTS     = flow4 flow6 flowu

PUB_HDRS     = $(INCDIR)/flow_cache.h \
               $(INCDIR)/flow4_cache.h \
               $(INCDIR)/flow6_cache.h \
               $(INCDIR)/flowu_cache.h \
               $(INCDIR)/fc_ops.h

# Per-arch objects: <variant>_<arch>.o
ARCH_OBJS    = $(foreach v,$(VARIANTS), \
                 $(LIBDIR)/$(v)_gen.o \
                 $(LIBDIR)/$(v)_sse.o \
                 $(LIBDIR)/$(v)_avx2.o \
                 $(LIBDIR)/$(v)_avx512.o)

# Dispatch wrapper
DISPATCH_OBJ = $(LIBDIR)/fc_dispatch.o

OBJS         = $(ARCH_OBJS) $(DISPATCH_OBJ)
LIB_STATIC   = $(LIBDIR)/libfcache.a
LIB_SHARED   = $(LIBDIR)/libfcache.so
LIB_SONAME   = libfcache.so.1

PREFIX       ?= /usr/local

.PHONY: all static shared clean install

all: static shared
static: $(LIB_STATIC)
shared: $(LIB_SHARED)

$(LIBDIR):
	mkdir -p $@

#---------------------------------------------------------------------------
# Arch-specific compilation rules
#---------------------------------------------------------------------------
# GEN (portable, no SIMD flags)
$(LIBDIR)/%_gen.o: $(SRCDIR)/%.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -DFC_ARCH_SUFFIX=_gen -c -o $@ $<

# SSE4.2
$(LIBDIR)/%_sse.o: $(SRCDIR)/%.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -msse4.2 -DFC_ARCH_SUFFIX=_sse -c -o $@ $<

# AVX2
$(LIBDIR)/%_avx2.o: $(SRCDIR)/%.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx2 -c -o $@ $<

# AVX-512
$(LIBDIR)/%_avx512.o: $(SRCDIR)/%.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx512f -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx512 -c -o $@ $<

#---------------------------------------------------------------------------
# Dispatch wrapper (no arch flags, no FC_ARCH_SUFFIX)
#---------------------------------------------------------------------------
$(DISPATCH_OBJ): $(SRCDIR)/fc_dispatch.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -c -o $@ $<

#---------------------------------------------------------------------------
# Library targets
#---------------------------------------------------------------------------
$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(LIB_SHARED): $(OBJS)
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -o $@ $^

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) *~ core core.*

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib $(PREFIX)/include/fcache
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 $(PUB_HDRS) $(PREFIX)/include/fcache/
