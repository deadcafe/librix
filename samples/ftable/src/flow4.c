/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot.h>

#include "flow4_table.h"

#define FT_FLOW4_BULK_STEP_KEYS 8u
#define FT_FLOW4_BULK_AHEAD_STEPS 4u
#define FT_FLOW4_BULK_AHEAD_KEYS \
    (FT_FLOW4_BULK_STEP_KEYS * FT_FLOW4_BULK_AHEAD_STEPS)
#define FT_FLOW4_BULK_CTX_RING 128u
#ifndef FT_FLOW4_GROW_OLD_BK_AHEAD
#define FT_FLOW4_GROW_OLD_BK_AHEAD 2u
#endif

#ifndef FT_FLOW4_GROW_REINSERT_AHEAD
#define FT_FLOW4_GROW_REINSERT_AHEAD 8u
#endif

#ifndef FT_FLOW4_GROW_CTX_RING
#define FT_FLOW4_GROW_CTX_RING 64u
#endif

struct ft_flow4_grow_ctx_s {
    struct ft_flow4_entry *entry;
    union rix_hash_hash_u  hash;
};

#ifdef FT_ARCH_SUFFIX
#define _FT_CAT2(a, b) a##b
#define _FT_CAT(a, b) _FT_CAT2(a, b)
#define _FT_API(name) _FT_CAT(name, FT_ARCH_SUFFIX)

#define ft_flow4_table_init_ex _FT_API(ft_flow4_table_init_ex)
#define ft_flow4_table_init _FT_API(ft_flow4_table_init)
#define ft_flow4_table_destroy _FT_API(ft_flow4_table_destroy)
#define ft_flow4_table_flush _FT_API(ft_flow4_table_flush)
#define ft_flow4_table_nb_entries _FT_API(ft_flow4_table_nb_entries)
#define ft_flow4_table_nb_bk _FT_API(ft_flow4_table_nb_bk)
#define ft_flow4_table_need_grow _FT_API(ft_flow4_table_need_grow)
#define ft_flow4_table_stats _FT_API(ft_flow4_table_stats)
#define ft_flow4_table_find _FT_API(ft_flow4_table_find)
#define ft_flow4_table_add _FT_API(ft_flow4_table_add)
#define ft_flow4_table_add_idx _FT_API(ft_flow4_table_add_idx)
#define ft_flow4_table_add_entry _FT_API(ft_flow4_table_add_entry)
#define ft_flow4_table_del _FT_API(ft_flow4_table_del)
#define ft_flow4_table_find_bulk _FT_API(ft_flow4_table_find_bulk)
#define ft_flow4_table_add_bulk _FT_API(ft_flow4_table_add_bulk)
#define ft_flow4_table_add_idx_bulk _FT_API(ft_flow4_table_add_idx_bulk)
#define ft_flow4_table_add_entry_bulk _FT_API(ft_flow4_table_add_entry_bulk)
#define ft_flow4_table_del_bulk _FT_API(ft_flow4_table_del_bulk)
#define ft_flow4_table_walk _FT_API(ft_flow4_table_walk)
#define ft_flow4_table_grow_2x _FT_API(ft_flow4_table_grow_2x)
#define ft_flow4_table_reserve _FT_API(ft_flow4_table_reserve)

int ft_flow4_table_init_ex(struct ft_flow4_table *ft,
                           void *array,
                           unsigned max_entries,
                           size_t stride,
                           size_t entry_offset,
                           const struct ft_flow4_config *cfg);
int ft_flow4_table_init(struct ft_flow4_table *ft,
                        struct ft_flow4_entry *pool,
                        unsigned max_entries,
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
int ft_flow4_table_reserve(struct ft_flow4_table *ft, unsigned min_entries);
#endif

#define FT_FLOW4_ENTRY_FLAG_ACTIVE 0x0001u

static inline union rix_hash_hash_u
ft_flow4_hash_start_mask_(const struct ft_flow4_key *key, uint32_t start_mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    union rix_hash_hash_u r;
    uint64_t w0, w1, w2;
    uint32_t h0, bk0, seed, h1;

    memcpy(&w0, (const char *)key, 8u);
    memcpy(&w1, (const char *)key + 8u, 8u);
    memcpy(&w2, (const char *)key + 16u, 8u);
    h0 = (uint32_t)__builtin_ia32_crc32di(0ULL, w0);
    h0 = (uint32_t)__builtin_ia32_crc32di((uint64_t)h0, w1);
    h0 = (uint32_t)__builtin_ia32_crc32di((uint64_t)h0, w2);
    if (h0 == 0u)
        h0 = 1u;
    bk0 = h0 & start_mask;
    seed = ~h0;
    do {
        h1 = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, w0);
        h1 = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1, w1);
        h1 = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1, w2);
        if (h1 == 0u)
            h1 = 1u;
        seed = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, (uint64_t)h0);
    } while ((h1 & start_mask) == bk0);

    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
