/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_slot_extra.h - slot variant with a third per-slot u32 (extra[]).
 *
 * Bucket layout (192B, 3 cache lines):
 *   hash [16]  (64B, cl0) - fingerprints
 *   idx  [16]  (64B, cl1) - 1-origin node idx
 *   extra[16]  (64B, cl2) - user-defined u32
 *
 * Opt-in only. Not included from rix_hash.h umbrella.
 */

#ifndef _RIX_HASH_SLOT_EXTRA_H_
#  define _RIX_HASH_SLOT_EXTRA_H_

#  include "rix_hash_common.h"

struct rix_hash_bucket_extra_s {
    u32 hash [RIX_HASH_BUCKET_ENTRY_SZ];
    u32 idx  [RIX_HASH_BUCKET_ENTRY_SZ];
    u32 extra[RIX_HASH_BUCKET_ENTRY_SZ];
} __attribute__((aligned(64)));

struct rix_hash_find_ctx_extra_s {
    union rix_hash_hash_u            hash;
    struct rix_hash_bucket_extra_s  *bk[2];
    const void                      *key;
    u32  fp;
    u32  fp_hits[2];
    u32  empties[2];
};

/* ---- accessor / prefetch helpers --------------------------------------- */

static RIX_FORCE_INLINE struct rix_hash_bucket_extra_s *
rix_hash_extra_bucket_of_idx(struct rix_hash_bucket_extra_s *buckets,
                             unsigned bk_idx)
{
    return &buckets[bk_idx];
}

static RIX_FORCE_INLINE u32 *
rix_hash_extra_bucket_hashes_of_idx(struct rix_hash_bucket_extra_s *buckets,
                                    unsigned bk_idx)
{
    return buckets[bk_idx].hash;
}

static RIX_FORCE_INLINE u32 *
rix_hash_extra_bucket_indices_of_idx(struct rix_hash_bucket_extra_s *buckets,
                                     unsigned bk_idx)
{
    return buckets[bk_idx].idx;
}

static RIX_FORCE_INLINE u32 *
rix_hash_extra_bucket_extras_of_idx(struct rix_hash_bucket_extra_s *buckets,
                                    unsigned bk_idx)
{
    return buckets[bk_idx].extra;
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_hashes_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->hash[0], 0, 1);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_indices_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->idx[0], 0, 1);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_extras_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    __builtin_prefetch(&bucket->extra[0], 0, 1);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    rix_hash_prefetch_extra_bucket_hashes_of(bucket);
    rix_hash_prefetch_extra_bucket_indices_of(bucket);
}

static RIX_FORCE_INLINE void
rix_hash_prefetch_extra_bucket_full_of(
    const struct rix_hash_bucket_extra_s *bucket)
{
    rix_hash_prefetch_extra_bucket_hashes_of(bucket);
    rix_hash_prefetch_extra_bucket_indices_of(bucket);
    rix_hash_prefetch_extra_bucket_extras_of(bucket);
}

#  define rix_hash_prefetch_extra_bucket_hashes_of_idx(buckets, bk_idx)       \
    rix_hash_prefetch_extra_bucket_hashes_of(&(buckets)[(unsigned)(bk_idx)])

#  define rix_hash_prefetch_extra_bucket_indices_of_idx(buckets, bk_idx)      \
    rix_hash_prefetch_extra_bucket_indices_of(&(buckets)[(unsigned)(bk_idx)])

#  define rix_hash_prefetch_extra_bucket_extras_of_idx(buckets, bk_idx)       \
    rix_hash_prefetch_extra_bucket_extras_of(&(buckets)[(unsigned)(bk_idx)])

/* PROTOTYPE / GENERATE macros are added in Task 2. */

#endif /* _RIX_HASH_SLOT_EXTRA_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
