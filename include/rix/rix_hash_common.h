/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_common.h - shared types, helpers, and prefetch for rix_hash variants.
 */

#ifndef _RIX_HASH_COMMON_H_
#  define _RIX_HASH_COMMON_H_
#  include "rix_defs_private.h"

#  include "rix_hash_arch.h"

#  ifndef RIX_HASH_FIND_U32X16
#    define RIX_HASH_FIND_U32X16(arr, val) \
        rix_hash_arch->find_u32x16((arr), (val))
#  endif

#  ifndef RIX_HASH_FIND_U32X16_2
#    define RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
        rix_hash_arch->find_u32x16_2((arr), (val0), (val1), (mask0), (mask1))
#  endif


/*
 * Map a generated hash-table prefix and an operation suffix to the actual
 * generated helper symbol.  This only performs token pasting; availability of
 * a given op depends on the selected RIX_HASH_GENERATE_* variant.
 */
#  ifndef RIX_HASH_FUNC
#    define RIX_HASH_FUNC(name, op) name##_##op
#  endif

/*===========================================================================
 * Fingerprint-variant bucket layout
 * 128 bytes = 2 x 64-byte cache lines, aligned to 64 bytes.
 *===========================================================================*/
struct rix_hash_bucket_s {
    u32 hash[RIX_HASH_BUCKET_ENTRY_SZ]; /* 64 bytes: fingerprints       */
    u32 idx [RIX_HASH_BUCKET_ENTRY_SZ]; /* 64 bytes: 1-origin node idx  */
} __attribute__((aligned(64)));

/*===========================================================================
 * Head struct and init macros
 *===========================================================================*/
#  define RIX_HASH_HEAD(name)                                                 \
    struct name {                                                             \
        unsigned rhh_mask;                                                    \
        unsigned rhh_nb;                                                      \
    }

/* RIX_HASH_INIT: see convenience macro API below */

/*===========================================================================
 * Find context (staged pipeline)
 *===========================================================================*/
struct rix_hash_find_ctx_s {
    union rix_hash_hash_u     hash;       /*  8B  offset  0 */
    struct rix_hash_bucket_s *bk[2];      /* 16B  offset  8 */
    const void               *key;        /*  8B  offset 24 */
    u32                  fp;              /*  4B  offset 32 */
    u32                  fp_hits[2];      /*  8B  offset 36 */
    u32                  empties[2];      /*  8B  offset 44 */
    /* 52B + 4B tail padding = 56B (8B aligned) */
};

/*---------------------------------------------------------------------------
 * rix_hash_prefetch_key - prefetch key data into L1/L2 before name_hash_key.
 *
 * hash_fn(key) loads key data from DRAM.  Issue this for all N keys in a
 * tight loop BEFORE the hash_key stage to hide that DRAM latency:
 *
 *   for (i = 0; i < N; i++) rix_hash_prefetch_key(keys[i]);  // Stage 0
 *   for (i = 0; i < N; i++) name_hash_key(..., keys[i]);      // Stage 1
 *   for (i = 0; i < N; i++) name_scan_bk(...);                // Stage 2
 *   for (i = 0; i < N; i++) name_cmp_key(...);                // Stage 3
 *
 * Since key_field is typically at offset 0 of the node struct, this prefetch
 * also covers the cmp_key stage's full-key comparison.
 *---------------------------------------------------------------------------*/
static RIX_FORCE_INLINE void
rix_hash_prefetch_key(const void *key)
{
    __builtin_prefetch(key, 0, 0);
}

/* Prefetch a node/entry before key comparison. */
static RIX_FORCE_INLINE void
rix_hash_prefetch_entry_of(const void *entry)
{
    __builtin_prefetch(entry, 0, 1);
}

/*
 * Prefetch an indexed entry after converting a 1-origin index to an entry
 * pointer. base must be a typed pointer to the entry array.
 */