#else
    union rix_hash_hash_u r = rix_hash_hash_bytes_fast(key, sizeof(*key),
                                                       start_mask);
    if (r.val32[0] == 0u)
        r.val32[0] = 1u;
    if (r.val32[1] == 0u)
        r.val32[1] = 1u;
    return r;
#endif
}

static inline union rix_hash_hash_u
ft_flow4_hash_pair_(const struct ft_flow4_table *ft,
                    const struct ft_flow4_key *key)
{
    return ft_flow4_hash_start_mask_(key, ft->start_mask);
}

static inline union rix_hash_hash_u
ft_flow4_hash_fn(const struct ft_flow4_key *key, uint32_t mask)
{
    return ft_flow4_hash_start_mask_(key, mask);
}

static inline int
ft_flow4_cmp(const struct ft_flow4_key *a, const struct ft_flow4_key *b)
{
    uint64_t a0, a1, a2, b0, b1, b2;

    memcpy(&a0, a, 8u);
    memcpy(&a1, (const char *)a + 8u, 8u);
    memcpy(&a2, (const char *)a + 16u, 8u);
    memcpy(&b0, b, 8u);
    memcpy(&b1, (const char *)b + 8u, 8u);
    memcpy(&b2, (const char *)b + 16u, 8u);
    return ((a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2)) ? 1 : 0;
}

static inline struct ft_flow4_entry *
ft_flow4_layout_entry_ptr_(const struct ft_flow4_table *ft, unsigned idx)
{
    if (idx == RIX_NIL)
        return NULL;
    return FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, idx,
                                ft->pool_entry_offset, struct ft_flow4_entry);
}

static inline unsigned
ft_flow4_layout_entry_idx_(const struct ft_flow4_table *ft,
                           const struct ft_flow4_entry *entry)
{
    if (entry == NULL)
        return RIX_NIL;
    return ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                           ft->pool_entry_offset, entry);
}

static inline struct ft_flow4_entry *
ft_flow4_find_hashed_(struct ft_flow4_table *ft,
                      const struct ft_flow4_key *key,
                      union rix_hash_hash_u h)
{
    unsigned bk0, bk1;
    u32 fp;
    u32 hits;

    _rix_hash_buckets(h, ft->ht_head.rhh_mask, &bk0, &bk1, &fp);
    hits = _RIX_HASH_FIND_U32X16(ft->buckets[bk0].hash, fp);
    while (hits != 0u) {
        unsigned bit = (unsigned)__builtin_ctz(hits);
        unsigned idx = ft->buckets[bk0].idx[bit];
        struct ft_flow4_entry *entry;

        hits &= hits - 1u;
        if (idx == (unsigned)RIX_NIL)
            continue;
        entry = ft_flow4_layout_entry_ptr_(ft, idx);
        RIX_ASSUME_NONNULL(entry);
        if (ft_flow4_cmp(&entry->key, key) == 0)
            return entry;
    }
    if (bk1 == bk0)
        return NULL;
    hits = _RIX_HASH_FIND_U32X16(ft->buckets[bk1].hash, fp);
    while (hits != 0u) {
        unsigned bit = (unsigned)__builtin_ctz(hits);
        unsigned idx = ft->buckets[bk1].idx[bit];
        struct ft_flow4_entry *entry;

        hits &= hits - 1u;
        if (idx == (unsigned)RIX_NIL)
            continue;
        entry = ft_flow4_layout_entry_ptr_(ft, idx);
        RIX_ASSUME_NONNULL(entry);
        if (ft_flow4_cmp(&entry->key, key) == 0)
            return entry;
    }
    return NULL;
}

#undef RIX_HASH_SLOT_DEFINE_INDEXERS
#define RIX_HASH_SLOT_DEFINE_INDEXERS(name, type)                              \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    const struct ft_flow4_table *ft =                                         \
        (const struct ft_flow4_table *)(const void *)base;                    \
    return ft_flow4_layout_entry_idx_(ft, p);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    const struct ft_flow4_table *ft =                                         \
        (const struct ft_flow4_table *)(const void *)base;                    \
    return ft_flow4_layout_entry_ptr_(ft, i);                                 \
}

RIX_HASH_GENERATE_STATIC_SLOT_EX(ft_flow4_ht, ft_flow4_entry, key, cur_hash,
                                 slot, ft_flow4_cmp, ft_flow4_hash_fn)

static inline struct ft_flow4_entry *
ft_flow4_hash_base_(struct ft_flow4_table *ft)
{
    return (struct ft_flow4_entry *)(void *)ft;
}

static inline int
ft_flow4_entry_is_active_(const struct ft_flow4_entry *entry)
{
    return (entry->flags & FT_FLOW4_ENTRY_FLAG_ACTIVE) != 0u;
}

static inline void
ft_flow4_entry_clear_(struct ft_flow4_entry *entry)
{
    memset(entry, 0, sizeof(*entry));
}

static inline void
ft_flow4_entry_meta_clear_(struct ft_flow4_entry *entry)
{
    entry->cur_hash = 0u;
    entry->hash0 = 0u;
    entry->hash1 = 0u;
    entry->slot = 0u;
    entry->flags = 0u;
    memset(entry->reserved0, 0, sizeof(entry->reserved0));
}

