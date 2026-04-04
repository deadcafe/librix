/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * fc_dispatch.c - Runtime arch dispatch for fcache variants.
 *
 * Provides the unsuffixed public API that forwards hot-path calls
 * through the selected ops table, and cold-path calls to the _gen
 * (generic) implementation.
 *
 * Call fc_arch_init(FC_ARCH_AUTO) once at startup before any cache
 * operations.  Without init, the generic implementation is used.
 */

#include <stdint.h>

#include <rix/rix_hash_arch.h>
#include "fc_ops.h"

extern void fc_flow4_cache_findadd_burst32_gen(struct fc_flow4_cache *fc,
                                               const struct flow4_key *keys,
                                               unsigned nb_keys, u64 now,
                                               struct fc_flow4_result *results);
extern void fc_flow4_cache_init_ex_gen(struct fc_flow4_cache *fc,
                                       struct rix_hash_bucket_s *buckets,
                                       unsigned nb_bk,
                                       void *array,
                                       unsigned max_entries,
                                       size_t stride,
                                       size_t entry_offset,
                                       const struct fc_flow4_config *cfg);
extern void fc_flow6_cache_init_ex_gen(struct fc_flow6_cache *fc,
                                       struct rix_hash_bucket_s *buckets,
                                       unsigned nb_bk,
                                       void *array,
                                       unsigned max_entries,
                                       size_t stride,
                                       size_t entry_offset,
                                       const struct fc_flow6_config *cfg);
extern void fc_flowu_cache_init_ex_gen(struct fc_flowu_cache *fc,
                                       struct rix_hash_bucket_s *buckets,
                                       unsigned nb_bk,
                                       void *array,
                                       unsigned max_entries,
                                       size_t stride,
                                       size_t entry_offset,
                                       const struct fc_flowu_config *cfg);
extern void fc_flow4_cache_findadd_burst32_sse(struct fc_flow4_cache *fc,
                                               const struct flow4_key *keys,
                                               unsigned nb_keys, u64 now,
                                               struct fc_flow4_result *results);
extern void fc_flow4_cache_findadd_burst32_avx2(struct fc_flow4_cache *fc,
                                                const struct flow4_key *keys,
                                                unsigned nb_keys, u64 now,
                                                struct fc_flow4_result *results);
extern void fc_flow4_cache_findadd_burst32_avx512(struct fc_flow4_cache *fc,
                                                  const struct flow4_key *keys,
                                                  unsigned nb_keys, u64 now,
                                                  struct fc_flow4_result *results);

extern void fc_flow6_cache_findadd_burst32_gen(struct fc_flow6_cache *fc,
                                               const struct flow6_key *keys,
                                               unsigned nb_keys, u64 now,
                                               struct fc_flow6_result *results);
extern void fc_flow6_cache_findadd_burst32_sse(struct fc_flow6_cache *fc,
                                               const struct flow6_key *keys,
                                               unsigned nb_keys, u64 now,
                                               struct fc_flow6_result *results);
extern void fc_flow6_cache_findadd_burst32_avx2(struct fc_flow6_cache *fc,
                                                const struct flow6_key *keys,
                                                unsigned nb_keys, u64 now,
                                                struct fc_flow6_result *results);
extern void fc_flow6_cache_findadd_burst32_avx512(struct fc_flow6_cache *fc,
                                                  const struct flow6_key *keys,
                                                  unsigned nb_keys, u64 now,
                                                  struct fc_flow6_result *results);

extern void fc_flowu_cache_findadd_burst32_gen(struct fc_flowu_cache *fc,
                                               const struct flowu_key *keys,
                                               unsigned nb_keys, u64 now,
                                               struct fc_flowu_result *results);
extern void fc_flowu_cache_findadd_burst32_sse(struct fc_flowu_cache *fc,
                                               const struct flowu_key *keys,
                                               unsigned nb_keys, u64 now,
                                               struct fc_flowu_result *results);
extern void fc_flowu_cache_findadd_burst32_avx2(struct fc_flowu_cache *fc,
                                                const struct flowu_key *keys,
                                                unsigned nb_keys, u64 now,
                                                struct fc_flowu_result *results);
