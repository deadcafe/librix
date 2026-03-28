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

#ifndef FT_FLOW_FAMILY_IPV4
#define FT_FLOW_FAMILY_IPV4 4u
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

#endif /* _FT_TABLE_COMMON_H_ */
