/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * ft_dispatch_extra.c - runtime arch dispatch for flowtable slot_extra
 * variant.  Only flow4 is supported; no variant switch.
 */

#include <string.h>

#include <rix/rix_hash_arch.h>

#include "flow_extra_common.h"
#include "flow_extra_key.h"
#include "flow_dispatch_extra.h"

/*===========================================================================
 * Maintain arch-variant declarations
 *===========================================================================*/
typedef unsigned (*ft_maintain_extra_fn_t)(const struct ft_maint_extra_ctx *,
                                           unsigned, u64, u64,
                                           u32 *, unsigned, unsigned,
                                           unsigned *);
typedef unsigned (*ft_maintain_extra_idx_bulk_fn_t)(
    const struct ft_maint_extra_ctx *, const u32 *, unsigned,
    u64, u64, u32 *, unsigned, unsigned, int);

#define FT_MAINT_EXTRA_DECLARE(suffix)                                         \
    extern unsigned ft_table_extra_maintain##suffix(                           \
        const struct ft_maint_extra_ctx *, unsigned, u64, u64,                 \
        u32 *, unsigned, unsigned, unsigned *);                                \
    extern unsigned ft_table_extra_maintain_idx_bulk##suffix(                  \
        const struct ft_maint_extra_ctx *, const u32 *, unsigned,              \
        u64, u64, u32 *, unsigned, unsigned, int)

FT_MAINT_EXTRA_DECLARE(_gen);
FT_MAINT_EXTRA_DECLARE(_sse);
FT_MAINT_EXTRA_DECLARE(_avx2);
FT_MAINT_EXTRA_DECLARE(_avx512);

/*===========================================================================
 * Active ops pointers (default to _gen)
 *===========================================================================*/
static const struct ft_flow4_extra_ops *ft_flow4_extra_active =
    &ft_flow4_extra_ops_gen;

static ft_maintain_extra_fn_t ft_maintain_extra_active =
    ft_table_extra_maintain_gen;
static ft_maintain_extra_idx_bulk_fn_t ft_maintain_extra_idx_bulk_active =
    ft_table_extra_maintain_idx_bulk_gen;

/*===========================================================================
 * Arch init
 *===========================================================================*/
static unsigned
ft_rix_hash_arch_enable_(unsigned arch_enable)
{
    unsigned enable = 0u;

    if (arch_enable == FT_ARCH_GEN)
        return 0u;
    if ((arch_enable & FT_ARCH_AVX512) != 0u)
        enable |= RIX_HASH_ARCH_AVX512 | RIX_HASH_ARCH_AVX2 | RIX_HASH_ARCH_SSE;
    else if ((arch_enable & FT_ARCH_AVX2) != 0u)
        enable |= RIX_HASH_ARCH_AVX2 | RIX_HASH_ARCH_SSE;
    else if ((arch_enable & FT_ARCH_SSE) != 0u)
        enable |= RIX_HASH_ARCH_SSE;
    return enable;
}

static void
ft_maint_extra_select_(unsigned arch_enable)
{
    ft_maintain_extra_active = ft_table_extra_maintain_gen;
    ft_maintain_extra_idx_bulk_active = ft_table_extra_maintain_idx_bulk_gen;
#if defined(__x86_64__)
    __builtin_cpu_init();
    if ((arch_enable & FT_ARCH_AVX512) &&
        __builtin_cpu_supports("avx512f")) {
        ft_maintain_extra_active = ft_table_extra_maintain_avx512;
        ft_maintain_extra_idx_bulk_active =
            ft_table_extra_maintain_idx_bulk_avx512;
    } else if ((arch_enable & (FT_ARCH_AVX2 | FT_ARCH_AVX512)) &&
               __builtin_cpu_supports("avx2")) {
        ft_maintain_extra_active = ft_table_extra_maintain_avx2;
        ft_maintain_extra_idx_bulk_active =
            ft_table_extra_maintain_idx_bulk_avx2;
    } else if ((arch_enable & (FT_ARCH_SSE | FT_ARCH_AVX2 |
                                FT_ARCH_AVX512)) &&
               __builtin_cpu_supports("sse4.2")) {
        ft_maintain_extra_active = ft_table_extra_maintain_sse;
        ft_maintain_extra_idx_bulk_active =
            ft_table_extra_maintain_idx_bulk_sse;
    }
#else
    (void)arch_enable;
#endif
}

