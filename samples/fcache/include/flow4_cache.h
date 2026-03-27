/**
 * @file flow4_cache.h
 * @brief IPv4 5-tuple flow cache built on cuckoo hashing (fcache).
 *
 * A high-performance, fixed-size flow cache for IPv4 5-tuple lookups.
 * Internally backed by a rix_hash cuckoo hash table with 4-stage
 * N-ahead pipelined batch lookup (hash_key_2bk -> scan_bk_empties ->
 * prefetch_node -> cmp_key_empties) to hide DRAM latency.
 *
 * In the search+insert datapath (`findadd` / `findadd_bulk`), misses are
 * inserted inline during the final compare stage instead of being queued
 * for a separate batch-insert pass.
 *
 * Typical datapath usage:
 * @code
 *   // 1. Batch lookup + auto-fill misses
 *   fc_flow4_cache_findadd_bulk(&fc, keys, BATCH, now_tsc, results);
 *
 *   // 2. Process results (results[i].entry_idx != 0 for all unless full)
 *   // 3. Periodic maintenance  (expire stale entries, cursor-managed)
 *   fc_flow4_cache_maintain_step(&fc, now_tsc, 0);
 * @endcode
 *
 * Orthogonal API:
 *   find     / find_bulk     -- search only (no insert)
 *   findadd  / findadd_bulk  -- search + insert on miss
 *   add      / add_bulk      -- insert only (no search)
 *   del      / del_bulk      -- remove by key
 *   del_idx  / del_idx_bulk  -- remove by pool index
 *
 * Entries are 64-byte cache-line aligned.  The cache uses TSC-based
 * timestamps for expiration with adaptive timeout scaling based on
 * fill level.
 */

/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_CACHE_H_
#define _FLOW4_CACHE_H_

#include <stdint.h>
#include <rix/rix_hash.h>
#include <rix/rix_queue.h>
#include "fc_cache_common.h"

/** @brief Cache-line size used for entry alignment. */
#define FC_CACHE_LINE_SIZE 64u

#ifndef FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
/** @brief Default insert-pressure threshold (empty slots per bucket). */
#define FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

/* Pipeline geometry defined in fc_cache_generate.h (single source of truth).
 *
 *  FLOW_CACHE_LOOKUP_STEP_KEYS   keys processed per pipeline stage.
 *  FLOW_CACHE_LOOKUP_AHEAD_STEPS number of step iterations between stages.
 *  FLOW_CACHE_LOOKUP_AHEAD_KEYS  total look-ahead window (derived).
 *
 *  Defaults: 8 x 4 = 32 keys.  Sized for hyper-threaded cores sharing
 *  L1/L2 prefetch queues.
 */

/**
 * @brief IPv4 5-tuple lookup key (24-byte fixed-width object).
 *
 * The common prefix through @c vrfid is intentionally ordered to match
 * @c fc_flowu_key as closely as possible for easier hex-dump inspection.
 * The @c family field is always 4 for flow4. The trailing @c pad and
 * @c zero fields must always be 0. Together they form a fixed 24-byte key
 * object (3 x 8 bytes), enabling the optimised flow4 CRC32C / compare path
 * on x86-64.
 */
struct fc_flow4_key {
    uint8_t  family;            /**< Always FC_FLOW_FAMILY_IPV4 (4). */
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;               /**< Must be 0. */
    uint32_t vrfid;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t zero;              /**< Must be 0; completes a fixed 24B object. */
};

