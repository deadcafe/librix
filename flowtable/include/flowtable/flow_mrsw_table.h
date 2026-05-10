/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_MRSW_TABLE_H_
#define _FLOW_MRSW_TABLE_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <rix/rix_hash.h>

#include "flow_common.h"
#include "flow_key.h"

/*
 * MRSW flow tables are intentionally separate from struct ft_table.
 *
 * This is a general flow table for a control-plane writer and user-plane
 * readers.  It is not a flow cache and intentionally has no per-entry
 * timestamp, touch, timeout, or reclaim path.  Reusing struct ft_table for
 * multi-reader lookup would mix the normal table's timestamp writes into the
 * reader path and either introduce C data races or add atomics to the existing
 * hot path.  MRSW therefore uses a separate bucket type and separate flow
 * entry metadata.  Existing ft_table users keep the original layout and hot
 * path.
 *
 * Concurrency contract:
 *   - any number of reader threads may call ft_flow*_mrsw_table_find*().
 *   - find does not touch entry metadata, timestamps, or lookup counters.
 *   - exactly one writer thread may call add/delete/flush/migrate.
 *   - entry keys must remain immutable while an entry is published.
 *   - removed entry storage must not be reused for a different key until an
 *     external reader grace period has elapsed.
 *   - migrate requires readers to be quiesced, or a higher-level RCU table
 *     pointer swap around this object.
 */

struct flow_mrsw_entry_meta {
    u32         cur_hash;
    u16         slot;
    u16         reserved0;
};

_Static_assert(sizeof(struct flow_mrsw_entry_meta) == 8u,
               "flow_mrsw_entry_meta must be 8 bytes");
_Static_assert(_Alignof(struct flow_mrsw_entry_meta) == _Alignof(u32),
               "flow_mrsw_entry_meta must keep u32 alignment");

struct flow4_mrsw_entry {
    struct flow4_key key;
    struct flow_mrsw_entry_meta meta;
};

_Static_assert(offsetof(struct flow4_mrsw_entry, meta) == sizeof(struct flow4_key),
               "flow4_mrsw_entry.meta must follow key");
_Static_assert(sizeof(struct flow4_mrsw_entry) == 32u,
               "flow4_mrsw_entry must be 32 bytes");

struct flow6_mrsw_entry {
    struct flow6_key key;
    struct flow_mrsw_entry_meta meta;
};

_Static_assert(offsetof(struct flow6_mrsw_entry, meta) == sizeof(struct flow6_key),
               "flow6_mrsw_entry.meta must follow key");
_Static_assert(sizeof(struct flow6_mrsw_entry) == 52u,
               "flow6_mrsw_entry must be 52 bytes");

struct flowu_mrsw_entry {
    struct flowu_key key;
    struct flow_mrsw_entry_meta meta;
};

_Static_assert(offsetof(struct flowu_mrsw_entry, meta) == sizeof(struct flowu_key),
               "flowu_mrsw_entry.meta must follow key");
_Static_assert(sizeof(struct flowu_mrsw_entry) == 52u,
               "flowu_mrsw_entry must be 52 bytes");

enum {
    FT_MRSW_TABLE_BUCKET_SIZE  = 128u,
    FT_MRSW_TABLE_BUCKET_ALIGN = 64u,
};

static inline struct rix_hash_mrsw_bucket_s *
ft_mrsw_table_bucket_carve(void *raw, size_t raw_size, unsigned *nb_bk_out)
{
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned =
        (addr + (_Alignof(struct rix_hash_mrsw_bucket_s) - 1u))
        & ~(uintptr_t)(_Alignof(struct rix_hash_mrsw_bucket_s) - 1u);
    size_t lost = (size_t)(aligned - addr);
    size_t usable = raw_size > lost ? raw_size - lost : 0u;
    unsigned nb = (unsigned)(usable / sizeof(struct rix_hash_mrsw_bucket_s));

    nb = ft_rounddown_pow2_u32(nb);
    *nb_bk_out = nb;
    return (struct rix_hash_mrsw_bucket_s *)aligned;
}

