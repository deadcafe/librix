/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW6_CACHE_H_
#define _FLOW6_CACHE_H_

#include <stdint.h>
#include <string.h>
#include <rix/rix_hash.h>
#include <rix/rix_queue.h>
#include <flow/flow_key.h>
#include "fc_cache_common.h"

#ifndef FC_CACHE_LINE_SIZE
#define FC_CACHE_LINE_SIZE 64u
#endif
#ifndef FC_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FC_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

static inline struct flow6_key
fc_flow6_key_make(const uint8_t *src_ip, const uint8_t *dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    struct flow6_key k;

    memset(&k, 0, sizeof(k));
    k.family = FC_FLOW_FAMILY_IPV6;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.proto = proto;
    k.vrfid = vrfid;
    memcpy(k.src_ip, src_ip, 16);
    memcpy(k.dst_ip, dst_ip, 16);
    return k;
}

struct fc_flow6_result {
    uint32_t entry_idx; /* 1-origin; 0 = miss / full */
};

struct fc_flow6_entry {
    struct flow6_entry hdr;
    RIX_SLIST_ENTRY(struct fc_flow6_entry) free_link;
} __attribute__((packed, aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc_flow6_entry) == FC_CACHE_LINE_SIZE,
                  "fc_flow6_entry must be 64 bytes");

RIX_HASH_HEAD(fc_flow6_ht);
RIX_SLIST_HEAD(fc_flow6_free_head, fc_flow6_entry);

struct fc_flow6_config {
    uint64_t timeout_tsc;
    unsigned ts_shift;
    unsigned pressure_empty_slots;
    uint64_t maint_interval_tsc;
    unsigned maint_base_bk;
    unsigned maint_fill_threshold;
};

struct fc_flow6_stats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t fills;
    uint64_t fill_full;
    uint64_t relief_calls;
    uint64_t relief_bucket_checks;
    uint64_t relief_evictions;
    uint64_t relief_bk0_evictions;
    uint64_t relief_bk1_evictions;
    uint64_t oldest_reclaim_calls;
    uint64_t oldest_reclaim_evictions;
    uint64_t maint_calls;
    uint64_t maint_bucket_checks;
    uint64_t maint_evictions;
    uint64_t maint_step_calls;
    uint64_t maint_step_skipped_bks;
};

enum fc_flow6_event {
    FC_FLOW6_EVENT_ALLOC = 1u,
    FC_FLOW6_EVENT_FREE_DELETE,
    FC_FLOW6_EVENT_FREE_TIMEOUT,
    FC_FLOW6_EVENT_FREE_PRESSURE,
    FC_FLOW6_EVENT_FREE_OLDEST,
    FC_FLOW6_EVENT_FREE_FLUSH,
    FC_FLOW6_EVENT_FREE_ROLLBACK,
};

typedef void (*fc_flow6_event_cb)(enum fc_flow6_event event,
                                  uint32_t entry_idx,
                                  void *arg);

struct fc_flow6_cache {
    /* --- CL0: lookup / fill hot path --- */
    struct rix_hash_bucket_s  *buckets;
    struct fc_flow6_entry    *pool;
    unsigned char            *pool_base;
    size_t                    pool_stride;
    size_t                    pool_entry_offset;
    struct fc_flow6_ht        ht_head;
    uint64_t                   timeout_tsc;
    uint64_t                   eff_timeout_tsc;
    uint64_t                   timeout_min_tsc;
    unsigned                   nb_bk;
    unsigned                   max_entries;
    unsigned                   total_slots;
    uint8_t                    ts_shift;
    unsigned                   pressure_empty_slots;
    /* --- CL1 --- */
    unsigned                   timeout_lo_entries;
    unsigned                   timeout_hi_entries;
    unsigned                   relief_mid_entries;
    unsigned                   relief_hi_entries;
    unsigned                   maint_cursor;
    uint64_t                   last_maint_tsc;
    uint64_t                   last_maint_fills;
    uint64_t                   maint_interval_tsc;
    unsigned                   maint_base_bk;
    unsigned                   maint_fill_threshold;
    unsigned                   last_maint_start_bk;
    unsigned                   last_maint_sweep_bk;
    fc_flow6_event_cb          event_cb;
    void                      *event_cb_arg;
    struct rix_hash_find_ctx_s *bulk_ctx;
    unsigned                   bulk_ctx_count;
    struct fc_flow6_free_head free_head;
    struct fc_flow6_stats     stats;
};

