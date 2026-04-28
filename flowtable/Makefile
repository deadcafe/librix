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
EXTRA_DISPATCH_SRC  = $(SRCDIR)/ft_dispatch_extra.c
ARCH_VARIANTS := gen sse avx2 avx512
FLOW_UNITS    := flow4 flow6 flowu
ARCH_UNITS    := $(FLOW_UNITS) ft_maintain
EXTRA_FLOW_UNITS := flow4_extra flow6_extra flowu_extra
EXTRA_ARCH_UNITS := $(EXTRA_FLOW_UNITS) ft_maintain_extra
ARCH_CFLAGS_gen :=
ARCH_CFLAGS_sse := -msse4.2
ARCH_CFLAGS_avx2 := -mavx2 -msse4.2
ARCH_CFLAGS_avx512 := -mavx512f -mavx2 -msse4.2

OBJS         = $(foreach unit,$(FLOW_UNITS), \
                   $(foreach arch,$(ARCH_VARIANTS), \
                       $(OBJDIR)/$(unit)_$(arch).o)) \
               $(OBJDIR)/ft_dispatch.o \
               $(foreach arch,$(ARCH_VARIANTS), \
                   $(OBJDIR)/ft_maintain_$(arch).o) \
               $(foreach unit,$(EXTRA_ARCH_UNITS), \
                   $(foreach arch,$(ARCH_VARIANTS), \
                       $(OBJDIR)/$(unit)_$(arch).o)) \
               $(OBJDIR)/ft_dispatch_extra.o
LIB_STATIC   = $(LIBDIR)/libftable.a
DEPS         = $(OBJS:.o=.d)

.PHONY: all static clean

all: static
static: $(LIB_STATIC)

$(OBJDIR) $(LIBDIR):
	mkdir -p $@

define COMPILE_ARCH_UNIT
$(OBJDIR)/$(1)_$(2).o: $(SRCDIR)/$(1).c | $(OBJDIR)
	$$(CC) $$(CFLAGS_BASE) $$(DEPFLAGS) -MF $$(@:.o=.d) -MT $$@ \
	    $$(ARCH_CFLAGS_$(2)) -DFT_ARCH_SUFFIX=_$(2) -c -o $$@ $$<
endef

$(foreach unit,$(ARCH_UNITS), \
  $(foreach arch,$(ARCH_VARIANTS), \
    $(eval $(call COMPILE_ARCH_UNIT,$(unit),$(arch)))))

define COMPILE_EXTRA_ARCH_UNIT
$(OBJDIR)/$(1)_$(2).o: $(SRCDIR)/$(1).c | $(OBJDIR)
	$$(CC) $$(CFLAGS_BASE) $$(DEPFLAGS) -MF $$(@:.o=.d) -MT $$@ \
	    $$(ARCH_CFLAGS_$(2)) -DFT_ARCH_SUFFIX=_$(2) -c -o $$@ $$<
endef

$(foreach unit,$(EXTRA_ARCH_UNITS), \
  $(foreach arch,$(ARCH_VARIANTS), \
    $(eval $(call COMPILE_EXTRA_ARCH_UNIT,$(unit),$(arch)))))

# dispatch
$(OBJDIR)/ft_dispatch.o: $(DISPATCH_SRC) | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(OBJDIR)/ft_dispatch_extra.o: $(EXTRA_DISPATCH_SRC) | $(OBJDIR)
	$(CC) $(CFLAGS_BASE) $(DEPFLAGS) -MF $(@:.o=.d) -MT $@ -c -o $@ $<

$(LIB_STATIC): $(OBJS) | $(LIBDIR)
	$(AR) rcs $@ $^

clean:
	rm -rf $(BUILDDIR) *~ core core.*

-include $(DEPS)
