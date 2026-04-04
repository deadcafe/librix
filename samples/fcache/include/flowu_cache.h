/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flowu_cache.h - Unified IPv4/IPv6 flow cache (fcache).
 *
 * Single hash table handles both address families.
 * Key includes family field so v4/v6 entries never collide.
 */

#ifndef _FLOWU_CACHE_H_
#define _FLOWU_CACHE_H_

#include <stdint.h>
#include <string.h>
#include <rix/rix_hash.h>
#include <rix/rix_queue.h>
#include <flow/flow_key.h>
#include "fc_cache_common.h"

#ifndef FC_CACHE_LINE_SIZE
#define FC_CACHE_LINE_SIZE 64u
#endif
#ifndef FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

/*===========================================================================
 * Key construction helpers
 *===========================================================================*/
static inline struct flowu_key
fc_flowu_key_v4(u32 src_ip, u32 dst_ip,
                  u16 src_port, u16 dst_port,
                  u8 proto, u32 vrfid)
{
    struct flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FC_FLOW_FAMILY_IPV4;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    k.addr.v4.src = src_ip;
    k.addr.v4.dst = dst_ip;
    return k;
}

static inline struct flowu_key
fc_flowu_key_v6(const u8 *src_ip, const u8 *dst_ip,
                  u16 src_port, u16 dst_port,
                  u8 proto, u32 vrfid)
{
    struct flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FC_FLOW_FAMILY_IPV6;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    memcpy(k.addr.v6.src, src_ip, 16);
    memcpy(k.addr.v6.dst, dst_ip, 16);
    return k;
}

struct fc_flowu_result {
    u32 entry_idx; /* 1-origin; 0 = miss / full */
};

struct fc_flowu_entry {
    struct flowu_entry hdr;
    RIX_SLIST_ENTRY(struct fc_flowu_entry) free_link;
} __attribute__((packed, aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc_flowu_entry) == FC_CACHE_LINE_SIZE,
                  "fc_flowu_entry must be 64 bytes");

RIX_HASH_HEAD(fc_flowu_ht);
RIX_SLIST_HEAD(fc_flowu_free_head, fc_flowu_entry);

struct fc_flowu_config {
    u64 timeout_tsc;
    unsigned ts_shift;
    unsigned pressure_empty_slots;
    u64 maint_interval_tsc;
    unsigned maint_base_bk;
    unsigned maint_fill_threshold;
};

struct fc_flowu_stats {
    u64 lookups;
    u64 hits;
    u64 misses;
    u64 fills;
    u64 fill_full;
    u64 relief_calls;
    u64 relief_bucket_checks;
    u64 relief_evictions;
    u64 relief_bk0_evictions;
    u64 relief_bk1_evictions;
    u64 oldest_reclaim_calls;
    u64 oldest_reclaim_evictions;
    u64 maint_calls;
    u64 maint_bucket_checks;
    u64 maint_evictions;
    u64 maint_step_calls;
    u64 maint_step_skipped_bks;
};

enum fc_flowu_event {
    FC_FLOWU_EVENT_ALLOC = 1u,
    FC_FLOWU_EVENT_FREE_DELETE,
    FC_FLOWU_EVENT_FREE_TIMEOUT,
    FC_FLOWU_EVENT_FREE_PRESSURE,
    FC_FLOWU_EVENT_FREE_OLDEST,
    FC_FLOWU_EVENT_FREE_FLUSH,
    FC_FLOWU_EVENT_FREE_ROLLBACK,
};

typedef void (*fc_flowu_event_cb)(enum fc_flowu_event event,
                                  u32 entry_idx,
                                  void *arg);

