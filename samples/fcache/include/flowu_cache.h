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
#include "fc_cache_common.h"

#ifndef FC_CACHE_LINE_SIZE
#define FC_CACHE_LINE_SIZE 64u
#endif
#ifndef FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

struct fc_flowu_key {
    uint8_t  family;        /*  1B: FC_FLOW_FAMILY_IPV4 / IPV6 */
    uint8_t  proto;         /*  1B */
    uint16_t src_port;      /*  2B */
    uint16_t dst_port;      /*  2B */
    uint16_t pad;           /*  2B */
    uint32_t vrfid;         /*  4B */
    union {                 /* 32B */
        struct {
            uint32_t src;
            uint32_t dst;
            uint8_t  _pad[24];
        } v4;
        struct {
            uint8_t  src[16];
            uint8_t  dst[16];
        } v6;
    } addr;
} __attribute__((packed));  /* 44B total */

/*===========================================================================
 * Key construction helpers
 *===========================================================================*/
static inline struct fc_flowu_key
fc_flowu_key_v4(uint32_t src_ip, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    struct fc_flowu_key k;
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

static inline struct fc_flowu_key
fc_flowu_key_v6(const uint8_t *src_ip, const uint8_t *dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    struct fc_flowu_key k;
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
    uint32_t entry_idx; /* 1-origin; 0 = miss / full */
};

struct fc_flowu_entry {
    struct fc_flowu_key   key;          /* 44B */
    uint32_t               cur_hash;     /* current-bucket hash */
    uint64_t               last_ts;      /* 0 = free / invalid */
    RIX_SLIST_ENTRY(struct fc_flowu_entry) free_link;
    uint16_t               slot;         /* slot in current bucket */
    uint16_t               reserved1;
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc_flowu_entry) == FC_CACHE_LINE_SIZE,
                  "fc_flowu_entry must be 64 bytes");

RIX_HASH_HEAD(fc_flowu_ht);
RIX_SLIST_HEAD(fc_flowu_free_head, fc_flowu_entry);

struct fc_flowu_config {
    uint64_t timeout_tsc;
    unsigned pressure_empty_slots;
    uint64_t maint_interval_tsc;
    unsigned maint_base_bk;
    unsigned maint_fill_threshold;
};

struct fc_flowu_stats {
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
                                  uint32_t entry_idx,
                                  void *arg);

struct fc_flowu_cache {
    /* --- CL0: lookup / fill hot path --- */
    struct rix_hash_bucket_s  *buckets;
    struct fc_flowu_entry    *pool;
    unsigned char            *pool_base;
    size_t                    pool_stride;
    size_t                    pool_entry_offset;
    struct fc_flowu_ht        ht_head;
    uint64_t                   timeout_tsc;
    uint64_t                   eff_timeout_tsc;
    uint64_t                   timeout_min_tsc;
    unsigned                   nb_bk;
    unsigned                   max_entries;
    unsigned                   total_slots;
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
fc_flowu_cache_record_ptr(struct fc_flowu_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_PTR(fc->pool_base, fc->pool_stride, entry_idx);
}

static inline const void *
fc_flowu_cache_record_cptr(const struct fc_flowu_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_CPTR(fc->pool_base, fc->pool_stride, entry_idx);
}

static inline struct fc_flowu_entry *
fc_flowu_cache_entry_ptr(struct fc_flowu_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return FC_RECORD_MEMBER_PTR(fc->pool_base, fc->pool_stride, entry_idx,
                                fc->pool_entry_offset, struct fc_flowu_entry);
}

static inline const struct fc_flowu_entry *
fc_flowu_cache_entry_cptr(const struct fc_flowu_cache *fc, uint32_t entry_idx)
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
    return fc != NULL ? fc->pool_stride : 0u;
}

static inline size_t
fc_flowu_cache_entry_offset(const struct fc_flowu_cache *fc)
{
    return fc != NULL ? fc->pool_entry_offset : 0u;
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
                               const struct fc_flowu_key *keys,
                               unsigned nb_keys, uint64_t now,
                               struct fc_flowu_result *results);
void fc_flowu_cache_findadd_bulk(struct fc_flowu_cache *fc,
                                  const struct fc_flowu_key *keys,
                                  unsigned nb_keys, uint64_t now,
                                  struct fc_flowu_result *results);
void fc_flowu_cache_add_bulk(struct fc_flowu_cache *fc,
                              const struct fc_flowu_key *keys,
                              unsigned nb_keys, uint64_t now,
                              struct fc_flowu_result *results);
void fc_flowu_cache_del_bulk(struct fc_flowu_cache *fc,
                              const struct fc_flowu_key *keys,
                              unsigned nb_keys);
void fc_flowu_cache_del_idx_bulk(struct fc_flowu_cache *fc,
                                  const uint32_t *idxs, unsigned nb_idxs);

/* single-key convenience */
uint32_t fc_flowu_cache_find(struct fc_flowu_cache *fc,
                              const struct fc_flowu_key *key, uint64_t now);
uint32_t fc_flowu_cache_findadd(struct fc_flowu_cache *fc,
                                 const struct fc_flowu_key *key, uint64_t now);
uint32_t fc_flowu_cache_add(struct fc_flowu_cache *fc,
                             const struct fc_flowu_key *key, uint64_t now);
void fc_flowu_cache_del(struct fc_flowu_cache *fc,
                         const struct fc_flowu_key *key);
int fc_flowu_cache_del_idx(struct fc_flowu_cache *fc, uint32_t entry_idx);

/* maintenance */
unsigned fc_flowu_cache_maintain(struct fc_flowu_cache *fc,
                                  unsigned start_bk, unsigned bucket_count,
                                  uint64_t now);
unsigned fc_flowu_cache_maintain_step_ex(struct fc_flowu_cache *fc,
                                          unsigned start_bk,
                                          unsigned bucket_count,
                                          unsigned skip_threshold,
                                          uint64_t now);
unsigned fc_flowu_cache_maintain_step(struct fc_flowu_cache *fc,
                                       uint64_t now, int idle);

/* query */
void fc_flowu_cache_stats(const struct fc_flowu_cache *fc,
                           struct fc_flowu_stats *out);

int fc_flowu_cache_walk(struct fc_flowu_cache *fc,
                         int (*cb)(uint32_t entry_idx, void *arg),
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
