#
# mk/cc.mk - compiler selection with optional ccache
#
# Usage:
#   make                      → auto-detect ccache, use default cc
#   make CC=clang             → use clang (ccache auto-detected)
#   make USE_CCACHE=0         → disable ccache
#   make USE_CCACHE=0 CC=gcc  → gcc without ccache
#

USE_CCACHE ?= auto

ifeq ($(USE_CCACHE),auto)
  _CCACHE := $(shell command -v ccache 2>/dev/null)
else ifeq ($(USE_CCACHE),1)
  _CCACHE := ccache
else
  _CCACHE :=
endif

ifneq ($(_CCACHE),)
  ifeq ($(findstring ccache,$(CC)),)
    override CC := $(_CCACHE) $(CC)
  endif
endif

# Strip CC from command-line overrides so sub-makes use the exported value
MAKEOVERRIDES := $(filter-out CC=%,$(MAKEOVERRIDES))
