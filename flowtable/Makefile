TOPDIR := $(shell cd .. && pwd)

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
               -I$(TOPDIR)/include \
               -I$(CURDIR)/include -I$(CURDIR)/src \
               -fPIC $(EXTRA_CFLAGS)
DEPFLAGS     = -MMD -MP

BUILDDIR    ?= build
OBJDIR      ?= $(BUILDDIR)/obj
LIBDIR      ?= $(BUILDDIR)/lib
SRCDIR       = src
INCDIR       = include
DISPATCH_SRC  = $(SRCDIR)/ft_dispatch.c

OBJS         = $(OBJDIR)/flow4_gen.o \
               $(OBJDIR)/flow4_sse.o \
               $(OBJDIR)/flow4_avx2.o \
               $(OBJDIR)/flow4_avx512.o \
               $(OBJDIR)/flow6_gen.o \
               $(OBJDIR)/flow6_sse.o \
               $(OBJDIR)/flow6_avx2.o \
               $(OBJDIR)/flow6_avx512.o \
               $(OBJDIR)/flowu_gen.o \
               $(OBJDIR)/flowu_sse.o \
               $(OBJDIR)/flowu_avx2.o \
               $(OBJDIR)/flowu_avx512.o \
               $(OBJDIR)/ft_dispatch.o \
               $(OBJDIR)/ft_maintain_gen.o \
               $(OBJDIR)/ft_maintain_sse.o \
               $(OBJDIR)/ft_maintain_avx2.o \
               $(OBJDIR)/ft_maintain_avx512.o
LIB_STATIC   = $(LIBDIR)/libftable.a
DEPS         = $(OBJS:.o=.d)

.PHONY: all static clean

all: static
static: $(LIB_STATIC)

$(OBJDIR) $(LIBDIR):
	mkdir -p $@

# flow4
$(OBJDIR)/flow4_gen.o: $(SRCDIR)/flow4.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -DFT_ARCH_SUFFIX=_gen -c -o $@ $<

$(OBJDIR)/flow4_sse.o: $(SRCDIR)/flow4.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -msse4.2 -DFT_ARCH_SUFFIX=_sse -c -o $@ $<

$(OBJDIR)/flow4_avx2.o: $(SRCDIR)/flow4.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx2 -c -o $@ $<

$(OBJDIR)/flow4_avx512.o: $(SRCDIR)/flow4.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx512f -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx512 -c -o $@ $<

# flow6
$(OBJDIR)/flow6_gen.o: $(SRCDIR)/flow6.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -DFT_ARCH_SUFFIX=_gen -c -o $@ $<

$(OBJDIR)/flow6_sse.o: $(SRCDIR)/flow6.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -msse4.2 -DFT_ARCH_SUFFIX=_sse -c -o $@ $<

$(OBJDIR)/flow6_avx2.o: $(SRCDIR)/flow6.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx2 -c -o $@ $<

$(OBJDIR)/flow6_avx512.o: $(SRCDIR)/flow6.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx512f -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx512 -c -o $@ $<

# flowu
$(OBJDIR)/flowu_gen.o: $(SRCDIR)/flowu.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -DFT_ARCH_SUFFIX=_gen -c -o $@ $<

$(OBJDIR)/flowu_sse.o: $(SRCDIR)/flowu.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -msse4.2 -DFT_ARCH_SUFFIX=_sse -c -o $@ $<

$(OBJDIR)/flowu_avx2.o: $(SRCDIR)/flowu.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx2 -c -o $@ $<

$(OBJDIR)/flowu_avx512.o: $(SRCDIR)/flowu.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx512f -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx512 -c -o $@ $<

# dispatch
$(OBJDIR)/ft_dispatch.o: $(DISPATCH_SRC) | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -c -o $@ $<

# maintain (protocol-free, per-arch)
$(OBJDIR)/ft_maintain_gen.o: $(SRCDIR)/ft_maintain.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -DFT_ARCH_SUFFIX=_gen -c -o $@ $<

$(OBJDIR)/ft_maintain_sse.o: $(SRCDIR)/ft_maintain.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -msse4.2 -DFT_ARCH_SUFFIX=_sse -c -o $@ $<

$(OBJDIR)/ft_maintain_avx2.o: $(SRCDIR)/ft_maintain.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx2 -c -o $@ $<

$(OBJDIR)/ft_maintain_avx512.o: $(SRCDIR)/ft_maintain.c | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -mavx512f -mavx2 -msse4.2 -DFT_ARCH_SUFFIX=_avx512 -c -o $@ $<

$(LIB_STATIC): $(OBJS) | $(LIBDIR)
	$(AR) rcs $@ $^

clean:
	rm -rf $(BUILDDIR) *~ core core.*

-include $(DEPS)
