/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <rix/rix_hash_arch.h>

#include "ft_ops.h"

extern int ft_flow4_table_init_ex_gen(struct ft_flow4_table *ft,
                                      void *array,
                                      unsigned max_entries,
                                      size_t stride,
                                      size_t entry_offset,
                                      const struct ft_flow4_config *cfg);
extern void ft_flow4_table_destroy_gen(struct ft_flow4_table *ft);
extern void ft_flow4_table_flush_gen(struct ft_flow4_table *ft);
extern unsigned ft_flow4_table_nb_entries_gen(const struct ft_flow4_table *ft);
extern unsigned ft_flow4_table_nb_bk_gen(const struct ft_flow4_table *ft);
extern unsigned ft_flow4_table_need_grow_gen(const struct ft_flow4_table *ft);
extern void ft_flow4_table_stats_gen(const struct ft_flow4_table *ft,
                                     struct ft_flow4_stats *out);
extern int ft_flow4_table_walk_gen(struct ft_flow4_table *ft,
                                   int (*cb)(uint32_t entry_idx, void *arg),
                                   void *arg);
extern int ft_flow4_table_grow_2x_gen(struct ft_flow4_table *ft);
extern int ft_flow4_table_reserve_gen(struct ft_flow4_table *ft,
                                      unsigned min_entries);

static const struct ft_flow4_ops *ft_flow4_active = &ft_flow4_ops_gen;

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

void
ft_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(ft_rix_hash_arch_enable_(arch_enable));
    FT_OPS_SELECT(flow4, arch_enable, &ft_flow4_active);
}

int
ft_flow4_table_init(struct ft_flow4_table *ft,
                    struct ft_flow4_entry *pool,
                    unsigned max_entries,
                    const struct ft_flow4_config *cfg)
{
    return ft_flow4_table_init_ex_gen(ft, pool, max_entries,
                                      sizeof(*pool), 0u, cfg);
}

int
ft_flow4_table_init_ex(struct ft_flow4_table *ft,
                       void *array,
                       unsigned max_entries,
                       size_t stride,
                       size_t entry_offset,
                       const struct ft_flow4_config *cfg)
{
    RIX_ASSERT(stride >= sizeof(struct ft_flow4_entry));
    RIX_ASSERT(entry_offset + sizeof(struct ft_flow4_entry) <= stride);
    RIX_ASSERT(FT_PTR_IS_ALIGNED(FT_BYTE_PTR_ADD(array, entry_offset),
                                 _Alignof(struct ft_flow4_entry)));
    return ft_flow4_table_init_ex_gen(ft, array, max_entries,
                                      stride, entry_offset, cfg);
}

void
ft_flow4_table_destroy(struct ft_flow4_table *ft)
{
    ft_flow4_table_destroy_gen(ft);
}

void
ft_flow4_table_flush(struct ft_flow4_table *ft)
{
    ft_flow4_table_flush_gen(ft);
}

unsigned
ft_flow4_table_nb_entries(const struct ft_flow4_table *ft)
{
    return ft_flow4_table_nb_entries_gen(ft);
}

unsigned
ft_flow4_table_nb_bk(const struct ft_flow4_table *ft)
{
    return ft_flow4_table_nb_bk_gen(ft);
}

unsigned
ft_flow4_table_need_grow(const struct ft_flow4_table *ft)
{
    return ft_flow4_table_need_grow_gen(ft);
}

void
ft_flow4_table_stats(const struct ft_flow4_table *ft, struct ft_flow4_stats *out)
{
    ft_flow4_table_stats_gen(ft, out);
}

int
ft_flow4_table_walk(struct ft_flow4_table *ft,
                    int (*cb)(uint32_t entry_idx, void *arg),
                    void *arg)
{
    return ft_flow4_table_walk_gen(ft, cb, arg);
}

int
ft_flow4_table_grow_2x(struct ft_flow4_table *ft)
{
    return ft_flow4_table_grow_2x_gen(ft);
}

int
ft_flow4_table_reserve(struct ft_flow4_table *ft, unsigned min_entries)
{
    return ft_flow4_table_reserve_gen(ft, min_entries);
}

uint32_t
ft_flow4_table_find(struct ft_flow4_table *ft, const struct ft_flow4_key *key)
{
    return ft_flow4_active->find(ft, key);
}

uint32_t
ft_flow4_table_add(struct ft_flow4_table *ft, const struct ft_flow4_key *key)
{
    return ft_flow4_active->add(ft, key);
}

uint32_t
ft_flow4_table_add_idx(struct ft_flow4_table *ft, uint32_t entry_idx)
{
    return ft_flow4_active->add_idx(ft, entry_idx);
}

uint32_t
ft_flow4_table_add_entry(struct ft_flow4_table *ft, struct ft_flow4_entry *entry)
{
    return ft_flow4_active->add_entry(ft, entry);
}

uint32_t
ft_flow4_table_del(struct ft_flow4_table *ft, const struct ft_flow4_key *key)
{
    return ft_flow4_active->del(ft, key);
}

void
ft_flow4_table_find_bulk(struct ft_flow4_table *ft,
                         const struct ft_flow4_key *keys,
                         unsigned nb_keys,
                         struct ft_flow4_result *results)
{
    ft_flow4_active->find_bulk(ft, keys, nb_keys, results);
}

void
ft_flow4_table_add_bulk(struct ft_flow4_table *ft,
                        const struct ft_flow4_key *keys,
                        unsigned nb_keys,
                        struct ft_flow4_result *results)
{
    ft_flow4_active->add_bulk(ft, keys, nb_keys, results);
}

void
ft_flow4_table_add_idx_bulk(struct ft_flow4_table *ft,
                            const uint32_t *entry_idxv,
                            unsigned nb_keys,
                            struct ft_flow4_result *results)
{
    ft_flow4_active->add_idx_bulk(ft, entry_idxv, nb_keys, results);
}

void
ft_flow4_table_add_entry_bulk(struct ft_flow4_table *ft,
                              struct ft_flow4_entry *const *entries,
                              unsigned nb_keys,
                              struct ft_flow4_result *results)
{
    ft_flow4_active->add_entry_bulk(ft, entries, nb_keys, results);
}

void
ft_flow4_table_del_bulk(struct ft_flow4_table *ft,
                        const struct ft_flow4_key *keys,
                        unsigned nb_keys,
                        struct ft_flow4_result *results)
{
    ft_flow4_active->del_bulk(ft, keys, nb_keys, results);
}
