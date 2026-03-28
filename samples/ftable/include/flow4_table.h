/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_TABLE_H_
#define _FLOW4_TABLE_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <rix/rix_defs_private.h>

#include "ft_table_common.h"

#define FT_TABLE_CACHE_LINE_SIZE 64u
#define FT_FLOW4_DEFAULT_GROW_FILL_PCT 60u
#define FT_FLOW4_DEFAULT_MIN_NB_BK 16384u
#define FT_FLOW4_DEFAULT_MAX_NB_BK 1048576u

struct ft_flow4_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t zero;
};

static inline struct ft_flow4_key
ft_flow4_key_make(uint32_t src_ip, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    return (struct ft_flow4_key){
        .family = FT_FLOW_FAMILY_IPV4,
        .proto = proto,
        .src_port = src_port,
        .dst_port = dst_port,
        .pad = 0u,
        .vrfid = vrfid,
        .src_ip = src_ip,
        .dst_ip = dst_ip,
        .zero = 0u,
    };
}

struct ft_flow4_result {
    uint32_t entry_idx;
};

struct ft_flow4_entry {
    struct ft_flow4_key key;
    uint32_t cur_hash;
    uint32_t hash0;
    uint32_t hash1;
    uint32_t next_free;
    uint16_t slot;
    uint16_t flags;
    uint8_t  reserved0[20];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct ft_flow4_entry) == FT_TABLE_CACHE_LINE_SIZE,
                  "ft_flow4_entry must be 64 bytes");

RIX_HASH_HEAD(ft_flow4_ht);

typedef void *(*ft_bucket_alloc_fn)(size_t size, size_t align, void *arg);
typedef void (*ft_bucket_free_fn)(void *ptr, size_t size, size_t align,
                                  void *arg);

struct ft_bucket_allocator {
    ft_bucket_alloc_fn alloc;
    ft_bucket_free_fn  free;
    void              *arg;
};

struct ft_flow4_config {
    unsigned start_nb_bk;
    unsigned max_nb_bk;
    unsigned grow_fill_pct;
    struct ft_bucket_allocator bucket_alloc;
};