extern void fc_flowu_cache_findadd_burst32_avx512(struct fc_flowu_cache *fc,
                                                  const struct flowu_key *keys,
                                                  unsigned nb_keys, u64 now,
                                                  struct fc_flowu_result *results);

/*===========================================================================
 * Per-variant dispatch state
 *===========================================================================*/
static const struct fc_flow4_ops *_fc_flow4_active = &fc_flow4_ops_gen;
static const struct fc_flow6_ops *_fc_flow6_active = &fc_flow6_ops_gen;
static const struct fc_flowu_ops *_fc_flowu_active = &fc_flowu_ops_gen;

/*===========================================================================
 * fc_arch_init -- one-time CPU detection and ops selection
 *===========================================================================*/
void
fc_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    FC_OPS_SELECT(flow4, arch_enable, &_fc_flow4_active);
    FC_OPS_SELECT(flow6, arch_enable, &_fc_flow6_active);
    FC_OPS_SELECT(flowu, arch_enable, &_fc_flowu_active);
}

/*===========================================================================
 * Cold-path wrappers -- forward to _gen (not SIMD-sensitive)
 *===========================================================================*/

/* flow4 cold-path */
void
fc_flow4_cache_init(struct fc_flow4_cache *fc,
                    struct rix_hash_bucket_s *buckets,
                    unsigned nb_bk,
                    struct fc_flow4_entry *pool,
                    unsigned max_entries,
                    const struct fc_flow4_config *cfg)
{
    fc_flow4_cache_init_ex_gen(fc, buckets, nb_bk, pool, max_entries,
                               sizeof(*pool), 0u, cfg);
}

void
fc_flow4_cache_init_ex(struct fc_flow4_cache *fc,
                       struct rix_hash_bucket_s *buckets,
                       unsigned nb_bk,
                       void *array,
                       unsigned max_entries,
                       size_t stride,
                       size_t entry_offset,
                       const struct fc_flow4_config *cfg)
{
    RIX_ASSERT(stride >= sizeof(struct fc_flow4_entry));
    RIX_ASSERT(entry_offset + sizeof(struct fc_flow4_entry) <= stride);
    RIX_ASSERT(FC_PTR_IS_ALIGNED(FC_BYTE_PTR_ADD(array, entry_offset),
                                 _Alignof(struct fc_flow4_entry)));
    fc_flow4_cache_init_ex_gen(fc, buckets, nb_bk, array, max_entries,
                               stride, entry_offset, cfg);
}

void
fc_flow4_cache_set_event_cb(struct fc_flow4_cache *fc,
                            fc_flow4_event_cb cb,
                            void *arg)
{
    fc->event_cb = cb;
    fc->event_cb_arg = arg;
}

void
fc_flow4_cache_flush(struct fc_flow4_cache *fc)
{
    fc_flow4_ops_gen.flush(fc);
}

unsigned
fc_flow4_cache_nb_entries(const struct fc_flow4_cache *fc)
{
    return fc_flow4_ops_gen.nb_entries(fc);
}

void
fc_flow4_cache_stats(const struct fc_flow4_cache *fc,
                     struct fc_flow4_stats *out)
{
    fc_flow4_ops_gen.stats(fc, out);
}

int
fc_flow4_cache_walk(struct fc_flow4_cache *fc,
                     int (*cb)(u32 entry_idx, void *arg), void *arg)
{
    return fc_flow4_ops_gen.walk(fc, cb, arg);
}

/* flow6 cold-path */
void
fc_flow6_cache_init(struct fc_flow6_cache *fc,
                    struct rix_hash_bucket_s *buckets,
                    unsigned nb_bk,
                    struct fc_flow6_entry *pool,
                    unsigned max_entries,
                    const struct fc_flow6_config *cfg)
{
    fc_flow6_cache_init_ex_gen(fc, buckets, nb_bk, pool, max_entries,
                               sizeof(*pool), 0u, cfg);
}

