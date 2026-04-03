/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FT_TABLE_COMMON_H_
#define _FT_TABLE_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>

#ifndef FT_TABLE_CACHE_LINE_SIZE
#define FT_TABLE_CACHE_LINE_SIZE 64u
#endif

#ifndef FT_FLOW_FAMILY_IPV4
#define FT_FLOW_FAMILY_IPV4 4u
#endif

#ifndef FT_FLOW_FAMILY_IPV6
#define FT_FLOW_FAMILY_IPV6 6u
#endif

/*===========================================================================
 * Architecture dispatch flags
 *===========================================================================*/
#ifndef FT_ARCH_GEN
#define FT_ARCH_GEN     0u
#endif
#ifndef FT_ARCH_SSE
#define FT_ARCH_SSE     (1u << 0)
#endif
#ifndef FT_ARCH_AVX2
#define FT_ARCH_AVX2    (1u << 1)
#endif
#ifndef FT_ARCH_AVX512
#define FT_ARCH_AVX512  (1u << 2)
#endif
#ifndef FT_ARCH_AUTO
#define FT_ARCH_AUTO    (FT_ARCH_SSE | FT_ARCH_AVX2 | FT_ARCH_AVX512)
#endif

#ifndef FT_MEMBER_PTR
#define FT_MEMBER_PTR(objp, member) (&((objp)->member))
#endif

#ifndef FT_BYTE_PTR
#define FT_BYTE_PTR(ptr) ((unsigned char *)(void *)(ptr))
#endif

#ifndef FT_BYTE_CPTR
#define FT_BYTE_CPTR(ptr) ((const unsigned char *)(const void *)(ptr))
#endif

#ifndef FT_PTR_ADDR
#define FT_PTR_ADDR(ptr) ((uintptr_t)(const void *)(ptr))
#endif

#ifndef FT_PTR_IS_ALIGNED
#define FT_PTR_IS_ALIGNED(ptr, align) \
    ((FT_PTR_ADDR(ptr) & (uintptr_t)((align) - 1u)) == 0u)
#endif

#ifndef FT_BYTE_PTR_ADD
#define FT_BYTE_PTR_ADD(base, bytes) \
    (FT_BYTE_PTR(base) + (size_t)(bytes))
#endif

#ifndef FT_BYTE_CPTR_ADD
#define FT_BYTE_CPTR_ADD(base, bytes) \
    (FT_BYTE_CPTR(base) + (size_t)(bytes))
#endif

#ifndef FT_RECORD_PTR
#define FT_RECORD_PTR(base, stride, idx)                                       \
    ((idx) == RIX_NIL ? NULL : (void *)FT_BYTE_PTR_ADD(                       \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FT_RECORD_CPTR
#define FT_RECORD_CPTR(base, stride, idx)                                      \
    ((idx) == RIX_NIL ? NULL : (const void *)FT_BYTE_CPTR_ADD(                \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FT_RECORD_MEMBER_PTR
#define FT_RECORD_MEMBER_PTR(base, stride, idx, member_offset, type)           \
    ((type *)__builtin_assume_aligned(                                         \
        FT_BYTE_PTR_ADD(FT_RECORD_PTR((base), (stride), (idx)),               \
                        (member_offset)),                                      \
        _Alignof(type)))
#endif

#ifndef FT_RECORD_MEMBER_CPTR
#define FT_RECORD_MEMBER_CPTR(base, stride, idx, member_offset, type)          \
    ((const type *)__builtin_assume_aligned(                                   \
        FT_BYTE_CPTR_ADD(FT_RECORD_CPTR((base), (stride), (idx)),             \
                         (member_offset)),                                     \
        _Alignof(type)))
#endif

static inline unsigned
ft_record_index_from_member_ptr(const void *base,
                                size_t stride,
                                size_t member_offset,
                                const void *member_ptr)
{
    uintptr_t member_addr;
    uintptr_t base_addr;
    ptrdiff_t delta;

    if (member_ptr == NULL)
        return RIX_NIL;
    member_addr = (uintptr_t)member_ptr;
    base_addr = (uintptr_t)FT_BYTE_CPTR_ADD(base, member_offset);
    delta = (ptrdiff_t)(member_addr - base_addr);
    RIX_ASSERT(delta >= 0);
    RIX_ASSERT(stride != 0u);
    RIX_ASSERT(((size_t)delta % stride) == 0u);
    return (unsigned)((size_t)delta / stride) + 1u;
}

static inline unsigned
ft_roundup_pow2_u32(unsigned v)
{
    if (v <= 1u)
        return 1u;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1u;
}

/*===========================================================================
 * Bucket allocator interface
 *===========================================================================*/
typedef void *(*ft_bucket_alloc_fn)(size_t size, size_t align, void *arg);
typedef void (*ft_bucket_free_fn)(void *ptr, size_t size, size_t align,
                                  void *arg);

struct ft_bucket_allocator {
    ft_bucket_alloc_fn alloc;
    ft_bucket_free_fn  free;
    void              *arg;
};

/*===========================================================================
 * Protocol-independent types (shared by all flow variants)
 *===========================================================================*/

struct ft_table_result {
    uint32_t entry_idx;
};

enum ft_add_policy {
    FT_ADD_IGNORE = 0u,
    FT_ADD_UPDATE = 1u,
};

struct ft_table_config {
    unsigned start_nb_bk;
    unsigned max_nb_bk;
    unsigned grow_fill_pct;
    struct ft_bucket_allocator bucket_alloc;
};

struct ft_table_stats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t adds;
    uint64_t add_existing;
    uint64_t add_failed;
    uint64_t dels;
    uint64_t del_miss;
    uint64_t grow_execs;
    uint64_t grow_failures;
    uint64_t reserve_calls;
};

/**
 * @brief One-time CPU detection and SIMD dispatch selection.
 *
 * Call once at startup before any table operations.
 *
 * @param arch_enable  Bitmask of FT_ARCH_* flags, or FT_ARCH_AUTO.
 */
void ft_arch_init(unsigned arch_enable);

#endif /* _FT_TABLE_COMMON_H_ */