static inline struct fc_flow4_key
fc_flow4_key_make(uint32_t src_ip, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    return (struct fc_flow4_key){
        .family = FC_FLOW_FAMILY_IPV4,
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

/**
 * @brief Per-key result returned by lookup / fill operations.
 *
 * @c entry_idx is a 1-origin pool index.  Zero means the key was not
 * found (miss) or the cache was full.  Use @c RIX_PTR_FROM_IDX(pool, idx)
 * to obtain the entry pointer.
 */
struct fc_flow4_result {
    uint32_t entry_idx; /**< 1-origin pool index; 0 = miss / full. */
};

/**
 * @brief Cache entry, one per cache line (64 bytes).
 *
 * Managed internally; callers access entries through pool + entry_idx.
 */
struct fc_flow4_entry {
    struct fc_flow4_key   key;          /**< 24B fixed-width lookup key. */
    uint32_t               cur_hash;     /**< Hash stored for O(1) remove. */
    uint64_t               last_ts;      /**< Last-access TSC; 0 = free. */
    RIX_SLIST_ENTRY(struct fc_flow4_entry) free_link;
    uint16_t               slot;         /**< Slot within current bucket. */
    uint16_t               reserved1;
    uint8_t                reserved0[16];
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc_flow4_entry) == FC_CACHE_LINE_SIZE,
                  "fc_flow4_entry must be 64 bytes");

RIX_HASH_HEAD(fc_flow4_ht);
RIX_SLIST_HEAD(fc_flow4_free_head, fc_flow4_entry);

/**
 * @brief Initialization parameters.
 *
 * Pass NULL to fc_flow4_cache_init() to use built-in defaults
 * (timeout_tsc = 1000000, pressure = FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS).
 */
struct fc_flow4_config {
    uint64_t timeout_tsc;           /**< Entry lifetime in TSC ticks.
                                         Adaptive scaling shrinks this
                                         as the cache fills. */
    unsigned pressure_empty_slots;  /**< Insert-pressure threshold.
                                         Relief eviction triggers when a
                                         target bucket has <= this many
                                         empty slots. */
    uint64_t maint_interval_tsc;    /**< Minimum interval between GC runs
                                         (TSC ticks).  0 = every call. */
    unsigned maint_base_bk;         /**< Buckets per GC step at 1x scale.
                                         0 = nb_bk (full sweep). */
    unsigned maint_fill_threshold;  /**< Fill count increase that triggers
                                         GC scale-up.  0 = disabled. */
};

/**
 * @brief Cumulative counters (monotonically increasing).
 *
 * Retrieve with fc_flow4_cache_stats().  All fields are updated
 * atomically per call (single-writer assumed).
 */
struct fc_flow4_stats {
    uint64_t lookups;               /**< Keys submitted to find/findadd. */
    uint64_t hits;                  /**< Keys found (find/findadd hit). */
    uint64_t misses;                /**< Keys not found (find/findadd miss). */
    uint64_t fills;                 /**< Entries inserted by add/findadd paths. */
    uint64_t fill_full;             /**< Inserts failed (cache full). */
    uint64_t relief_calls;          /**< Times insert-relief was invoked. */
    uint64_t relief_bucket_checks;  /**< Buckets scanned by relief. */
    uint64_t relief_evictions;      /**< Entries evicted by relief (total). */
    uint64_t relief_bk0_evictions;  /**< Relief evictions from primary bucket. */
    uint64_t relief_bk1_evictions;  /**< Relief evictions from alternate bucket. */
    uint64_t oldest_reclaim_calls;  /**< Last-resort oldest reclaim attempts. */
    uint64_t oldest_reclaim_evictions; /**< Entries evicted by oldest reclaim. */
    uint64_t maint_calls;           /**< Times maintain was called. */
    uint64_t maint_bucket_checks;   /**< Buckets scanned by maintain. */
    uint64_t maint_evictions;       /**< Entries evicted by maintain. */
    uint64_t maint_step_calls;      /**< Times maintain_step was called. */
    uint64_t maint_step_skipped_bks;/**< Buckets skipped by SIMD empty check. */
};

enum fc_flow4_event {
    FC_FLOW4_EVENT_ALLOC = 1u,
    FC_FLOW4_EVENT_FREE_DELETE,
    FC_FLOW4_EVENT_FREE_TIMEOUT,
    FC_FLOW4_EVENT_FREE_PRESSURE,
    FC_FLOW4_EVENT_FREE_OLDEST,
    FC_FLOW4_EVENT_FREE_FLUSH,
    FC_FLOW4_EVENT_FREE_ROLLBACK,
};

typedef void (*fc_flow4_event_cb)(enum fc_flow4_event event,
                                  uint32_t entry_idx,
                                  void *arg);

/**
 * @brief Flow cache instance.
 *
 * All storage (buckets, pool) is caller-provided; the cache itself
 * contains only metadata and pointers.  This allows placement in
 * shared memory with no internal heap allocation.
 */
struct fc_flow4_cache {
    /* --- CL0: lookup / fill hot path --- */
    struct rix_hash_bucket_s  *buckets;
    struct fc_flow4_entry    *pool;
    unsigned char            *pool_base;
    size_t                    pool_stride;
    size_t                    pool_entry_offset;
    struct fc_flow4_ht        ht_head;
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
    unsigned                   last_maint_start_bk;  /**< Start bk of last sweep. */
    unsigned                   last_maint_sweep_bk;   /**< Buckets swept last time. */
    fc_flow4_event_cb          event_cb;
    void                      *event_cb_arg;
    struct rix_hash_find_ctx_s *bulk_ctx;            /**< Optional external scratch. */
    unsigned                   bulk_ctx_count;        /**< Scratch ctx count. */
    struct fc_flow4_free_head free_head;
    struct fc_flow4_stats     stats;
};

/**
 * @brief Query rounded sizing, alignments, and packed layout footprint.
 *
 * @p nb_entries is rounded up to the cache's internal pool granularity.
 * Bucket count is derived from the rounded entry count.
 *
 * @param[in]  nb_entries Requested live-entry capacity.
 * @param[out] attr       Filled size/alignment attributes.
 * @return 0 on success, -1 if @p attr is NULL.
 */
static inline int
fc_flow4_cache_size_query(unsigned nb_entries, struct fc_cache_size_attr *attr)
{
    return fc_cache_size_query_common(nb_entries,
                                      sizeof(struct fc_flow4_cache),
                                      _Alignof(struct fc_flow4_cache),
                                      sizeof(struct fc_flow4_entry),
                                      _Alignof(struct fc_flow4_entry),
                                      attr);
}

/**
 * @brief Initialize a flow cache.
 *
 * Zeroes all buckets and pool entries, builds the free list, and sets
 * internal thresholds derived from @p nb_bk.
 *
 * @param[out] fc          Cache instance to initialize.
 * @param[in]  buckets     Bucket array, @p nb_bk elements.
 *                         Must be aligned to @c sizeof(struct rix_hash_bucket_s).
 * @param[in]  nb_bk       Number of buckets (must be a power of 2).
 * @param[in]  pool        Entry pool, @p max_entries elements.
 *                         Must be 64-byte aligned.
 * @param[in]  max_entries Pool capacity (number of entries).
 * @param[in]  cfg         Configuration, or NULL for defaults.
 */
void fc_flow4_cache_init(struct fc_flow4_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc_flow4_entry *pool,
                          unsigned max_entries,
                          const struct fc_flow4_config *cfg);

void fc_flow4_cache_init_ex(struct fc_flow4_cache *fc,
                            struct rix_hash_bucket_s *buckets,
                            unsigned nb_bk,
                            void *array,
                            unsigned max_entries,
                            size_t stride,
                            size_t entry_offset,
                            const struct fc_flow4_config *cfg);

void fc_flow4_cache_set_event_cb(struct fc_flow4_cache *fc,
                                 fc_flow4_event_cb cb,
                                 void *arg);

/**
 * @brief Return the base address of the user record that owns @p entry_idx.
 *
 * For @c fc_flow4_cache_init(), this is equivalent to
 * @c &pool[entry_idx-1].  For @c fc_flow4_cache_init_ex(), this returns the
 * fixed-stride record base passed by the caller.  Any bytes outside the
 * embedded @c struct fc_flow4_entry are opaque to the library and may be
 * freely interpreted by the user.
 *
 * @param[in] fc         Cache instance.
 * @param[in] entry_idx  1-based entry index.
 * @return Record base, or NULL on invalid input.
 */
static inline void *
fc_flow4_cache_record_ptr(struct fc_flow4_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return (void *)(fc->pool_base
                    + ((size_t)(entry_idx - 1u) * fc->pool_stride));
}

static inline const void *
fc_flow4_cache_record_cptr(const struct fc_flow4_cache *fc, uint32_t entry_idx)
{
    if (fc == NULL || entry_idx == 0u || entry_idx > fc->max_entries)
        return NULL;
    return (const void *)(fc->pool_base
                          + ((size_t)(entry_idx - 1u) * fc->pool_stride));
}

/**
 * @brief Return the embedded flow-cache entry for @p entry_idx.
 *
 * This is the public counterpart of the layout indirection used internally by
 * @c fc_flow4_cache_init_ex().
 *
 * @param[in] fc         Cache instance.
 * @param[in] entry_idx  1-based entry index.
 * @return Entry pointer, or NULL on invalid input.
 */
static inline struct fc_flow4_entry *
fc_flow4_cache_entry_ptr(struct fc_flow4_cache *fc, uint32_t entry_idx)
{
    unsigned char *rec = (unsigned char *)fc_flow4_cache_record_ptr(fc,
                                                                    entry_idx);
    void *entryp;

    if (rec == NULL)
        return NULL;
    entryp = __builtin_assume_aligned(rec + fc->pool_entry_offset,
                                      _Alignof(struct fc_flow4_entry));
    return (struct fc_flow4_entry *)entryp;
}

static inline const struct fc_flow4_entry *
fc_flow4_cache_entry_cptr(const struct fc_flow4_cache *fc, uint32_t entry_idx)
{
    const unsigned char *rec =
        (const unsigned char *)fc_flow4_cache_record_cptr(fc, entry_idx);
    const void *entryp;

    if (rec == NULL)
        return NULL;
    entryp = __builtin_assume_aligned(rec + fc->pool_entry_offset,
                                      _Alignof(struct fc_flow4_entry));
    return (const struct fc_flow4_entry *)entryp;
}

static inline size_t
fc_flow4_cache_record_stride(const struct fc_flow4_cache *fc)
{
    return fc != NULL ? fc->pool_stride : 0u;
}

static inline size_t
fc_flow4_cache_entry_offset(const struct fc_flow4_cache *fc)
{
    return fc != NULL ? fc->pool_entry_offset : 0u;
}

#define FC_FLOW4_CACHE_RECORD_PTR_AS(fc, type, entry_idx) \
    ((type *)fc_flow4_cache_record_ptr((fc), (entry_idx)))

#define FC_FLOW4_CACHE_RECORD_CPTR_AS(fc, type, entry_idx) \
    ((const type *)fc_flow4_cache_record_cptr((fc), (entry_idx)))

static inline int
fc_flow4_cache_init_attr(struct fc_cache_size_attr *attr,
                         const struct fc_flow4_config *cfg)
{
    if (attr == NULL ||
        attr->cache_ptr == NULL ||
        attr->buckets_ptr == NULL ||
        attr->pool_ptr == NULL)
        return -1;
    fc_flow4_cache_init((struct fc_flow4_cache *)attr->cache_ptr,
                        (struct rix_hash_bucket_s *)attr->buckets_ptr,
                        attr->nb_bk,
                        (struct fc_flow4_entry *)attr->pool_ptr,
                        attr->nb_entries,
                        cfg);
    ((struct fc_flow4_cache *)attr->cache_ptr)->bulk_ctx =
        (struct rix_hash_find_ctx_s *)attr->scratch_ptr;
    ((struct fc_flow4_cache *)attr->cache_ptr)->bulk_ctx_count =
        attr->scratch_ctx_count;
    return 0;
}

/**
 * @brief Remove all entries and reset the cache to its initial state.
 *
 * Bucket and pool memory is reused (not freed).  Stats are preserved.
 *
 * @param[in,out] fc  Cache instance.
 */
void fc_flow4_cache_flush(struct fc_flow4_cache *fc);

/**
 * @brief Return the number of live entries in the cache.
 *
 * @param[in] fc  Cache instance.
 * @return Number of entries currently stored.
 */
unsigned fc_flow4_cache_nb_entries(const struct fc_flow4_cache *fc);

/*===========================================================================
 * Bulk operations (pipeline-optimized, dispatched through ops table)
 *===========================================================================*/

/**
 * @brief Pipelined batch lookup (search only, no insert).
 *
 * 4-stage N-ahead pipeline.  Hits update @c last_ts to @p now
 * (pass @c now=0 to suppress timestamp update).  Misses set
 * @c entry_idx=0.
 *
 * @param[in,out] fc        Cache instance.
 * @param[in]     keys      Array of @p nb_keys lookup keys.
 * @param[in]     nb_keys   Number of keys.
 * @param[in]     now       Current TSC; 0 = no timestamp update.
 * @param[out]    results   Per-key results.
 */
void fc_flow4_cache_find_bulk(struct fc_flow4_cache *fc,
                               const struct fc_flow4_key *keys,
                               unsigned nb_keys, uint64_t now,
                               struct fc_flow4_result *results);

/**
 * @brief Pipelined batch lookup + automatic miss insertion.
 *
 * 4-stage N-ahead pipeline.  Hits update @c last_ts.  Misses are
 * inserted inline (hash reused, no rehash).  On return,
 * @c results[i].entry_idx is non-zero unless the cache is full.
 *
 * @param[in,out] fc        Cache instance.
 * @param[in]     keys      Array of @p nb_keys lookup keys.
 * @param[in]     nb_keys   Number of keys.
 * @param[in]     now       Current TSC timestamp.
 * @param[out]    results   Per-key results.
 */
void fc_flow4_cache_findadd_bulk(struct fc_flow4_cache *fc,
                                  const struct fc_flow4_key *keys,
                                  unsigned nb_keys, uint64_t now,
                                  struct fc_flow4_result *results);

/**
 * @brief Small-burst lookup + automatic miss insertion, specialized for up to 32 keys.
 *
 * This API keeps the same semantics as @c fc_flow4_cache_findadd_bulk but uses
 * a shorter lookahead pipeline that was introduced for DPDK-style small bursts.
 * It is valid for any @p nb_keys and is intended for algorithm comparison
 * against the regular bulk path.
 *
 * @param[in,out] fc        Cache instance.
 * @param[in]     keys      Array of @p nb_keys lookup keys.
 * @param[in]     nb_keys   Number of keys.
 * @param[in]     now       Current TSC timestamp.
 * @param[out]    results   Per-key results.
 */
void fc_flow4_cache_findadd_burst32(struct fc_flow4_cache *fc,
                                     const struct fc_flow4_key *keys,
                                     unsigned nb_keys, uint64_t now,
                                     struct fc_flow4_result *results);

/**
 * @brief Pipelined batch insert (no duplicate check).
 *
 * 2-stage pipeline (hash+prefetch -> alloc+insert).  Always inserts
 * a new entry; caller must ensure no duplicate exists.
 *
 * @param[in,out] fc        Cache instance.
 * @param[in]     keys      Array of @p nb_keys keys to insert.
 * @param[in]     nb_keys   Number of keys.
 * @param[in]     now       Current TSC timestamp.
 * @param[out]    results   Per-key results (entry_idx of new entries).
 */
void fc_flow4_cache_add_bulk(struct fc_flow4_cache *fc,
                              const struct fc_flow4_key *keys,
                              unsigned nb_keys, uint64_t now,
                              struct fc_flow4_result *results);

/**
 * @brief Pipelined batch delete by key.
 *
 * 4-stage pipeline (hash -> scan -> prefetch -> match+remove).
 *
 * @param[in,out] fc        Cache instance.
 * @param[in]     keys      Array of @p nb_keys keys to remove.
 * @param[in]     nb_keys   Number of keys.
 */
void fc_flow4_cache_del_bulk(struct fc_flow4_cache *fc,
                              const struct fc_flow4_key *keys,
                              unsigned nb_keys);

/**
 * @brief Pipelined batch delete by pool index.
 *
 * 2-stage pipeline (prefetch -> validate+remove).
 *
 * @param[in,out] fc     Cache instance.
 * @param[in]     idxs   Array of 1-origin pool indices.
 * @param[in]     nb_idxs Number of indices.
 */
void fc_flow4_cache_del_idx_bulk(struct fc_flow4_cache *fc,
                                  const uint32_t *idxs,
                                  unsigned nb_idxs);

/*===========================================================================
 * Single-key convenience wrappers (call bulk with n=1)
 *===========================================================================*/

/** @brief Find a single key.  Returns 1-origin entry_idx or 0. */
uint32_t fc_flow4_cache_find(struct fc_flow4_cache *fc,
                              const struct fc_flow4_key *key,
                              uint64_t now);

/** @brief Find or insert a single key.  Returns entry_idx or 0 if full. */
uint32_t fc_flow4_cache_findadd(struct fc_flow4_cache *fc,
                                 const struct fc_flow4_key *key,
                                 uint64_t now);

/** @brief Insert a single key.  Returns entry_idx or 0 if full. */
uint32_t fc_flow4_cache_add(struct fc_flow4_cache *fc,
                             const struct fc_flow4_key *key,
                             uint64_t now);

/** @brief Remove a single entry by key. */
void fc_flow4_cache_del(struct fc_flow4_cache *fc,
                         const struct fc_flow4_key *key);

/** @brief Remove a single entry by pool index.  Returns 1 if removed. */
int fc_flow4_cache_del_idx(struct fc_flow4_cache *fc, uint32_t entry_idx);

/**
 * @brief Expire stale entries from a range of buckets.
 *
 * Scans @p bucket_count consecutive buckets starting from @p start_bk
 * (wrapping at the table boundary) and removes any entry whose
 * @c last_ts is older than the effective timeout.  The effective
 * timeout is automatically scaled down as the cache fills.
 *
 * Designed to be called periodically with a rotating cursor, e.g.:
 * @code
 *   evicted = fc_flow4_cache_maintain(&fc, cursor, 64, now);
 *   cursor = (cursor + 64) & (nb_bk - 1);
 * @endcode
 *
 * @param[in,out] fc            Cache instance.
 * @param[in]     start_bk      First bucket index (masked internally).
 * @param[in]     bucket_count  Number of buckets to scan.
 * @param[in]     now           Current TSC timestamp.
 * @return Number of entries evicted.
 */
unsigned fc_flow4_cache_maintain(struct fc_flow4_cache *fc,
                                  unsigned start_bk,
                                  unsigned bucket_count,
                                  uint64_t now);

/**
 * @brief Adaptive GC for DPDK-style poll loops.
 *
 * Call unconditionally from the main loop.  Internally throttled:
 * a GC pass runs only when enough time has elapsed or enough new
 * entries have been inserted since the last run.  The sweep width
 * scales with the trigger ratio so idle->active transitions clean
 * up stale entries promptly.
 *
 * When @p idle is true the full table is swept regardless of
 * throttle state, allowing rapid cleanup when no packets arrive.
 *
 * Typical DPDK usage:
 * @code
 *   nb_rx = rte_eth_rx_burst(...);
 *   // lookup + fill ...
 *   fc_flow4_cache_maintain_step(&fc, now_tsc, nb_rx == 0);
 * @endcode
 *
 * @param[in,out] fc    Cache instance.
 * @param[in]     now   Current TSC timestamp.
 * @param[in]     idle  True when no traffic (e.g. rx_burst == 0).
 * @return Number of entries evicted.
 */
/**
 * @brief Low-level cursor-managed maintenance with full parameter control.
 *
 * Scans @p bucket_count buckets starting from @p start_bk, using
 * @p skip_threshold to filter sparse buckets.  The internal cursor
 * is set to @p start_bk before scanning and advanced afterwards.
 *
 * @param[in,out] fc              Cache instance.
 * @param[in]     start_bk        First bucket to scan (masked internally).
 * @param[in]     bucket_count    Number of buckets to scan.
 * @param[in]     skip_threshold  Buckets with used_slots <= this are skipped.
 *                                0 = evaluate all buckets.
 * @param[in]     now             Current TSC timestamp.
 * @return Number of entries evicted.
 */
unsigned fc_flow4_cache_maintain_step_ex(struct fc_flow4_cache *fc,
                                          unsigned start_bk,
                                          unsigned bucket_count,
                                          unsigned skip_threshold,
                                          uint64_t now);

unsigned fc_flow4_cache_maintain_step(struct fc_flow4_cache *fc,
                                       uint64_t now,
                                       int idle);


/**
 * @brief Snapshot cumulative statistics.
 *
 * Copies the internal counters into @p out.  The caller can diff
 * successive snapshots to obtain per-interval rates.
 *
 * @param[in]  fc   Cache instance.
 * @param[out] out  Destination for the snapshot.
 */
void fc_flow4_cache_stats(const struct fc_flow4_cache *fc,
                           struct fc_flow4_stats *out);

/**
 * @brief Iterate all live entries, calling @p cb for each.
 *
 * Walks the pool sequentially.  For each entry with @c last_ts != 0,
 * invokes @p cb(entry_idx, arg) where @c entry_idx is 1-origin.
 * If @p cb returns 0 the walk continues; if it returns a negative
 * value the walk aborts and that value is returned.
 *
 * @param[in,out] fc   Cache instance.
 * @param[in]     cb   Callback; return 0 to continue, <0 to abort.
 * @param[in]     arg  Opaque argument forwarded to @p cb.
 * @return 0 if all entries visited, or the negative value from @p cb.
 */
int fc_flow4_cache_walk(struct fc_flow4_cache *fc,
                         int (*cb)(uint32_t entry_idx, void *arg),
                         void *arg);

#endif /* _FLOW4_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
