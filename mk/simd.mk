#
# mk/simd.mk - SIMD level selection for hash table compilation
#
# Include from sub-Makefiles that build hash-variant code.
# Override on the command line: make SIMD=avx512
#
# SIMD=gen     → -msse4.2 only  (Generic scalar search; CRC32C hash retained)
# SIMD=avx2    → -mavx2 -msse4.2                     (default)
# SIMD=avx512  → -mavx512f -mavx2 -msse4.2
#
# Note: the compiler flag controls which SIMD code is compiled in.
# rix_hash_arch_init() still selects at runtime among compiled variants.
# For example, SIMD=avx512 with rix_hash_arch_init(RIX_HASH_ARCH_AVX2)
# compiles AVX-512 but caps runtime selection at AVX2.
#

SIMD ?= avx2

ifeq ($(SIMD),gen)
  SIMD_FLAGS := -msse4.2
else ifeq ($(SIMD),avx2)
  SIMD_FLAGS := -mavx2 -msse4.2
else ifeq ($(SIMD),avx512)
  SIMD_FLAGS := -mavx512f -mavx2 -msse4.2
else
  $(error Unknown SIMD='$(SIMD)'. Valid values: gen  avx2  avx512)
endif
