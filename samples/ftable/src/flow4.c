/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>

#include <rix/rix_hash_slot.h>

#include "flow4_table.h"

/*
 * Direct CRC32C hash for 24B flow4 key -- bypasses rix_hash_arch->hash_bytes
 * function-pointer dispatch.  24B = 3 x crc32q, no remainder handling.
 */
static inline union rix_hash_hash_u
ft_flow4_hash_fn(const struct flow4_key *key, uint32_t mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    union rix_hash_hash_u r;
    uint64_t w0, w1, w2;
    uint32_t h0, bk0, seed, h1;

    memcpy(&w0, (const char *)key,      8u);
    memcpy(&w1, (const char *)key + 8u,  8u);
    memcpy(&w2, (const char *)key + 16u, 8u);
    h0  = (uint32_t)__builtin_ia32_crc32di(0ULL,          w0);
    h0  = (uint32_t)__builtin_ia32_crc32di((uint64_t)h0,  w1);
    h0  = (uint32_t)__builtin_ia32_crc32di((uint64_t)h0,  w2);
    if (h0 == 0u)
        h0 = 1u;
    bk0  = h0 & mask;
    seed = ~h0;
    do {
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, w0);
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1,   w1);
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)h1,   w2);
        if (h1 == 0u)
            h1 = 1u;
        seed = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, (uint64_t)h0);
    } while ((h1 & mask) == bk0);

    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
#else
    union rix_hash_hash_u r = rix_hash_hash_bytes_fast(key, sizeof(*key),
                                                       mask);
    if (r.val32[0] == 0u)
        r.val32[0] = 1u;
    if (r.val32[1] == 0u)
        r.val32[1] = 1u;
    return r;
#endif
}

/*
 * Inline 24B key comparison -- avoids function-pointer overhead.
 * 24B = 3 x uint64_t XOR-OR.
 */
static inline int
ft_flow4_cmp(const struct flow4_key *a, const struct flow4_key *b)
{
    uint64_t a0, a1, a2, b0, b1, b2;

    memcpy(&a0, a,                            8u);
    memcpy(&a1, (const char *)a + 8u,         8u);
    memcpy(&a2, (const char *)a + 16u,        8u);
    memcpy(&b0, b,                            8u);
    memcpy(&b1, (const char *)b + 8u,         8u);
    memcpy(&b2, (const char *)b + 16u,        8u);
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

#define FTG_LAYOUT_INIT_STORAGE(ft, array, stride, entry_offset)               \
    do {                                                                       \
        (ft)->pool_base = (unsigned char *)(array);                            \
        (ft)->pool_stride = (stride);                                          \
        (ft)->pool_entry_offset = (entry_offset);                              \
        (ft)->pool = FT_RECORD_MEMBER_PTR((ft)->pool_base, (ft)->pool_stride, \
                                          1u, (ft)->pool_entry_offset,         \
                                          struct ft_flow4_entry);              \
    } while (0)

#define FTG_LAYOUT_HASH_BASE(ft)                                               \
    ((struct ft_flow4_entry *)(void *)(ft))

#define FTG_LAYOUT_ENTRY_PTR(ft, idx) ft_flow4_layout_entry_ptr_((ft), (idx))
#define FTG_LAYOUT_ENTRY_INDEX(ft, entry)                                      \
    ft_flow4_layout_entry_idx_((ft), (entry))

#define FTG_LAYOUT_ENTRY_AT(ft, off0)                                          \
    ft_flow4_layout_entry_ptr_((ft), (unsigned)(off0) + 1u)

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

#include "ft_table_generate.h"

#ifdef FT_ARCH_SUFFIX
int _FTG_API(flow4, init_ex)(struct ft_flow4_table *ft,
                             void *array,
                             unsigned max_entries,
                             size_t stride,
                             size_t entry_offset,
                             const struct ft_flow4_config *cfg);
int _FTG_API(flow4, init)(struct ft_flow4_table *ft,
                          struct ft_flow4_entry *pool,
                          unsigned max_entries,
                          const struct ft_flow4_config *cfg);
void _FTG_API(flow4, destroy)(struct ft_flow4_table *ft);
void _FTG_API(flow4, flush)(struct ft_flow4_table *ft);
unsigned _FTG_API(flow4, nb_entries)(const struct ft_flow4_table *ft);
unsigned _FTG_API(flow4, nb_bk)(const struct ft_flow4_table *ft);
unsigned _FTG_API(flow4, need_grow)(const struct ft_flow4_table *ft);
void _FTG_API(flow4, stats)(const struct ft_flow4_table *ft,
                            struct ft_flow4_stats *out);
uint32_t _FTG_API(flow4, find)(struct ft_flow4_table *ft,
                               const struct flow4_key *key);
uint32_t _FTG_API(flow4, add_entry)(struct ft_flow4_table *ft,
                                    uint32_t entry_idx);
uint32_t _FTG_API(flow4, del)(struct ft_flow4_table *ft,
                              const struct flow4_key *key);
uint32_t _FTG_API(flow4, del_idx)(struct ft_flow4_table *ft,
                                  uint32_t entry_idx);
void _FTG_API(flow4, find_bulk)(struct ft_flow4_table *ft,
                                const struct flow4_key *keys,
                                unsigned nb_keys,
                                struct ft_flow4_result *results);
void _FTG_API(flow4, add_entry_bulk)(struct ft_flow4_table *ft,
                                     const uint32_t *entry_idxv,
                                     unsigned nb_keys,
                                     struct ft_flow4_result *results);
void _FTG_API(flow4, del_bulk)(struct ft_flow4_table *ft,
                               const struct flow4_key *keys,
                               unsigned nb_keys,
                               struct ft_flow4_result *results);
int _FTG_API(flow4, walk)(struct ft_flow4_table *ft,
                          int (*cb)(uint32_t entry_idx, void *arg),
                          void *arg);
int _FTG_API(flow4, grow_2x)(struct ft_flow4_table *ft);
int _FTG_API(flow4, reserve)(struct ft_flow4_table *ft,
                             unsigned min_entries);
#endif

FT_TABLE_GENERATE(flow4,
                  FT_FLOW4_DEFAULT_MIN_NB_BK,
                  FT_FLOW4_DEFAULT_MAX_NB_BK,
                  FT_FLOW4_DEFAULT_GROW_FILL_PCT,
                  FT_FLOW4_ENTRY_FLAG_ACTIVE,
                  ft_flow4_hash_fn, ft_flow4_cmp)

#ifdef FT_ARCH_SUFFIX
#include "ft_ops.h"
FT_OPS_TABLE(flow4, FT_ARCH_SUFFIX);
#endif

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