struct ft_flow4_stats {
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

struct ft_flow4_table {
    struct rix_hash_bucket_s *buckets;
    struct ft_flow4_entry    *pool;
    unsigned char            *pool_base;
    size_t                    pool_stride;
    size_t                    pool_entry_offset;
    struct ft_flow4_ht        ht_head;
    struct ft_bucket_allocator bucket_alloc;
    unsigned                   start_mask;
    unsigned                   nb_bk;
    unsigned                   max_nb_bk;
    unsigned                   max_entries;
    unsigned                   grow_fill_pct;
    unsigned                   need_grow;
    unsigned                   free_head;
    struct ft_flow4_stats      stats;
};

int ft_flow4_table_init(struct ft_flow4_table *ft,
                        struct ft_flow4_entry *pool,
                        unsigned max_entries,
                        const struct ft_flow4_config *cfg);

int ft_flow4_table_init_ex(struct ft_flow4_table *ft,
                           void *array,
                           unsigned max_entries,
                           size_t stride,
                           size_t entry_offset,
                           const struct ft_flow4_config *cfg);

void ft_flow4_table_destroy(struct ft_flow4_table *ft);
void ft_flow4_table_flush(struct ft_flow4_table *ft);
unsigned ft_flow4_table_nb_entries(const struct ft_flow4_table *ft);
unsigned ft_flow4_table_nb_bk(const struct ft_flow4_table *ft);
unsigned ft_flow4_table_need_grow(const struct ft_flow4_table *ft);
void ft_flow4_table_stats(const struct ft_flow4_table *ft,
                          struct ft_flow4_stats *out);

uint32_t ft_flow4_table_find(struct ft_flow4_table *ft,
                             const struct ft_flow4_key *key);
uint32_t ft_flow4_table_add(struct ft_flow4_table *ft,
                            const struct ft_flow4_key *key);
uint32_t ft_flow4_table_add_idx(struct ft_flow4_table *ft,
                                uint32_t entry_idx);
uint32_t ft_flow4_table_add_entry(struct ft_flow4_table *ft,
                                  struct ft_flow4_entry *entry);
uint32_t ft_flow4_table_del(struct ft_flow4_table *ft,
                            const struct ft_flow4_key *key);

void ft_flow4_table_find_bulk(struct ft_flow4_table *ft,
                              const struct ft_flow4_key *keys,
                              unsigned nb_keys,
                              struct ft_flow4_result *results);
void ft_flow4_table_add_bulk(struct ft_flow4_table *ft,
                             const struct ft_flow4_key *keys,
                             unsigned nb_keys,
                             struct ft_flow4_result *results);
void ft_flow4_table_add_idx_bulk(struct ft_flow4_table *ft,
                                 const uint32_t *entry_idxv,
                                 unsigned nb_keys,
                                 struct ft_flow4_result *results);
void ft_flow4_table_add_entry_bulk(struct ft_flow4_table *ft,
                                   struct ft_flow4_entry *const *entries,
                                   unsigned nb_keys,
                                   struct ft_flow4_result *results);
void ft_flow4_table_del_bulk(struct ft_flow4_table *ft,
                             const struct ft_flow4_key *keys,
                             unsigned nb_keys,
                             struct ft_flow4_result *results);

int ft_flow4_table_walk(struct ft_flow4_table *ft,
                        int (*cb)(uint32_t entry_idx, void *arg),
                        void *arg);

int ft_flow4_table_grow_2x(struct ft_flow4_table *ft);
int ft_flow4_table_reserve(struct ft_flow4_table *ft,
                           unsigned min_entries);

#define FT_FLOW4_TABLE_INIT_TYPED(ft, array, max_entries, type, member, cfg) \
    ft_flow4_table_init_ex((ft), (array), (max_entries), sizeof(type),        \
                           offsetof(type, member), (cfg))

static inline void *
ft_flow4_table_record_ptr(struct ft_flow4_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_PTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline const void *
ft_flow4_table_record_cptr(const struct ft_flow4_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_CPTR(ft->pool_base, ft->pool_stride, entry_idx);
}

static inline struct ft_flow4_entry *
ft_flow4_table_entry_ptr(struct ft_flow4_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, entry_idx,
                                ft->pool_entry_offset, struct ft_flow4_entry);
}

static inline const struct ft_flow4_entry *
ft_flow4_table_entry_cptr(const struct ft_flow4_table *ft, uint32_t entry_idx)
{
    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return NULL;
    return FT_RECORD_MEMBER_CPTR(ft->pool_base, ft->pool_stride, entry_idx,
                                 ft->pool_entry_offset,
                                 struct ft_flow4_entry);
}

static inline size_t
ft_flow4_table_record_stride(const struct ft_flow4_table *ft)
{
    return ft == NULL ? 0u : ft->pool_stride;
}

static inline size_t
ft_flow4_table_entry_offset(const struct ft_flow4_table *ft)
{
    return ft == NULL ? 0u : ft->pool_entry_offset;
}

#define FT_FLOW4_TABLE_RECORD_PTR_AS(ft, type, entry_idx) \
    ((type *)ft_flow4_table_record_ptr((ft), (entry_idx)))

#define FT_FLOW4_TABLE_RECORD_CPTR_AS(ft, type, entry_idx) \
    ((const type *)ft_flow4_table_record_cptr((ft), (entry_idx)))

#define FT_FLOW4_TABLE_RECORD_FROM_ENTRY(type, member, entry_ptr) \
    RIX_CONTAINER_OF((entry_ptr), type, member)

#define FT_FLOW4_TABLE_ENTRY_FROM_RECORD(record_ptr, member) \
    FT_MEMBER_PTR((record_ptr), member)

#endif /* _FLOW4_TABLE_H_ */
