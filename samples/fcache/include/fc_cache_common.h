/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FC_CACHE_COMMON_H_
#define _FC_CACHE_COMMON_H_

#include <stddef.h>

#ifndef FC_CACHE_BULK_CTX_COUNT
#define FC_CACHE_BULK_CTX_COUNT 128u
#endif

#ifndef FC_FLOW_FAMILY_IPV4
#define FC_FLOW_FAMILY_IPV4  4u
#endif

#ifndef FC_FLOW_FAMILY_IPV6
#define FC_FLOW_FAMILY_IPV6  6u
#endif

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