void
fc_flow6_cache_init_ex(struct fc_flow6_cache *fc,
                       struct rix_hash_bucket_s *buckets,
                       unsigned nb_bk,
                       void *array,
                       unsigned max_entries,
                       size_t stride,
                       size_t entry_offset,
                       const struct fc_flow6_config *cfg)
{
    RIX_ASSERT(stride >= sizeof(struct fc_flow6_entry));
    RIX_ASSERT(entry_offset + sizeof(struct fc_flow6_entry) <= stride);
    RIX_ASSERT(FC_PTR_IS_ALIGNED(FC_BYTE_PTR_ADD(array, entry_offset),
                                 _Alignof(struct fc_flow6_entry)));
    fc_flow6_cache_init_ex_gen(fc, buckets, nb_bk, array, max_entries,
                               stride, entry_offset, cfg);
}

void
fc_flow6_cache_set_event_cb(struct fc_flow6_cache *fc,
                            fc_flow6_event_cb cb,
                            void *arg)
{
    fc->event_cb = cb;
    fc->event_cb_arg = arg;
}

void
fc_flow6_cache_flush(struct fc_flow6_cache *fc)
{
    fc_flow6_ops_gen.flush(fc);
}

unsigned
fc_flow6_cache_nb_entries(const struct fc_flow6_cache *fc)
{
    return fc_flow6_ops_gen.nb_entries(fc);
}

void
fc_flow6_cache_stats(const struct fc_flow6_cache *fc,
                     struct fc_flow6_stats *out)
{
    fc_flow6_ops_gen.stats(fc, out);
}

int
fc_flow6_cache_walk(struct fc_flow6_cache *fc,
                     int (*cb)(u32 entry_idx, void *arg), void *arg)
{
    return fc_flow6_ops_gen.walk(fc, cb, arg);
}

/* flowu cold-path */
void
fc_flowu_cache_init(struct fc_flowu_cache *fc,
                    struct rix_hash_bucket_s *buckets,
                    unsigned nb_bk,
                    struct fc_flowu_entry *pool,
                    unsigned max_entries,
                    const struct fc_flowu_config *cfg)
{
    fc_flowu_cache_init_ex_gen(fc, buckets, nb_bk, pool, max_entries,
                               sizeof(*pool), 0u, cfg);
}

void
fc_flowu_cache_init_ex(struct fc_flowu_cache *fc,
                       struct rix_hash_bucket_s *buckets,
                       unsigned nb_bk,
                       void *array,
                       unsigned max_entries,
                       size_t stride,
                       size_t entry_offset,
                       const struct fc_flowu_config *cfg)
{
    RIX_ASSERT(stride >= sizeof(struct fc_flowu_entry));
    RIX_ASSERT(entry_offset + sizeof(struct fc_flowu_entry) <= stride);
    RIX_ASSERT(FC_PTR_IS_ALIGNED(FC_BYTE_PTR_ADD(array, entry_offset),
                                 _Alignof(struct fc_flowu_entry)));
    fc_flowu_cache_init_ex_gen(fc, buckets, nb_bk, array, max_entries,
                               stride, entry_offset, cfg);
}

void
fc_flowu_cache_set_event_cb(struct fc_flowu_cache *fc,
                            fc_flowu_event_cb cb,
                            void *arg)
{
    fc->event_cb = cb;
    fc->event_cb_arg = arg;
}

void
fc_flowu_cache_flush(struct fc_flowu_cache *fc)
{
    fc_flowu_ops_gen.flush(fc);
}

unsigned
fc_flowu_cache_nb_entries(const struct fc_flowu_cache *fc)
{
    return fc_flowu_ops_gen.nb_entries(fc);
}

void
fc_flowu_cache_stats(const struct fc_flowu_cache *fc,
                     struct fc_flowu_stats *out)
{
    fc_flowu_ops_gen.stats(fc, out);
}

int
fc_flowu_cache_walk(struct fc_flowu_cache *fc,
                     int (*cb)(u32 entry_idx, void *arg), void *arg)
{
    return fc_flowu_ops_gen.walk(fc, cb, arg);
}

/*===========================================================================
 * Hot-path bulk wrappers -- dispatch through selected ops table
 *===========================================================================*/