#  ifndef rix_hash_prefetch_entry_of_idx
#    define rix_hash_prefetch_entry_of_idx(base, idx)                         \
        do {                                                                  \
            unsigned __rix_idx_tmp = (unsigned)(idx);                         \
            if (__rix_idx_tmp != RIX_NIL)                                     \
                rix_hash_prefetch_entry_of(RIX_PTR_FROM_IDX((base),           \
                                                             __rix_idx_tmp)); \
        } while (0)
#  endif

/* Return a pointer to buckets[bk_idx]. */
#  ifndef rix_hash_bucket_of_idx
#    define rix_hash_bucket_of_idx(buckets, bk_idx) \
        (&(buckets)[(unsigned)(bk_idx)])
#  endif

/* Return buckets[bk_idx].hash[]. */
#  ifndef rix_hash_bucket_hashes_of_idx
#    define rix_hash_bucket_hashes_of_idx(buckets, bk_idx) \
        (rix_hash_bucket_of_idx((buckets), (bk_idx))->hash)
#  endif

/* Return buckets[bk_idx].idx[]. */
#  ifndef rix_hash_bucket_indices_of_idx
#    define rix_hash_bucket_indices_of_idx(buckets, bk_idx) \
        (rix_hash_bucket_of_idx((buckets), (bk_idx))->idx)
#  endif

/* Prefetch only the fingerprint/hash line of a bucket pointer. */
static RIX_FORCE_INLINE void
rix_hash_prefetch_bucket_hashes_of(const struct rix_hash_bucket_s *bucket)
{
    __builtin_prefetch(&bucket->hash[0], 0, 1);
}

/* Prefetch only the idx[] line of a bucket pointer. */
static RIX_FORCE_INLINE void
rix_hash_prefetch_bucket_indices_of(const struct rix_hash_bucket_s *bucket)
{
    __builtin_prefetch(&bucket->idx[0], 0, 1);
}

/* Prefetch the hash[] line of buckets[bk_idx]. */
#  ifndef rix_hash_prefetch_bucket_hashes_of_idx
#    define rix_hash_prefetch_bucket_hashes_of_idx(buckets, bk_idx)           \
        rix_hash_prefetch_bucket_hashes_of(&(buckets)[(unsigned)(bk_idx)])
#  endif

/* Prefetch the idx[] line of buckets[bk_idx]. */
#  ifndef rix_hash_prefetch_bucket_indices_of_idx
#    define rix_hash_prefetch_bucket_indices_of_idx(buckets, bk_idx)          \
        rix_hash_prefetch_bucket_indices_of(&(buckets)[(unsigned)(bk_idx)])
#  endif

/* Prefetch both cache lines of a bucket for ordinary read-mostly access. */
static RIX_FORCE_INLINE void
rix_hash_prefetch_bucket_of(const struct rix_hash_bucket_s *bucket)
{
    rix_hash_prefetch_bucket_hashes_of(bucket);
    rix_hash_prefetch_bucket_indices_of(bucket);
}

/*===========================================================================
 * Internal bucket-index arithmetic helpers
 *===========================================================================*/

/*
 * Derive the two candidate bucket indices and the fingerprint from a hash.
 *
 * The hash function guarantees (val32[0] & mask) != (val32[1] & mask), so
 * no collision handling is needed here.
 *
 *   bk0 = val32[0] & mask          (primary bucket)
 *   bk1 = val32[1] & mask          (secondary bucket, guaranteed != bk0)
 *   fp  = val32[0] ^ val32[1]      (fingerprint: XOR of both hashes)
 *
 * fp != 0 is guaranteed: since (val32[0] & mask) != (val32[1] & mask), at
 * least one lower bit differs, so the XOR is non-zero.  This means fp never
 * matches the empty-slot sentinel (hash[s] = 0), eliminating the need for a
 * _nilm filter in fingerprint searches.
 *
 * Node invariant: node->hash_field always holds the hash that corresponds
 * to the bucket where the node currently resides:
 *   node->hash_field & mask == current_bucket_index
 * On insert into bk0: hash_field = val32[0].  Into bk1: hash_field = val32[1].
 * On kickout to alt bucket: hash_field = fp ^ hash_field_old (1 XOR, no rehash).
 * Alternate bucket: alt_bk = (fp ^ hash_field) & mask  (same XOR trick).
 * Remove: bk = hash_field & mask -> direct O(1) slot lookup, no hash call.
 */