static inline size_t
ft_mrsw_table_bucket_size(unsigned max_entries)
{
    /* MRSW keeps one bucket control word in the 16th slot position, so bucket
     * sizing follows rix_hash_mrsw's 15-slot, <=70% fill guidance.
     */
    unsigned nb_bk = rix_hash_mrsw_nb_bk_hint(max_entries);

    if (nb_bk < FT_TABLE_MIN_NB_BK)
        nb_bk = FT_TABLE_MIN_NB_BK;
    return (size_t)ft_roundup_pow2_u32(nb_bk) *
           sizeof(struct rix_hash_mrsw_bucket_s);
}

static inline size_t
ft_mrsw_table_bucket_mem_size(unsigned nb_bk)
{
    return (size_t)nb_bk * sizeof(struct rix_hash_mrsw_bucket_s);
}

/*
 * Reader-path counters (lookups/hits/misses/force_expired) are intentionally
 * absent from MRSW: the lockless reader path must not write to a shared
 * counter on every find.  Only the writer-side counters are tracked.
 */
struct ft_mrsw_atomic_flow_stats {
    _Atomic u64 adds;
    _Atomic u64 add_existing;
    _Atomic u64 add_failed;
    _Atomic u64 dels;
    _Atomic u64 del_miss;
};

struct ft_mrsw_table_stats {
    struct ft_mrsw_atomic_flow_stats core;
    _Atomic u64 grow_execs;
    _Atomic u64 grow_failures;
};

/*
 * status.entries is derived from ht_head.rhh_nb to avoid double counting.
 */
struct ft_mrsw_flow_status {
    _Atomic u32 kickouts;
    _Atomic u32 add_bk0;
    _Atomic u32 add_bk1;
};

RIX_HASH_MRSW_HEAD(ft_mrsw_table_ht);

struct ft_mrsw_table {
    struct rix_hash_mrsw_bucket_s *buckets;
    unsigned char                 *pool_base;
    size_t                         pool_stride;
    size_t                         pool_entry_offset;
    struct ft_mrsw_table_ht        ht_head;
    unsigned                       start_mask;
    unsigned                       nb_bk;
    unsigned                       max_entries;
    u8                             variant;
    u8                             reserved0[7];
    struct ft_mrsw_table_stats     stats;
    struct ft_mrsw_flow_status     status;
};

int ft_mrsw_table_init(struct ft_mrsw_table *ft,
                       enum ft_table_variant variant,
                       void *array,
                       unsigned max_entries,
                       size_t stride,
                       size_t entry_offset,
                       void *buckets,
                       size_t bucket_size,
                       const struct ft_table_config *cfg);
void ft_mrsw_table_destroy(struct ft_mrsw_table *ft);
void ft_mrsw_table_flush(struct ft_mrsw_table *ft);
unsigned ft_mrsw_table_nb_entries(const struct ft_mrsw_table *ft);
unsigned ft_mrsw_table_nb_bk(const struct ft_mrsw_table *ft);
void ft_mrsw_table_stats(const struct ft_mrsw_table *ft,
                         struct ft_table_stats *out);
void ft_mrsw_table_status(const struct ft_mrsw_table *ft,
                          struct flow_status *out);
u32 ft_mrsw_table_add_idx(struct ft_mrsw_table *ft, u32 entry_idx, u64 now);
unsigned ft_mrsw_table_add_idx_bulk(struct ft_mrsw_table *ft,
                                    u32 *entry_idxv,
                                    unsigned nb_keys,
                                    enum ft_add_policy policy,
                                    u64 now,
                                    u32 *unused_idxv);
u32 ft_mrsw_table_del_idx(struct ft_mrsw_table *ft, u32 entry_idx);
unsigned ft_mrsw_table_del_idx_bulk(struct ft_mrsw_table *ft,
                                    const u32 *entry_idxv,
                                    unsigned nb_keys,
                                    u32 *unused_idxv);
int ft_mrsw_table_walk(struct ft_mrsw_table *ft,
                       int (*cb)(u32 entry_idx, void *arg),
                       void *arg);
int ft_mrsw_table_migrate(struct ft_mrsw_table *ft,
                          void *new_buckets,
                          size_t new_bucket_size);