/*--- flow4 bulk ---*/
void
fc_flow4_cache_find_bulk(struct fc_flow4_cache *fc,
                          const struct flow4_key *keys,
                          unsigned nb_keys, u64 now,
                          struct fc_flow4_result *results)
{
    _fc_flow4_active->find_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flow4_cache_findadd_bulk(struct fc_flow4_cache *fc,
                             const struct flow4_key *keys,
                             unsigned nb_keys, u64 now,
                             struct fc_flow4_result *results)
{
    _fc_flow4_active->findadd_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flow4_cache_findadd_burst32(struct fc_flow4_cache *fc,
                                const struct flow4_key *keys,
                                unsigned nb_keys, u64 now,
                                struct fc_flow4_result *results)
{
    if (nb_keys > 32u) {
        _fc_flow4_active->findadd_bulk(fc, keys, nb_keys, now, results);
        return;
    }

    if (_fc_flow4_active == &fc_flow4_ops_avx512) {
        fc_flow4_cache_findadd_burst32_avx512(fc, keys, nb_keys, now, results);
    } else if (_fc_flow4_active == &fc_flow4_ops_avx2) {
        fc_flow4_cache_findadd_burst32_avx2(fc, keys, nb_keys, now, results);
    } else if (_fc_flow4_active == &fc_flow4_ops_sse) {
        fc_flow4_cache_findadd_burst32_sse(fc, keys, nb_keys, now, results);
    } else {
        fc_flow4_cache_findadd_burst32_gen(fc, keys, nb_keys, now, results);
    }
}

void
fc_flow4_cache_add_bulk(struct fc_flow4_cache *fc,
                         const struct flow4_key *keys,
                         unsigned nb_keys, u64 now,
                         struct fc_flow4_result *results)
{
    _fc_flow4_active->add_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flow4_cache_del_bulk(struct fc_flow4_cache *fc,
                         const struct flow4_key *keys,
                         unsigned nb_keys)
{
    _fc_flow4_active->del_bulk(fc, keys, nb_keys);
}

void
fc_flow4_cache_del_idx_bulk(struct fc_flow4_cache *fc,
                             const u32 *idxs, unsigned nb_idxs)
{
    _fc_flow4_active->del_idx_bulk(fc, idxs, nb_idxs);
}

unsigned
fc_flow4_cache_maintain(struct fc_flow4_cache *fc,
                        unsigned start_bk, unsigned bucket_count,
                        u64 now)
{
    return _fc_flow4_active->maintain(fc, start_bk, bucket_count, now);
}

unsigned
fc_flow4_cache_maintain_step_ex(struct fc_flow4_cache *fc,
                                unsigned start_bk, unsigned bucket_count,
                                unsigned skip_threshold, u64 now)
{
    return _fc_flow4_active->maintain_step_ex(fc, start_bk, bucket_count,
                                               skip_threshold, now);
}

unsigned
fc_flow4_cache_maintain_step(struct fc_flow4_cache *fc,
                             u64 now, int idle)
{
    return _fc_flow4_active->maintain_step(fc, now, idle);
}

/*--- flow6 bulk ---*/
void
fc_flow6_cache_find_bulk(struct fc_flow6_cache *fc,
                          const struct flow6_key *keys,
                          unsigned nb_keys, u64 now,
                          struct fc_flow6_result *results)
{
    _fc_flow6_active->find_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flow6_cache_findadd_bulk(struct fc_flow6_cache *fc,
                             const struct flow6_key *keys,
                             unsigned nb_keys, u64 now,
                             struct fc_flow6_result *results)
{
    _fc_flow6_active->findadd_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flow6_cache_findadd_burst32(struct fc_flow6_cache *fc,
                                const struct flow6_key *keys,
                                unsigned nb_keys, u64 now,
                                struct fc_flow6_result *results)
{
    if (nb_keys > 32u) {
        _fc_flow6_active->findadd_bulk(fc, keys, nb_keys, now, results);
        return;
    }