static RIX_FORCE_INLINE u32
rix_hash_fp(const union rix_hash_hash_u h, unsigned mask,
            unsigned *bk0_out, unsigned *bk1_out)
{
    *bk0_out = h.val32[0] & mask;
    *bk1_out = h.val32[1] & mask;
    return h.val32[0] ^ h.val32[1];
}

/*===========================================================================
 * RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)
 * RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)
 * RIX_HASH_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)
 * RIX_HASH_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field,
 *                           cmp_fn, hash_fn)
 *
 *   name       - head struct tag AND generated-function prefix (must match
 *                the tag given to RIX_HASH_HEAD)
 *   type       - element struct type (used as "struct type"; no typedef)
 *   key_field  - field name inside struct type that holds the key.
 *                MUST be a struct type (e.g. struct flow_5tuple key;).
 *                Key size is arbitrary (no alignment constraint); the hash
 *                function handles 8->4->2->1-byte partial reads internally.
 *                Using a struct rather than a plain array (e.g. u8[N])
 *                gives type-safe API parameters via __typeof__:
 *                  name_find(..., const struct my_key *key)
 *                  name_hash_key(..., const struct my_key *key)
 *                Compile errors arise if the wrong key type is passed.
 *   hash_field - field name inside struct type (u32) that stores the
 *                current bucket's hash (updated on kickout to always track
 *                the bucket where the node currently resides).
 *                Must be placed before key_field in the struct (typically
 *                at offset 0, padded to align key_field).
 *                Written at insert time; used during kickout to reconstruct
 *                bk1 = (fp ^ cur_hash) & mask without re-hashing, and during
 *                remove to locate bk0 = cur_hash & mask without any hash call.
 *   cmp_fn     - key comparison function:
 *                  int cmp_fn(const KEY_TYPE *a, const KEY_TYPE *b);
 *                where KEY_TYPE is typeof(((struct type *)0)->key_field).
 *                Returns 0 if the two keys are equal, non-zero otherwise.
 *                A SIMD-optimized implementation (SSE4.1 / AVX2 / AVX-512)
 *                is recommended for hot paths.  A memcmp wrapper also works:
 *                  static int my_cmp(const struct my_key *a,
 *                                    const struct my_key *b) {
 *                      return memcmp(a, b, sizeof(*a));
 *                  }
 *
 * Default hashing is provided internally via rix_hash_bytes_fast()
 * (CRC32C on x86_64+SSE4.2, FNV-1a fallback elsewhere, plus fixed-size
 * fast paths where available).
 *
 * EX variants allow callers to provide:
 *   union rix_hash_hash_u hash_fn(const key_type *key, u32 mask);
 * This keeps the typed API while letting fixed-size or domain-specific hash
 * functions be selected explicitly at generate time.
 *
 * SLOT variants additionally require:
 *   slot_field - field name inside struct type that stores the slot index
 *                inside the current bucket.
 *
 *                Contract:
 *                  0 <= node->slot_field < RIX_HASH_BUCKET_ENTRY_SZ
 *                  buckets[current_bucket].idx[node->slot_field] == node_idx
 *
 *                slot_field may be any caller-defined integer type, as long
 *                as it can represent the range
 *                  [0, RIX_HASH_BUCKET_ENTRY_SZ - 1].
 *                rix_hash does not impose a specific integer type.
 *
 *                SLOT variants update both hash_field and slot_field on
 *                insert and kickout so remove can directly clear the slot
 *                without scanning idx[16].  Invariant violations are
 *                programmer errors and may trigger RIX_ASSERT.
 *
 * Generated functions:
 *
 *   Staged find (x1):
 *     void           name_hash_key      (ctx, head, buckets, const key_type *key)
 *     void           name_scan_bk       (ctx, head, buckets)
 *     void           name_prefetch_node (ctx, base)
 *     struct type   *name_cmp_key       (ctx, base)
 *
 *   Staged find (xN bulk; FORCE_INLINE + constant n -> compiler unrolls):
 *     void           name_hash_key_n    (ctx, n, head, buckets, const key_type * const *keys)
 *     void           name_scan_bk_n     (ctx, n, head, buckets)
 *     void           name_prefetch_node_n(ctx, n, base)
 *     void           name_cmp_key_n     (ctx, n, base, results)
 *
 *   Single-shot ops:
 *     struct type   *name_find  (head, buckets, base, const key_type *key)
 *     struct type   *name_insert(head, buckets, base, elm)
 *     struct type   *name_remove(head, buckets, base, elm)
 *     int            name_walk  (head, buckets, base, cb, arg)
 *
 *   (key_type = __typeof__(((struct type *)0)->key_field))
 *===========================================================================*/