void
ft_arch_extra_init(unsigned arch_enable)
{
    rix_hash_arch_init(ft_rix_hash_arch_enable_(arch_enable));
    FT_OPS_EXTRA_SELECT(flow4, arch_enable, &ft_flow4_extra_active);
    ft_maint_extra_select_(arch_enable);
}

/*===========================================================================
 * Bucket carve helper (pow2-aligned subregion for rix_hash_bucket_extra_s)
 *===========================================================================*/
static struct rix_hash_bucket_extra_s *
ft_table_extra_bucket_carve(void *raw, size_t raw_size, unsigned *nb_bk_out)
{
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned =
        (addr + (_Alignof(struct rix_hash_bucket_extra_s) - 1u))
        & ~(uintptr_t)(_Alignof(struct rix_hash_bucket_extra_s) - 1u);
    size_t lost = (size_t)(aligned - addr);
    size_t usable = raw_size > lost ? raw_size - lost : 0u;
    unsigned nb = (unsigned)(usable /
                             sizeof(struct rix_hash_bucket_extra_s));

    nb = ft_rounddown_pow2_u32(nb);
    *nb_bk_out = nb;
    return (struct rix_hash_bucket_extra_s *)aligned;
}

/*===========================================================================
 * Top-level init
 *===========================================================================*/
int
ft_table_extra_init(struct ft_table_extra *ft,
                    enum ft_table_variant variant,
                    void *array,
                    unsigned max_entries,
                    size_t stride,
                    size_t entry_offset,
                    void *buckets_raw,
                    size_t bucket_size,
                    const struct ft_table_extra_config *cfg)
{
    struct ft_table_extra_config defcfg;
    struct rix_hash_bucket_extra_s *buckets;
    unsigned nb_bk;

    if (ft == NULL || array == NULL || max_entries == 0u)
        return -1;
    if (buckets_raw == NULL || bucket_size == 0u)
        return -1;
    if (variant != FT_TABLE_VARIANT_FLOW4)
        return -1;

    buckets = ft_table_extra_bucket_carve(buckets_raw, bucket_size, &nb_bk);
    if (nb_bk < FT_TABLE_MIN_NB_BK)
        return -1;

    memset(&defcfg, 0, sizeof(defcfg));
    if (cfg == NULL)
        cfg = &defcfg;

    RIX_ASSERT(stride >= sizeof(struct flow4_extra_entry));
    RIX_ASSERT(entry_offset + sizeof(struct flow4_extra_entry) <= stride);

    memset(ft, 0, sizeof(*ft));
    ft->variant = (u8)variant;
    ft->ts_shift = (u8)((cfg->ts_shift != 0u)
        ? flow_timestamp_shift_clamp(cfg->ts_shift)
        : FLOW_TIMESTAMP_DEFAULT_SHIFT);
    ft->max_entries = max_entries;
    ft->start_mask = nb_bk - 1u;
    ft->pool_base = (unsigned char *)array;
    ft->pool_stride = stride;
    ft->pool_entry_offset = entry_offset;

    memset(buckets, 0, (size_t)nb_bk * sizeof(*buckets));
    ft->buckets = buckets;
    ft->nb_bk = nb_bk;
    ft->ht_head.rhh_mask = nb_bk - 1u;
    ft->ht_head.rhh_nb = 0u;
    return 0;
}

/*===========================================================================
 * Cold-path top-level (routed through _gen ops)
 *===========================================================================*/
void
ft_table_extra_destroy(struct ft_table_extra *ft)
{
    if (ft == NULL)
        return;
    ft_flow4_extra_ops_gen.destroy(ft);
}

void
ft_table_extra_flush(struct ft_table_extra *ft)
{
    if (ft == NULL)
        return;
    ft_flow4_extra_ops_gen.flush(ft);
}