struct fc_flowu_cache {
    /* --- CL0: lookup / fill hot path --- */
    struct rix_hash_bucket_s  *buckets;
    struct fc_flowu_entry    *pool;
    unsigned char            *pool_base;
    size_t                    pool_stride;
    size_t                    pool_entry_offset;
    struct fc_flowu_ht        ht_head;
    u64                   timeout_tsc;
    u64                   eff_timeout_tsc;
    u64                   timeout_min_tsc;
    unsigned                   nb_bk;
    unsigned                   max_entries;
    unsigned                   total_slots;
    u8                    ts_shift;
    unsigned                   pressure_empty_slots;
    /* --- CL1 --- */
    unsigned                   timeout_lo_entries;
    unsigned                   timeout_hi_entries;
    unsigned                   relief_mid_entries;
    unsigned                   relief_hi_entries;
    unsigned                   maint_cursor;
    u64                   last_maint_tsc;
    u64                   last_maint_fills;
    u64                   maint_interval_tsc;
    unsigned                   maint_base_bk;
    unsigned                   maint_fill_threshold;
    unsigned                   last_maint_start_bk;
    unsigned                   last_maint_sweep_bk;
    fc_flowu_event_cb          event_cb;
    void                      *event_cb_arg;
    struct rix_hash_find_ctx_s *bulk_ctx;
    unsigned                   bulk_ctx_count;
    struct fc_flowu_free_head free_head;
    struct fc_flowu_stats     stats;
};

static inline int
fc_flowu_cache_size_query(unsigned nb_entries, struct fc_cache_size_attr *attr)
{
    return fc_cache_size_query_common(nb_entries,
                                      sizeof(struct fc_flowu_cache),
                                      _Alignof(struct fc_flowu_cache),
                                      sizeof(struct fc_flowu_entry),
                                      _Alignof(struct fc_flowu_entry),
                                      attr);
}

void fc_flowu_cache_init(struct fc_flowu_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc_flowu_entry *pool,
                          unsigned max_entries,
                          const struct fc_flowu_config *cfg);
void fc_flowu_cache_init_ex(struct fc_flowu_cache *fc,
                            struct rix_hash_bucket_s *buckets,
                            unsigned nb_bk,
                            void *array,
                            unsigned max_entries,
                            size_t stride,
                            size_t entry_offset,
                            const struct fc_flowu_config *cfg);
void fc_flowu_cache_set_event_cb(struct fc_flowu_cache *fc,
                                 fc_flowu_event_cb cb,
                                 void *arg);

#define FC_FLOWU_CACHE_INIT_TYPED(fc, buckets, nb_bk, array, max_entries, type, member, cfg) \
    fc_flowu_cache_init_ex((fc), (buckets), (nb_bk), (array), (max_entries),  \
                           sizeof(type), offsetof(type, member), (cfg))

static inline void *
fc_flowu_cache_record_ptr(struct fc_flowu_cache *fc, u32 entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_PTR(fc->pool_base, fc->pool_stride, entry_idx);
}

static inline const void *
fc_flowu_cache_record_cptr(const struct fc_flowu_cache *fc, u32 entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_CPTR(fc->pool_base, fc->pool_stride, entry_idx);
}

static inline struct fc_flowu_entry *
fc_flowu_cache_entry_ptr(struct fc_flowu_cache *fc, u32 entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_MEMBER_PTR(fc->pool_base, fc->pool_stride, entry_idx,
                                fc->pool_entry_offset, struct fc_flowu_entry);
}

static inline const struct fc_flowu_entry *
fc_flowu_cache_entry_cptr(const struct fc_flowu_cache *fc, u32 entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_MEMBER_CPTR(fc->pool_base, fc->pool_stride, entry_idx,
                                 fc->pool_entry_offset,
                                 struct fc_flowu_entry);
}

static inline size_t
fc_flowu_cache_record_stride(const struct fc_flowu_cache *fc)
{
    return fc != NULL ? fc->pool_stride : 0;
}

static inline size_t
fc_flowu_cache_entry_offset(const struct fc_flowu_cache *fc)
{
    return fc != NULL ? fc->pool_entry_offset : 0;
}

#define FC_FLOWU_CACHE_RECORD_PTR_AS(fc, type, entry_idx) \
    ((type *)fc_flowu_cache_record_ptr((fc), (entry_idx)))

#define FC_FLOWU_CACHE_RECORD_CPTR_AS(fc, type, entry_idx) \
    ((const type *)fc_flowu_cache_record_cptr((fc), (entry_idx)))

