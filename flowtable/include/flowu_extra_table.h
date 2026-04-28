/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOWU_EXTRA_TABLE_H_
#define _FLOWU_EXTRA_TABLE_H_

#include "flow_extra_common.h"
#include "flow_extra_key.h"

/**
 * @brief Initialize a flowu extra table over a flowu_extra_entry pool.
 *        Thin wrapper around ft_table_extra_init() that fixes the variant
 *        tag, stride and entry offset.
 */
static inline int
flowu_extra_table_init(struct ft_table_extra *ft,
                       struct flowu_extra_entry *entry_array,
                       unsigned max_entries,
                       void *buckets, size_t bucket_size,
                       const struct ft_table_extra_config *cfg)
{
    return ft_table_extra_init(ft, FT_TABLE_VARIANT_FLOWU,
                               entry_array, max_entries,
                               sizeof(struct flowu_extra_entry), 0u,
                               buckets, bucket_size, cfg);
}

/**
 * @brief Translate a 1-origin entry index into a record pointer in the pool.
 * @return Pointer to the entry, or NULL if @p idx is @c RIX_NIL.
 */
static inline struct flowu_extra_entry *
flowu_extra_table_entry_ptr(const struct ft_table_extra *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset,
                                struct flowu_extra_entry);
}

/**
 * @brief Insert an entry whose @c key field is already populated.
 *
 * The entry index is derived from the entry's position in the pool.  On
 * duplicate, the existing entry's bucket extra[] timestamp is refreshed
 * and the existing index is returned.
 *
 * @param ft     Table handle.
 * @param entry  Caller-owned entry in the pool with its key set.
 * @param now    Current TSC; encoded into bucket extra[].
 * @return Registered entry index (1-origin), or @c RIX_NIL when the table
 *         is full.
 */
u32 flowu_extra_table_add(struct ft_table_extra *ft,
                          struct flowu_extra_entry *entry, u64 now);

/**
 * @brief Look up an entry by key without touching its timestamp.
 *
 * @param ft  Table handle.
 * @param key Lookup key.
 * @return Entry index (1-origin) on hit, @c RIX_NIL on miss.
 *
 * Equivalent to flowu_extra_table_find_touch() with @c now == 0.
 */
u32 flowu_extra_table_find(struct ft_table_extra *ft,
                           const struct flowu_extra_key *key);

/**
 * @brief Look up an entry by key and, on hit, refresh its bucket-side
 *        timestamp to @p now.
 *
 * @param ft  Table handle.
 * @param key Lookup key.
 * @param now Current TSC value to encode into the bucket extra[] timestamp.
 *            Pass @c 0 to skip the refresh (behaves like the non-touching
 *            flowu_extra_table_find()).
 * @return Entry index (1-origin) on hit, @c RIX_NIL on miss.
 */
u32 flowu_extra_table_find_touch(struct ft_table_extra *ft,
                                 const struct flowu_extra_key *key,
                                 u64 now);

/**
 * @brief Remove the entry matching @p key.
 * @return The freed entry index on success, @c RIX_NIL on miss.
 */
u32 flowu_extra_table_del(struct ft_table_extra *ft,
                          const struct flowu_extra_key *key);

/**
 * @brief Bulk lookup; for each @p keys[i] writes its entry index (or 0 on
 *        miss) into @p results[i].
 *
 * @param ft        Table handle.
 * @param keys      Array of @p nb_keys lookup keys.
 * @param nb_keys   Number of keys.
 * @param now       If non-zero, refresh bucket extra[] timestamp on hit.
 *                  Pass 0 for non-touching lookup.
 * @param results   Output array (length @p nb_keys) of registered indices.
 * @return Number of hits.
 */
unsigned flowu_extra_table_find_bulk(struct ft_table_extra *ft,
                                     const struct flowu_extra_key *keys,
                                     unsigned nb_keys, u64 now,
                                     u32 *results);

/**
 * @brief Bulk delete; appends each freed index to @p idx_out.
 * @return Number of entries freed (== entries appended to @p idx_out).
 */
unsigned flowu_extra_table_del_bulk(struct ft_table_extra *ft,
                                    const struct flowu_extra_key *keys,
                                    unsigned nb_keys, u32 *idx_out);

/**
 * @brief Recommended raw-bucket-memory size for a flowu extra table holding
 *        @p max_entries entries.
 */
size_t flowu_extra_table_bucket_size(unsigned max_entries);

#endif /* _FLOWU_EXTRA_TABLE_H_ */