/* Derive the key type from key_field for use in typed API parameters. */
#  define RIX_HASH_KEY_TYPE(type, key_field)     \
    __typeof__(((struct type *)0)->key_field)

#  define RIX_HASH_SLOT_TYPE(type, slot_field)   \
    __typeof__(((struct type *)0)->slot_field)

#  define RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, attr) \
    attr void name##_init(struct name *head, unsigned nb_bk);                  \
    attr struct type *name##_insert(struct name *head,                         \
                                    struct rix_hash_bucket_s *buckets,         \
                                    struct type *base,                         \
                                    struct type *elm);                         \
    attr unsigned name##_remove_at(struct name *head,                          \
                                   struct rix_hash_bucket_s *buckets,          \
                                   unsigned bk,                                \
                                   unsigned slot);                             \
    attr struct type *name##_remove(struct name *head,                         \
                                    struct rix_hash_bucket_s *buckets,         \
                                    struct type *base,                         \
                                    struct type *elm);                         \
    attr int name##_walk(struct name *head,                                    \
                         struct rix_hash_bucket_s *buckets,                    \
                         struct type *base,                                    \
                         int (*cb)(struct type *, void *),                     \
                         void *arg);

#  define RIX_HASH_PROTOTYPE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_DEFAULT_HASH_FN_NAME(name) name ## _default_hash

#  define RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)              \
    static RIX_UNUSED RIX_FORCE_INLINE union rix_hash_hash_u                   \
    RIX_HASH_DEFAULT_HASH_FN_NAME(name)(                                      \
        const RIX_HASH_KEY_TYPE(type, key_field) *key, u32 mask)              \
    {                                                                          \
        return rix_hash_bytes_fast((const void *)key,                     \
                                        sizeof(((struct type *)0)->key_field), \
                                        mask);                                 \
    }

/*===========================================================================
 * Sizing helper
 *
 * rix_hash_nb_bk_hint - recommended nb_bk for a target ~50% slot fill.
 *
 * Each bucket holds RIX_HASH_BUCKET_ENTRY_SZ (16) slots.
 * For 50% fill: total_slots = nb_bk * 16 = 2 * max_entries
 *               => nb_bk = ceil(max_entries / 8), rounded up to power of 2.
 * nb_bk must be a power of 2 (used as a bitmask inside the hash table).
 * Minimum returned value is 2.
 *===========================================================================*/
static RIX_FORCE_INLINE unsigned
rix_hash_nb_bk_hint(unsigned max_entries)
{
    unsigned n = (max_entries + 7u) / 8u;   /* ceil(max_entries / 8) */
    if (n < 2u)
        n = 2u;
    /* round up to next power of 2 */
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1u;
}

#endif /* _RIX_HASH_COMMON_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
