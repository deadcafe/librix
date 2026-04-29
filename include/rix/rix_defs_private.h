/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 */

/*
 * INTERNAL USE ONLY.
 * This is a private implementation header of the rix library.
 * Do not include it directly; it is pulled in automatically by
 * rix_queue.h, rix_hash.h, rix_tree.h, and rix_hash_key.h.
 */

#ifndef _RIX_DEFS_PRIVATE_H_
#  define _RIX_DEFS_PRIVATE_H_

#  include <assert.h>
#  include <limits.h>
#  include <stddef.h>
#  include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#  ifndef RIX_CACHE_LINE_SIZE
#    define RIX_CACHE_LINE_SIZE 64u
#  endif

#  ifdef __cplusplus
extern "C" {
#  endif

/* --------------------------------------------------------------------------
 * Index basics
 * -------------------------------------------------------------------------- */
#  if defined(_MSC_VER)
#    define RIX_FORCE_INLINE __forceinline
#  elif defined(__GNUC__) || defined(__clang__)
#    define RIX_FORCE_INLINE __attribute__((always_inline)) inline
#  else
#    define RIX_FORCE_INLINE inline
#  endif

#  ifndef RIX_LIKELY
#    if defined(__GNUC__) || defined(__clang__)
#      define RIX_LIKELY(x)   __builtin_expect(!!(x), 1)
#      define RIX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#    else
#      define RIX_LIKELY(x)   (x)
#      define RIX_UNLIKELY(x) (x)
#    endif
#  endif

#  ifndef RIX_UNUSED
#    if defined(__GNUC__) || defined(__clang__)
#      define RIX_UNUSED __attribute__((unused))
#    else
#      define RIX_UNUSED
#    endif
#  endif

/* 0 is the sentinel NIL index (end marker). */
#  ifndef RIX_NIL
#    define RIX_NIL ((unsigned)0u)
#  endif

/* Return non-zero if i is RIX_NIL. */
#  define RIX_IDX_IS_NIL(i) ((unsigned)(i) == RIX_NIL)

/* Return non-zero if i is within [1 .. cap]. RIX_NIL is NOT counted as valid.
 */
#  define RIX_IDX_IS_VALID(i, cap)                                            \
    ((unsigned)(i) >= 1u && (unsigned)(i) <= (unsigned)(cap))

/* Convert element pointer (p) to 1-origin index relative to array base (base).
 * If p == NULL, return RIX_NIL (0).
 * REQUIREMENT: (p) and (base) must be pointers to the SAME element type.
 */
#  define RIX_IDX_FROM_PTR(base, p)                                           \
    ((unsigned)((p) ? (unsigned)((p) - (base)) + 1u : (unsigned)RIX_NIL))

/* Convert 1-origin index (i) back to element pointer using array base (base).
 * If i == RIX_NIL (0), return NULL.
 */
#  define RIX_PTR_FROM_IDX(base, i)                                           \
    (((unsigned)(i) != RIX_NIL) ? ((base) + ((unsigned)(i) - 1u)) : NULL)

/* Internal helper when the caller has already proven i != RIX_NIL.
 * Keeping this separate avoids GCC false positives from the nullable macro
 * after aggressive inlining through generated wrappers. */
static RIX_UNUSED RIX_FORCE_INLINE void *
rix_ptr_from_idx_valid_(void *base, size_t elem_size, unsigned i)
{
    assert(i != RIX_NIL);
#    if defined(__GNUC__) || defined(__clang__)
    if (RIX_UNLIKELY(i == RIX_NIL))
        __builtin_unreachable();
#    endif
    return (void *)((unsigned char *)base + (size_t)(i - 1u) * elem_size);
}

/* Convert 1-origin index to 0-origin offset (undefined for RIX_NIL). */
#  define RIX_IDX_TO_OFF0(i) ((size_t)((unsigned)(i) - 1u))

/* --------------------------------------------------------------------------
 * Diagnostics & assertions
 * -------------------------------------------------------------------------- */

/* Runtime assertion (enabled when NDEBUG is not defined). */
#  ifndef RIX_ASSERT
#    define RIX_ASSERT(cond) assert(cond)
#  endif

/* Compile-time assertion wrapper (C11). */
#  ifndef RIX_STATIC_ASSERT
#    define RIX_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#  endif

/* Optional helper to validate pointer/index round-trip in debug builds.
 * Example:
 *   RIX_ASSERT_RELATION(base, idx, ptr);
 */
#  define RIX_ASSERT_RELATION(base, idx, ptr)                                 \
    do {                                                                      \
        const unsigned __rix_idx_tmp = (unsigned)(idx);                       \
        const void *__rix_ptr_tmp = (const void *)(ptr);                      \
        RIX_ASSERT((__rix_ptr_tmp == NULL) == RIX_IDX_IS_NIL(__rix_idx_tmp)); \
        RIX_ASSERT(RIX_PTR_FROM_IDX((base), __rix_idx_tmp) == (ptr));         \
        RIX_ASSERT(RIX_IDX_FROM_PTR((base), (ptr)) == __rix_idx_tmp);         \
    } while (0)

/* Optimizer hint: promise that ptr is non-NULL.
 * Use where a bitmap guarantees the slot is occupied but GCC cannot
 * prove it after deep inlining (e.g. insert duplicate-scan loop).
 * -O2: compiles to nothing.  -O0: dead branch (never taken). */
#  define RIX_ASSUME_NONNULL(ptr)                                             \
    do {                                                                      \
        if (__builtin_expect((ptr) == NULL, 0))                               \
            __builtin_unreachable();                                          \
    } while (0)

/* --------------------------------------------------------------------------
 * Small utilities (pure C11, no compiler-specific builtins)
 * -------------------------------------------------------------------------- */

#  ifndef RIX_MIN
#    define RIX_MIN(a, b) (((a) < (b)) ? (a) : (b))
#  endif
#  ifndef RIX_MAX
#    define RIX_MAX(a, b) (((a) > (b)) ? (a) : (b))
#  endif

/* Number of elements in a static array. */
#  ifndef RIX_COUNT_OF
#    define RIX_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#  endif

/* Byte offset of a field within a struct. */
#  ifndef RIX_OFFSET_OF
#    define RIX_OFFSET_OF(type, member) offsetof(type, member)
#  endif

/* container_of: get struct pointer from pointer to its member. */
#  ifndef RIX_CONTAINER_OF
#    define RIX_CONTAINER_OF(ptr, type, member)                               \
    ((type *)__builtin_assume_aligned(                                        \
        (void *)((char *)(ptr) - RIX_OFFSET_OF(type, member)), _Alignof(type)))
#  endif

/* Swap two variables of the same type. */
#  ifndef RIX_SWAP
#    define RIX_SWAP(T, a, b)                                                 \
    do {                                                                      \
        T __rix_tmp__ = (a);                                                  \
        (a) = (b);                                                            \
        (b) = __rix_tmp__;                                                    \
    } while (0)
#  endif

/* Clamp x into [lo, hi]. */
#  ifndef RIX_CLAMP
#    define RIX_CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))
#  endif

/* Align n up/down to the nearest multiple of a (a must be power of two). */
#  ifndef RIX_ALIGN_DOWN
#    define RIX_ALIGN_DOWN(n, a) (((size_t)(n)) & ~((size_t)(a) - 1u))
#  endif
#  ifndef RIX_ALIGN_UP
#    define RIX_ALIGN_UP(n, a) RIX_ALIGN_DOWN((size_t)(n) + ((size_t)(a) - 1u), (a))
#  endif

/* --------------------------------------------------------------------------
 * Build-time sanity checks (optional)
 * -------------------------------------------------------------------------- */

/* Ensure unsigned is at least 32-bit (typical for index domain). */
RIX_STATIC_ASSERT(sizeof(unsigned) >= 4, "unsigned must be >= 32 bits");

/* Ensure pointer subtraction yields ptrdiff_t (C guarantee). */
RIX_STATIC_ASSERT(sizeof(ptrdiff_t) >= sizeof(void *), "ptrdiff_t must be >= pointer size");

#  ifdef __cplusplus
}
#  endif

#endif /* _RIX_DEFS_PRIVATE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