unsigned
ft_table_extra_nb_entries(const struct ft_table_extra *ft)
{
    if (ft == NULL)
        return 0u;
    return ft_flow4_extra_ops_gen.nb_entries(ft);
}

unsigned
ft_table_extra_nb_bk(const struct ft_table_extra *ft)
{
    if (ft == NULL)
        return 0u;
    return ft_flow4_extra_ops_gen.nb_bk(ft);
}

void
ft_table_extra_stats_get(const struct ft_table_extra *ft,
                         struct ft_table_extra_stats *out)
{
    if (ft == NULL) {
        if (out != NULL)
            memset(out, 0, sizeof(*out));
        return;
    }
    ft_flow4_extra_ops_gen.stats(ft, out);
}

void
ft_table_extra_status_get(const struct ft_table_extra *ft,
                          struct flow_status *out)
{
    if (ft == NULL) {
        if (out != NULL)
            memset(out, 0, sizeof(*out));
        return;
    }
    ft_flow4_extra_ops_gen.status(ft, out);
}

int
ft_table_extra_walk(struct ft_table_extra *ft,
                    int (*cb)(u32 entry_idx, void *arg), void *arg)
{
    if (ft == NULL)
        return -1;
    return ft_flow4_extra_ops_gen.walk(ft, cb, arg);
}

int
ft_table_extra_migrate(struct ft_table_extra *ft,
                       void *new_buckets, size_t new_bucket_size)
{
    if (ft == NULL)
        return -1;
    return ft_flow4_extra_ops_gen.migrate(ft, new_buckets, new_bucket_size);
}

/*===========================================================================
 * Hot-path top-level (routed through active ops)
 *===========================================================================*/
u32
ft_table_extra_add_idx(struct ft_table_extra *ft, u32 entry_idx, u64 now)
{
    u32 unused;

    if (ft == NULL)
        return 0u;
    ft_flow4_extra_active->add_idx_bulk(ft, &entry_idx, 1u, FT_ADD_IGNORE,
                                        now, &unused);
    return entry_idx;
}

unsigned
ft_table_extra_add_idx_bulk(struct ft_table_extra *ft,
                            u32 *entry_idxv, unsigned nb_keys,
                            enum ft_add_policy policy, u64 now,
                            u32 *unused_idxv)
{
    if (ft == NULL)
        return 0u;
    return ft_flow4_extra_active->add_idx_bulk(ft, entry_idxv, nb_keys,
                                               policy, now, unused_idxv);
}

unsigned
ft_table_extra_add_idx_bulk_maint(struct ft_table_extra *ft,
                                  u32 *entry_idxv, unsigned nb_keys,
                                  enum ft_add_policy policy, u64 now,
                                  u64 timeout, u32 *unused_idxv,
                                  unsigned max_unused, unsigned min_bk_used)
{
    if (ft == NULL)
        return 0u;
    return ft_flow4_extra_active->add_idx_bulk_maint(
        ft, entry_idxv, nb_keys, policy, now, timeout,
        unused_idxv, max_unused, min_bk_used);
}

u32
ft_table_extra_del_idx(struct ft_table_extra *ft, u32 entry_idx)
{
    u32 unused;

    if (ft == NULL)
        return 0u;
    return ft_flow4_extra_active->del_idx_bulk(ft, &entry_idx, 1u, &unused)
        ? entry_idx : 0u;
}

unsigned
ft_table_extra_del_idx_bulk(struct ft_table_extra *ft,
                            const u32 *entry_idxv, unsigned nb_keys,
                            u32 *unused_idxv)
{
    if (ft == NULL)
        return 0u;
    return ft_flow4_extra_active->del_idx_bulk(ft, entry_idxv, nb_keys,
                                               unused_idxv);
}

/*===========================================================================
 * Touch: rewrite bk->extra[slot] for an existing entry
 *===========================================================================*/
void
ft_table_extra_touch(struct ft_table_extra *ft, u32 entry_idx, u64 now)
{
    struct flow_entry_meta_extra *meta;
    struct rix_hash_bucket_extra_s *bk;
    unsigned char *record;

    if (ft == NULL || entry_idx == 0u || entry_idx > ft->max_entries)
        return;

    record = ft->pool_base + (size_t)(entry_idx - 1u) * ft->pool_stride
        + ft->pool_entry_offset;
    meta = &((struct flow4_extra_entry *)(void *)record)->meta;
    bk = &ft->buckets[meta->cur_hash & ft->start_mask];
    flow_extra_ts_set(bk, meta->slot,
                      flow_timestamp_encode(now, ft->ts_shift));
}