static inline int
fc_flow6_cache_size_query(unsigned nb_entries, struct fc_cache_size_attr *attr)
{
    return fc_cache_size_query_common(nb_entries,
                                      sizeof(struct fc_flow6_cache),
                                      _Alignof(struct fc_flow6_cache),
                                      sizeof(struct fc_flow6_entry),
                                      _Alignof(struct fc_flow6_entry),
                                      attr);
}

void fc_flow6_cache_init(struct fc_flow6_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc_flow6_entry *pool,
                          unsigned max_entries,
                          const struct fc_flow6_config *cfg);
void fc_flow6_cache_init_ex(struct fc_flow6_cache *fc,
                            struct rix_hash_bucket_s *buckets,
                            unsigned nb_bk,
                            void *array,
                            unsigned max_entries,
                            size_t stride,
                            size_t entry_offset,
                            const struct fc_flow6_config *cfg);
void fc_flow6_cache_set_event_cb(struct fc_flow6_cache *fc,
                                 fc_flow6_event_cb cb,
                                 void *arg);

#define FC_FLOW6_CACHE_INIT_TYPED(fc, buckets, nb_bk, array, max_entries, type, member, cfg) \
    fc_flow6_cache_init_ex((fc), (buckets), (nb_bk), (array), (max_entries),  \
                           sizeof(type), offsetof(type, member), (cfg))

static inline void *
fc_flow6_cache_record_ptr(struct fc_flow6_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_PTR(fc->pool_base, fc->pool_stride, entry_idx);
}

static inline const void *
fc_flow6_cache_record_cptr(const struct fc_flow6_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_CPTR(fc->pool_base, fc->pool_stride, entry_idx);
}

static inline struct fc_flow6_entry *
fc_flow6_cache_entry_ptr(struct fc_flow6_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_MEMBER_PTR(fc->pool_base, fc->pool_stride, entry_idx,
                                fc->pool_entry_offset, struct fc_flow6_entry);
}

static inline const struct fc_flow6_entry *
fc_flow6_cache_entry_cptr(const struct fc_flow6_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_MEMBER_CPTR(fc->pool_base, fc->pool_stride, entry_idx,
                                 fc->pool_entry_offset,
                                 struct fc_flow6_entry);
}

static inline size_t
fc_flow6_cache_record_stride(const struct fc_flow6_cache *fc)
{
    return fc != NULL ? fc->pool_stride : 0;
}

static inline size_t
fc_flow6_cache_entry_offset(const struct fc_flow6_cache *fc)
{
    return fc != NULL ? fc->pool_entry_offset : 0;
}

#define FC_FLOW6_CACHE_RECORD_PTR_AS(fc, type, entry_idx) \
    ((type *)fc_flow6_cache_record_ptr((fc), (entry_idx)))

#define FC_FLOW6_CACHE_RECORD_CPTR_AS(fc, type, entry_idx) \
    ((const type *)fc_flow6_cache_record_cptr((fc), (entry_idx)))

#define FC_FLOW6_CACHE_RECORD_FROM_ENTRY(type, member, entry_ptr) \
    RIX_CONTAINER_OF((entry_ptr), type, member)

#define FC_FLOW6_CACHE_ENTRY_FROM_RECORD(record_ptr, member) \
    FC_MEMBER_PTR((record_ptr), member)

