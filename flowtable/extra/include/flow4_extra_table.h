/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_EXTRA_TABLE_H_
#define _FLOW4_EXTRA_TABLE_H_

#include "flow_extra_common.h"
#include "flow_extra_key.h"

static inline int
flow4_extra_table_init(struct ft_table_extra *ft,
                       struct flow4_extra_entry *entry_array,
                       unsigned max_entries,
                       void *buckets, size_t bucket_size,
                       const struct ft_table_extra_config *cfg)
{
    return FT_TABLE_EXTRA_INIT_TYPED(ft, FT_TABLE_VARIANT_FLOW4,
                                     entry_array, max_entries,
                                     struct flow4_extra_entry, meta,
                                     buckets, bucket_size, cfg);
}

static inline struct flow4_extra_entry *
flow4_extra_table_entry_ptr(const struct ft_table_extra *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset,
                                struct flow4_extra_entry);
}

u32 flow4_extra_table_add(struct ft_table_extra *ft,
                          struct flow4_extra_entry *entry, u64 now);
u32 flow4_extra_table_find(const struct ft_table_extra *ft,
                           const struct flow4_extra_key *key);
u32 flow4_extra_table_del(struct ft_table_extra *ft,
                          const struct flow4_extra_key *key);

unsigned flow4_extra_table_find_bulk(struct ft_table_extra *ft,
                                     const struct flow4_extra_key *keys,
                                     unsigned nb_keys, u64 now,
                                     u32 *results);
unsigned flow4_extra_table_del_bulk(struct ft_table_extra *ft,
                                    const struct flow4_extra_key *keys,
                                    unsigned nb_keys, u32 *idx_out);

size_t flow4_extra_table_bucket_size(unsigned max_entries);

#endif /* _FLOW4_EXTRA_TABLE_H_ */
