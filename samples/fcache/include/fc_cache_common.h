/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FC_CACHE_COMMON_H_
#define _FC_CACHE_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifndef FC_CACHE_BULK_CTX_COUNT
#define FC_CACHE_BULK_CTX_COUNT 128u
#endif

#ifndef FC_FLOW_FAMILY_IPV4
#define FC_FLOW_FAMILY_IPV4  4u
#endif

#ifndef FC_FLOW_FAMILY_IPV6
#define FC_FLOW_FAMILY_IPV6  6u
#endif

#ifndef FC_MEMBER_PTR
#define FC_MEMBER_PTR(objp, member) (&((objp)->member))
#endif

/*
 * Generic byte-address helpers for init_ex() layouts.
 *
 * Naming:
 *   BYTE   : treat the operand as a byte-addressable pointer.
 *   PTR    : mutable pointer result.
 *   CPTR   : const pointer result ("const pointer").
 *   RECORD : one fixed-stride caller-owned record.
 *   MEMBER : an embedded object inside one record.
 *
 * Why these helpers exist:
 *   init_ex() accepts (base, stride, entry_offset), so the implementation
 *   must map between
 *
 *     entry_idx  <->  record base  <->  embedded entry/member
 *
 *   without assuming a contiguous entry[] array.
 *
 *   Writing raw expressions such as
 *
 *     base + (idx - 1) * stride + entry_offset
 *
 *   at many call sites makes the code hard to audit and easy to get wrong.
 *   These helpers centralize the byte-address arithmetic and alignment rules
 *   in one place.
 */
#ifndef FC_BYTE_PTR
#define FC_BYTE_PTR(ptr) ((unsigned char *)(void *)(ptr))
#endif

#ifndef FC_BYTE_CPTR
#define FC_BYTE_CPTR(ptr) ((const unsigned char *)(const void *)(ptr))
#endif

#ifndef FC_PTR_ADDR
#define FC_PTR_ADDR(ptr) ((uintptr_t)(const void *)(ptr))
#endif

#ifndef FC_PTR_IS_ALIGNED
#define FC_PTR_IS_ALIGNED(ptr, align) \
    ((FC_PTR_ADDR(ptr) & (uintptr_t)((align) - 1u)) == 0u)
#endif

#ifndef FC_BYTE_PTR_ADD
#define FC_BYTE_PTR_ADD(base, bytes) \
    (FC_BYTE_PTR(base) + (size_t)(bytes))
#endif

#ifndef FC_BYTE_CPTR_ADD
#define FC_BYTE_CPTR_ADD(base, bytes) \
    (FC_BYTE_CPTR(base) + (size_t)(bytes))
#endif