#define FC_FLOWU_CACHE_RECORD_FROM_ENTRY(type, member, entry_ptr) \
    RIX_CONTAINER_OF((entry_ptr), type, member)

#define FC_FLOWU_CACHE_ENTRY_FROM_RECORD(record_ptr, member) \
    FC_MEMBER_PTR((record_ptr), member)

static inline int
fc_flowu_cache_init_attr(struct fc_cache_size_attr *attr,
                         const struct fc_flowu_config *cfg)
{
    if (attr == NULL ||
        attr->cache_ptr == NULL ||
        attr->buckets_ptr == NULL ||
        attr->pool_ptr == NULL)
        return -1;
    fc_flowu_cache_init((struct fc_flowu_cache *)attr->cache_ptr,
                        (struct rix_hash_bucket_s *)attr->buckets_ptr,
                        attr->nb_bk,
                        (struct fc_flowu_entry *)attr->pool_ptr,
                        attr->nb_entries,
                        cfg);
    ((struct fc_flowu_cache *)attr->cache_ptr)->bulk_ctx =
        (struct rix_hash_find_ctx_s *)attr->scratch_ptr;
    ((struct fc_flowu_cache *)attr->cache_ptr)->bulk_ctx_count =
        attr->scratch_ctx_count;
    return 0;
}
void fc_flowu_cache_flush(struct fc_flowu_cache *fc);
unsigned fc_flowu_cache_nb_entries(const struct fc_flowu_cache *fc);
/* bulk operations */
void fc_flowu_cache_find_bulk(struct fc_flowu_cache *fc,
                               const struct flowu_key *keys,
                               unsigned nb_keys, u64 now,
                               struct fc_flowu_result *results);
void fc_flowu_cache_findadd_bulk(struct fc_flowu_cache *fc,
                                  const struct flowu_key *keys,
                                  unsigned nb_keys, u64 now,
                                  struct fc_flowu_result *results);
void fc_flowu_cache_findadd_burst32(struct fc_flowu_cache *fc,
                                     const struct flowu_key *keys,
                                     unsigned nb_keys, u64 now,
                                     struct fc_flowu_result *results);
void fc_flowu_cache_add_bulk(struct fc_flowu_cache *fc,
                              const struct flowu_key *keys,
                              unsigned nb_keys, u64 now,
                              struct fc_flowu_result *results);
void fc_flowu_cache_del_bulk(struct fc_flowu_cache *fc,
                              const struct flowu_key *keys,
                              unsigned nb_keys);
void fc_flowu_cache_del_idx_bulk(struct fc_flowu_cache *fc,
                                  const u32 *idxs, unsigned nb_idxs);

/* single-key convenience */
u32 fc_flowu_cache_find(struct fc_flowu_cache *fc,
                              const struct flowu_key *key, u64 now);
u32 fc_flowu_cache_findadd(struct fc_flowu_cache *fc,
                                 const struct flowu_key *key, u64 now);
u32 fc_flowu_cache_add(struct fc_flowu_cache *fc,
                             const struct flowu_key *key, u64 now);
void fc_flowu_cache_del(struct fc_flowu_cache *fc,
                         const struct flowu_key *key);
int fc_flowu_cache_del_idx(struct fc_flowu_cache *fc, u32 entry_idx);

/* maintenance */
unsigned fc_flowu_cache_maintain(struct fc_flowu_cache *fc,
                                  unsigned start_bk, unsigned bucket_count,
                                  u64 now);
unsigned fc_flowu_cache_maintain_step_ex(struct fc_flowu_cache *fc,
                                          unsigned start_bk,
                                          unsigned bucket_count,
                                          unsigned skip_threshold,
                                          u64 now);
unsigned fc_flowu_cache_maintain_step(struct fc_flowu_cache *fc,
                                       u64 now, int idle);

/* query */
void fc_flowu_cache_stats(const struct fc_flowu_cache *fc,
                           struct fc_flowu_stats *out);

int fc_flowu_cache_walk(struct fc_flowu_cache *fc,
                         int (*cb)(u32 entry_idx, void *arg),
                         void *arg);

#endif /* _FLOWU_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