/*===========================================================================
 * Maintain (arch-dispatched)
 *===========================================================================*/
unsigned
ft_table_extra_maintain(const struct ft_maint_extra_ctx *ctx,
                        unsigned start_bk,
                        u64 now, u64 expire_tsc,
                        u32 *expired_idxv, unsigned max_expired,
                        unsigned min_bk_entries, unsigned *next_bk)
{
    return ft_maintain_extra_active(ctx, start_bk, now, expire_tsc,
                                    expired_idxv, max_expired,
                                    min_bk_entries, next_bk);
}

unsigned
ft_table_extra_maintain_idx_bulk(const struct ft_maint_extra_ctx *ctx,
                                 const u32 *entry_idxv, unsigned nb_idx,
                                 u64 now, u64 expire_tsc,
                                 u32 *expired_idxv, unsigned max_expired,
                                 unsigned min_bk_entries, int enable_filter)
{
    return ft_maintain_extra_idx_bulk_active(ctx, entry_idxv, nb_idx,
                                             now, expire_tsc,
                                             expired_idxv, max_expired,
                                             min_bk_entries, enable_filter);
}

/*===========================================================================
 * flow4_extra_table_* per-prefix wrappers (key-addressable API)
 *===========================================================================*/
u32
flow4_extra_table_add(struct ft_table_extra *ft,
                      struct flow4_extra_entry *entry, u64 now)
{
    u32 idx;

    if (ft == NULL || entry == NULL)
        return 0u;
    idx = ft_record_index_from_member_ptr(ft->pool_base, ft->pool_stride,
                                          ft->pool_entry_offset, entry);
    if (idx == RIX_NIL)
        return 0u;
    return ft_table_extra_add_idx(ft, idx, now);
}

u32
flow4_extra_table_find(const struct ft_table_extra *ft,
                       const struct flow4_extra_key *key)
{
    struct ft_table_result result = { .entry_idx = 0u };

    if (ft == NULL || key == NULL)
        return 0u;
    ft_flow4_extra_active->find_bulk((struct ft_table_extra *)ft,
                                     key, 1u, 0u, &result);
    return result.entry_idx;
}

u32
flow4_extra_table_del(struct ft_table_extra *ft,
                      const struct flow4_extra_key *key)
{
    u32 unused = 0u;

    if (ft == NULL || key == NULL)
        return 0u;
    return ft_flow4_extra_active->del_key_bulk(ft, key, 1u, &unused)
        ? unused : 0u;
}

unsigned
flow4_extra_table_find_bulk(struct ft_table_extra *ft,
                            const struct flow4_extra_key *keys,
                            unsigned nb_keys, u64 now, u32 *results)
{
    struct ft_table_result r[64];
    unsigned i, produced, chunk;

    if (ft == NULL || keys == NULL || results == NULL || nb_keys == 0u)
        return 0u;
    produced = 0u;
    while (produced < nb_keys) {
        chunk = nb_keys - produced;
        if (chunk > 64u)
            chunk = 64u;
        ft_flow4_extra_active->find_bulk(ft, keys + produced, chunk, now, r);
        for (i = 0u; i < chunk; i++)
            results[produced + i] = r[i].entry_idx;
        produced += chunk;
    }
    return nb_keys;
}

unsigned
flow4_extra_table_del_bulk(struct ft_table_extra *ft,
                           const struct flow4_extra_key *keys,
                           unsigned nb_keys, u32 *idx_out)
{
    if (ft == NULL || keys == NULL || idx_out == NULL)
        return 0u;
    return ft_flow4_extra_active->del_key_bulk(ft, keys, nb_keys, idx_out);
}

size_t
flow4_extra_table_bucket_size(unsigned max_entries)
{
    return ft_table_extra_bucket_mem_size(max_entries);
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