    if (_fc_flow6_active == &fc_flow6_ops_avx512) {
        fc_flow6_cache_findadd_burst32_avx512(fc, keys, nb_keys, now, results);
    } else if (_fc_flow6_active == &fc_flow6_ops_avx2) {
        fc_flow6_cache_findadd_burst32_avx2(fc, keys, nb_keys, now, results);
    } else if (_fc_flow6_active == &fc_flow6_ops_sse) {
        fc_flow6_cache_findadd_burst32_sse(fc, keys, nb_keys, now, results);
    } else {
        fc_flow6_cache_findadd_burst32_gen(fc, keys, nb_keys, now, results);
    }
}

void
fc_flow6_cache_add_bulk(struct fc_flow6_cache *fc,
                         const struct flow6_key *keys,
                         unsigned nb_keys, u64 now,
                         struct fc_flow6_result *results)
{
    _fc_flow6_active->add_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flow6_cache_del_bulk(struct fc_flow6_cache *fc,
                         const struct flow6_key *keys,
                         unsigned nb_keys)
{
    _fc_flow6_active->del_bulk(fc, keys, nb_keys);
}

void
fc_flow6_cache_del_idx_bulk(struct fc_flow6_cache *fc,
                             const u32 *idxs, unsigned nb_idxs)
{
    _fc_flow6_active->del_idx_bulk(fc, idxs, nb_idxs);
}

unsigned
fc_flow6_cache_maintain(struct fc_flow6_cache *fc,
                        unsigned start_bk, unsigned bucket_count,
                        u64 now)
{
    return _fc_flow6_active->maintain(fc, start_bk, bucket_count, now);
}

unsigned
fc_flow6_cache_maintain_step_ex(struct fc_flow6_cache *fc,
                                unsigned start_bk, unsigned bucket_count,
                                unsigned skip_threshold, u64 now)
{
    return _fc_flow6_active->maintain_step_ex(fc, start_bk, bucket_count,
                                               skip_threshold, now);
}

unsigned
fc_flow6_cache_maintain_step(struct fc_flow6_cache *fc,
                             u64 now, int idle)
{
    return _fc_flow6_active->maintain_step(fc, now, idle);
}

/*--- flowu bulk ---*/
void
fc_flowu_cache_find_bulk(struct fc_flowu_cache *fc,
                          const struct flowu_key *keys,
                          unsigned nb_keys, u64 now,
                          struct fc_flowu_result *results)
{
    _fc_flowu_active->find_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flowu_cache_findadd_bulk(struct fc_flowu_cache *fc,
                             const struct flowu_key *keys,
                             unsigned nb_keys, u64 now,
                             struct fc_flowu_result *results)
{
    _fc_flowu_active->findadd_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flowu_cache_findadd_burst32(struct fc_flowu_cache *fc,
                                const struct flowu_key *keys,
                                unsigned nb_keys, u64 now,
                                struct fc_flowu_result *results)
{
    if (nb_keys > 32u) {
        _fc_flowu_active->findadd_bulk(fc, keys, nb_keys, now, results);
        return;
    }