static inline void
ft_flow4_free_push_(struct ft_flow4_table *ft, unsigned idx,
                    struct ft_flow4_entry *entry)
{
    entry->next_free = ft->free_head;
    entry->flags = 0u;
    ft->free_head = idx;
}

static inline struct ft_flow4_entry *
ft_flow4_free_pop_(struct ft_flow4_table *ft, unsigned *idx_out)
{
    while (ft->free_head != RIX_NIL) {
        unsigned idx = ft->free_head;
        struct ft_flow4_entry *entry = ft_flow4_layout_entry_ptr_(ft, idx);

        RIX_ASSUME_NONNULL(entry);
        ft->free_head = entry->next_free;
        entry->next_free = RIX_NIL;
        if (!ft_flow4_entry_is_active_(entry)) {
            *idx_out = idx;
            return entry;
        }
    }
    *idx_out = RIX_NIL;
    return NULL;
}

static inline unsigned
ft_flow4_default_start_nb_bk_(unsigned max_entries)
{
    unsigned hinted;

    hinted = (max_entries + (RIX_HASH_BUCKET_ENTRY_SZ - 1u))
           / RIX_HASH_BUCKET_ENTRY_SZ;
    if (hinted < FT_FLOW4_DEFAULT_MIN_NB_BK)
        hinted = FT_FLOW4_DEFAULT_MIN_NB_BK;
    return ft_roundup_pow2_u32(hinted);
}

static inline unsigned
ft_flow4_required_nb_bk_(unsigned entries, unsigned fill_pct)
{
    uint64_t need_slots;
    unsigned need_bk;

    if (entries == 0u)
        return FT_FLOW4_DEFAULT_MIN_NB_BK;
    need_slots = ((uint64_t)entries * 100u + (uint64_t)fill_pct - 1u)
               / (uint64_t)fill_pct;
    need_bk = (unsigned)((need_slots + (uint64_t)RIX_HASH_BUCKET_ENTRY_SZ - 1u)
                       / (uint64_t)RIX_HASH_BUCKET_ENTRY_SZ);
    if (need_bk < FT_FLOW4_DEFAULT_MIN_NB_BK)
        need_bk = FT_FLOW4_DEFAULT_MIN_NB_BK;
    return ft_roundup_pow2_u32(need_bk);
}

static inline unsigned
ft_flow4_fill_pct_(const struct ft_flow4_table *ft)
{
    uint64_t slots;

    if (ft->nb_bk == 0u)
        return 0u;
    slots = (uint64_t)ft->nb_bk * (uint64_t)RIX_HASH_BUCKET_ENTRY_SZ;
    return (unsigned)(((uint64_t)ft->ht_head.rhh_nb * 100u) / slots);
}

static inline void
ft_flow4_mark_need_grow_(struct ft_flow4_table *ft)
{
    if (ft->need_grow == 0u) {
        ft->need_grow = 1u;
        ft->stats.grow_marks++;
    }
}