int ft_flow4_mrsw_table_init(struct ft_mrsw_table *ft,
                             void *array,
                             unsigned max_entries,
                             size_t stride,
                             size_t entry_offset,
                             void *buckets,
                             size_t bucket_size,
                             const struct ft_table_config *cfg);
int ft_flow6_mrsw_table_init(struct ft_mrsw_table *ft,
                             void *array,
                             unsigned max_entries,
                             size_t stride,
                             size_t entry_offset,
                             void *buckets,
                             size_t bucket_size,
                             const struct ft_table_config *cfg);
int ft_flowu_mrsw_table_init(struct ft_mrsw_table *ft,
                             void *array,
                             unsigned max_entries,
                             size_t stride,
                             size_t entry_offset,
                             void *buckets,
                             size_t bucket_size,
                             const struct ft_table_config *cfg);

u32 ft_flow4_mrsw_table_find(struct ft_mrsw_table *ft,
                             const struct flow4_key *key,
                             u64 now);
u32 ft_flow6_mrsw_table_find(struct ft_mrsw_table *ft,
                             const struct flow6_key *key,
                             u64 now);
u32 ft_flowu_mrsw_table_find(struct ft_mrsw_table *ft,
                             const struct flowu_key *key,
                             u64 now);

void ft_flow4_mrsw_table_find_bulk(struct ft_mrsw_table *ft,
                                   const struct flow4_key *keys,
                                   unsigned nb_keys,
                                   u64 now,
                                   struct ft_table_result *results);
void ft_flow6_mrsw_table_find_bulk(struct ft_mrsw_table *ft,
                                   const struct flow6_key *keys,
                                   unsigned nb_keys,
                                   u64 now,
                                   struct ft_table_result *results);
void ft_flowu_mrsw_table_find_bulk(struct ft_mrsw_table *ft,
                                   const struct flowu_key *keys,
                                   unsigned nb_keys,
                                   u64 now,
                                   struct ft_table_result *results);

unsigned ft_flow4_mrsw_table_del_key_bulk(struct ft_mrsw_table *ft,
                                          const struct flow4_key *keys,
                                          unsigned nb_keys,
                                          u32 *unused_idxv);
unsigned ft_flow6_mrsw_table_del_key_bulk(struct ft_mrsw_table *ft,
                                          const struct flow6_key *keys,
                                          unsigned nb_keys,
                                          u32 *unused_idxv);
unsigned ft_flowu_mrsw_table_del_key_bulk(struct ft_mrsw_table *ft,
                                          const struct flowu_key *keys,
                                          unsigned nb_keys,
                                          u32 *unused_idxv);

#define FT_MRSW_TABLE_INIT_TYPED(ft, variant, array, max_entries, type, member, \
                                 buckets, bucket_size, cfg)                   \
    ft_mrsw_table_init((ft), (variant), (array), (max_entries), sizeof(type), \
                       offsetof(type, member), (buckets), (bucket_size), (cfg))

#define FT_FLOW4_MRSW_TABLE_INIT_TYPED(ft, array, max_entries, type, member,  \
                                       buckets, bucket_size, cfg)             \
    ft_flow4_mrsw_table_init((ft), (array), (max_entries), sizeof(type),      \
                             offsetof(type, member), (buckets),              \
                             (bucket_size), (cfg))

#define FT_FLOW6_MRSW_TABLE_INIT_TYPED(ft, array, max_entries, type, member,  \
                                       buckets, bucket_size, cfg)             \
    ft_flow6_mrsw_table_init((ft), (array), (max_entries), sizeof(type),      \
                             offsetof(type, member), (buckets),              \
                             (bucket_size), (cfg))

#define FT_FLOWU_MRSW_TABLE_INIT_TYPED(ft, array, max_entries, type, member,  \
                                       buckets, bucket_size, cfg)             \
    ft_flowu_mrsw_table_init((ft), (array), (max_entries), sizeof(type),      \
                             offsetof(type, member), (buckets),              \
                             (bucket_size), (cfg))

#endif /* _FLOW_MRSW_TABLE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
