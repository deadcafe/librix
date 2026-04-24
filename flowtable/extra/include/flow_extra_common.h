/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_extra_common.h - protocol-independent types for the flowtable
 * slot_extra variant. Mirrors flow_common.h but uses the 192 B bucket
 * (rix_hash_bucket_extra_s) and a slimmed maintenance context.
 */

#ifndef _FLOW_EXTRA_COMMON_H_
#define _FLOW_EXTRA_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>
#include <rix/rix_hash_slot_extra.h>
#include <rix/rix_queue.h>

#include "flow_common.h"   /* reuse FT_ARCH_*, ft_add_policy, flow_stats,
                              flow_status, ft_table_variant */

#ifndef FT_TABLE_EXTRA_BUCKET_SIZE
#define FT_TABLE_EXTRA_BUCKET_SIZE 192u
#endif

struct ft_table_extra_config {
    unsigned ts_shift;
};

struct ft_table_extra_stats {
    struct flow_stats core;
    u64 grow_execs;
    u64 grow_failures;
    u64 maint_calls;
    u64 maint_bucket_checks;
    u64 maint_evictions;
    u64 maint_extras_loaded;   /* EXTRA: bucket extra[] reads in maintain */
};

RIX_HASH_HEAD(ft_table_extra_ht);

struct ft_table_extra {
    struct rix_hash_bucket_extra_s *buckets;
    unsigned char                  *pool_base;
    size_t                          pool_stride;
    size_t                          pool_entry_offset;
    struct ft_table_extra_ht        ht_head;
    unsigned                        start_mask;
    unsigned                        nb_bk;
    unsigned                        max_entries;
    u8                              variant;
    u8                              ts_shift;
    struct ft_table_extra_stats     stats;
    struct flow_status              status;
};

/*
 * Maintenance context.  ft_table_extra_maintain (bucket sweep) only needs
 * buckets/rhh_nb/stats/rhh_mask/ts_shift.  ft_table_extra_maintain_idx_bulk
 * needs to resolve (bucket, slot) from entry->meta, so the pool pointer,
 * stride, entry offset, and max_entries are also provided.
 */
struct ft_maint_extra_ctx {
    struct rix_hash_bucket_extra_s *buckets;
    unsigned                       *rhh_nb;
    struct ft_table_extra_stats    *stats;
    unsigned char                  *pool_base;
    size_t                          pool_stride;
    size_t                          meta_off;
    unsigned                        max_entries;
    unsigned                        rhh_mask;
    u8                              ts_shift;
};

int ft_table_extra_init(struct ft_table_extra *ft,
                        enum ft_table_variant variant,
                        void *array,
                        unsigned max_entries,
                        size_t stride,
                        size_t entry_offset,
                        void *buckets,
                        size_t bucket_size,
                        const struct ft_table_extra_config *cfg);

void ft_table_extra_destroy(struct ft_table_extra *ft);
void ft_table_extra_flush(struct ft_table_extra *ft);
unsigned ft_table_extra_nb_entries(const struct ft_table_extra *ft);
unsigned ft_table_extra_nb_bk(const struct ft_table_extra *ft);
void ft_table_extra_stats_get(const struct ft_table_extra *ft,
                              struct ft_table_extra_stats *out);
void ft_table_extra_status_get(const struct ft_table_extra *ft,
                               struct flow_status *out);

u32 ft_table_extra_add_idx(struct ft_table_extra *ft, u32 entry_idx,
                           u64 now);
unsigned ft_table_extra_add_idx_bulk(struct ft_table_extra *ft,
                                     u32 *entry_idxv, unsigned nb_keys,
                                     enum ft_add_policy policy, u64 now,
                                     u32 *unused_idxv);
unsigned ft_table_extra_add_idx_bulk_maint(struct ft_table_extra *ft,
                                           u32 *entry_idxv,
                                           unsigned nb_keys,
                                           enum ft_add_policy policy,
                                           u64 now, u64 timeout,
                                           u32 *unused_idxv,
                                           unsigned max_unused,
                                           unsigned min_bk_used);
u32 ft_table_extra_del_idx(struct ft_table_extra *ft, u32 entry_idx);
unsigned ft_table_extra_del_idx_bulk(struct ft_table_extra *ft,
                                     const u32 *entry_idxv,
                                     unsigned nb_keys,
                                     u32 *unused_idxv);
int ft_table_extra_walk(struct ft_table_extra *ft,
                        int (*cb)(u32 entry_idx, void *arg),
                        void *arg);
int ft_table_extra_migrate(struct ft_table_extra *ft,
                           void *new_buckets, size_t new_bucket_size);

void ft_table_extra_touch(struct ft_table_extra *ft, u32 entry_idx,
                          u64 now);

unsigned ft_table_extra_maintain(const struct ft_maint_extra_ctx *ctx,
                                 unsigned start_bk,
                                 u64 now, u64 expire_tsc,
                                 u32 *expired_idxv, unsigned max_expired,
                                 unsigned min_bk_entries,
                                 unsigned *next_bk);

unsigned ft_table_extra_maintain_idx_bulk(const struct ft_maint_extra_ctx *ctx,
                                          const u32 *entry_idxv,
                                          unsigned nb_idx,
                                          u64 now, u64 expire_tsc,
                                          u32 *expired_idxv,
                                          unsigned max_expired,
                                          unsigned min_bk_entries,
                                          int enable_filter);

size_t ft_table_extra_bucket_size(unsigned max_entries);
size_t ft_table_extra_bucket_mem_size(unsigned nb_bk);

void ft_arch_extra_init(unsigned arch_enable);

#define FT_TABLE_EXTRA_INIT_TYPED(ft, variant, array, max_entries, type,     \
                                  member, buckets, bucket_size, cfg)         \
    ft_table_extra_init((ft), (variant), (array), (max_entries),            \
                        sizeof(type), offsetof(type, member),               \
                        (buckets), (bucket_size), (cfg))

#endif /* _FLOW_EXTRA_COMMON_H_ */