static inline size_t
ft_flow4_bucket_bytes_(unsigned nb_bk)
{
    return (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
}

static int
ft_flow4_alloc_buckets_(struct ft_flow4_table *ft,
                        unsigned nb_bk,
                        struct rix_hash_bucket_s **out)
{
    size_t bytes = ft_flow4_bucket_bytes_(nb_bk);
    void *ptr;

    ptr = ft->bucket_alloc.alloc(bytes, _Alignof(struct rix_hash_bucket_s),
                                 ft->bucket_alloc.arg);
    if (ptr == NULL)
        return -1;
    memset(ptr, 0, bytes);
    *out = (struct rix_hash_bucket_s *)ptr;
    return 0;
}

static void
ft_flow4_init_storage_(struct ft_flow4_table *ft, void *array,
                       size_t stride, size_t entry_offset)
{
    ft->pool_base = FT_BYTE_PTR(array);
    ft->pool_stride = stride;
    ft->pool_entry_offset = entry_offset;
    ft->pool = FT_RECORD_MEMBER_PTR(ft->pool_base, ft->pool_stride, 1u,
                                    ft->pool_entry_offset,
                                    struct ft_flow4_entry);
}

static int
ft_flow4_insert_hashed_(struct ft_flow4_table *ft,
                        struct ft_flow4_entry *entry,
                        const union rix_hash_hash_u h,
                        uint32_t *entry_idx_out)
{
    struct ft_flow4_entry *ret;

    entry->hash0 = h.val32[0];
    entry->hash1 = h.val32[1];
    ret = ft_flow4_ht_insert_hashed(&ft->ht_head, ft->buckets,
                                    ft_flow4_hash_base_(ft), entry, h);
    if (ret == NULL) {
        entry->flags = FT_FLOW4_ENTRY_FLAG_ACTIVE;
        *entry_idx_out = ft_flow4_layout_entry_idx_(ft, entry);
        if (ft_flow4_fill_pct_(ft) >= ft->grow_fill_pct &&
            ft->nb_bk < ft->max_nb_bk)
            ft_flow4_mark_need_grow_(ft);
        return 0;
    }
    if (ret != entry) {
        *entry_idx_out = ft_flow4_layout_entry_idx_(ft, ret);
        return 1;
    }
    *entry_idx_out = 0u;
    return -1;
}

static inline int
ft_flow4_rehash_insert_hashed_(struct ft_flow4_ht *head,
                               struct rix_hash_bucket_s *buckets,
                               struct ft_flow4_table *ft,
                               struct ft_flow4_entry *entry,
                               union rix_hash_hash_u h)
{
    unsigned mask = head->rhh_mask;
    unsigned bk0, bk1;
    u32 fp;

    _rix_hash_buckets(h, mask, &bk0, &bk1, &fp);
    entry->cur_hash = h.val32[0];

    for (unsigned pass = 0u; pass < 2u; pass++) {
        unsigned bki = (pass == 0u) ? bk0 : bk1;
        struct rix_hash_bucket_s *bk = &buckets[bki];
        u32 nilm = _RIX_HASH_FIND_U32X16(bk->hash, 0u);

        if (nilm != 0u) {
            unsigned slot = (unsigned)__builtin_ctz(nilm);

            bk->hash[slot] = fp;
            bk->idx[slot] = ft_flow4_layout_entry_idx_(ft, entry);
            if (pass == 1u)
                entry->cur_hash = h.val32[1];
            entry->slot = (uint16_t)slot;
            entry->flags = FT_FLOW4_ENTRY_FLAG_ACTIVE;
            head->rhh_nb++;
            return 0;
        }
    }

    {
        int pos;
        unsigned bki;
        struct rix_hash_bucket_s *bk;

        pos = ft_flow4_ht_kickout(buckets, ft_flow4_hash_base_(ft),
                                  mask, bk0, RIX_HASH_FOLLOW_DEPTH);
        if (pos >= 0) {
            bki = bk0;
        } else {
            pos = ft_flow4_ht_kickout(buckets, ft_flow4_hash_base_(ft),
                                      mask, bk1, RIX_HASH_FOLLOW_DEPTH);
            if (pos < 0)
                return -1;
            bki = bk1;
            entry->cur_hash = h.val32[1];
        }

        bk = &buckets[bki];
        bk->hash[pos] = fp;
        bk->idx[pos] = ft_flow4_layout_entry_idx_(ft, entry);
        entry->slot = (uint16_t)pos;
        entry->flags = FT_FLOW4_ENTRY_FLAG_ACTIVE;
        head->rhh_nb++;
        return 0;
    }
}

int
ft_flow4_table_init_ex(struct ft_flow4_table *ft,
                       void *array,
                       unsigned max_entries,
                       size_t stride,
                       size_t entry_offset,
                       const struct ft_flow4_config *cfg)
{
    struct ft_flow4_config defcfg;
    struct rix_hash_bucket_s *buckets;
    unsigned start_nb_bk;
    unsigned max_nb_bk;

    if (ft == NULL || array == NULL || max_entries == 0u)
        return -1;

    memset(&defcfg, 0, sizeof(defcfg));
    if (cfg == NULL)
        cfg = &defcfg;
    if (cfg->bucket_alloc.alloc == NULL || cfg->bucket_alloc.free == NULL)
        return -1;

    RIX_ASSERT(stride >= sizeof(struct ft_flow4_entry));
    RIX_ASSERT(entry_offset + sizeof(struct ft_flow4_entry) <= stride);
    RIX_ASSERT(FT_PTR_IS_ALIGNED(FT_BYTE_PTR_ADD(array, entry_offset),
                                 _Alignof(struct ft_flow4_entry)));

    start_nb_bk = cfg->start_nb_bk ? ft_roundup_pow2_u32(cfg->start_nb_bk)
                                   : ft_flow4_default_start_nb_bk_(max_entries);
    max_nb_bk = cfg->max_nb_bk ? ft_roundup_pow2_u32(cfg->max_nb_bk)
                               : FT_FLOW4_DEFAULT_MAX_NB_BK;
    if (start_nb_bk < ft_flow4_default_start_nb_bk_(max_entries))
        start_nb_bk = ft_flow4_default_start_nb_bk_(max_entries);
    if (max_nb_bk < start_nb_bk)
        max_nb_bk = start_nb_bk;

    memset(ft, 0, sizeof(*ft));
    ft->bucket_alloc = cfg->bucket_alloc;
    ft->grow_fill_pct = cfg->grow_fill_pct ? cfg->grow_fill_pct
                                           : FT_FLOW4_DEFAULT_GROW_FILL_PCT;
    ft->max_entries = max_entries;
    ft->max_nb_bk = max_nb_bk;
    ft->start_mask = start_nb_bk - 1u;
    ft_flow4_init_storage_(ft, array, stride, entry_offset);

    if (ft_flow4_alloc_buckets_(ft, start_nb_bk, &buckets) != 0)
        return -1;
    ft->buckets = buckets;
    ft->nb_bk = start_nb_bk;
    ft_flow4_ht_init(&ft->ht_head, start_nb_bk);
    ft->free_head = RIX_NIL;
    ft_flow4_table_flush(ft);
    return 0;
}

int
ft_flow4_table_init(struct ft_flow4_table *ft,
                    struct ft_flow4_entry *pool,
                    unsigned max_entries,
                    const struct ft_flow4_config *cfg)
{
    return ft_flow4_table_init_ex(ft, pool, max_entries,
                                  sizeof(*pool), 0u, cfg);
}

void
ft_flow4_table_destroy(struct ft_flow4_table *ft)
{
    if (ft == NULL)
        return;
    if (ft->buckets != NULL && ft->bucket_alloc.free != NULL) {
        ft->bucket_alloc.free(ft->buckets, ft_flow4_bucket_bytes_(ft->nb_bk),
                              _Alignof(struct rix_hash_bucket_s),
                              ft->bucket_alloc.arg);
    }
    memset(ft, 0, sizeof(*ft));
}

void
ft_flow4_table_flush(struct ft_flow4_table *ft)
{
    if (ft == NULL || ft->buckets == NULL)
        return;
    memset(ft->buckets, 0, ft_flow4_bucket_bytes_(ft->nb_bk));
    ft_flow4_ht_init(&ft->ht_head, ft->nb_bk);
    ft->free_head = RIX_NIL;
    ft->need_grow = 0u;
    for (unsigned i = ft->max_entries; i > 0u; i--) {
        struct ft_flow4_entry *entry = ft_flow4_layout_entry_ptr_(ft, i);

        RIX_ASSUME_NONNULL(entry);
        ft_flow4_entry_meta_clear_(entry);
        ft_flow4_free_push_(ft, i, entry);
    }
}

unsigned
ft_flow4_table_nb_entries(const struct ft_flow4_table *ft)
{
    return ft == NULL ? 0u : ft->ht_head.rhh_nb;
}

unsigned
ft_flow4_table_nb_bk(const struct ft_flow4_table *ft)
{
    return ft == NULL ? 0u : ft->nb_bk;
}

unsigned
ft_flow4_table_need_grow(const struct ft_flow4_table *ft)
{
    return ft == NULL ? 0u : ft->need_grow;
}

void
ft_flow4_table_stats(const struct ft_flow4_table *ft, struct ft_flow4_stats *out)
{
    if (out == NULL)
        return;
    if (ft == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = ft->stats;
}

uint32_t
ft_flow4_table_find(struct ft_flow4_table *ft, const struct ft_flow4_key *key)
{
    struct ft_flow4_entry *entry;
    union rix_hash_hash_u h;

    if (ft == NULL || key == NULL || ft->buckets == NULL)
        return 0u;
    ft->stats.lookups++;
    h = ft_flow4_hash_pair_(ft, key);
    entry = ft_flow4_find_hashed_(ft, key, h);
    if (entry == NULL) {
        ft->stats.misses++;
        return 0u;
    }
    ft->stats.hits++;
    return ft_flow4_layout_entry_idx_(ft, entry);
}

uint32_t
ft_flow4_table_add(struct ft_flow4_table *ft, const struct ft_flow4_key *key)
{
    struct ft_flow4_entry *entry;
    unsigned idx;
    uint32_t out_idx;

    if (ft == NULL || key == NULL || ft->buckets == NULL)
        return 0u;
    entry = ft_flow4_free_pop_(ft, &idx);
    if (entry == NULL) {
        ft->stats.add_failed++;
        return 0u;
    }
    ft_flow4_entry_clear_(entry);
    entry->key = *key;
    out_idx = ft_flow4_table_add_idx(ft, idx);
    if (out_idx != idx)
        ft_flow4_free_push_(ft, idx, entry);
    return out_idx;
}

uint32_t
ft_flow4_table_add_idx(struct ft_flow4_table *ft, uint32_t entry_idx)
{
    struct ft_flow4_entry *entry;
    union rix_hash_hash_u h;
    uint32_t out_idx = entry_idx;
    int rc;

    if (ft == NULL || ft->buckets == NULL || entry_idx == 0u
        || entry_idx > ft->max_entries)
        return 0u;
    entry = ft_flow4_layout_entry_ptr_(ft, entry_idx);
    RIX_ASSUME_NONNULL(entry);
    if (ft_flow4_entry_is_active_(entry)) {
        ft->stats.add_existing++;
        return entry_idx;
    }
    ft_flow4_entry_meta_clear_(entry);
    h = ft_flow4_hash_pair_(ft, &entry->key);
    rc = ft_flow4_insert_hashed_(ft, entry, h, &out_idx);
    if (rc == 0) {
        ft->stats.adds++;
        return out_idx;
    }
    ft_flow4_entry_meta_clear_(entry);
    if (rc > 0) {
        ft->stats.add_existing++;
        return out_idx;
    }
    ft->stats.add_failed++;
    if (ft->nb_bk < ft->max_nb_bk)
        ft_flow4_mark_need_grow_(ft);
    return 0u;
}

uint32_t
ft_flow4_table_add_entry(struct ft_flow4_table *ft, struct ft_flow4_entry *entry)
{
    if (ft == NULL || entry == NULL)
        return 0u;
    return ft_flow4_table_add_idx(ft, ft_flow4_layout_entry_idx_(ft, entry));
}

uint32_t
ft_flow4_table_del(struct ft_flow4_table *ft, const struct ft_flow4_key *key)
{
    struct ft_flow4_entry *entry;
    union rix_hash_hash_u h;
    unsigned idx;

    if (ft == NULL || key == NULL || ft->buckets == NULL)
        return 0u;
    h = ft_flow4_hash_pair_(ft, key);
    entry = ft_flow4_find_hashed_(ft, key, h);
    if (entry == NULL) {
        ft->stats.del_miss++;
        return 0u;
    }
    idx = ft_flow4_layout_entry_idx_(ft, entry);
    if (ft_flow4_ht_remove(&ft->ht_head, ft->buckets,
                           ft_flow4_hash_base_(ft), entry) == NULL)
        return 0u;
    ft->stats.dels++;
    ft_flow4_entry_meta_clear_(entry);
    ft_flow4_free_push_(ft, idx, entry);
    return idx;
}

void
ft_flow4_table_find_bulk(struct ft_flow4_table *ft,
                         const struct ft_flow4_key *keys,
                         unsigned nb_keys,
                         struct ft_flow4_result *results)
{
    if (results == NULL)
        return;
    for (unsigned i = 0; i < nb_keys; i++)
        results[i].entry_idx = ft_flow4_table_find(ft, &keys[i]);
}

void
ft_flow4_table_add_bulk(struct ft_flow4_table *ft,
                        const struct ft_flow4_key *keys,
                        unsigned nb_keys,
                        struct ft_flow4_result *results)
{
    if (results == NULL)
        return;
    if (ft == NULL || ft->buckets == NULL) {
        for (unsigned i = 0; i < nb_keys; i++)
            results[i].entry_idx = 0u;
        return;
    }
    for (unsigned i = 0; i < nb_keys; i++)
        results[i].entry_idx = ft_flow4_table_add(ft, &keys[i]);
}

void
ft_flow4_table_add_idx_bulk(struct ft_flow4_table *ft,
                            const uint32_t *entry_idxv,
                            unsigned nb_keys,
                            struct ft_flow4_result *results)
{
    const unsigned step_keys = FT_FLOW4_BULK_STEP_KEYS;
    const unsigned ahead_keys = FT_FLOW4_BULK_AHEAD_KEYS;
    const unsigned total = nb_keys + ahead_keys;
    union rix_hash_hash_u hashes[FT_FLOW4_BULK_CTX_RING];

    if (results == NULL)
        return;
    if (ft == NULL || ft->buckets == NULL || entry_idxv == NULL) {
        for (unsigned i = 0; i < nb_keys; i++)
            results[i].entry_idx = 0u;
        return;
    }

    for (unsigned i = 0; i < nb_keys; i++) {
        struct ft_flow4_entry *entry;

        results[i].entry_idx = 0u;
        if (entry_idxv[i] == 0u || entry_idxv[i] > ft->max_entries)
            continue;
        entry = ft_flow4_layout_entry_ptr_(ft, entry_idxv[i]);
        RIX_ASSUME_NONNULL(entry);
        rix_hash_prefetch_key(&entry->key);
    }

    for (unsigned i = 0; i < total; i += step_keys) {
        if (i < nb_keys) {
            unsigned n = (i + step_keys <= nb_keys) ? step_keys : (nb_keys - i);

            for (unsigned j = 0; j < n; j++) {
                unsigned idx = i + j;
                uint32_t entry_idx = entry_idxv[idx];
                struct ft_flow4_entry *entry;
                unsigned bk0, bk1;
                u32 fp_unused;
                union rix_hash_hash_u h;

                if (entry_idx == 0u || entry_idx > ft->max_entries)
                    continue;
                entry = ft_flow4_layout_entry_ptr_(ft, entry_idx);
                RIX_ASSUME_NONNULL(entry);
                h = ft_flow4_hash_pair_(ft, &entry->key);
                hashes[idx & (FT_FLOW4_BULK_CTX_RING - 1u)] = h;
                _rix_hash_buckets(h, ft->ht_head.rhh_mask, &bk0, &bk1,
                                  &fp_unused);
                rix_hash_prefetch_bucket(&ft->buckets[bk0]);
                if (bk1 != bk0)
                    rix_hash_prefetch_bucket(&ft->buckets[bk1]);
            }
        }

        if (i >= ahead_keys && i - ahead_keys < nb_keys) {
            unsigned base = i - ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys
                                                       : (nb_keys - base);

            for (unsigned j = 0; j < n; j++) {
                unsigned idx = base + j;
                uint32_t entry_idx = entry_idxv[idx];
                struct ft_flow4_entry *entry;
                uint32_t out_idx;
                int rc;

                if (entry_idx == 0u || entry_idx > ft->max_entries)
                    continue;
                entry = ft_flow4_layout_entry_ptr_(ft, entry_idx);
                RIX_ASSUME_NONNULL(entry);
                if (ft_flow4_entry_is_active_(entry)) {
                    ft->stats.add_existing++;
                    results[idx].entry_idx = entry_idx;
                    continue;
                }

                ft_flow4_entry_meta_clear_(entry);
                out_idx = entry_idx;
                rc = ft_flow4_insert_hashed_(
                    ft, entry, hashes[idx & (FT_FLOW4_BULK_CTX_RING - 1u)],
                    &out_idx);
                if (rc == 0) {
                    ft->stats.adds++;
                    results[idx].entry_idx = out_idx;
                } else {
                    ft_flow4_entry_meta_clear_(entry);
                    if (rc > 0) {
                        ft->stats.add_existing++;
                        results[idx].entry_idx = out_idx;
                    } else {
                        ft->stats.add_failed++;
                        if (ft->nb_bk < ft->max_nb_bk)
                            ft_flow4_mark_need_grow_(ft);
                        results[idx].entry_idx = 0u;
                    }
                }
            }
        }
    }
}

void
ft_flow4_table_add_entry_bulk(struct ft_flow4_table *ft,
                              struct ft_flow4_entry *const *entries,
                              unsigned nb_keys,
                              struct ft_flow4_result *results)
{
    if (results == NULL)
        return;
    for (unsigned i = 0; i < nb_keys; i++) {
        struct ft_flow4_entry *entry =
            (entries != NULL) ? entries[i] : NULL;

        results[i].entry_idx = ft_flow4_table_add_entry(ft, entry);
    }
}

void
ft_flow4_table_del_bulk(struct ft_flow4_table *ft,
                        const struct ft_flow4_key *keys,
                        unsigned nb_keys,
                        struct ft_flow4_result *results)
{
    if (results == NULL)
        return;
    for (unsigned i = 0; i < nb_keys; i++)
        results[i].entry_idx = ft_flow4_table_del(ft, &keys[i]);
}

int
ft_flow4_table_walk(struct ft_flow4_table *ft,
                    int (*cb)(uint32_t entry_idx, void *arg),
                    void *arg)
{
    if (ft == NULL || cb == NULL)
        return -1;
    for (unsigned i = 1u; i <= ft->max_entries; i++) {
        const struct ft_flow4_entry *entry = ft_flow4_layout_entry_ptr_(ft, i);

        RIX_ASSUME_NONNULL(entry);
        if (!ft_flow4_entry_is_active_(entry))
            continue;
        if (cb(i, arg) != 0)
            return 1;
    }
    return 0;
}

int
ft_flow4_table_grow_2x(struct ft_flow4_table *ft)
{
    struct rix_hash_bucket_s *new_buckets;
    struct ft_flow4_ht new_head;
    struct ft_flow4_grow_ctx_s ctx[FT_FLOW4_GROW_CTX_RING];
    unsigned new_nb_bk;
    unsigned new_mask;
    unsigned produced = 0u;
    unsigned consumed = 0u;
    unsigned old_mask;

    if (ft == NULL || ft->buckets == NULL)
        return -1;
    if (ft->nb_bk >= ft->max_nb_bk) {
        ft->stats.grow_failures++;
        return -1;
    }
    new_nb_bk = ft->nb_bk << 1;
    if (new_nb_bk == 0u || new_nb_bk > ft->max_nb_bk) {
        ft->stats.grow_failures++;
        return -1;
    }
    if (ft_flow4_alloc_buckets_(ft, new_nb_bk, &new_buckets) != 0) {
        ft->stats.grow_failures++;
        return -1;
    }
    ft_flow4_ht_init(&new_head, new_nb_bk);
    new_mask = new_head.rhh_mask;
    old_mask = ft->ht_head.rhh_mask;

    /*
     * Grow walks the old table only to harvest occupied entry indices.
     * Prefetch only the idx[] line on the old side; hash[] is not consulted.
     */
    for (unsigned bk = 0u;
         bk < FT_FLOW4_GROW_OLD_BK_AHEAD && bk <= old_mask;
         bk++)
        rix_hash_prefetch_bucket_idx(&ft->buckets[bk]);

    for (unsigned bk = 0u; bk <= old_mask; bk++) {
        const struct rix_hash_bucket_s *old_bk = &ft->buckets[bk];
        unsigned prefetch_bk = bk + FT_FLOW4_GROW_OLD_BK_AHEAD;

        if (prefetch_bk <= old_mask)
            rix_hash_prefetch_bucket_idx(&ft->buckets[prefetch_bk]);

        for (unsigned slot = 0u; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {
            unsigned idx = old_bk->idx[slot];
            struct ft_flow4_entry *entry;

            if (idx == (unsigned)RIX_NIL)
                continue;
            entry = ft_flow4_layout_entry_ptr_(ft, idx);
            RIX_ASSUME_NONNULL(entry);
            rix_hash_prefetch_entry(entry);
        }

        for (unsigned slot = 0u; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {
            unsigned idx = old_bk->idx[slot];
            struct ft_flow4_entry *entry;
            union rix_hash_hash_u h;
            unsigned bk0, bk1;
            u32 fp_unused;

            if (idx == (unsigned)RIX_NIL)
                continue;
            entry = ft_flow4_layout_entry_ptr_(ft, idx);
            RIX_ASSUME_NONNULL(entry);
            h.val32[0] = entry->hash0;
            h.val32[1] = entry->hash1;
            ctx[produced & (FT_FLOW4_GROW_CTX_RING - 1u)].entry = entry;
            ctx[produced & (FT_FLOW4_GROW_CTX_RING - 1u)].hash = h;
            _rix_hash_buckets(h, new_mask, &bk0, &bk1, &fp_unused);
            /*
             * Rehash writes into a 2x larger table, so fill is low enough that
             * bk0 usually has a free slot. Eagerly prefetch only bk0.hash[].
             * bk1 and idx[] are touched lazily in the commit path on demand.
             */
            rix_hash_prefetch_bucket_hash(&new_buckets[bk0]);
            produced++;

            if (produced - consumed > FT_FLOW4_GROW_REINSERT_AHEAD) {
                struct ft_flow4_grow_ctx_s *gc =
                    &ctx[consumed & (FT_FLOW4_GROW_CTX_RING - 1u)];

                if (ft_flow4_rehash_insert_hashed_(&new_head, new_buckets,
                                                   ft, gc->entry,
                                                   gc->hash) != 0) {
                    ft->bucket_alloc.free(new_buckets,
                                          ft_flow4_bucket_bytes_(new_nb_bk),
                                          _Alignof(struct rix_hash_bucket_s),
                                          ft->bucket_alloc.arg);
                    ft->stats.grow_failures++;
                    return -1;
                }
                consumed++;
            }
        }
    }

    while (consumed < produced) {
        struct ft_flow4_grow_ctx_s *gc =
            &ctx[consumed & (FT_FLOW4_GROW_CTX_RING - 1u)];

        if (ft_flow4_rehash_insert_hashed_(&new_head, new_buckets,
                                           ft, gc->entry, gc->hash) != 0) {
            ft->bucket_alloc.free(new_buckets,
                                  ft_flow4_bucket_bytes_(new_nb_bk),
                                  _Alignof(struct rix_hash_bucket_s),
                                  ft->bucket_alloc.arg);
            ft->stats.grow_failures++;
            return -1;
        }
        consumed++;
    }
    ft->bucket_alloc.free(ft->buckets, ft_flow4_bucket_bytes_(ft->nb_bk),
                          _Alignof(struct rix_hash_bucket_s),
                          ft->bucket_alloc.arg);
    ft->buckets = new_buckets;
    ft->ht_head = new_head;
    ft->nb_bk = new_nb_bk;
    ft->need_grow = ft_flow4_fill_pct_(ft) >= ft->grow_fill_pct ? 1u : 0u;
    ft->stats.grow_execs++;
    return 0;
}

int
ft_flow4_table_reserve(struct ft_flow4_table *ft, unsigned min_entries)
{
    unsigned required_nb_bk;

    if (ft == NULL)
        return -1;
    ft->stats.reserve_calls++;
    required_nb_bk = ft_flow4_required_nb_bk_(min_entries, ft->grow_fill_pct);
    if (required_nb_bk > ft->max_nb_bk)
        return -1;
    while (ft->nb_bk < required_nb_bk) {
        if (ft_flow4_table_grow_2x(ft) != 0)
            return -1;
    }
    ft->need_grow = ft_flow4_fill_pct_(ft) >= ft->grow_fill_pct ? 1u : 0u;
    return 0;
}

#ifdef FT_ARCH_SUFFIX
#include "ft_ops.h"

#define FT_OPS_NAME(prefix, suffix) _FT_CAT(ft_##prefix##_ops, suffix)

#define FT_OPS_TABLE(prefix, suffix)                                           \
const struct ft_##prefix##_ops FT_OPS_NAME(prefix, suffix) = {                 \
    .find = ft_##prefix##_table_find,                                          \
    .add = ft_##prefix##_table_add,                                            \
    .add_idx = ft_##prefix##_table_add_idx,                                    \
    .add_entry = ft_##prefix##_table_add_entry,                                \
    .del = ft_##prefix##_table_del,                                            \
    .find_bulk = ft_##prefix##_table_find_bulk,                                \
    .add_bulk = ft_##prefix##_table_add_bulk,                                  \
    .add_idx_bulk = ft_##prefix##_table_add_idx_bulk,                          \
    .add_entry_bulk = ft_##prefix##_table_add_entry_bulk,                      \
    .del_bulk = ft_##prefix##_table_del_bulk,                                  \
}

FT_OPS_TABLE(flow4, FT_ARCH_SUFFIX);
#endif
