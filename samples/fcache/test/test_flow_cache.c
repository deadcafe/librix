/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flow_cache.h"
#include "fc_cache_common.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

/*===========================================================================
 * Key makers
 *===========================================================================*/
static struct fc_flow4_key
make_key4(unsigned i)
{
    return fc_flow4_key_make(0x0a000001u + i,
                             0x0a100001u + i,
                             (uint16_t)(1000u + i),
                             (uint16_t)(2000u + i),
                             17u, 1u);
}

static struct fc_flow6_key
make_key6(unsigned i)
{
    uint8_t src[16] = {0x20, 0x01, 0x0d, 0xb8};
    uint8_t dst[16] = {0x20, 0x01, 0x0d, 0xb9};
    uint32_t v;

    v = i;
    memcpy(src + 12, &v, 4);
    v = i + 0x1000u;
    memcpy(dst + 12, &v, 4);
    return fc_flow6_key_make(src, dst,
                             (uint16_t)(1000u + i),
                             (uint16_t)(2000u + i),
                             6u, 1u);
}

static struct fc_flowu_key
make_keyu_v4(unsigned i)
{
    return fc_flowu_key_v4(
        0x0a000001u + i, 0x0a100001u + i,
        (uint16_t)(1000u + i), (uint16_t)(2000u + i),
        17u, 1u);
}

static struct fc_flowu_key
make_keyu_v6(unsigned i)
{
    uint8_t src[16] = {0x20, 0x01, 0x0d, 0xb8};
    uint8_t dst[16] = {0x20, 0x01, 0x0d, 0xb9};
    uint32_t v = i;

    memcpy(src + 12, &v, 4);
    v = i + 0x1000u;
    memcpy(dst + 12, &v, 4);
    return fc_flowu_key_v6(
        src, dst,
        (uint16_t)(1000u + i), (uint16_t)(2000u + i),
        6u, 1u);
}

/*===========================================================================
 * Macro-templated test body
 *
 * PREFIX   : flow4, flow6, flowu
 * KEY_T    : struct fc_flow4_key, etc.
 * RESULT_T : struct fc_flow4_result, etc.
 * ENTRY_T  : struct fc_flow4_entry, etc.
 * CACHE_T  : struct fc_flow4_cache, etc.
 * CONFIG_T : struct fc_flow4_config, etc.
 * STATS_T  : struct fc_flow4_stats, etc.
 * MAKE_KEY : make_key4, make_key6, make_keyu_v4
 *===========================================================================*/
#define COUNT_HITS(RESULT_T, results, n) \
    ({ unsigned _hits = 0u; \
       for (unsigned _i = 0; _i < (n); _i++) \
           if ((results)[_i].entry_idx != 0u) _hits++; \
       _hits; })

#define DEFINE_TESTS(PREFIX, KEY_T, RESULT_T, ENTRY_T, CACHE_T, CONFIG_T, STATS_T, MAKE_KEY) \
\
static void \
test_##PREFIX##_lookup_fill_remove(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " lookup/fill/remove\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(i); \
\
    /* First lookup auto-fills all misses */ \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, NB_KEYS, 1u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != NB_KEYS) \
        FAILF("nb_entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), NB_KEYS); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("result[%u] entry_idx is 0 after auto-fill", i); \
    } \
\
    /* Second lookup: all hit */ \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, NB_KEYS, 20u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("result[%u] entry_idx is 0", i); \
    } \
\
    if (!fc_##PREFIX##_cache_del_idx(&fc, results[3].entry_idx)) \
        FAIL("del_idx failed"); \
    if (fc_##PREFIX##_cache_del_idx(&fc, results[3].entry_idx)) \
        FAIL("del_idx should fail on removed entry"); \
\
    /* Removed key: lookup auto-fills it again */ \
    fc_##PREFIX##_cache_findadd_bulk(&fc, &keys[3], 1u, 30u, &results[3]); \
    if (results[3].entry_idx == 0u) \
        FAIL("removed key should be auto-filled"); \
} \
\
static void \
test_##PREFIX##_pressure_relief(void) \
{ \
    enum { NB_BK = 4u, MAX_ENTRIES = 32u, FILL = 32u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = 10u, .pressure_empty_slots = 15u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    KEY_T newcomer; \
    RESULT_T newcomer_result; \
\
    printf("[T] fc " #PREFIX " pressure relief on fill_miss\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(i); \
\
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != FILL) \
        FAILF("pressure test initial fill entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), FILL); \
\
    /* Newcomer: lookup auto-fills via relief eviction */ \
    newcomer = MAKE_KEY(1000u); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, &newcomer, 1u, 1000u, \
                                      &newcomer_result); \
    if (newcomer_result.entry_idx == 0u) \
        FAIL("newcomer_result.entry_idx should be non-zero"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != FILL) \
        FAILF("pressure test final entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), FILL); \
\
    /* Newcomer re-lookup: should hit */ \
    fc_##PREFIX##_cache_findadd_bulk(&fc, &newcomer, 1u, 1001u, \
                                      &newcomer_result); \
    if (newcomer_result.entry_idx == 0u) \
        FAIL("newcomer should hit after pressure-relief fill"); \
\
    /* One old entry was evicted */ \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, FILL, 1002u, results); \
    if (COUNT_HITS(RESULT_T, results, FILL) != FILL - 1u) \
        FAILF("expected exactly one old entry to be evicted, hits=%u", \
              COUNT_HITS(RESULT_T, results, FILL)); \
} \
\
static void \
test_##PREFIX##_fill_miss_full_without_relief(void) \
{ \
    enum { NB_BK = 4u, MAX_ENTRIES = 16u, FILL = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = UINT64_C(1) << 40, .pressure_empty_slots = 15u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    KEY_T newcomer; \
    RESULT_T newcomer_result; \
\
    printf("[T] fc " #PREFIX " oldest fallback on fresh full\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(2000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
\
    /* Newcomer: relief won't evict fresh entries, final fallback should. */ \
    newcomer = MAKE_KEY(3000u); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, &newcomer, 1u, 101u, \
                                      &newcomer_result); \
    if (newcomer_result.entry_idx == 0u) \
        FAIL("fresh-fill newcomer should insert via oldest fallback"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != FILL) \
        FAILF("fresh-fill entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), FILL); \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, FILL, 102u, results); \
    if (COUNT_HITS(RESULT_T, results, FILL) != FILL - 1u) \
        FAILF("fresh-fill expected exactly one eviction, hits=%u", \
              COUNT_HITS(RESULT_T, results, FILL)); \
} \
\
static void \
test_##PREFIX##_fill_miss_full_same_now_fails(void) \
{ \
    enum { NB_BK = 4u, MAX_ENTRIES = 16u, FILL = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = UINT64_C(1) << 40, .pressure_empty_slots = 15u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    KEY_T newcomer; \
    RESULT_T newcomer_result; \
\
    printf("[T] fc " #PREFIX " same-now fallback guard\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(2100u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
\
    newcomer = MAKE_KEY(3100u); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, &newcomer, 1u, 100u, \
                                      &newcomer_result); \
    if (newcomer_result.entry_idx != 0u) \
        FAILF("same-now newcomer_result.entry_idx=%u expected 0", \
              newcomer_result.entry_idx); \
} \
\
static void \
test_##PREFIX##_duplicate_miss_batch(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u, NB_KEYS = 2u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " duplicate miss batch\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    keys[0] = MAKE_KEY(4000u); \
    keys[1] = keys[0]; \
\
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, NB_KEYS, 1u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 1u) \
        FAILF("duplicate batch nb_entries=%u expected 1", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
    if (results[0].entry_idx == 0u || results[1].entry_idx == 0u) \
        FAIL("duplicate batch entry_idx should be non-zero"); \
    if (results[0].entry_idx != results[1].entry_idx) \
        FAILF("duplicate batch entry mismatch %u vs %u", \
              results[0].entry_idx, results[1].entry_idx); \
} \
\
static void \
test_##PREFIX##_flush_and_invalid_remove(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u, NB_KEYS = 4u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " flush and invalid remove\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(5000u + i); \
\
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, NB_KEYS, 1u, results); \
\
    if (fc_##PREFIX##_cache_del_idx(&fc, 0u)) \
        FAIL("del_idx(0) should fail"); \
    if (fc_##PREFIX##_cache_del_idx(&fc, MAX_ENTRIES + 1u)) \
        FAIL("del_idx(out-of-range) should fail"); \
\
    fc_##PREFIX##_cache_flush(&fc); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAILF("flush left nb_entries=%u expected 0", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
    /* Post-flush lookup auto-fills again */ \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, NB_KEYS, 20u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != NB_KEYS) \
        FAILF("post-flush nb_entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), NB_KEYS); \
} \
\
static void \
test_##PREFIX##_bulk_wraparound(void) \
{ \
    enum { NB_BK = 32u, MAX_ENTRIES = 256u, \
           NB_KEYS = FC_CACHE_BULK_CTX_COUNT + 17u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
    uint32_t idxs[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " bulk wraparound\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(5500u + i); \
\
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 1u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAIL("find_bulk should not insert during wraparound test"); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx != 0u) \
            FAILF("wraparound find miss: result[%u] should be 0", i); \
    } \
\
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, NB_KEYS, 100u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != NB_KEYS) \
        FAILF("wraparound findadd nb_entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), (unsigned)NB_KEYS); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("wraparound findadd: result[%u] should be non-zero", i); \
    } \