static inline int
fc_flow6_cache_init_attr(struct fc_cache_size_attr *attr,
                         const struct fc_flow6_config *cfg)
{
    if (attr == NULL ||
        attr->cache_ptr == NULL ||
        attr->buckets_ptr == NULL ||
        attr->pool_ptr == NULL)
        return -1;
    fc_flow6_cache_init((struct fc_flow6_cache *)attr->cache_ptr,
                        (struct rix_hash_bucket_s *)attr->buckets_ptr,
                        attr->nb_bk,
                        (struct fc_flow6_entry *)attr->pool_ptr,
                        attr->nb_entries,
                        cfg);
    ((struct fc_flow6_cache *)attr->cache_ptr)->bulk_ctx =
        (struct rix_hash_find_ctx_s *)attr->scratch_ptr;
    ((struct fc_flow6_cache *)attr->cache_ptr)->bulk_ctx_count =
        attr->scratch_ctx_count;
    return 0;
}
void fc_flow6_cache_flush(struct fc_flow6_cache *fc);
unsigned fc_flow6_cache_nb_entries(const struct fc_flow6_cache *fc);
/* bulk operations */
void fc_flow6_cache_find_bulk(struct fc_flow6_cache *fc,
                               const struct flow6_key *keys,
                               unsigned nb_keys, uint64_t now,
                               struct fc_flow6_result *results);
void fc_flow6_cache_findadd_bulk(struct fc_flow6_cache *fc,
                                  const struct flow6_key *keys,
                                  unsigned nb_keys, uint64_t now,
                                  struct fc_flow6_result *results);
void fc_flow6_cache_findadd_burst32(struct fc_flow6_cache *fc,
                                     const struct flow6_key *keys,
                                     unsigned nb_keys, uint64_t now,
                                     struct fc_flow6_result *results);
void fc_flow6_cache_add_bulk(struct fc_flow6_cache *fc,
                              const struct flow6_key *keys,
                              unsigned nb_keys, uint64_t now,
                              struct fc_flow6_result *results);
void fc_flow6_cache_del_bulk(struct fc_flow6_cache *fc,
                              const struct flow6_key *keys,
                              unsigned nb_keys);
void fc_flow6_cache_del_idx_bulk(struct fc_flow6_cache *fc,
                                  const uint32_t *idxs, unsigned nb_idxs);

/* single-key convenience */
uint32_t fc_flow6_cache_find(struct fc_flow6_cache *fc,
                              const struct flow6_key *key, uint64_t now);
uint32_t fc_flow6_cache_findadd(struct fc_flow6_cache *fc,
                                 const struct flow6_key *key, uint64_t now);
uint32_t fc_flow6_cache_add(struct fc_flow6_cache *fc,
                             const struct flow6_key *key, uint64_t now);
void fc_flow6_cache_del(struct fc_flow6_cache *fc,
                         const struct flow6_key *key);
int fc_flow6_cache_del_idx(struct fc_flow6_cache *fc, uint32_t entry_idx);

/* maintenance */
unsigned fc_flow6_cache_maintain(struct fc_flow6_cache *fc,
                                  unsigned start_bk, unsigned bucket_count,
                                  uint64_t now);
unsigned fc_flow6_cache_maintain_step_ex(struct fc_flow6_cache *fc,
                                          unsigned start_bk,
                                          unsigned bucket_count,
                                          unsigned skip_threshold,
                                          uint64_t now);
unsigned fc_flow6_cache_maintain_step(struct fc_flow6_cache *fc,
                                       uint64_t now, int idle);

/* query */
void fc_flow6_cache_stats(const struct fc_flow6_cache *fc,
                           struct fc_flow6_stats *out);

int fc_flow6_cache_walk(struct fc_flow6_cache *fc,
                         int (*cb)(uint32_t entry_idx, void *arg),
                         void *arg);

#endif /* _FLOW6_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