#ifndef FC_RECORD_PTR
#define FC_RECORD_PTR(base, stride, idx)                                      \
    ((idx) == RIX_NIL ? NULL : (void *)FC_BYTE_PTR_ADD(                      \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FC_RECORD_CPTR
#define FC_RECORD_CPTR(base, stride, idx)                                     \
    ((idx) == RIX_NIL ? NULL : (const void *)FC_BYTE_CPTR_ADD(               \
        (base), RIX_IDX_TO_OFF0(idx) * (stride)))
#endif

#ifndef FC_RECORD_MEMBER_PTR
#define FC_RECORD_MEMBER_PTR(base, stride, idx, member_offset, type)          \
    ((type *)__builtin_assume_aligned(                                        \
        FC_BYTE_PTR_ADD(FC_RECORD_PTR((base), (stride), (idx)),              \
                        (member_offset)),                                     \
        _Alignof(type)))
#endif

#ifndef FC_RECORD_MEMBER_CPTR
#define FC_RECORD_MEMBER_CPTR(base, stride, idx, member_offset, type)         \
    ((const type *)__builtin_assume_aligned(                                  \
        FC_BYTE_CPTR_ADD(FC_RECORD_CPTR((base), (stride), (idx)),            \
                         (member_offset)),                                    \
        _Alignof(type)))
#endif

static inline unsigned
fc_record_index_from_member_ptr(const void *base,
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
    base_addr = (uintptr_t)FC_BYTE_CPTR_ADD(base, member_offset);
    delta = (ptrdiff_t)(member_addr - base_addr);
    RIX_ASSERT(delta >= 0);
    RIX_ASSERT(stride != 0u);
    RIX_ASSERT(((size_t)delta % stride) == 0u);
    return (unsigned)((size_t)delta / stride) + 1u;
}

struct fc_cache_size_attr {
    unsigned requested_entries;
    unsigned nb_entries;
    unsigned nb_bk;
    unsigned total_slots;
    unsigned scratch_ctx_count;

    void   *cache_ptr;
    void   *buckets_ptr;
    void   *pool_ptr;
    void   *scratch_ptr;

    size_t cache_align;
    size_t cache_bytes;
    size_t cache_offset;

    size_t buckets_align;
    size_t buckets_bytes;
    size_t buckets_offset;

    size_t pool_align;
    size_t pool_bytes;
    size_t pool_offset;

    size_t scratch_align;
    size_t scratch_bytes;
    size_t scratch_offset;

    size_t total_bytes;
};

static inline unsigned
fc_cache_roundup_entries(unsigned desired)
{
    if (desired < 64u)
        desired = 64u;
    desired--;
    desired |= desired >> 1;
    desired |= desired >> 2;
    desired |= desired >> 4;
    desired |= desired >> 8;
    desired |= desired >> 16;
    return desired + 1u;
}

static inline size_t
fc_cache_align_up(size_t value, size_t align)
{
    return (align <= 1u) ? value : ((value + align - 1u) / align) * align;
}

static inline int
fc_cache_size_query_common(unsigned requested_entries,
                           size_t cache_bytes,
                           size_t cache_align,
                           size_t entry_bytes,
                           size_t entry_align,
                           struct fc_cache_size_attr *attr)
{
    size_t off = 0u;
    unsigned nb_entries;
    unsigned nb_bk;

    if (attr == NULL)
        return -1;

    nb_entries = fc_cache_roundup_entries(requested_entries);
    nb_bk = rix_hash_nb_bk_hint(nb_entries);

    attr->requested_entries = requested_entries;
    attr->nb_entries = nb_entries;
    attr->nb_bk = nb_bk;
    attr->total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    attr->scratch_ctx_count = FC_CACHE_BULK_CTX_COUNT;
    attr->cache_ptr = NULL;
    attr->buckets_ptr = NULL;
    attr->pool_ptr = NULL;
    attr->scratch_ptr = NULL;

    attr->cache_align = cache_align;
    attr->cache_bytes = cache_bytes;
    off = fc_cache_align_up(off, cache_align);
    attr->cache_offset = off;
    off += cache_bytes;

    attr->buckets_align = _Alignof(struct rix_hash_bucket_s);
    attr->buckets_bytes = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    off = fc_cache_align_up(off, attr->buckets_align);
    attr->buckets_offset = off;
    off += attr->buckets_bytes;

    attr->pool_align = entry_align;
    attr->pool_bytes = (size_t)nb_entries * entry_bytes;
    off = fc_cache_align_up(off, entry_align);
    attr->pool_offset = off;
    off += attr->pool_bytes;

    attr->scratch_align = _Alignof(struct rix_hash_find_ctx_s);
    attr->scratch_bytes =
        (size_t)FC_CACHE_BULK_CTX_COUNT * sizeof(struct rix_hash_find_ctx_s);
    off = fc_cache_align_up(off, attr->scratch_align);
    attr->scratch_offset = off;
    off += attr->scratch_bytes;

    attr->total_bytes = off;
    return 0;
}

static inline int
fc_cache_size_bind(void *base, struct fc_cache_size_attr *attr)
{
    unsigned char *p;

    if (base == NULL || attr == NULL)
        return -1;

    p = (unsigned char *)base;
    attr->cache_ptr = p + attr->cache_offset;
    attr->buckets_ptr = p + attr->buckets_offset;
    attr->pool_ptr = p + attr->pool_offset;
    attr->scratch_ptr = p + attr->scratch_offset;
    return 0;
}

#endif /* _FC_CACHE_COMMON_H_ */