\
    fc_##PREFIX##_cache_del_bulk(&fc, keys, NB_KEYS); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAILF("wraparound del_bulk: nb_entries=%u expected 0", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
\
    fc_##PREFIX##_cache_add_bulk(&fc, keys, NB_KEYS, 200u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != NB_KEYS) \
        FAILF("wraparound add_bulk nb_entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), (unsigned)NB_KEYS); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("wraparound add_bulk: result[%u] should be non-zero", i); \
        idxs[i] = results[i].entry_idx; \
    } \
\
    fc_##PREFIX##_cache_del_idx_bulk(&fc, idxs, NB_KEYS); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAILF("wraparound del_idx_bulk: nb_entries=%u expected 0", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
\
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 300u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx != 0u) \
            FAILF("wraparound final miss: result[%u] should be 0", i); \
    } \
} \
\
static void \
test_##PREFIX##_maintenance(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, FILL = 24u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = 10u, .pressure_empty_slots = 1u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    STATS_T stats; \
    unsigned before_entries; \
    unsigned evicted; \
\
    printf("[T] fc " #PREFIX " maintenance bucket count\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(6000u + i); \
\
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
\
    before_entries = fc_##PREFIX##_cache_nb_entries(&fc); \
    evicted = fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1000u); \
    if (evicted == 0u) \
        FAIL("maintenance should evict at least one expired entry"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != before_entries - evicted) \
        FAILF("maintenance entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), before_entries - evicted); \
\
    fc_##PREFIX##_cache_stats(&fc, &stats); \
    if (stats.maint_calls != 1u) \
        FAILF("maint_calls=%" PRIu64 " expected 1", stats.maint_calls); \
    if (stats.maint_bucket_checks != NB_BK) \
        FAILF("maint_bucket_checks=%" PRIu64 " expected %u", \
              stats.maint_bucket_checks, NB_BK); \
    if (stats.maint_evictions != evicted) \
        FAILF("maint_evictions=%" PRIu64 " expected %u", \
              stats.maint_evictions, evicted); \
} \
\
static void \
test_##PREFIX##_timeout_boundary(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = 100u, .pressure_empty_slots = 1u }; \
    KEY_T keys[2]; \
    RESULT_T results[2]; \
\
    printf("[T] fc " #PREFIX " timeout boundary\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    keys[0] = MAKE_KEY(7000u); \
    keys[1] = MAKE_KEY(7001u); \
\
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, 2u, 1000u, results); \
\
    if (fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1099u) != 0u) \
        FAIL("should not evict before timeout boundary"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 2u) \
        FAIL("entries should remain at timeout boundary - 1"); \