    if (_fc_flowu_active == &fc_flowu_ops_avx512) {
        fc_flowu_cache_findadd_burst32_avx512(fc, keys, nb_keys, now, results);
    } else if (_fc_flowu_active == &fc_flowu_ops_avx2) {
        fc_flowu_cache_findadd_burst32_avx2(fc, keys, nb_keys, now, results);
    } else if (_fc_flowu_active == &fc_flowu_ops_sse) {
        fc_flowu_cache_findadd_burst32_sse(fc, keys, nb_keys, now, results);
    } else {
        fc_flowu_cache_findadd_burst32_gen(fc, keys, nb_keys, now, results);
    }
}

void
fc_flowu_cache_add_bulk(struct fc_flowu_cache *fc,
                         const struct flowu_key *keys,
                         unsigned nb_keys, u64 now,
                         struct fc_flowu_result *results)
{
    _fc_flowu_active->add_bulk(fc, keys, nb_keys, now, results);
}

void
fc_flowu_cache_del_bulk(struct fc_flowu_cache *fc,
                         const struct flowu_key *keys,
                         unsigned nb_keys)
{
    _fc_flowu_active->del_bulk(fc, keys, nb_keys);
}

void
fc_flowu_cache_del_idx_bulk(struct fc_flowu_cache *fc,
                             const u32 *idxs, unsigned nb_idxs)
{
    _fc_flowu_active->del_idx_bulk(fc, idxs, nb_idxs);
}

unsigned
fc_flowu_cache_maintain(struct fc_flowu_cache *fc,
                        unsigned start_bk, unsigned bucket_count,
                        u64 now)
{
    return _fc_flowu_active->maintain(fc, start_bk, bucket_count, now);
}

unsigned
fc_flowu_cache_maintain_step_ex(struct fc_flowu_cache *fc,
                                unsigned start_bk, unsigned bucket_count,
                                unsigned skip_threshold, u64 now)
{
    return _fc_flowu_active->maintain_step_ex(fc, start_bk, bucket_count,
                                               skip_threshold, now);
}

unsigned
fc_flowu_cache_maintain_step(struct fc_flowu_cache *fc,
                             u64 now, int idle)
{
    return _fc_flowu_active->maintain_step(fc, now, idle);
}

/*===========================================================================
 * Single-key convenience wrappers -- call bulk(n=1)
 *===========================================================================*/

/*--- flow4 single ---*/
u32
fc_flow4_cache_find(struct fc_flow4_cache *fc,
                     const struct flow4_key *key, u64 now)
{
    struct fc_flow4_result r;
    _fc_flow4_active->find_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

u32
fc_flow4_cache_findadd(struct fc_flow4_cache *fc,
                        const struct flow4_key *key, u64 now)
{
    struct fc_flow4_result r;
    _fc_flow4_active->findadd_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

u32
fc_flow4_cache_add(struct fc_flow4_cache *fc,
                    const struct flow4_key *key, u64 now)
{
    struct fc_flow4_result r;
    _fc_flow4_active->add_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

void
fc_flow4_cache_del(struct fc_flow4_cache *fc,
                    const struct flow4_key *key)
{
    _fc_flow4_active->del_bulk(fc, key, 1);
}

int
fc_flow4_cache_del_idx(struct fc_flow4_cache *fc, u32 entry_idx)
{
    return fc_flow4_ops_gen.remove_idx(fc, entry_idx);
}

/*--- flow6 single ---*/
u32
fc_flow6_cache_find(struct fc_flow6_cache *fc,
                     const struct flow6_key *key, u64 now)
{
    struct fc_flow6_result r;
    _fc_flow6_active->find_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

u32
fc_flow6_cache_findadd(struct fc_flow6_cache *fc,
                        const struct flow6_key *key, u64 now)
{
    struct fc_flow6_result r;
    _fc_flow6_active->findadd_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

u32
fc_flow6_cache_add(struct fc_flow6_cache *fc,
                    const struct flow6_key *key, u64 now)
{
    struct fc_flow6_result r;
    _fc_flow6_active->add_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

void
fc_flow6_cache_del(struct fc_flow6_cache *fc,
                    const struct flow6_key *key)
{
    _fc_flow6_active->del_bulk(fc, key, 1);
}

int
fc_flow6_cache_del_idx(struct fc_flow6_cache *fc, u32 entry_idx)
{
    return fc_flow6_ops_gen.remove_idx(fc, entry_idx);
}

/*--- flowu single ---*/
u32
fc_flowu_cache_find(struct fc_flowu_cache *fc,
                     const struct flowu_key *key, u64 now)
{
    struct fc_flowu_result r;
    _fc_flowu_active->find_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

u32
fc_flowu_cache_findadd(struct fc_flowu_cache *fc,
                        const struct flowu_key *key, u64 now)
{
    struct fc_flowu_result r;
    _fc_flowu_active->findadd_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

u32
fc_flowu_cache_add(struct fc_flowu_cache *fc,
                    const struct flowu_key *key, u64 now)
{
    struct fc_flowu_result r;
    _fc_flowu_active->add_bulk(fc, key, 1, now, &r);
    return r.entry_idx;
}

void
fc_flowu_cache_del(struct fc_flowu_cache *fc,
                    const struct flowu_key *key)
{
    _fc_flowu_active->del_bulk(fc, key, 1);
}

int
fc_flowu_cache_del_idx(struct fc_flowu_cache *fc, u32 entry_idx)
{
    return fc_flowu_ops_gen.remove_idx(fc, entry_idx);
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
