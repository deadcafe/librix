/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOWU_TABLE_H_
#define _FLOWU_TABLE_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <rix/rix_defs_private.h>

#include "ft_table_common.h"
#include <flow/flow_key.h>

#define FT_FLOWU_DEFAULT_GROW_FILL_PCT 60u
#define FT_FLOWU_DEFAULT_MIN_NB_BK    16384u
#define FT_FLOWU_DEFAULT_MAX_NB_BK    1048576u
#define FT_FLOWU_ENTRY_FLAG_ACTIVE     0x0001u

struct ft_flowu_entry {
    union {
        struct flowu_entry_hdr hdr;
        struct {
            struct flowu_key key;
            union {
                struct flow_hashtbl_elm htbl_elm;
                struct {
                    uint32_t cur_hash;
                    uint16_t slot;
                    uint16_t reserved1;
                };
            };
        };
    };
    uint32_t hash0;
    uint32_t hash1;
    uint16_t flags;
    uint8_t  reserved0[2];
} __attribute__((packed, aligned(FT_TABLE_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct ft_flowu_entry) == FT_TABLE_CACHE_LINE_SIZE,
                  "ft_flowu_entry must be 64 bytes");

RIX_HASH_HEAD(ft_flowu_ht);

struct ft_flowu_result {
    uint32_t entry_idx;
};

struct ft_flowu_config {
    unsigned start_nb_bk;
    unsigned max_nb_bk;
    unsigned grow_fill_pct;
    struct ft_bucket_allocator bucket_alloc;
};

struct ft_flowu_stats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t adds;
    uint64_t add_existing;
    uint64_t add_failed;
    uint64_t dels;
    uint64_t del_miss;
    uint64_t grow_marks;
    uint64_t grow_execs;
    uint64_t grow_failures;
    uint64_t reserve_calls;
};

struct ft_flowu_table {
    struct rix_hash_bucket_s   *buckets;
    struct ft_flowu_entry      *pool;
    unsigned char              *pool_base;
    size_t                      pool_stride;
    size_t                      pool_entry_offset;
    struct ft_flowu_ht          ht_head;
    struct ft_bucket_allocator  bucket_alloc;
    unsigned                    start_mask;
    unsigned                    nb_bk;
    unsigned                    max_nb_bk;
    unsigned                    max_entries;
    unsigned                    grow_fill_pct;
    unsigned                    need_grow;
    struct ft_flowu_stats       stats;
};

/*===========================================================================
 * Init / destroy / query
 *===========================================================================*/

int ft_flowu_table_init_ex(struct ft_flowu_table *ft,
                           void *array,
                           unsigned max_entries,
                           size_t stride,
                           size_t entry_offset,
                           const struct ft_flowu_config *cfg);

int ft_flowu_table_init(struct ft_flowu_table *ft,
                        struct ft_flowu_entry *pool,
                        unsigned max_entries,
                        const struct ft_flowu_config *cfg);

void ft_flowu_table_destroy(struct ft_flowu_table *ft);
void ft_flowu_table_flush(struct ft_flowu_table *ft);
unsigned ft_flowu_table_nb_entries(const struct ft_flowu_table *ft);
unsigned ft_flowu_table_nb_bk(const struct ft_flowu_table *ft);
unsigned ft_flowu_table_need_grow(const struct ft_flowu_table *ft);
void ft_flowu_table_stats(const struct ft_flowu_table *ft,
                          struct ft_flowu_stats *out);

/*===========================================================================
 * Single-key operations
 *===========================================================================*/

uint32_t ft_flowu_table_find(struct ft_flowu_table *ft,
                             const struct flowu_key *key);

uint32_t ft_flowu_table_add_entry(struct ft_flowu_table *ft,
                                  uint32_t entry_idx);

uint32_t ft_flowu_table_del(struct ft_flowu_table *ft,
                            const struct flowu_key *key);

uint32_t ft_flowu_table_del_idx(struct ft_flowu_table *ft,
                                uint32_t entry_idx);

/*===========================================================================
 * Bulk operations
 *===========================================================================*/

void ft_flowu_table_find_bulk(struct ft_flowu_table *ft,
                              const struct flowu_key *keys,
                              unsigned nb_keys,
                              struct ft_flowu_result *results);

void ft_flowu_table_add_entry_bulk(struct ft_flowu_table *ft,
                                   const uint32_t *entry_idxv,
                                   unsigned nb_keys,
                                   struct ft_flowu_result *results);

void ft_flowu_table_del_bulk(struct ft_flowu_table *ft,
                             const struct flowu_key *keys,
                             unsigned nb_keys,
                             struct ft_flowu_result *results);

/*===========================================================================
 * Walk / grow
 *===========================================================================*/

int ft_flowu_table_walk(struct ft_flowu_table *ft,
                        int (*cb)(uint32_t entry_idx, void *arg),
                        void *arg);

int ft_flowu_table_grow_2x(struct ft_flowu_table *ft);
int ft_flowu_table_reserve(struct ft_flowu_table *ft,
                           unsigned min_entries);

/*===========================================================================
 * Convenience macros / inline helpers
 *===========================================================================*/

#define FT_FLOWU_TABLE_INIT_TYPED(ft, array, max_entries, type, member, cfg) \
    ft_flowu_table_init_ex((ft), (array), (max_entries), sizeof(type),        \
                           offsetof(type, member), (cfg))

static inline void *
ft_flowu_table_record_ptr(struct ft_flowu_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_PTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline const void *
ft_flowu_table_record_cptr(const struct ft_flowu_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_CPTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline struct ft_flowu_entry *
ft_flowu_table_entry_ptr(struct ft_flowu_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, entry_idx,
                                ft->pool_entry_offset, struct ft_flowu_entry);
}

static inline const struct ft_flowu_entry *
ft_flowu_table_entry_cptr(const struct ft_flowu_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_CPTR(ft->pool_base, ft->pool_stride, entry_idx,
                                 ft->pool_entry_offset,
                                 struct ft_flowu_entry);
}

static inline size_t
ft_flowu_table_record_stride(const struct ft_flowu_table *ft)
{
    return ft == NULL ? 0u : ft->pool_stride;
}

static inline size_t
ft_flowu_table_entry_offset(const struct ft_flowu_table *ft)
{
    return ft == NULL ? 0u : ft->pool_entry_offset;
}

#define FT_FLOWU_TABLE_RECORD_PTR_AS(ft, type, entry_idx) \
    ((type *)ft_flowu_table_record_ptr((ft), (entry_idx)))

#define FT_FLOWU_TABLE_RECORD_CPTR_AS(ft, type, entry_idx) \
    ((const type *)ft_flowu_table_record_cptr((ft), (entry_idx)))

#define FT_FLOWU_TABLE_RECORD_FROM_ENTRY(type, member, entry_ptr) \
    RIX_CONTAINER_OF((entry_ptr), type, member)

#define FT_FLOWU_TABLE_ENTRY_FROM_RECORD(record_ptr, member) \
    FT_MEMBER_PTR((record_ptr), member)

#endif /* _FLOWU_TABLE_H_ */