\
    if (fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1101u) == 0u) \
        FAIL("should evict after timeout boundary"); \
} \
\
static void \
test_##PREFIX##_maintain_step(void) \
{ \
    enum { NB_BK = 16u, MAX_ENTRIES = 64u, FILL = 48u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    STATS_T stats; \
    unsigned filled; \
    unsigned total_evicted; \
\
    printf("[T] fc " #PREFIX " maintain_step\n"); \
    /* interval=0 -> run every call, base_bk=nb_bk -> full sweep */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(8000u + i); \
\
    /* Fill entries at ts=100 */ \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    filled = fc_##PREFIX##_cache_nb_entries(&fc); \
    if (filled < FILL / 2u) \
        FAILF("maintain_step initial fill too low: %u/%u", filled, \
              (unsigned)FILL); \
\
    /* Before timeout: should evict nothing */ \
    if (fc_##PREFIX##_cache_maintain_step(&fc, 199u, 0) != 0u) \
        FAIL("maintain_step pre-timeout should not evict"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != filled) \
        FAIL("maintain_step pre-timeout entries changed"); \
\
    /* After timeout: full sweep evicts most entries. */ \
    /* Sparse buckets (used_slots <= pressure_empty_slots) are */ \
    /* intentionally skipped. */ \
    total_evicted = fc_##PREFIX##_cache_maintain_step(&fc, 1000u, 0); \
    if (total_evicted == 0u) \
        FAIL("maintain_step should evict expired entries"); \
    unsigned remaining = fc_##PREFIX##_cache_nb_entries(&fc); \
    if (total_evicted + remaining != filled) \
        FAILF("maintain_step: evicted=%u + remaining=%u != %u", \
              total_evicted, remaining, filled); \
    if (total_evicted < filled / 2u) \
        FAILF("maintain_step: evicted=%u too few (fill=%u)", \
              total_evicted, filled); \
\
    fc_##PREFIX##_cache_stats(&fc, &stats); \
    if (stats.maint_step_calls != 2u) \
        FAILF("maint_step_calls=%" PRIu64 " expected 2", \
              stats.maint_step_calls); \
    if (remaining > 0u && stats.maint_step_skipped_bks == 0u) \
        FAIL("maintain_step should skip sparse buckets"); \
\
    /* Full cleanup via maintain() for remaining sparse entries */ \
    fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1000u); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAIL("maintain() should clean up remaining entries"); \
\
    /* Idle=true test: full sweep regardless of throttle */ \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(9000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 2000u, results); \
    filled = fc_##PREFIX##_cache_nb_entries(&fc); \
    total_evicted = fc_##PREFIX##_cache_maintain_step(&fc, 3000u, 1); \
    remaining = fc_##PREFIX##_cache_nb_entries(&fc); \
    if (total_evicted == 0u) \
        FAIL("maintain_step idle should evict"); \
    if (total_evicted + remaining != filled) \
        FAILF("maintain_step idle: evicted=%u + remaining=%u != %u", \
              total_evicted, remaining, filled); \
\
    /* Throttle test: interval=1000, within interval -> skip */ \
    fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 3000u); \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    cfg.maint_interval_tsc = 1000u; \
    cfg.maint_base_bk = NB_BK / 4u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(10000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    /* First call: elapsed is large (100 - 0 = 100 >= interval... */ \
    /* actually last_maint_tsc=0, so elapsed=500, scale=0 since 500/1000=0 */ \
    /* -> skip!  Force first run with idle=1 */ \
    total_evicted = fc_##PREFIX##_cache_maintain_step(&fc, 500u, 1); \
    /* Now last_maint_tsc=500; call at 600 -> elapsed=100 < 1000 -> skip */ \
    if (fc_##PREFIX##_cache_maintain_step(&fc, 600u, 0) != 0u) \
        FAIL("maintain_step should throttle within interval"); \
    /* Call at 1600 -> elapsed=1100, scale=1 -> sweep base_bk=4 */ \
    total_evicted = fc_##PREFIX##_cache_maintain_step(&fc, 1600u, 0); \
    fc_##PREFIX##_cache_stats(&fc, &stats); \
    if (stats.maint_bucket_checks == 0u) \
        FAIL("maintain_step after interval should scan buckets"); \
\
    /* [A] maint_fill_threshold trigger */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    cfg.maint_interval_tsc = 100000u; /* very long - time won't trigger */ \
    cfg.maint_base_bk = NB_BK / 4u; \
    cfg.maint_fill_threshold = 10u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(11000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    /* fills=48, threshold=10 -> entry_scale=4, time_scale=0 -> sweep=4*4=16=NB_BK */ \
    fc.last_maint_tsc = 100u; /* reset so time won't trigger */ \
    fc.last_maint_fills = 0u; /* 48 fills since last maint */ \
    total_evicted = fc_##PREFIX##_cache_maintain_step(&fc, 1000u, 0); \
    if (total_evicted == 0u) \
        FAIL("fill_threshold should trigger GC"); \
\
    /* [B] scale-up: elapsed = 5 * interval -> sweep = 5 * base_bk */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    cfg.maint_interval_tsc = 100u; \
    cfg.maint_base_bk = 2u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(12000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    fc.last_maint_tsc = 500u; \
    /* elapsed=500, scale=5, sweep=5*2=10 */ \
    total_evicted = fc_##PREFIX##_cache_maintain_step(&fc, 1000u, 0); \
    /* Should have swept 10 buckets (clamped to NB_BK if over) */ \
    if (fc.last_maint_sweep_bk != 10u) \
        FAILF("scale-up: sweep_bk=%u expected 10", fc.last_maint_sweep_bk); \
\
    /* [C] last_maint_start_bk / last_maint_sweep_bk verification */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    (void)fc_##PREFIX##_cache_maintain_step(&fc, 1000u, 0); \
    if (fc.last_maint_start_bk != 0u) \
        FAILF("last_maint_start_bk=%u expected 0", fc.last_maint_start_bk); \
    if (fc.last_maint_sweep_bk != NB_BK) \
        FAILF("last_maint_sweep_bk=%u expected %u", \
              fc.last_maint_sweep_bk, (unsigned)NB_BK); \
\
    /* [D] idle=1 cleans ALL entries including sparse-bucket ones */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(13000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    { \
        unsigned filled_d = fc_##PREFIX##_cache_nb_entries(&fc); \
        total_evicted = fc_##PREFIX##_cache_maintain_step(&fc, 1000u, 1); \
        remaining = fc_##PREFIX##_cache_nb_entries(&fc); \
        if (remaining != 0u) \
            FAILF("idle full sweep: remaining=%u, expected 0", remaining); \
        if (total_evicted != filled_d) \
            FAILF("idle full sweep: evicted=%u, expected %u", \
                  total_evicted, filled_d); \
    } \
\
    /* [E] Cursor continuity: 4 x NB_BK/4 covers all buckets */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    /* interval=0 -> time_scale=1 always, sweep=base_bk */ \
    cfg.maint_base_bk = NB_BK / 4u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(14000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    total_evicted = 0u; \
    for (unsigned s = 0; s < 4u; s++) \
        total_evicted += fc_##PREFIX##_cache_maintain_step( \
            &fc, 1000u + s * 2u, 0); \
    /* cursor should have wrapped back to 0 */ \
    if (fc.maint_cursor != 0u) \
        FAILF("cursor continuity: cursor=%u expected 0", fc.maint_cursor); \
    if (total_evicted < FILL / 2u) \
        FAILF("cursor continuity: evicted=%u too few", total_evicted); \
\
    /* [F] _ex cursor override + record verification */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(15000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    (void)fc_##PREFIX##_cache_maintain_step_ex(&fc, 4u, 8u, 0u, 1000u); \
    if (fc.last_maint_start_bk != 4u) \
        FAILF("_ex start_bk=%u expected 4", fc.last_maint_start_bk); \
    if (fc.last_maint_sweep_bk != 8u) \
        FAILF("_ex sweep_bk=%u expected 8", fc.last_maint_sweep_bk); \
    /* cursor should advance to 4+8=12 */ \
    if (fc.maint_cursor != 12u) \
        FAILF("_ex cursor=%u expected 12", fc.maint_cursor); \
\
    /* [G] Both triggers: max(time_scale, entry_scale) */ \
    memset(&cfg, 0, sizeof(cfg)); \
    cfg.timeout_tsc = 100u; \
    cfg.pressure_empty_slots = 1u; \
    cfg.maint_interval_tsc = 100u; \
    cfg.maint_base_bk = 2u; \
    cfg.maint_fill_threshold = 10u; \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(16000u + i); \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, FILL, 100u, results); \
    fc.last_maint_tsc = 700u; \
    fc.last_maint_fills = 38u; /* added=48-38=10, entry_scale=1 */ \
    /* elapsed=1000-700=300, time_scale=3 > entry_scale=1 -> sweep=3*2=6 */ \
    (void)fc_##PREFIX##_cache_maintain_step(&fc, 1000u, 0); \
    if (fc.last_maint_sweep_bk != 6u) \
        FAILF("max trigger: sweep_bk=%u expected 6", fc.last_maint_sweep_bk); \
    /* Now test entry_scale > time_scale */ \
    fc.last_maint_tsc = 990u; \
    fc.last_maint_fills = 0u; /* added=fills_now-0, entry_scale large */ \
    /* elapsed=1010-990=20, time_scale=0 but entry_scale > 0 -> runs */ \
    fc_##PREFIX##_cache_stats(&fc, &stats); \
    fc.last_maint_fills = stats.fills - 30u; /* added=30, scale=3 -> sweep=6 */ \
    (void)fc_##PREFIX##_cache_maintain_step(&fc, 1010u, 0); \
    if (fc.last_maint_sweep_bk != 6u) \
        FAILF("entry trigger: sweep_bk=%u expected 6", fc.last_maint_sweep_bk); \
} \
\
static void \
test_##PREFIX##_size_query(void) \
{ \
    struct fc_cache_size_attr attr; \
    CACHE_T *fcp; \
    ENTRY_T *poolp; \
    struct rix_hash_bucket_s *bucketsp; \
    unsigned char *layout; \
    unsigned expected_entries; \
    unsigned expected_bk; \
\
    printf("[T] fc " #PREFIX " size_query\n"); \
    if (fc_##PREFIX##_cache_size_query(1000u, &attr) != 0) \
        FAIL("size_query should succeed"); \
    expected_entries = fc_cache_roundup_entries(1000u); \
    expected_bk = rix_hash_nb_bk_hint(expected_entries); \
    if (attr.requested_entries != 1000u) \
        FAILF("size_query requested=%u expected 1000", attr.requested_entries); \
    if (attr.nb_entries != expected_entries) \
        FAILF("size_query nb_entries=%u expected %u", \
              attr.nb_entries, expected_entries); \
    if (attr.nb_bk != expected_bk) \
        FAILF("size_query nb_bk=%u expected %u", attr.nb_bk, expected_bk); \
    if (attr.total_slots != expected_bk * RIX_HASH_BUCKET_ENTRY_SZ) \
        FAILF("size_query total_slots=%u expected %u", \
              attr.total_slots, expected_bk * RIX_HASH_BUCKET_ENTRY_SZ); \
    if (attr.cache_bytes != sizeof(CACHE_T)) \
        FAILF("size_query cache_bytes=%zu expected %zu", \
              attr.cache_bytes, sizeof(CACHE_T)); \
    if (attr.pool_bytes != (size_t)expected_entries * sizeof(ENTRY_T)) \
        FAILF("size_query pool_bytes=%zu expected %zu", \
              attr.pool_bytes, (size_t)expected_entries * sizeof(ENTRY_T)); \
    if (attr.buckets_bytes != (size_t)expected_bk * sizeof(struct rix_hash_bucket_s)) \
        FAILF("size_query buckets_bytes=%zu expected %zu", \
              attr.buckets_bytes, (size_t)expected_bk * sizeof(struct rix_hash_bucket_s)); \
    if ((attr.cache_offset % attr.cache_align) != 0u) \
        FAIL("size_query cache_offset not aligned"); \
    if ((attr.buckets_offset % attr.buckets_align) != 0u) \
        FAIL("size_query buckets_offset not aligned"); \
    if ((attr.pool_offset % attr.pool_align) != 0u) \
        FAIL("size_query pool_offset not aligned"); \
    if ((attr.scratch_offset % attr.scratch_align) != 0u) \
        FAIL("size_query scratch_offset not aligned"); \
    if (attr.total_bytes < attr.scratch_offset + attr.scratch_bytes) \
        FAIL("size_query total_bytes too small"); \
    if (fc_##PREFIX##_cache_size_query(1000u, NULL) != -1) \
        FAIL("size_query(NULL) should fail"); \
    layout = (unsigned char *)malloc(attr.total_bytes); \
    if (layout == NULL) \
        FAIL("size_query malloc failed"); \
    if (fc_cache_size_bind(layout, &attr) != 0) \
        FAIL("size_bind should succeed"); \
    fcp = (CACHE_T *)attr.cache_ptr; \
    bucketsp = (struct rix_hash_bucket_s *)attr.buckets_ptr; \
    poolp = (ENTRY_T *)attr.pool_ptr; \
    if (fc_##PREFIX##_cache_init_attr(&attr, NULL) != 0) \
        FAIL("init_attr should succeed"); \
    if (fc_##PREFIX##_cache_nb_entries(fcp) != 0u) \
        FAIL("init_attr cache should start empty"); \
    if (fcp->bulk_ctx != attr.scratch_ptr) \
        FAIL("init_attr bulk_ctx mismatch"); \
    if (fcp->bulk_ctx_count != attr.scratch_ctx_count) \
        FAILF("init_attr bulk_ctx_count=%u expected %u", \
              fcp->bulk_ctx_count, attr.scratch_ctx_count); \
    if (bucketsp == NULL || poolp == NULL) \
        FAIL("bind pointers should be non-null"); \
    if (fc_##PREFIX##_cache_init_attr(NULL, NULL) != -1) \
        FAIL("init_attr(NULL) should fail"); \
    free(layout); \
} \
\
/*--- find_bulk / find ---*/ \
static void \
test_##PREFIX##_find_bulk(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " find_bulk\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(20000u + i); \
\
    /* find on empty cache: all miss, no insert */ \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 1u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAIL("find_bulk should not insert"); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx != 0u) \
            FAILF("find_bulk miss: result[%u] should be 0", i); \
    } \
\
    /* Pre-fill, then find: all hit */ \
    fc_##PREFIX##_cache_findadd_bulk(&fc, keys, NB_KEYS, 100u, results); \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 200u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("find_bulk hit: result[%u] should be non-zero", i); \
    } \
\
    /* Verify last_ts updated to 200 */ \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (pool[results[i].entry_idx - 1u].last_ts != 200u) \
            FAILF("find_bulk touch: last_ts=%" PRIu64 " expected 200", \
                  pool[results[i].entry_idx - 1u].last_ts); \
    } \
\
    /* now=0: no last_ts update */ \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 0u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (pool[results[i].entry_idx - 1u].last_ts != 200u) \
            FAILF("find_bulk now=0: last_ts=%" PRIu64 " should stay 200", \
                  pool[results[i].entry_idx - 1u].last_ts); \
    } \
} \
\
static void \
test_##PREFIX##_find_single(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T key; \
    uint32_t idx; \
\
    printf("[T] fc " #PREFIX " find (single)\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    key = MAKE_KEY(21000u); \
\
    /* miss */ \
    idx = fc_##PREFIX##_cache_find(&fc, &key, 1u); \
    if (idx != 0u) \
        FAIL("find single miss should return 0"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAIL("find single should not insert"); \
\
    /* insert, then find */ \
    idx = fc_##PREFIX##_cache_findadd(&fc, &key, 100u); \
    if (idx == 0u) \
        FAIL("findadd single should return non-zero"); \
    { \
        uint32_t found = fc_##PREFIX##_cache_find(&fc, &key, 200u); \
        if (found != idx) \
            FAILF("find single hit: idx=%u expected %u", found, idx); \
    } \
} \
\
/*--- add_bulk / add ---*/ \
static void \
test_##PREFIX##_add_bulk(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " add_bulk\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(22000u + i); \
\
    /* add into empty cache */ \
    fc_##PREFIX##_cache_add_bulk(&fc, keys, NB_KEYS, 100u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != NB_KEYS) \
        FAILF("add_bulk nb_entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), NB_KEYS); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("add_bulk result[%u] should be non-zero", i); \
    } \
\
    /* verify added entries are findable */ \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 200u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("add_bulk: find after add result[%u] should hit", i); \
    } \
} \
\
static void \
test_##PREFIX##_add_single(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T key; \
    uint32_t idx; \
\
    printf("[T] fc " #PREFIX " add (single)\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    key = MAKE_KEY(23000u); \
\
    idx = fc_##PREFIX##_cache_add(&fc, &key, 100u); \
    if (idx == 0u) \
        FAIL("add single should return non-zero"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 1u) \
        FAIL("add single nb_entries should be 1"); \
\
    /* findable */ \
    { \
        uint32_t found = fc_##PREFIX##_cache_find(&fc, &key, 200u); \
        if (found != idx) \
            FAILF("add single: find idx=%u expected %u", found, idx); \
    } \
} \
\
static void \
test_##PREFIX##_add_bulk_full(void) \
{ \
    enum { NB_BK = 4u, MAX_ENTRIES = 16u, FILL = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[FILL + 1u]; \
    RESULT_T results[FILL + 1u]; \
\
    printf("[T] fc " #PREFIX " add_bulk full\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < FILL + 1u; i++) \
        keys[i] = MAKE_KEY(24000u + i); \
\
    /* fill to capacity */ \
    fc_##PREFIX##_cache_add_bulk(&fc, keys, FILL, 100u, results); \
\
    /* one more: should evict the oldest non-current entry and insert */ \
    fc_##PREFIX##_cache_add_bulk(&fc, &keys[FILL], 1u, 200u, &results[FILL]); \
    if (results[FILL].entry_idx == 0u) \
        FAIL("add_bulk full: extra entry should insert via oldest fallback"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != FILL) \
        FAILF("add_bulk full: entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), FILL); \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, FILL, 201u, results); \
    if (COUNT_HITS(RESULT_T, results, FILL) != FILL - 1u) \
        FAILF("add_bulk full: expected exactly one eviction, hits=%u", \
              COUNT_HITS(RESULT_T, results, FILL)); \
} \
\
/*--- del_bulk / del ---*/ \
static void \
test_##PREFIX##_del_bulk(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " del_bulk\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(25000u + i); \
\
    /* add, then del */ \
    fc_##PREFIX##_cache_add_bulk(&fc, keys, NB_KEYS, 100u, results); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != NB_KEYS) \
        FAIL("del_bulk: pre-fill failed"); \
\
    fc_##PREFIX##_cache_del_bulk(&fc, keys, NB_KEYS); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAILF("del_bulk: nb_entries=%u expected 0", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
\
    /* find confirms all gone */ \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 200u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx != 0u) \
            FAILF("del_bulk: key[%u] should be gone", i); \
    } \
\
    /* del on empty cache: no crash */ \
    fc_##PREFIX##_cache_del_bulk(&fc, keys, NB_KEYS); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAIL("del_bulk on empty should be no-op"); \
} \
\
static void \
test_##PREFIX##_del_single(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T key; \
\
    printf("[T] fc " #PREFIX " del (single)\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    key = MAKE_KEY(26000u); \
\
    /* add then del by key */ \
    (void)fc_##PREFIX##_cache_add(&fc, &key, 100u); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 1u) \
        FAIL("del single: pre-add failed"); \
    fc_##PREFIX##_cache_del(&fc, &key); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAIL("del single: entry should be removed"); \
\
    /* del non-existent: no crash */ \
    fc_##PREFIX##_cache_del(&fc, &key); \
} \
\
/*--- del_idx_bulk ---*/ \
static void \
test_##PREFIX##_del_idx_bulk(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
    uint32_t idxs[NB_KEYS]; \
\
    printf("[T] fc " #PREFIX " del_idx_bulk\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(27000u + i); \
\
    fc_##PREFIX##_cache_add_bulk(&fc, keys, NB_KEYS, 100u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        idxs[i] = results[i].entry_idx; \
\
    /* bulk remove by idx */ \
    fc_##PREFIX##_cache_del_idx_bulk(&fc, idxs, NB_KEYS); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAILF("del_idx_bulk: nb_entries=%u expected 0", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
\
    /* verify all gone */ \
    fc_##PREFIX##_cache_find_bulk(&fc, keys, NB_KEYS, 200u, results); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx != 0u) \
            FAILF("del_idx_bulk: key[%u] should be gone", i); \
    } \
\
    /* invalid indices: no crash */ \
    { \
        uint32_t bad_idxs[] = { 0u, MAX_ENTRIES + 1u }; \
        fc_##PREFIX##_cache_del_idx_bulk(&fc, bad_idxs, 2u); \
    } \
} \
\
/*--- walk ---*/ \
static int \
test_##PREFIX##_walk_count_cb(uint32_t entry_idx, void *arg) \
{ \
    (void)entry_idx; \
    unsigned *count = (unsigned *)arg; \
    (*count)++; \
    return 0; \
} \
\
static int \
test_##PREFIX##_walk_abort_cb(uint32_t entry_idx, void *arg) \
{ \
    (void)entry_idx; \
    unsigned *count = (unsigned *)arg; \
    (*count)++; \
    if (*count >= 3u) \
        return -42; \
    return 0; \
} \
\
static void \
test_##PREFIX##_walk(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
    unsigned count; \
    int rc; \
\
    printf("[T] fc " #PREFIX " walk\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
\
    /* walk on empty cache: cb never called */ \
    count = 0u; \
    rc = fc_##PREFIX##_cache_walk(&fc, test_##PREFIX##_walk_count_cb, &count); \
    if (rc != 0) \
        FAILF("walk empty: rc=%d expected 0", rc); \
    if (count != 0u) \
        FAILF("walk empty: count=%u expected 0", count); \
\
    /* add entries, walk all */ \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(29000u + i); \
    fc_##PREFIX##_cache_add_bulk(&fc, keys, NB_KEYS, 100u, results); \
\
    count = 0u; \
    rc = fc_##PREFIX##_cache_walk(&fc, test_##PREFIX##_walk_count_cb, &count); \
    if (rc != 0) \
        FAILF("walk full: rc=%d expected 0", rc); \
    if (count != NB_KEYS) \
        FAILF("walk full: count=%u expected %u", count, NB_KEYS); \
\
    /* walk with early abort */ \
    count = 0u; \
    rc = fc_##PREFIX##_cache_walk(&fc, test_##PREFIX##_walk_abort_cb, &count); \
    if (rc != -42) \
        FAILF("walk abort: rc=%d expected -42", rc); \
    if (count != 3u) \
        FAILF("walk abort: count=%u expected 3", count); \
} \
\
/*--- findadd single ---*/ \
static void \
test_##PREFIX##_findadd_single(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T key; \
    uint32_t idx1, idx2; \
\
    printf("[T] fc " #PREFIX " findadd (single)\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    key = MAKE_KEY(28000u); \
\
    /* first call: miss -> insert */ \
    idx1 = fc_##PREFIX##_cache_findadd(&fc, &key, 100u); \
    if (idx1 == 0u) \
        FAIL("findadd single miss should insert"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 1u) \
        FAIL("findadd single: nb_entries should be 1"); \
\
    /* second call: hit -> same idx */ \
    idx2 = fc_##PREFIX##_cache_findadd(&fc, &key, 200u); \
    if (idx2 != idx1) \
        FAILF("findadd single hit: idx=%u expected %u", idx2, idx1); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 1u) \
        FAIL("findadd single hit: nb_entries should stay 1"); \
}

/*===========================================================================
 * Instantiate tests for all three variants
 *===========================================================================*/
DEFINE_TESTS(flow4, struct fc_flow4_key, struct fc_flow4_result,
             struct fc_flow4_entry, struct fc_flow4_cache,
             struct fc_flow4_config, struct fc_flow4_stats,
             make_key4)

DEFINE_TESTS(flow6, struct fc_flow6_key, struct fc_flow6_result,
             struct fc_flow6_entry, struct fc_flow6_cache,
             struct fc_flow6_config, struct fc_flow6_stats,
             make_key6)

DEFINE_TESTS(flowu, struct fc_flowu_key, struct fc_flowu_result,
             struct fc_flowu_entry, struct fc_flowu_cache,
             struct fc_flowu_config, struct fc_flowu_stats,
             make_keyu_v4)

struct test_flow4_user {
    struct fc_flow4_entry entry;
    struct {
        uint32_t cookie;
        uint32_t allocs;
        uint32_t frees;
        uint32_t last_event;
        uint64_t touch;
    } body;
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(offsetof(struct test_flow4_user, body) == FC_CACHE_LINE_SIZE,
                  "test_flow4_user body must start on the 2nd cache line");

struct test_flow4_event_log {
    struct test_flow4_user *users;
    unsigned alloc_count;
    unsigned free_count;
    unsigned last_idx;
    enum fc_flow4_event last_event;
};

static void
test_flow4_event_cb(enum fc_flow4_event event, uint32_t entry_idx, void *arg)
{
    struct test_flow4_event_log *log = (struct test_flow4_event_log *)arg;
    struct test_flow4_user *user = &log->users[entry_idx - 1u];

    log->last_idx = entry_idx;
    log->last_event = event;
    if (event == FC_FLOW4_EVENT_ALLOC) {
        log->alloc_count++;
        user->body.allocs++;
    } else {
        log->free_count++;
        user->body.frees++;
    }
    user->body.last_event = (uint32_t)event;
    user->body.touch = user->body.touch * UINT64_C(1315423911)
                       + (uint64_t)entry_idx
                       + ((uint64_t)event << 32);
}

static void
test_flow4_init_ex_and_event_cb(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 16u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct test_flow4_user users[MAX_ENTRIES];
    struct fc_flow4_cache fc;
    struct fc_flow4_result result;
    struct test_flow4_event_log log = { .users = users };
    struct fc_flow4_key key;

    printf("[T] fc flow4 init_ex/event_cb\n");
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        users[i].body.cookie = 0xabc00000u + i;
        users[i].body.allocs = 0u;
        users[i].body.frees = 0u;
        users[i].body.last_event = 0u;
        users[i].body.touch = 0u;
    }

    FC_FLOW4_CACHE_INIT_TYPED(&fc, buckets, NB_BK, users, MAX_ENTRIES,
                              struct test_flow4_user, entry, NULL);
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (users[i].body.cookie != 0xabc00000u + i)
            FAILF("init_ex cookie[%u]=0x%08x expected 0x%08x",
                  i, users[i].body.cookie, 0xabc00000u + i);
        if (users[i].body.allocs != 0u || users[i].body.frees != 0u
            || users[i].body.last_event != 0u || users[i].body.touch != 0u)
            FAIL("init_ex should not touch user body counters");
    }

    fc_flow4_cache_set_event_cb(&fc, test_flow4_event_cb, &log);
    key = make_key4(32000u);
    fc_flow4_cache_findadd_bulk(&fc, &key, 1u, 100u, &result);
    if (result.entry_idx == 0u)
        FAIL("init_ex findadd should allocate an entry");
    if (log.alloc_count != 1u || log.free_count != 0u)
        FAILF("event counts after alloc: alloc=%u free=%u expected 1/0",
              log.alloc_count, log.free_count);
    if (log.last_event != FC_FLOW4_EVENT_ALLOC ||
        log.last_idx != result.entry_idx)
        FAIL("event callback should report alloc idx");
    if (users[result.entry_idx - 1u].body.allocs != 1u)
        FAIL("alloc callback should touch the matching user slot");
    if (FC_FLOW4_CACHE_RECORD_FROM_ENTRY(struct test_flow4_user, entry,
                                         fc_flow4_cache_entry_ptr(&fc, result.entry_idx))
        != &users[result.entry_idx - 1u])
        FAIL("record_from_entry should recover the owning flow4 user record");
    if (FC_FLOW4_CACHE_ENTRY_FROM_RECORD(&users[result.entry_idx - 1u], entry)
        != fc_flow4_cache_entry_ptr(&fc, result.entry_idx))
        FAIL("entry_from_record should return the embedded flow4 entry");
    if (users[result.entry_idx - 1u].body.last_event != FC_FLOW4_EVENT_ALLOC ||
        users[result.entry_idx - 1u].body.touch == 0u)
        FAIL("alloc callback should write user body state");

    if (!fc_flow4_cache_del_idx(&fc, result.entry_idx))
        FAIL("del_idx on init_ex entry should succeed");
    if (log.free_count != 1u)
        FAILF("free count=%u expected 1", log.free_count);
    if (log.last_event != FC_FLOW4_EVENT_FREE_DELETE ||
        log.last_idx != result.entry_idx)
        FAIL("delete should report FREE_DELETE for the same idx");
    if (users[result.entry_idx - 1u].body.frees != 1u)
        FAIL("free callback should touch the matching user slot");
    if (users[result.entry_idx - 1u].body.last_event != FC_FLOW4_EVENT_FREE_DELETE)
        FAIL("free callback should update last_event in user body");

    fc_flow4_cache_findadd_bulk(&fc, &key, 1u, 200u, &result);
    if (result.entry_idx == 0u)
        FAIL("reinsert after delete should succeed");
    fc_flow4_cache_flush(&fc);
    if (log.free_count < 2u)
        FAILF("flush should report at least one additional free, got %u",
              log.free_count);
    if (log.last_event != FC_FLOW4_EVENT_FREE_FLUSH)
        FAIL("flush should report FREE_FLUSH");
    if (users[result.entry_idx - 1u].body.cookie !=
        0xabc00000u + (result.entry_idx - 1u))
        FAIL("flush should not clobber user body");
}

enum {
    TEST_FLOW4_VAR_HDR_OFF = FC_CACHE_LINE_SIZE,
    TEST_FLOW4_VAR_BODY_OFF = FC_CACHE_LINE_SIZE * 2u,
    TEST_FLOW4_VAR_BODY_SZ = 96u,
    TEST_FLOW4_VAR_STRIDE = FC_CACHE_LINE_SIZE * 4u,
    TEST_FLOW4_VAR_NB_BK = 8u,
    TEST_FLOW4_VAR_MAX_ENTRIES = 16u
};

struct test_flow4_var_hdr {
    uint32_t cookie;
    uint32_t allocs;
    uint32_t frees;
    uint32_t last_event;
    uint64_t touch;
};

struct test_flow4_var_ctx {
    struct fc_flow4_cache *fc;
    unsigned alloc_count;
    unsigned free_count;
    unsigned last_idx;
    enum fc_flow4_event last_event;
};

static inline struct test_flow4_var_hdr *
test_flow4_var_hdr_ptr(unsigned char *rec)
{
    void *p = __builtin_assume_aligned(FC_BYTE_PTR_ADD(rec,
                                                       TEST_FLOW4_VAR_BODY_OFF),
                                       _Alignof(struct test_flow4_var_hdr));
    return (struct test_flow4_var_hdr *)p;
}

static void
test_flow4_var_event_cb(enum fc_flow4_event event, uint32_t entry_idx, void *arg)
{
    struct test_flow4_var_ctx *ctx = (struct test_flow4_var_ctx *)arg;
    unsigned char *rec =
        FC_FLOW4_CACHE_RECORD_PTR_AS(ctx->fc, unsigned char, entry_idx);
    struct test_flow4_var_hdr *hdr;
    uint8_t *body;

    if (rec == NULL)
        return;
    hdr = test_flow4_var_hdr_ptr(rec);
    body = FC_BYTE_PTR_ADD(rec, TEST_FLOW4_VAR_BODY_OFF);
    ctx->last_idx = entry_idx;
    ctx->last_event = event;
    if (event == FC_FLOW4_EVENT_ALLOC) {
        ctx->alloc_count++;
        hdr->allocs++;
    } else {
        ctx->free_count++;
        hdr->frees++;
    }
    hdr->last_event = (uint32_t)event;
    hdr->touch = hdr->touch * UINT64_C(11400714819323198485)
                 + (uint64_t)entry_idx
                 + ((uint64_t)event << 32);
    body[TEST_FLOW4_VAR_BODY_SZ - 1u] ^= (uint8_t)entry_idx;
}

static void
test_flow4_init_ex_varbody_mapping(void)
{
    struct rix_hash_bucket_s buckets[TEST_FLOW4_VAR_NB_BK];
    unsigned char
        records[TEST_FLOW4_VAR_MAX_ENTRIES][TEST_FLOW4_VAR_STRIDE]
        __attribute__((aligned(FC_CACHE_LINE_SIZE)));
    struct fc_flow4_cache fc;
    struct fc_flow4_result result;
    struct test_flow4_var_ctx ctx;
    struct fc_flow4_key key;

    printf("[T] fc flow4 init_ex/varbody mapping\n");
    memset(records, 0, sizeof(records));
    for (unsigned i = 0; i < TEST_FLOW4_VAR_MAX_ENTRIES; i++) {
        struct test_flow4_var_hdr *hdr = test_flow4_var_hdr_ptr(records[i]);

        hdr->cookie = 0xdef00000u + i;
    }

    fc_flow4_cache_init_ex(&fc, buckets, TEST_FLOW4_VAR_NB_BK, records,
                           TEST_FLOW4_VAR_MAX_ENTRIES,
                           TEST_FLOW4_VAR_STRIDE,
                           TEST_FLOW4_VAR_HDR_OFF, NULL);
    if (fc_flow4_cache_record_stride(&fc) != TEST_FLOW4_VAR_STRIDE)
        FAIL("record stride should match init_ex stride");
    if (fc_flow4_cache_entry_offset(&fc) != TEST_FLOW4_VAR_HDR_OFF)
        FAIL("entry offset should match init_ex entry_offset");
    if (fc_flow4_cache_record_ptr(&fc, 1u) != (void *)&records[0][0])
        FAIL("record_ptr(1) should point at record base");
    if ((void *)fc_flow4_cache_entry_ptr(&fc, 1u) !=
        (void *)&records[0][TEST_FLOW4_VAR_HDR_OFF])
        FAIL("entry_ptr(1) should point at embedded entry");

    ctx.fc = &fc;
    ctx.alloc_count = 0u;
    ctx.free_count = 0u;
    ctx.last_idx = 0u;
    ctx.last_event = 0u;
    fc_flow4_cache_set_event_cb(&fc, test_flow4_var_event_cb, &ctx);

    key = make_key4(33000u);
    fc_flow4_cache_findadd_bulk(&fc, &key, 1u, 100u, &result);
    if (result.entry_idx == 0u)
        FAIL("varbody findadd should allocate an entry");
    {
        unsigned char *rec =
            FC_FLOW4_CACHE_RECORD_PTR_AS(&fc, unsigned char, result.entry_idx);
        struct test_flow4_var_hdr *hdr;

        if (rec == NULL)
            FAIL("record pointer should not be NULL for live entry");
        hdr = test_flow4_var_hdr_ptr(rec);

        if (hdr->cookie != 0xdef00000u + (result.entry_idx - 1u))
            FAIL("varbody cookie should survive init_ex");
        if (hdr->allocs != 1u || hdr->last_event != FC_FLOW4_EVENT_ALLOC
            || hdr->touch == 0u)
            FAIL("varbody alloc callback should update mapped body");
        if (rec[TEST_FLOW4_VAR_BODY_OFF + TEST_FLOW4_VAR_BODY_SZ - 1u]
            != (uint8_t)result.entry_idx)
            FAIL("varbody callback should be able to write arbitrary payload");
    }

    fc_flow4_cache_flush(&fc);
    if (ctx.free_count == 0u || ctx.last_event != FC_FLOW4_EVENT_FREE_FLUSH)
        FAIL("varbody flush should emit free event");
}

struct test_flow6_user {
    struct fc_flow6_entry entry;
    struct {
        uint32_t cookie;
        uint32_t allocs;
        uint32_t frees;
        uint32_t last_event;
        uint64_t touch;
    } body;
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(offsetof(struct test_flow6_user, body) == FC_CACHE_LINE_SIZE,
                  "test_flow6_user body must start on the 2nd cache line");

struct test_flow6_event_log {
    struct test_flow6_user *users;
    unsigned alloc_count;
    unsigned free_count;
    unsigned last_idx;
    enum fc_flow6_event last_event;
};

static void
test_flow6_event_cb(enum fc_flow6_event event, uint32_t entry_idx, void *arg)
{
    struct test_flow6_event_log *log = (struct test_flow6_event_log *)arg;
    struct test_flow6_user *user = &log->users[entry_idx - 1u];

    log->last_idx = entry_idx;
    log->last_event = event;
    if (event == FC_FLOW6_EVENT_ALLOC) {
        log->alloc_count++;
        user->body.allocs++;
    } else {
        log->free_count++;
        user->body.frees++;
    }
    user->body.last_event = (uint32_t)event;
    user->body.touch = user->body.touch * UINT64_C(1315423911)
                       + (uint64_t)entry_idx
                       + ((uint64_t)event << 32);
}

static void
test_flow6_init_ex_and_event_cb(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 16u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct test_flow6_user users[MAX_ENTRIES];
    struct fc_flow6_cache fc;
    struct fc_flow6_result result;
    struct test_flow6_event_log log = { .users = users };
    struct fc_flow6_key key;

    printf("[T] fc flow6 init_ex/event_cb\n");
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        users[i].body.cookie = 0xdef00000u + i;
        users[i].body.allocs = 0u;
        users[i].body.frees = 0u;
        users[i].body.last_event = 0u;
        users[i].body.touch = 0u;
    }

    FC_FLOW6_CACHE_INIT_TYPED(&fc, buckets, NB_BK, users, MAX_ENTRIES,
                              struct test_flow6_user, entry, NULL);
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (users[i].body.cookie != 0xdef00000u + i)
            FAILF("flow6 init_ex cookie[%u]=0x%08x expected 0x%08x",
                  i, users[i].body.cookie, 0xdef00000u + i);
        if (users[i].body.allocs != 0u || users[i].body.frees != 0u
            || users[i].body.last_event != 0u || users[i].body.touch != 0u)
            FAIL("flow6 init_ex should not touch user body counters");
    }

    fc_flow6_cache_set_event_cb(&fc, test_flow6_event_cb, &log);
    key = make_key6(32000u);
    fc_flow6_cache_findadd_bulk(&fc, &key, 1u, 100u, &result);
    if (result.entry_idx == 0u)
        FAIL("flow6 init_ex findadd should allocate an entry");
    if (log.alloc_count != 1u || log.free_count != 0u)
        FAILF("flow6 event counts after alloc: alloc=%u free=%u expected 1/0",
              log.alloc_count, log.free_count);
    if (log.last_event != FC_FLOW6_EVENT_ALLOC ||
        log.last_idx != result.entry_idx)
        FAIL("flow6 event callback should report alloc idx");
    if (users[result.entry_idx - 1u].body.allocs != 1u)
        FAIL("flow6 alloc callback should touch the matching user slot");
    if (FC_FLOW6_CACHE_RECORD_FROM_ENTRY(struct test_flow6_user, entry,
                                         fc_flow6_cache_entry_ptr(&fc, result.entry_idx))
        != &users[result.entry_idx - 1u])
        FAIL("record_from_entry should recover the owning flow6 user record");
    if (FC_FLOW6_CACHE_ENTRY_FROM_RECORD(&users[result.entry_idx - 1u], entry)
        != fc_flow6_cache_entry_ptr(&fc, result.entry_idx))
        FAIL("entry_from_record should return the embedded flow6 entry");
    if (users[result.entry_idx - 1u].body.last_event != FC_FLOW6_EVENT_ALLOC ||
        users[result.entry_idx - 1u].body.touch == 0u)
        FAIL("flow6 alloc callback should write user body state");

    if (!fc_flow6_cache_del_idx(&fc, result.entry_idx))
        FAIL("flow6 del_idx on init_ex entry should succeed");
    if (log.free_count != 1u)
        FAILF("flow6 free count=%u expected 1", log.free_count);
    if (log.last_event != FC_FLOW6_EVENT_FREE_DELETE ||
        log.last_idx != result.entry_idx)
        FAIL("flow6 delete should report FREE_DELETE for the same idx");
    if (users[result.entry_idx - 1u].body.frees != 1u)
        FAIL("flow6 free callback should touch the matching user slot");
    if (users[result.entry_idx - 1u].body.last_event != FC_FLOW6_EVENT_FREE_DELETE)
        FAIL("flow6 free callback should update last_event in user body");

    fc_flow6_cache_findadd_bulk(&fc, &key, 1u, 200u, &result);
    if (result.entry_idx == 0u)
        FAIL("flow6 reinsert after delete should succeed");
    fc_flow6_cache_flush(&fc);
    if (log.free_count < 2u)
        FAILF("flow6 flush should report at least one additional free, got %u",
              log.free_count);
    if (log.last_event != FC_FLOW6_EVENT_FREE_FLUSH)
        FAIL("flow6 flush should report FREE_FLUSH");
}

struct test_flowu_user {
    struct fc_flowu_entry entry;
    struct {
        uint32_t cookie;
        uint32_t allocs;
        uint32_t frees;
        uint32_t last_event;
        uint64_t touch;
    } body;
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(offsetof(struct test_flowu_user, body) == FC_CACHE_LINE_SIZE,
                  "test_flowu_user body must start on the 2nd cache line");

struct test_flowu_event_log {
    struct test_flowu_user *users;
    unsigned alloc_count;
    unsigned free_count;
    unsigned last_idx;
    enum fc_flowu_event last_event;
};

static void
test_flowu_event_cb(enum fc_flowu_event event, uint32_t entry_idx, void *arg)
{
    struct test_flowu_event_log *log = (struct test_flowu_event_log *)arg;
    struct test_flowu_user *user = &log->users[entry_idx - 1u];

    log->last_idx = entry_idx;
    log->last_event = event;
    if (event == FC_FLOWU_EVENT_ALLOC) {
        log->alloc_count++;
        user->body.allocs++;
    } else {
        log->free_count++;
        user->body.frees++;
    }
    user->body.last_event = (uint32_t)event;
    user->body.touch = user->body.touch * UINT64_C(1315423911)
                       + (uint64_t)entry_idx
                       + ((uint64_t)event << 32);
}

static void
test_flowu_init_ex_and_event_cb(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 16u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct test_flowu_user users[MAX_ENTRIES];
    struct fc_flowu_cache fc;
    struct fc_flowu_result result;
    struct test_flowu_event_log log = { .users = users };
    struct fc_flowu_key key;

    printf("[T] fc flowu init_ex/event_cb\n");
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        users[i].body.cookie = 0x12340000u + i;
        users[i].body.allocs = 0u;
        users[i].body.frees = 0u;
        users[i].body.last_event = 0u;
        users[i].body.touch = 0u;
    }

    FC_FLOWU_CACHE_INIT_TYPED(&fc, buckets, NB_BK, users, MAX_ENTRIES,
                              struct test_flowu_user, entry, NULL);
    for (unsigned i = 0; i < MAX_ENTRIES; i++) {
        if (users[i].body.cookie != 0x12340000u + i)
            FAILF("flowu init_ex cookie[%u]=0x%08x expected 0x%08x",
                  i, users[i].body.cookie, 0x12340000u + i);
        if (users[i].body.allocs != 0u || users[i].body.frees != 0u
            || users[i].body.last_event != 0u || users[i].body.touch != 0u)
            FAIL("flowu init_ex should not touch user body counters");
    }

    fc_flowu_cache_set_event_cb(&fc, test_flowu_event_cb, &log);
    key = make_keyu_v4(32000u);
    fc_flowu_cache_findadd_bulk(&fc, &key, 1u, 100u, &result);
    if (result.entry_idx == 0u)
        FAIL("flowu init_ex findadd should allocate an entry");
    if (log.alloc_count != 1u || log.free_count != 0u)
        FAILF("flowu event counts after alloc: alloc=%u free=%u expected 1/0",
              log.alloc_count, log.free_count);
    if (log.last_event != FC_FLOWU_EVENT_ALLOC ||
        log.last_idx != result.entry_idx)
        FAIL("flowu event callback should report alloc idx");
    if (users[result.entry_idx - 1u].body.allocs != 1u)
        FAIL("flowu alloc callback should touch the matching user slot");
    if (FC_FLOWU_CACHE_RECORD_FROM_ENTRY(struct test_flowu_user, entry,
                                         fc_flowu_cache_entry_ptr(&fc, result.entry_idx))
        != &users[result.entry_idx - 1u])
        FAIL("record_from_entry should recover the owning flowu user record");
    if (FC_FLOWU_CACHE_ENTRY_FROM_RECORD(&users[result.entry_idx - 1u], entry)
        != fc_flowu_cache_entry_ptr(&fc, result.entry_idx))
        FAIL("entry_from_record should return the embedded flowu entry");
    if (users[result.entry_idx - 1u].body.last_event != FC_FLOWU_EVENT_ALLOC ||
        users[result.entry_idx - 1u].body.touch == 0u)
        FAIL("flowu alloc callback should write user body state");

    if (!fc_flowu_cache_del_idx(&fc, result.entry_idx))
        FAIL("flowu del_idx on init_ex entry should succeed");
    if (log.free_count != 1u)
        FAILF("flowu free count=%u expected 1", log.free_count);
    if (log.last_event != FC_FLOWU_EVENT_FREE_DELETE ||
        log.last_idx != result.entry_idx)
        FAIL("flowu delete should report FREE_DELETE for the same idx");
    if (users[result.entry_idx - 1u].body.frees != 1u)
        FAIL("flowu free callback should touch the matching user slot");
    if (users[result.entry_idx - 1u].body.last_event != FC_FLOWU_EVENT_FREE_DELETE)
        FAIL("flowu free callback should update last_event in user body");

    fc_flowu_cache_findadd_bulk(&fc, &key, 1u, 200u, &result);
    if (result.entry_idx == 0u)
        FAIL("flowu reinsert after delete should succeed");
    fc_flowu_cache_flush(&fc);
    if (log.free_count < 2u)
        FAILF("flowu flush should report at least one additional free, got %u",
              log.free_count);
    if (log.last_event != FC_FLOWU_EVENT_FREE_FLUSH)
        FAIL("flowu flush should report FREE_FLUSH");
}

/*===========================================================================
 * flowu-specific: v4/v6 coexistence
 *===========================================================================*/
static void
test_flowu_v4_v6_coexist(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_V4 = 4u, NB_V6 = 4u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc_flowu_entry pool[MAX_ENTRIES];
    struct fc_flowu_cache fc;
    struct fc_flowu_key keys[NB_V4 + NB_V6];
    struct fc_flowu_result results[NB_V4 + NB_V6];
    unsigned total = NB_V4 + NB_V6;

    printf("[T] fc flowu v4/v6 coexistence\n");
    fc_flowu_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL);

    for (unsigned i = 0; i < NB_V4; i++)
        keys[i] = make_keyu_v4(i);
    for (unsigned i = 0; i < NB_V6; i++)
        keys[NB_V4 + i] = make_keyu_v6(i);

    /* First lookup auto-fills all */
    fc_flowu_cache_findadd_bulk(&fc, keys, total, 1u, results);
    if (fc_flowu_cache_nb_entries(&fc) != total)
        FAILF("coexist nb_entries=%u expected %u",
              fc_flowu_cache_nb_entries(&fc), total);

    /* Re-lookup: all hit */
    fc_flowu_cache_findadd_bulk(&fc, keys, total, 20u, results);

    /* Verify v4 and v6 got distinct entries */
    for (unsigned i = 0; i < total; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("coexist result[%u] entry_idx is 0", i);
        for (unsigned j = i + 1; j < total; j++) {
            if (results[i].entry_idx == results[j].entry_idx)
                FAILF("coexist result[%u] == result[%u] = %u",
                      i, j, results[i].entry_idx);
        }
    }

    /* Remove a v4 entry; v6 entries should still hit */
    if (!fc_flowu_cache_del_idx(&fc, results[0].entry_idx))
        FAIL("coexist del_idx v4 entry failed");
    fc_flowu_cache_findadd_bulk(&fc, &keys[NB_V4], NB_V6, 30u,
                                  &results[NB_V4]);
    for (unsigned i = NB_V4; i < total; i++) {
        if (results[i].entry_idx == 0u)
            FAIL("coexist v6 entries should still hit after v4 remove");
    }
}

/*===========================================================================
 * Run all tests
 *===========================================================================*/
#define RUN_TESTS(PREFIX) \
    test_##PREFIX##_lookup_fill_remove(); \
    test_##PREFIX##_pressure_relief(); \
    test_##PREFIX##_fill_miss_full_without_relief(); \
    test_##PREFIX##_fill_miss_full_same_now_fails(); \
    test_##PREFIX##_duplicate_miss_batch(); \
    test_##PREFIX##_flush_and_invalid_remove(); \
    test_##PREFIX##_bulk_wraparound(); \
    test_##PREFIX##_maintenance(); \
    test_##PREFIX##_timeout_boundary(); \
    test_##PREFIX##_maintain_step(); \
    test_##PREFIX##_size_query(); \
    test_##PREFIX##_find_bulk(); \
    test_##PREFIX##_find_single(); \
    test_##PREFIX##_add_bulk(); \
    test_##PREFIX##_add_single(); \
    test_##PREFIX##_add_bulk_full(); \
    test_##PREFIX##_del_bulk(); \
    test_##PREFIX##_del_single(); \
    test_##PREFIX##_del_idx_bulk(); \
    test_##PREFIX##_walk(); \
    test_##PREFIX##_findadd_single()

static unsigned
parse_arch_opt(int *argc_p, char ***argv_p)
{
    unsigned enable = FC_ARCH_AUTO;

    if (*argc_p >= 3 && strcmp((*argv_p)[1], "--arch") == 0) {
        const char *name = (*argv_p)[2];

        if (strcmp(name, "gen") == 0)         enable = FC_ARCH_GEN;
        else if (strcmp(name, "sse") == 0)    enable = FC_ARCH_SSE;
        else if (strcmp(name, "avx2") == 0)   enable = FC_ARCH_SSE | FC_ARCH_AVX2;
        else if (strcmp(name, "avx512") == 0) enable = FC_ARCH_AUTO;
        else if (strcmp(name, "auto") == 0)   enable = FC_ARCH_AUTO;
        else fprintf(stderr, "unknown arch: %s\n", name);
        *argc_p -= 2;
        *argv_p += 2;
    }
    return enable;
}

static const char *
arch_label(unsigned enable)
{
    if (enable & FC_ARCH_AVX512) return "avx512";
    if (enable & FC_ARCH_AVX2)   return "avx2";
    if (enable & FC_ARCH_SSE)    return "sse";
    return "gen";
}

int
main(int argc, char **argv)
{
    unsigned arch_enable = parse_arch_opt(&argc, &argv);

    fc_arch_init(arch_enable);
    printf("[arch: %s]\n", arch_label(arch_enable));

    RUN_TESTS(flow4);
    test_flow4_init_ex_and_event_cb();
    test_flow4_init_ex_varbody_mapping();
    RUN_TESTS(flow6);
    test_flow6_init_ex_and_event_cb();
    RUN_TESTS(flowu);
    test_flowu_init_ex_and_event_cb();
    test_flowu_v4_v6_coexist();

    printf("ALL FCACHE TESTS PASSED (flow4 + flow6 + flowu)\n");
    return 0;
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
