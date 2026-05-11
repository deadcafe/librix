/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_mrsw.h - multi-reader / single-writer cuckoo hash table.
 *
 * This is an independent hash-table variant.  It reuses the normal bucket
 * layouts through ctrl/reserved aliases: MRSW readers only use slots 0..14
 * and reserve physical slot 15 for a packed seq/valid control word that lets
 * readers reject observations that overlap a writer touching that bucket.
 *
 * Concurrency contract:
 *   - any number of reader threads may call find / staged find concurrently.
 *   - exactly one writer thread may call insert / remove / remove_at.
 *   - node keys are immutable while the node is published.
 *   - removed node storage must not be freed or reused for a different key
 *     until callers have provided an external reader grace period.
 *   - resize/grow is outside this primitive; perform it only after readers
 *     have quiesced, or publish a new table through a higher-level RCU scheme.
 *
 * Reader progress:
 *   readers do not take a lock, but they retry while a writer is active or
 *   when a writer overlapped the lookup.  A continuously active writer can
 *   therefore starve readers.
 *
 * Memory ordering:
 *   - Each bucket control word is one atomic u32:
 *       bits  0..16: seqcount generation (17 bits, wraps at 131072).
 *       bits 17..31: valid bitmap for usable slots 0..14 (15 bits).
 *   - Reader:
 *       ctrl0 = acquire load of bucket ctrl.
 *       build a hash-hit bitmap, AND it with ctrl.valid, then read idx/key
 *       candidates from the resulting valid-hit slots.
 *       If a key is found in a valid slot, return it immediately.  The hit
 *       path does not perform a final ctrl verify; this is a deliberate
 *       throughput tradeoff.  When the lookup overlaps insert/remove/kickout,
 *       it may return a slot that was visible in the ctrl snapshot used for
 *       the scan, or either copy of a transient duplicate created by
 *       publish-before-unpublish kickout ordering.
 *       If no key is found, acquire-load the searched bucket ctrl words again
 *       and retry if either changed.  This avoids false negatives when a
 *       writer published or moved the key while the reader was searching.
 *     A lookup must not span one full 17-bit seq wrap of either candidate
 *     bucket.  With the single-writer contract that means a reader must not
 *     overlap 131072 completed ctrl updates to the same candidate bucket.
 *   - Writer:
 *       insert/publish: write idx and hash first, then release-update ctrl by
 *         incrementing seq and setting the valid bit in one atomic u32 update.
 *       remove/unpublish: release-update ctrl by incrementing seq and clearing
 *         the valid bit in one atomic u32 update.  hash/idx may keep stale
 *         values; valid is the source of truth for slot visibility.
 *       kickout/move: publish the entry in the alternate bucket first, then
 *         unpublish the old bucket slot.  During this handoff the same entry
 *         can be visible in both buckets.  That temporary duplicate is
 *         intentional; the reverse order can create a false negative window.
 *     The release update on ctrl pairs with reader acquire loads.  A hit is
 *     allowed to complete from the ctrl snapshot that exposed the valid slot;
 *     a miss is accepted only after both searched ctrl words are verified
 *     unchanged.  This gives strong no-false-negative behavior for misses
 *     without putting the hit path through a second ctrl load.
 *   - ctrl is the only ordered atomic object in the bucket.  hash[]/idx[] are
 *     ordinary payload words so the hot hash scan can use the same SIMD
 *     find_u32x16 dispatch as the normal hash table.  Miss-side ctrl verify
 *     discards overlapped payload observations that could otherwise become a
 *     false negative.
 *
 * Bucket layout:
 *   MRSW keeps the classic 128 B / 2 cache-line bucket envelope by using
 *   slot 15 of the hash line for the packed seq/valid control word.  The
 *   spare u32 after idx[15] is reserved.
 *   Therefore each bucket has 15 usable entries.
 *
 * Hash scan vs. ctrl word at slot 15:
 *   The reader-side SIMD scan (find_u32x16(hash, fp)) intentionally
 *   covers all 16 words of the hash cache line, including the ctrl word at
 *   physical slot 15.  Two consequences are handled by design:
 *   (a) The seq half of ctrl can coincidentally equal fp.  Any bit-15 hit in
 *       the resulting bitmap is masked off because the ctrl_valid mask spans
 *       only bits 0..14 (slots 0..14).  Slot 15 therefore can never produce
 *       an observable match.
 *   (b) Concurrent CAS updates to ctrl are atomic at the 4-byte word
 *       granularity used by the SIMD load.  A vector load can only observe a
 *       value that ctrl held at some moment, never a torn intermediate, so
 *       the masked-out bit-15 result is always self-consistent with that
 *       atomic snapshot.
 *   This unifies the SIMD path with the non-MRSW fingerprint variant at zero
 *   extra cost (the SIMD compare is intrinsically 16-wide).
 */

#ifndef _RIX_HASH_MRSW_H_
#  define _RIX_HASH_MRSW_H_

#  include "rix_hash_common.h"
#  include "rix_hash_u64.h"        /* MRSW U64 reuses rix_hash64_bucket_s */
#  include "rix_hash_slot_extra.h" /* MRSW SLOT_EXTRA reuses bucket_extra */

#  include <stdatomic.h>

/*
 * MRSW reuses struct rix_hash_bucket_s defined in rix_hash_common.h.  The
 * unified bucket exposes ctrl/reserved through anonymous unions so MRSW can
 * access the control word atomically while pure FP/SLOT/keyonly variants
 * still see 16-slot hash[]/idx[] arrays.  MRSW restricts itself to slots
 * 0..14 (15 usable entries) so that hash[15]/idx[15] remain ctrl/reserved.
 */
#  define RIX_HASH_MRSW_BUCKET_ENTRY_SZ (RIX_HASH_BUCKET_ENTRY_SZ - 1u)

#  define RIX_HASH_MRSW_VALID_MASK                                           \
    ((UINT32_C(1) << RIX_HASH_MRSW_BUCKET_ENTRY_SZ) - UINT32_C(1))
#  define RIX_HASH_MRSW_CTRL_VALID_SHIFT 17u
#  define RIX_HASH_MRSW_CTRL_SEQ_MASK                                        \
    ((UINT32_C(1) << RIX_HASH_MRSW_CTRL_VALID_SHIFT) - UINT32_C(1))
#  define RIX_HASH_MRSW_CTRL_VALID_MASK                                      \
    (RIX_HASH_MRSW_VALID_MASK << RIX_HASH_MRSW_CTRL_VALID_SHIFT)

/* Recommended nb_bk for the Green/Yellow boundary of MRSW bucket fill.
 * MRSW buckets have 15 usable slots and should be planned about 5 percentage
 * points below pure 16-slot tables: Green <70%, Yellow 70..80%, Red >80%.
 * This helper sizes max_entries at <=70% MRSW slot fill before the final
 * power-of-two bucket rounding:
 * nb_bk = ceil(max_entries * 100 / (15 * 70)).
 */
static RIX_FORCE_INLINE unsigned
rix_hash_mrsw_nb_bk_hint(unsigned max_entries)
{
    unsigned n = (max_entries == 0u)
               ? 1u
               : (unsigned)(((uint64_t)max_entries * 100u + 1049u) / 1050u);
    if (n < 2u)
        n = 2u;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1u;
}

#  define RIX_HASH_MRSW_HEAD(name)                                           \
    struct name {                                                             \
        unsigned         rhh_mask;                                            \
        _Atomic unsigned rhh_nb;                                              \
    }

struct rix_hash_mrsw_find_ctx_s {
    union rix_hash_hash_u          hash;
    struct rix_hash_bucket_s *bk[2];
    struct rix_hash_bucket_s *buckets;
    const void                    *key;
    u32                            ctrl[2];
    unsigned                       hash_mask;
    unsigned                       bk_mask;
    u32                            fp;
    u32                            fp_hits[2];
};

/*
 * MRSW U32 / U64: u32/u64 keys are stored directly in bk->hash[] (which is
 * reinterpreted as keys[] for these variants).  The MRSW ctrl/reserved
 * fields at slot 15 are unchanged so the same unified bucket type and the
 * same valid/seq protocol cover all variants.
 */
struct rix_hash_mrsw_u32_find_ctx_s {
    struct rix_hash_bucket_s *bk[2];
    struct rix_hash_bucket_s *buckets;
    u32                       ctrl[2];
    u32                       hits[2];
    u32                       key;
    unsigned                  bk_mask;
};

/* MRSW U64 reuses struct rix_hash64_bucket_s defined in rix_hash_u64.h.
 * The find ctx and ctrl helpers are declared later (after the
 * rix_hash_mrsw_ctrl_* primitives are defined). */
struct rix_hash_mrsw_u64_find_ctx_s {
    struct rix_hash64_bucket_s *bk[2];
    struct rix_hash64_bucket_s *buckets;
    u32                         ctrl[2];
    u32                         hits[2];
    u64                         key;
    unsigned                    bk_mask;
};

/* MRSW SLOT_EXTRA reuses struct rix_hash_bucket_extra_s (192 B, 3 CL).
 * Slot 15 of hash[] / idx[] aliases ctrl / reserved (same as the other
 * MRSW variants); extra[0..14] holds caller-defined u32 values that the
 * writer maintains across kickout.  Slot 15 of extra[] is unused. */
struct rix_hash_mrsw_extra_find_ctx_s {
    union rix_hash_hash_u            hash;
    struct rix_hash_bucket_extra_s  *bk[2];
    struct rix_hash_bucket_extra_s  *buckets;
    const void                      *key;
    u32                              ctrl[2];
    unsigned                         hash_mask;
    unsigned                         bk_mask;
    u32                              fp;
    u32                              fp_hits[2];
};

static RIX_FORCE_INLINE u32
rix_hash_mrsw_ctrl_seq(u32 ctrl)
{
    return ctrl & RIX_HASH_MRSW_CTRL_SEQ_MASK;
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_ctrl_valid(u32 ctrl)
{
    return (ctrl >> RIX_HASH_MRSW_CTRL_VALID_SHIFT) &
           RIX_HASH_MRSW_VALID_MASK;
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_ctrl_slot_bit(unsigned slot)
{
    return UINT32_C(1) << (RIX_HASH_MRSW_CTRL_VALID_SHIFT + slot);
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_ctrl_next_seq(u32 ctrl)
{
    return (ctrl & ~RIX_HASH_MRSW_CTRL_SEQ_MASK) |
           ((ctrl + UINT32_C(1)) & RIX_HASH_MRSW_CTRL_SEQ_MASK);
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_bucket_valid_load(struct rix_hash_bucket_s *bk,
                                memory_order order)
{
    u32 ctrl = atomic_load_explicit(&bk->ctrl, order);
    return rix_hash_mrsw_ctrl_valid(ctrl);
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_bucket_valid_set(struct rix_hash_bucket_s *bk,
                               unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) | bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_bucket_valid_clear(struct rix_hash_bucket_s *bk,
                                 unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) & ~bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_bucket_read_begin(struct rix_hash_bucket_s *bk)
{
    return atomic_load_explicit(&bk->ctrl, memory_order_acquire);
}

static RIX_FORCE_INLINE int
rix_hash_mrsw_bucket_read_retry(struct rix_hash_bucket_s *bk,
                                u32 ctrl)
{
    u32 now = atomic_load_explicit(&bk->ctrl, memory_order_acquire);
    return now != ctrl;
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_buckets_init(struct rix_hash_bucket_s *buckets,
                           unsigned nb_bk)
{
    for (unsigned b = 0u; b < nb_bk; b++) {
        struct rix_hash_bucket_s *bk = buckets + b;
        atomic_init(&bk->ctrl, 0u);
        bk->reserved = 0u;
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
            bk->hash[s] = 0u;
            bk->idx [s] = (u32)RIX_NIL;
        }
    }
}

/* U64 bucket ctrl helpers (operate on struct rix_hash64_bucket_s).        */
static RIX_FORCE_INLINE u32
rix_hash_mrsw_u64_bucket_valid_load(struct rix_hash64_bucket_s *bk,
                                    memory_order order)
{
    return rix_hash_mrsw_ctrl_valid(atomic_load_explicit(&bk->ctrl, order));
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_u64_bucket_valid_set(struct rix_hash64_bucket_s *bk,
                                   unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) | bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_u64_bucket_valid_clear(struct rix_hash64_bucket_s *bk,
                                     unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) & ~bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_u64_bucket_read_begin(struct rix_hash64_bucket_s *bk)
{
    return atomic_load_explicit(&bk->ctrl, memory_order_acquire);
}

static RIX_FORCE_INLINE int
rix_hash_mrsw_u64_bucket_read_retry(struct rix_hash64_bucket_s *bk, u32 ctrl)
{
    u32 now = atomic_load_explicit(&bk->ctrl, memory_order_acquire);
    return now != ctrl;
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_u64_buckets_init(struct rix_hash64_bucket_s *buckets,
                               unsigned nb_bk)
{
    for (unsigned b = 0u; b < nb_bk; b++) {
        struct rix_hash64_bucket_s *bk = buckets + b;
        atomic_init(&bk->ctrl, 0u);
        bk->reserved = 0u;
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
            bk->key[s] = 0u;
            bk->idx[s] = (u32)RIX_NIL;
        }
    }
}

/* SLOT_EXTRA bucket ctrl helpers (operate on rix_hash_bucket_extra_s).      */
static RIX_FORCE_INLINE u32
rix_hash_mrsw_extra_bucket_valid_load(struct rix_hash_bucket_extra_s *bk,
                                      memory_order order)
{
    return rix_hash_mrsw_ctrl_valid(atomic_load_explicit(&bk->ctrl, order));
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_extra_bucket_valid_set(struct rix_hash_bucket_extra_s *bk,
                                     unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) | bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_extra_bucket_valid_clear(struct rix_hash_bucket_extra_s *bk,
                                       unsigned slot)
{
    u32 bit = rix_hash_mrsw_ctrl_slot_bit(slot);
    u32 old = atomic_load_explicit(&bk->ctrl, memory_order_relaxed);
    for (;;) {
        u32 next = rix_hash_mrsw_ctrl_next_seq(old) & ~bit;
        if (atomic_compare_exchange_weak_explicit(&bk->ctrl, &old, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

static RIX_FORCE_INLINE u32
rix_hash_mrsw_extra_bucket_read_begin(struct rix_hash_bucket_extra_s *bk)
{
    return atomic_load_explicit(&bk->ctrl, memory_order_acquire);
}

static RIX_FORCE_INLINE int
rix_hash_mrsw_extra_bucket_read_retry(struct rix_hash_bucket_extra_s *bk,
                                      u32 ctrl)
{
    u32 now = atomic_load_explicit(&bk->ctrl, memory_order_acquire);
    return now != ctrl;
}

static RIX_FORCE_INLINE void
rix_hash_mrsw_extra_buckets_init(struct rix_hash_bucket_extra_s *buckets,
                                 unsigned nb_bk)
{
    for (unsigned b = 0u; b < nb_bk; b++) {
        struct rix_hash_bucket_extra_s *bk = buckets + b;
        atomic_init(&bk->ctrl, 0u);
        bk->reserved = 0u;
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
            bk->hash[s] = 0u;
            bk->idx[s] = (u32)RIX_NIL;
            bk->extra[s] = 0u;
        }
    }
}

#  ifndef RIX_HASH_MRSW_HOOK
#    define RIX_HASH_MRSW_HOOK(name, event, head, buckets, bk, slot)          \
    do {                                                                      \
        (void)(head);                                                         \
        (void)(buckets);                                                      \
        (void)(bk);                                                           \
        (void)(slot);                                                         \
    } while (0)
#  endif

#  define RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, attr) \
    attr void name##_init(struct name *head,                                  \
                          struct rix_hash_bucket_s *buckets,            \
                          unsigned nb_bk);                                    \
    attr struct type *name##_insert(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,   \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_s *buckets,    \
                                   unsigned bk,                               \
                                   unsigned slot);                            \
    attr struct type *name##_remove(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,   \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    /* walk: writer-only iteration.  If the per-bucket ctrl word changes      \
     * between the begin/end snapshots the walk aborts and returns -1.        \
     * Callers must serialize with the writer (or call from the writer        \
     * thread itself) to obtain a consistent traversal. */                    \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,              \
                         struct type *base,                                   \
                         int (*cb)(struct type *, void *),                    \
                         void *arg);

/* All public PROTOTYPE_* macros expand to the same external symbols (the FP
 * and SLOT variants generate identical signatures; slot_field is internal).
 * Therefore _EX, _STATIC, _SLOT and _STATIC_SLOT variants are thin wrappers
 * around the same _INTERNAL macro -- only the linkage attribute differs. */
#  define RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)  \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, hash_field,       \
                                     cmp_fn, )
#  define RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, hash_field,       \
                                     cmp_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_PROTOTYPE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE(name, type, key_field, hash_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn)

#  define RIX_HASH_MRSW_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn, hash_fn, )

#  define RIX_HASH_MRSW_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn, hash_fn, RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE(name, type, key_field, hash_field, cmp_fn)   \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn,                                   \
                                    RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_MRSW_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field,        \
                                    cmp_fn,                                   \
                                    RIX_HASH_DEFAULT_HASH_FN_NAME(name),      \
                                    RIX_UNUSED static)

/* SLOT variant: requires slot_field of an integer type that can represent   \
 * 0..RIX_HASH_MRSW_BUCKET_ENTRY_SZ-1.  Insert/kickout updates the field; the \
 * remove path uses elm->slot_field for direct O(1) bucket-slot lookup.       \
 * The slot_field is only mutated by the writer; readers do not touch it. */
#  define RIX_HASH_MRSW_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn, hash_fn, )

#  define RIX_HASH_MRSW_GENERATE_SLOT_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn, hash_fn,         \
                                         RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn,                  \
                                         RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_MRSW_GENERATE_SLOT_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,   \
                                         slot_field, cmp_fn,                  \
                                         RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                         RIX_UNUSED static)

/* KEYONLY variant: no hash_field, no slot_field in the node.  Kickout re-    \
 * hashes the victim's key to find its alternate bucket; remove re-hashes    \
 * the element key and probes both candidate buckets.  Find/insert behave    \
 * like the FP variant but with no node-side hash_field maintenance. */
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY(name, type, key_field, cmp_fn)      \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, /*hash_field*/_,  \
                                     cmp_fn, )
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY_STATIC(name, type, key_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_INTERNAL(name, type, key_field, /*hash_field*/_,  \
                                     cmp_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_KEYONLY(name, type, key_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_KEYONLY_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_KEYONLY_STATIC(name, type, key_field, cmp_fn)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            hash_fn, )

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            hash_fn, RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY(name, type, key_field, cmp_fn)       \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_STATIC(name, type, key_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn,    \
                                            RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                            RIX_UNUSED static)

/* U32 variant: 32-bit keys stored directly in bk->hash[] (reinterpreted as  \
 * keys[]).  No fingerprint, no cmp_fn needed (key match in bucket scan is   \
 * authoritative once filtered by valid).  Hash function:                    \
 *   rix_hash_arch->hash_u32 (shared with pure U32).                          \
 * Empty slots have valid bit = 0; hash[] of unused slots may contain        \
 * stale keys that are ignored.                                              \
 */
#  define RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, attr)   \
    attr void name##_init(struct name *head,                                  \
                          struct rix_hash_bucket_s *buckets,                  \
                          unsigned nb_bk);                                    \
    attr type *name##_insert(struct name *head,                               \
                             struct rix_hash_bucket_s *buckets,               \
                             type *base, type *elm);                          \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_s *buckets,         \
                                   unsigned bk, unsigned slot);               \
    attr type *name##_remove(struct name *head,                               \
                             struct rix_hash_bucket_s *buckets,               \
                             type *base, type *elm);                          \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,                   \
                         type *base, int (*cb)(type *, void *), void *arg);

#  define RIX_HASH_MRSW_PROTOTYPE_U32(name, type, key_field)                  \
    RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRSW_PROTOTYPE_U32_STATIC(name, type, key_field)           \
    RIX_HASH_MRSW_PROTOTYPE_U32_INTERNAL(name, type, key_field, RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE_U32(name, type, key_field)                   \
    RIX_HASH_MRSW_GENERATE_U32_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRSW_GENERATE_U32_STATIC(name, type, key_field)            \
    RIX_HASH_MRSW_GENERATE_U32_INTERNAL(name, type, key_field, RIX_UNUSED static)

/* U64 variant: 64-bit keys stored directly in bk->key[] of the shared      \
 * struct rix_hash64_bucket_s.  Slot 15 of key[] aliases _Atomic u32 ctrl + \
 * 4 spare bytes; slot 15 of idx[] aliases u32 reserved.  SIMD scan via    \
 * rix_hash_arch->find_u64x16; hash via rix_hash_arch->hash_u64. */
#  define RIX_HASH_MRSW_PROTOTYPE_U64_INTERNAL(name, type, key_field, attr)   \
    attr void name##_init(struct name *head,                                  \
                          struct rix_hash64_bucket_s *buckets,                \
                          unsigned nb_bk);                                    \
    attr type *name##_insert(struct name *head,                               \
                             struct rix_hash64_bucket_s *buckets,             \
                             type *base, type *elm);                          \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash64_bucket_s *buckets,       \
                                   unsigned bk, unsigned slot);               \
    attr type *name##_remove(struct name *head,                               \
                             struct rix_hash64_bucket_s *buckets,             \
                             type *base, type *elm);                          \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash64_bucket_s *buckets,                 \
                         type *base, int (*cb)(type *, void *), void *arg);

#  define RIX_HASH_MRSW_PROTOTYPE_U64(name, type, key_field)                  \
    RIX_HASH_MRSW_PROTOTYPE_U64_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRSW_PROTOTYPE_U64_STATIC(name, type, key_field)           \
    RIX_HASH_MRSW_PROTOTYPE_U64_INTERNAL(name, type, key_field, RIX_UNUSED static)

#  define RIX_HASH_MRSW_GENERATE_U64(name, type, key_field)                   \
    RIX_HASH_MRSW_GENERATE_U64_INTERNAL(name, type, key_field, )
#  define RIX_HASH_MRSW_GENERATE_U64_STATIC(name, type, key_field)            \
    RIX_HASH_MRSW_GENERATE_U64_INTERNAL(name, type, key_field, RIX_UNUSED static)

/* SLOT_EXTRA variant: extends SLOT with per-slot u32 extra[].  Writer copies
 * extra on kickout and sets it on insert.  Reader does not touch extra[] in
 * find; explicit get/set helpers are provided for caller-controlled access. */
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, attr) \
    attr void name##_init(struct name *head,                                  \
                          struct rix_hash_bucket_extra_s *buckets,            \
                          unsigned nb_bk);                                    \
    attr struct type *name##_insert(struct name *head,                        \
                                    struct rix_hash_bucket_extra_s *buckets,  \
                                    struct type *base, struct type *elm,      \
                                    u32 extra);                               \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_extra_s *buckets,   \
                                   unsigned bk, unsigned slot);               \
    attr struct type *name##_remove(struct name *head,                        \
                                    struct rix_hash_bucket_extra_s *buckets,  \
                                    struct type *base, struct type *elm);     \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         struct type *base,                                   \
                         int (*cb)(struct type *, void *), void *arg);

#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, )
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn)
#  define RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_PROTOTYPE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn)

#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, )
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_STATIC_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, RIX_UNUSED static)
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, \
                                               RIX_HASH_DEFAULT_HASH_FN_NAME(name), )
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_STATIC(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, \
                                               RIX_HASH_DEFAULT_HASH_FN_NAME(name), \
                                               RIX_UNUSED static)

#  ifndef RIX_HASH_MRSW_DEFINE_INDEXERS
#    define RIX_HASH_MRSW_DEFINE_INDEXERS(name, type)                         \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p) {                        \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i) {                                  \
    return (struct type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);    \
}
#  endif

/* ---------------------------------------------------------------------- *\
 * Internal generators are split into reusable building blocks so that the   *
 * FP and SLOT variants can share readers, find pipeline, walk, and the      *
 * remove_at_inner / remove_at primitives, while the writer-side ops         *
 * (flipflop, kickout, insert_hashed_idx, insert, remove) differ in slot     *
 * field maintenance.                                                        *
 *                                                                           *
 *   COMMON_PRE  : init, indexers, scan helpers, staged find, find,          *
 *                 remove_at_inner, remove_at                                *
 *   FP_OPS      : flipflop, kickout, insert_hashed_idx, insert_hashed,      *
 *                 insert, remove (linear bucket scan)                       *
 *   SLOT_OPS    : flipflop+slot_field, kickout, insert_hashed_idx+slot,     *
 *                 insert_hashed, insert, remove (direct slot lookup)        *
 *   COMMON_POST : walk                                                      *
 *                                                                           *
 *   GENERATE_INTERNAL      = COMMON_PRE + FP_OPS  + COMMON_POST             *
 *   GENERATE_SLOT_INTERNAL = COMMON_PRE + SLOT_OPS + COMMON_POST             *
\* ---------------------------------------------------------------------- */
#  define RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
attr void                                                                     \
name##_init(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                           \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    rix_hash_mrsw_buckets_init(buckets, nb_bk);                               \
}                                                                             \
RIX_HASH_MRSW_DEFINE_INDEXERS(name, type)                                     \
/* Intentional benign race: hash[] is non-atomic payload validated by         \
 * the bucket ctrl seq+valid protocol.  See rix_hash_mrsw.h header. */        \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_hashes(struct rix_hash_bucket_s *bk,                  \
                          u32 fp,                                             \
                          u32 valid)                                          \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, fp);                            \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_find_empty(struct rix_hash_bucket_s *buckets,                     \
                  unsigned bk_idx)                                            \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                     \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                          \
    return empty ? (int)__builtin_ctz(empty) : -1;                            \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_find_ctx_s *ctx,                  \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_s *buckets,                \
                       const RIX_HASH_KEY_TYPE(type, key_field) *key,         \
                       unsigned hash_mask,                                    \
                       unsigned bk_mask)                                      \
{                                                                             \
    union rix_hash_hash_u h = hash_fn(key, hash_mask);                        \
    unsigned bk0, bk1;                                                        \
    u32 fp = rix_hash_fp(h, bk_mask, &bk0, &bk1);                             \
    ctx->hash      = h;                                                       \
    ctx->fp        = fp;                                                      \
    ctx->key       = (const void *)key;                                       \
    ctx->buckets   = buckets;                                                 \
    ctx->hash_mask = hash_mask;                                               \
    ctx->bk_mask   = bk_mask;                                                 \
    ctx->bk[0]     = buckets + bk0;                                           \
    ctx->bk[1]     = buckets + bk1;                                           \
    __builtin_prefetch(ctx->bk[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_find_ctx_s *ctx,                         \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets,                       \
                const RIX_HASH_KEY_TYPE(type, key_field) *key)                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_masked(ctx, head, buckets, key, mask, mask);              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_find_ctx_s *ctx,                          \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused))) \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_bucket_read_begin(ctx->bk[0]);               \
    ctx->fp_hits[0] = name##_scan_bucket_hashes(ctx->bk[0], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[0]));           \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_find_ctx_s *ctx,                    \
                     struct type *base)                                       \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = ctx->fp_hits[i];                                           \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = ctx->bk[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                struct type *node = name##_hptr(base, idx);                   \
                if (node != NULL)                                             \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
/* Unified bucket-half comparison; FORCE_INLINE folds the constant 'which'. */\
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD struct type *       \
name##_cmp_key_bk_once(struct rix_hash_mrsw_find_ctx_s *ctx,                  \
                       struct type *base, unsigned which)                     \
{                                                                             \
    u32 hits = ctx->fp_hits[which];                                           \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        struct type *node = name##_hptr(base, idx);                           \
        if (cmp_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,      \
                   &node->key_field) == 0)                                    \
            return node;                                                      \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_bk0_once(struct rix_hash_mrsw_find_ctx_s *ctx,                 \
                        struct type *base)                                    \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 0u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_bk1(struct rix_hash_mrsw_find_ctx_s *ctx)                     \
{                                                                             \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_find_ctx_s *ctx)                         \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_bucket_read_begin(ctx->bk[1]);               \
    ctx->fp_hits[1] = name##_scan_bucket_hashes(ctx->bk[1], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[1]));           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_bk1_once(struct rix_hash_mrsw_find_ctx_s *ctx,                 \
                        struct type *base)                                    \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_once(struct rix_hash_mrsw_find_ctx_s *ctx,                     \
                    struct type *base)                                        \
{                                                                             \
    struct type *ret = name##_cmp_key_bk0_once(ctx, base);                    \
    if (ret != NULL)                                                          \
        return ret;                                                           \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk1_once(ctx, base);                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_miss_retry(struct rix_hash_mrsw_find_ctx_s *ctx)                       \
{                                                                             \
    int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0], ctx->ctrl[0]);   \
    int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1], ctx->ctrl[1]);   \
    return retry0 || retry1;                                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_mrsw_find_ctx_s *ctx,                          \
               struct type *base)                                             \
{                                                                             \
    for (;;) {                                                                \
        struct type *ret = name##_cmp_key_once(ctx, base);                    \
        if (ret != NULL)                                                      \
            return ret;                                                       \
        RIX_HASH_MRSW_HOOK(#name, "reader_before_verify", NULL,              \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0], ctx->ctrl[0]); \
        RIX_HASH_MRSW_HOOK(#name, "reader_between_verify", NULL,             \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1], ctx->ctrl[1]); \
        if (!retry0 && !retry1)                                               \
            return ret;                                                       \
        RIX_HASH_MRSW_HOOK(#name, "reader_retry", NULL, ctx->buckets,        \
                           retry0 ? (unsigned)(ctx->bk[0] - ctx->buckets)    \
                                  : (unsigned)(ctx->bk[1] - ctx->buckets),    \
                           0u);                                               \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_find_ctx_s *ctx,                \
                         unsigned n,                                          \
                         struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,              \
                         const RIX_HASH_KEY_TYPE(type, key_field) * const *keys, \
                         unsigned hash_mask,                                  \
                         unsigned bk_mask)                                    \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_find_ctx_s *ctx,                       \
                  unsigned n,                                                 \
                  struct name *head,                                          \
                  struct rix_hash_bucket_s *buckets,                     \
                  const RIX_HASH_KEY_TYPE(type, key_field) * const *keys)     \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, mask, mask);        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_find_ctx_s *ctx,                        \
                 unsigned n,                                                  \
                 struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets)                      \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_find_ctx_s *ctx,                  \
                       unsigned n,                                            \
                       struct type *base)                                     \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_find_ctx_s *ctx,                        \
                 unsigned n,                                                  \
                 struct type *base,                                           \
                 struct type **results)                                       \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr struct type *                                                            \
name##_find(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                           \
            struct type *base,                                                \
            const RIX_HASH_KEY_TYPE(type, key_field) *key)                    \
{                                                                             \
    struct rix_hash_mrsw_find_ctx_s ctx;                                      \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head,                                     \
                       struct rix_hash_bucket_s *buckets,                \
                       unsigned bk,                                           \
                       unsigned slot)                                         \
{                                                                             \
    struct rix_hash_bucket_s *b = buckets + bk;                          \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed);     \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_bucket_valid_clear(b, slot);                                \
    RIX_HASH_MRSW_HOOK(#name, "remove_hash", head, buckets, bk, slot);        \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                      \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    return name##_remove_at_inner(head, buckets, bk, slot);                   \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                           \
            struct type *base,                                                \
            int (*cb)(struct type *, void *),                                 \
            void *arg)                                                        \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_s *bk = buckets + b;                      \
        u32 ctrl = rix_hash_mrsw_bucket_read_begin(bk);                       \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 idx = bk->idx[s];                                             \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            struct type *node = name##_hptr(base, idx);                       \
            int rc = cb(node, arg);                                           \
            if (rc)                                                           \
                return rc;                                                    \
        }                                                                     \
        if (rix_hash_mrsw_bucket_read_retry(bk, ctrl))                        \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}

/* ---- Writer ops factored: FP and SLOT share everything except slot_field  *
 *      maintenance and the _remove body.                                    *
 *      slot_set(elm, val, type, slot_field) writes elm->slot_field for SLOT *
 *      and is a no-op for FP.  remove_body(name, type, hash_field,          *
 *      slot_field, attr) expands to the variant-specific _remove body.      */
/* ---- per-variant node-side write & alt-bucket helpers ---------------- *
 * Three orthogonal slots specialize _RHM_OPS_INTERNAL per variant:        *
 *   slot_set      : writes node->slot_field        (SLOT only)            *
 *   node_hash_flip: nd->hash_field ^= fp on kickout (FP/SLOT, no-op KO)   *
 *   node_hash_set : elm->hash_field = h.val32[N] on insert (FP/SLOT only) *
 *   alt_bk_expr   : compute alt bucket index                              *
 *                   FP/SLOT use the XOR trick on stored hash_field;       *
 *                   KEYONLY re-hashes the node's key.                     *
 */
#  define _RHM_NO_SLOT_SET(elm, val, sf_type, sf)                             \
    ((void)(elm), (void)(val))
#  define _RHM_DO_SLOT_SET(elm, val, sf_type, sf)                             \
    ((elm)->sf = (RIX_HASH_SLOT_TYPE(sf_type, sf))(unsigned)(val))

#  define _RHM_NO_HASH_FLIP(target, fp, hf)    ((void)(target), (void)(fp))
#  define _RHM_DO_HASH_FLIP(target, fp, hf)    ((target)->hf ^= (fp))

#  define _RHM_NO_HASH_SET(target, val, hf)    ((void)(target), (void)(val))
#  define _RHM_DO_HASH_SET(target, val, hf)    ((target)->hf = (val))

#  define _RHM_ALT_BK_HASHED(fp, nd, bk_idx, mask, type, key_field, hash_field, hash_fn) \
    (((fp) ^ (nd)->hash_field) & (mask))

#  define _RHM_ALT_BK_REHASH(fp, nd, bk_idx, mask, type, key_field, hash_field, hash_fn) \
    (__extension__ ({                                                         \
        union rix_hash_hash_u _ab_h = hash_fn(                                \
            (const RIX_HASH_KEY_TYPE(type, key_field) *)&(nd)->key_field,     \
            (mask));                                                          \
        unsigned _ab_b0 = _ab_h.val32[0] & (mask);                            \
        unsigned _ab_b1 = _ab_h.val32[1] & (mask);                            \
        ((bk_idx) == _ab_b0) ? _ab_b1 : _ab_b0;                               \
    }))

/* remove body variants.  Signature is uniform; unused parameters are        \
 * silently dropped by each macro. */
#  define _RHM_REMOVE_FP(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned bk = (unsigned)(elm->hash_field & head->rhh_mask);               \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
    struct type *ret = NULL;                                                  \
    u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed);     \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u32 idx = b->idx[s];                                                  \
        if (idx == (u32)node_idx &&                                           \
            name##_remove_at_inner(head, buckets, bk, s) !=                   \
                (unsigned)RIX_NIL) {                                          \
            ret = elm;                                                        \
            break;                                                            \
        }                                                                     \
    }                                                                         \
    return ret;                                                               \
}

#  define _RHM_REMOVE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned bk = (unsigned)(elm->hash_field & head->rhh_mask);               \
    unsigned slot = (unsigned)elm->slot_field;                                \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return NULL;                                                          \
    if (b->idx[slot] != (u32)node_idx)                                        \
        return NULL;                                                          \
    if (name##_remove_at_inner(head, buckets, bk, slot) ==                    \
        (unsigned)RIX_NIL)                                                    \
        return NULL;                                                          \
    return elm;                                                               \
}

/* KEYONLY: re-hash key, probe both candidate buckets. */
#  define _RHM_REMOVE_KEYONLY(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u h = hash_fn(                                        \
        (const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, mask);   \
    unsigned bks[2] = { h.val32[0] & mask, h.val32[1] & mask };               \
    for (int i = 0; i < 2; i++) {                                             \
        struct rix_hash_bucket_s *b = buckets + bks[i];                       \
        u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed); \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            if (b->idx[s] == (u32)node_idx &&                                 \
                name##_remove_at_inner(head, buckets, bks[i], s) !=           \
                    (unsigned)RIX_NIL)                                        \
                return elm;                                                   \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}

#  define _RHM_OPS_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr, slot_set, hash_flip, hash_set, alt_bk_expr, remove_body) \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_flipflop(struct rix_hash_bucket_s *buckets,                            \
                struct type *base,                                            \
                unsigned mask,                                                \
                unsigned bk_idx,                                              \
                unsigned slot)                                                \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 fp = bk->hash[slot];                                                  \
    u32 idx = bk->idx[slot];                                                  \
    if (fp == 0u || idx == (u32)RIX_NIL)                                      \
        return -1;                                                            \
    struct type *nd = name##_hptr(base, idx);                                 \
    unsigned ab = alt_bk_expr(fp, nd, bk_idx, mask, type, key_field,          \
                              hash_field, hash_fn);                           \
    int alt_slot = name##_find_empty(buckets, ab);                            \
    if (alt_slot < 0)                                                         \
        return -1;                                                            \
    struct rix_hash_bucket_s *alt = buckets + ab;                             \
    hash_flip(nd, fp, hash_field);                                            \
    slot_set(nd, alt_slot, type, slot_field);                                 \
    /* Publish-before-unpublish: alt is set, then bk is cleared.  Readers     \
     * may see the entry in both buckets transiently; reverse order would    \
     * create a false-negative window. */                                     \
    alt->idx[alt_slot] = idx;                                                 \
    RIX_HASH_MRSW_HOOK(#name, "move_idx", NULL, buckets, ab,                  \
                       (unsigned)alt_slot);                                   \
    alt->hash[alt_slot] = fp;                                                 \
    rix_hash_mrsw_bucket_valid_set(alt, (unsigned)alt_slot);                  \
    RIX_HASH_MRSW_HOOK(#name, "move_hash", NULL, buckets, ab,                 \
                       (unsigned)alt_slot);                                   \
    RIX_HASH_MRSW_HOOK(#name, "move_before_old_clear", NULL, buckets,         \
                       bk_idx, slot);                                         \
    rix_hash_mrsw_bucket_valid_clear(bk, slot);                               \
    return (int)slot;                                                         \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_kickout(struct rix_hash_bucket_s *buckets,                             \
               struct type *base,                                             \
               unsigned mask,                                                 \
               unsigned bk_idx,                                               \
               int depth)                                                     \
{                                                                             \
    if (depth <= 0)                                                           \
        return -1;                                                            \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        if (name##_flipflop(buckets, base, mask, bk_idx, s) >= 0)             \
            return (int)s;                                                    \
    }                                                                         \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u32 fp = bk->hash[s];                                                 \
        u32 si = bk->idx[s];                                                  \
        if (si == (u32)RIX_NIL)                                               \
            continue;                                                         \
        struct type *sn = name##_hptr(base, si);                              \
        unsigned ab = alt_bk_expr(fp, sn, bk_idx, mask, type, key_field,      \
                                  hash_field, hash_fn);                       \
        if (name##_kickout(buckets, base, mask, ab, depth - 1) >= 0) {        \
            u32 now_fp = bk->hash[s];                                         \
            u32 now_idx = bk->idx[s];                                         \
            u32 now_valid = rix_hash_mrsw_bucket_valid_load(bk,               \
                                                            memory_order_relaxed); \
            if (now_fp != fp || now_idx != si ||                              \
                (now_valid & (UINT32_C(1) << s)) == 0u) {                     \
                int fs = name##_find_empty(buckets, bk_idx);                  \
                if (fs >= 0)                                                  \
                    return fs;                                                \
                valid = now_valid;                                            \
                continue;                                                     \
            }                                                                 \
            name##_flipflop(buckets, base, mask, bk_idx, s);                  \
            return (int)s;                                                    \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_insert_hashed_idx(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,                   \
                         struct type *base,                                   \
                         struct type *elm,                                    \
                         union rix_hash_hash_u h)                             \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned bk0, bk1;                                                        \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 fp = rix_hash_fp(h, mask, &bk0, &bk1);                                \
    struct rix_hash_bucket_s *bks[2] = { buckets + bk0, buckets + bk1 };      \
    u32 fp_hits_v[2];                                                         \
    int empty_slot_v[2];                                                      \
    hash_set(elm, h.val32[0], hash_field);                                    \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bks[i],                   \
                                                    memory_order_acquire);    \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        fp_hits_v[i] = name##_scan_bucket_hashes(bks[i], fp, valid);          \
        empty_slot_v[i] = empty ? (int)__builtin_ctz(empty) : -1;             \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = fp_hits_v[i];                                              \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 node_idx = bks[i]->idx[bit];                                  \
            if (node_idx == (u32)RIX_NIL)                                     \
                continue;                                                     \
            struct type *node = name##_hptr(base, node_idx);                  \
            if (cmp_fn(&elm->key_field, &node->key_field) == 0)               \
                return node_idx;                                              \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        int slot = empty_slot_v[i];                                           \
        if (slot >= 0) {                                                      \
            unsigned bki = (i == 0) ? bk0 : bk1;                              \
            struct rix_hash_bucket_s *bk = bks[i];                            \
            if (i == 1)                                                       \
                hash_set(elm, h.val32[1], hash_field);                        \
            slot_set(elm, slot, type, slot_field);                            \
            bk->idx[slot] = elm_idx;                                          \
            RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,       \
                               (unsigned)slot);                               \
            bk->hash[slot] = fp;                                              \
            rix_hash_mrsw_bucket_valid_set(bk, (unsigned)slot);               \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            return 0u;                                                        \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = name##_kickout(buckets, base, mask, bk0,                        \
                             RIX_HASH_FOLLOW_DEPTH);                          \
        if (pos >= 0) {                                                       \
            bki = bk0;                                                        \
        } else {                                                              \
            pos = name##_kickout(buckets, base, mask, bk1,                    \
                                 RIX_HASH_FOLLOW_DEPTH);                      \
            if (pos < 0)                                                      \
                return elm_idx;                                               \
            bki = bk1;                                                        \
            hash_set(elm, h.val32[1], hash_field);                            \
        }                                                                     \
        struct rix_hash_bucket_s *bk = buckets + bki;                         \
        slot_set(elm, pos, type, slot_field);                                 \
        bk->idx[pos] = elm_idx;                                               \
        RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,           \
                           (unsigned)pos);                                    \
        bk->hash[pos] = fp;                                                   \
        rix_hash_mrsw_bucket_valid_set(bk, (unsigned)pos);                    \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        return 0u;                                                            \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_insert_hashed(struct name *head,                                       \
                     struct rix_hash_bucket_s *buckets,                       \
                     struct type *base,                                       \
                     struct type *elm,                                        \
                     union rix_hash_hash_u h)                                 \
{                                                                             \
    u32 ret_idx = name##_insert_hashed_idx(head, buckets, base, elm, h);      \
    return (ret_idx == 0u) ? NULL : name##_hptr(base, ret_idx);               \
}                                                                             \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    union rix_hash_hash_u h =                                                 \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field,  \
                head->rhh_mask);                                              \
    return name##_insert_hashed(head, buckets, base, elm, h);                 \
}                                                                             \
remove_body(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr)

#  define RIX_HASH_MRSW_GENERATE_FP_OPS_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
    _RHM_OPS_INTERNAL(name, type, key_field, hash_field,                      \
                      /* slot_field unused */ key_field,                      \
                      cmp_fn, hash_fn, attr,                                  \
                      _RHM_NO_SLOT_SET, _RHM_DO_HASH_FLIP, _RHM_DO_HASH_SET,  \
                      _RHM_ALT_BK_HASHED, _RHM_REMOVE_FP)

#  define RIX_HASH_MRSW_GENERATE_SLOT_OPS_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
    _RHM_OPS_INTERNAL(name, type, key_field, hash_field, slot_field,          \
                      cmp_fn, hash_fn, attr,                                  \
                      _RHM_DO_SLOT_SET, _RHM_DO_HASH_FLIP, _RHM_DO_HASH_SET,  \
                      _RHM_ALT_BK_HASHED, _RHM_REMOVE_SLOT)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_OPS_INTERNAL(name, type, key_field, cmp_fn, hash_fn, attr) \
    _RHM_OPS_INTERNAL(name, type, key_field,                                  \
                      /* hash_field unused */ key_field,                      \
                      /* slot_field unused */ key_field,                      \
                      cmp_fn, hash_fn, attr,                                  \
                      _RHM_NO_SLOT_SET, _RHM_NO_HASH_FLIP, _RHM_NO_HASH_SET,  \
                      _RHM_ALT_BK_REHASH, _RHM_REMOVE_KEYONLY)

/* ---- public composed generators -------------------------------------- */
#  define RIX_HASH_MRSW_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
    RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field,         \
                                               hash_field, cmp_fn, hash_fn,   \
                                               attr)                          \
    RIX_HASH_MRSW_GENERATE_FP_OPS_INTERNAL(name, type, key_field, hash_field, \
                                           cmp_fn, hash_fn, attr)

#  define RIX_HASH_MRSW_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
    RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field,         \
                                               hash_field, cmp_fn, hash_fn,   \
                                               attr)                          \
    RIX_HASH_MRSW_GENERATE_SLOT_OPS_INTERNAL(name, type, key_field,           \
                                             hash_field, slot_field, cmp_fn,  \
                                             hash_fn, attr)

#  define RIX_HASH_MRSW_GENERATE_KEYONLY_INTERNAL(name, type, key_field, cmp_fn, hash_fn, attr) \
    RIX_HASH_MRSW_GENERATE_COMMON_PRE_INTERNAL(name, type, key_field,         \
                                               /* hash_field */ key_field,    \
                                               cmp_fn, hash_fn, attr)         \
    RIX_HASH_MRSW_GENERATE_KEYONLY_OPS_INTERNAL(name, type, key_field,        \
                                                cmp_fn, hash_fn, attr)

/* ---- U32 variant generator -------------------------------------------- *
 * Structurally distinct from FP/SLOT/KEYONLY: bk->hash[] is reinterpreted   *
 * as a u32 keys[] array; no fingerprint, no cmp_fn.  The MRSW ctrl/valid   *
 * protocol is identical.  This generator does not share the common-pre     *
 * macros above because the find pipeline omits the fp/cmp_fn step.         */
#  define RIX_HASH_MRSW_GENERATE_U32_INTERNAL(name, type, key_field, attr)    \
attr void                                                                     \
name##_init(struct name *head, struct rix_hash_bucket_s *buckets,             \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    rix_hash_mrsw_buckets_init(buckets, nb_bk);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(type *base, const type *p)                                        \
{                                                                             \
    return RIX_IDX_FROM_PTR(base, (type *)(uintptr_t)p);                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_hptr(type *base, unsigned i)                                           \
{                                                                             \
    return (type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);           \
}                                                                             \
/* Intentional benign race: hash[] (u32 keys) is non-atomic; the bucket      \
 * ctrl seq+valid protocol guards observations. */                            \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_keys(struct rix_hash_bucket_s *bk, u32 key, u32 valid)     \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, key);                           \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_find_empty(struct rix_hash_bucket_s *buckets, unsigned bk_idx)         \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                          \
    return empty ? (int)__builtin_ctz(empty) : -1;                            \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_u32_find_ctx_s *ctx,              \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_s *buckets,                     \
                       u32 key,                                               \
                       unsigned hash_mask __attribute__((unused)),             \
                       unsigned bk_mask)                                      \
{                                                                             \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, bk_mask);          \
    ctx->key     = key;                                                       \
    ctx->buckets = buckets;                                                   \
    ctx->bk_mask = bk_mask;                                                   \
    ctx->bk[0]   = buckets + (h.val32[0] & bk_mask);                          \
    ctx->bk[1]   = buckets + (h.val32[1] & bk_mask);                          \
    __builtin_prefetch(ctx->bk[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_u32_find_ctx_s *ctx,                     \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets, u32 key)                   \
{                                                                             \
    name##_hash_key_masked(ctx, head, buckets, key, head->rhh_mask,           \
                           head->rhh_mask);                                   \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_u32_find_ctx_s *ctx,                      \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused)))     \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_bucket_read_begin(ctx->bk[0]);               \
    ctx->hits[0] = name##_scan_bucket_keys(ctx->bk[0], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[0]));               \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->hits[1] = 0u;                                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)    \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 h = ctx->hits[i];                                                 \
        while (h) {                                                           \
            unsigned bit = (unsigned)__builtin_ctz(h);                        \
            h &= h - 1u;                                                      \
            u32 idx = ctx->bk[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                type *node = name##_hptr(base, idx);                          \
                if (node)                                                     \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD type *              \
name##_cmp_key_bk_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base,  \
                       unsigned which)                                        \
{                                                                             \
    u32 hits = ctx->hits[which];                                              \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        return name##_hptr(base, idx);                                        \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk0_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 0u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_bk1(struct rix_hash_mrsw_u32_find_ctx_s *ctx)                 \
{                                                                             \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_u32_find_ctx_s *ctx)                     \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_bucket_read_begin(ctx->bk[1]);               \
    ctx->hits[1] = name##_scan_bucket_keys(ctx->bk[1], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[1]));               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk1_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_once(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)     \
{                                                                             \
    type *r = name##_cmp_key_bk0_once(ctx, base);                             \
    if (r != NULL)                                                            \
        return r;                                                             \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk1_once(ctx, base);                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_miss_retry(struct rix_hash_mrsw_u32_find_ctx_s *ctx)                   \
{                                                                             \
    int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0], ctx->ctrl[0]);   \
    int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1], ctx->ctrl[1]);   \
    return retry0 || retry1;                                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key(struct rix_hash_mrsw_u32_find_ctx_s *ctx, type *base)          \
{                                                                             \
    for (;;) {                                                                \
        type *r = name##_cmp_key_once(ctx, base);                             \
        if (r != NULL)                                                        \
            return r;                                                         \
        RIX_HASH_MRSW_HOOK(#name, "reader_before_verify", NULL,               \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry0 = rix_hash_mrsw_bucket_read_retry(ctx->bk[0],              \
                                                     ctx->ctrl[0]);           \
        RIX_HASH_MRSW_HOOK(#name, "reader_between_verify", NULL,              \
                           ctx->buckets,                                      \
                           (unsigned)(ctx->bk[0] - ctx->buckets), 0u);        \
        int retry1 = rix_hash_mrsw_bucket_read_retry(ctx->bk[1],              \
                                                     ctx->ctrl[1]);           \
        if (!retry0 && !retry1)                                               \
            return NULL;                                                      \
        RIX_HASH_MRSW_HOOK(#name, "reader_retry", NULL, ctx->buckets,         \
                           retry0 ? (unsigned)(ctx->bk[0] - ctx->buckets)     \
                                  : (unsigned)(ctx->bk[1] - ctx->buckets),    \
                           0u);                                               \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_u32_find_ctx_s *ctx,            \
                         unsigned n, struct name *head,                       \
                         struct rix_hash_bucket_s *buckets,                   \
                         const u32 *keys,                                     \
                         unsigned hash_mask, unsigned bk_mask)                \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,       \
                  struct name *head, struct rix_hash_bucket_s *buckets,       \
                  const u32 *keys)                                            \
{                                                                             \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, head->rhh_mask,     \
                             head->rhh_mask);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,        \
                 struct name *head, struct rix_hash_bucket_s *buckets)        \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,  \
                       type *base)                                            \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_u32_find_ctx_s *ctx, unsigned n,        \
                 type *base, type **results)                                  \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr type *                                                                   \
name##_find(struct name *head, struct rix_hash_bucket_s *buckets,             \
            type *base, u32 key)                                              \
{                                                                             \
    struct rix_hash_mrsw_u32_find_ctx_s ctx;                                  \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head, struct rix_hash_bucket_s *buckets,  \
                       unsigned bk, unsigned slot)                            \
{                                                                             \
    struct rix_hash_bucket_s *b = buckets + bk;                               \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed);     \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_bucket_valid_clear(b, slot);                                \
    RIX_HASH_MRSW_HOOK(#name, "remove_hash", head, buckets, bk, slot);        \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head, struct rix_hash_bucket_s *buckets,        \
                 unsigned bk, unsigned slot)                                  \
{                                                                             \
    return name##_remove_at_inner(head, buckets, bk, slot);                   \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head, struct rix_hash_bucket_s *buckets,             \
            type *base, int (*cb)(type *, void *), void *arg)                 \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_s *bk = buckets + b;                           \
        u32 ctrl = rix_hash_mrsw_bucket_read_begin(bk);                       \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 idx = bk->idx[s];                                             \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            type *node = name##_hptr(base, idx);                              \
            int rc = cb(node, arg);                                           \
            if (rc)                                                           \
                return rc;                                                    \
        }                                                                     \
        if (rix_hash_mrsw_bucket_read_retry(bk, ctrl))                        \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_flipflop(struct rix_hash_bucket_s *buckets, unsigned mask,             \
                unsigned bk_idx, unsigned slot)                               \
{                                                                             \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 key = bk->hash[slot];                                                 \
    u32 idx = bk->idx[slot];                                                  \
    if (idx == (u32)RIX_NIL)                                                  \
        return -1;                                                            \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
    unsigned b0 = h.val32[0] & mask;                                          \
    unsigned b1 = h.val32[1] & mask;                                          \
    unsigned ab = (bk_idx == b0) ? b1 : b0;                                   \
    int alt_slot = name##_find_empty(buckets, ab);                            \
    if (alt_slot < 0)                                                         \
        return -1;                                                            \
    struct rix_hash_bucket_s *alt = buckets + ab;                             \
    /* Publish-before-unpublish */                                            \
    alt->idx[alt_slot] = idx;                                                 \
    RIX_HASH_MRSW_HOOK(#name, "move_idx", NULL, buckets, ab,                  \
                       (unsigned)alt_slot);                                   \
    alt->hash[alt_slot] = key;                                                \
    rix_hash_mrsw_bucket_valid_set(alt, (unsigned)alt_slot);                  \
    RIX_HASH_MRSW_HOOK(#name, "move_hash", NULL, buckets, ab,                 \
                       (unsigned)alt_slot);                                   \
    RIX_HASH_MRSW_HOOK(#name, "move_before_old_clear", NULL, buckets,         \
                       bk_idx, slot);                                         \
    rix_hash_mrsw_bucket_valid_clear(bk, slot);                               \
    return (int)slot;                                                         \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_kickout(struct rix_hash_bucket_s *buckets, unsigned mask,              \
               unsigned bk_idx, int depth)                                    \
{                                                                             \
    if (depth <= 0)                                                           \
        return -1;                                                            \
    struct rix_hash_bucket_s *bk = buckets + bk_idx;                          \
    u32 valid = rix_hash_mrsw_bucket_valid_load(bk, memory_order_relaxed);    \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        if (name##_flipflop(buckets, mask, bk_idx, s) >= 0)                   \
            return (int)s;                                                    \
    }                                                                         \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u32 key = bk->hash[s];                                                \
        u32 si = bk->idx[s];                                                  \
        if (si == (u32)RIX_NIL)                                               \
            continue;                                                         \
        union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);         \
        unsigned b0 = h.val32[0] & mask;                                      \
        unsigned b1 = h.val32[1] & mask;                                      \
        unsigned ab = (bk_idx == b0) ? b1 : b0;                               \
        if (name##_kickout(buckets, mask, ab, depth - 1) >= 0) {              \
            u32 now_key = bk->hash[s];                                        \
            u32 now_idx = bk->idx[s];                                         \
            u32 now_valid = rix_hash_mrsw_bucket_valid_load(bk,               \
                                                            memory_order_relaxed); \
            if (now_key != key || now_idx != si ||                            \
                (now_valid & (UINT32_C(1) << s)) == 0u) {                     \
                int fs = name##_find_empty(buckets, bk_idx);                  \
                if (fs >= 0)                                                  \
                    return fs;                                                \
                valid = now_valid;                                            \
                continue;                                                     \
            }                                                                 \
            name##_flipflop(buckets, mask, bk_idx, s);                        \
            return (int)s;                                                    \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
attr type *                                                                   \
name##_insert(struct name *head, struct rix_hash_bucket_s *buckets,           \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
    unsigned bk0 = h.val32[0] & mask;                                         \
    unsigned bk1 = h.val32[1] & mask;                                         \
    struct rix_hash_bucket_s *bks[2] = { buckets + bk0, buckets + bk1 };      \
    u32 hits_v[2];                                                            \
    int empty_slot_v[2];                                                      \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_bucket_valid_load(bks[i],                   \
                                                    memory_order_acquire);    \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        hits_v[i] = name##_scan_bucket_keys(bks[i], key, valid);              \
        empty_slot_v[i] = empty ? (int)__builtin_ctz(empty) : -1;             \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = hits_v[i];                                                 \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = bks[i]->idx[bit];                                       \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            return name##_hptr(base, idx);                                    \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        int slot = empty_slot_v[i];                                           \
        if (slot >= 0) {                                                      \
            unsigned bki = (i == 0) ? bk0 : bk1;                              \
            struct rix_hash_bucket_s *bk = bks[i];                            \
            bk->idx[slot] = elm_idx;                                          \
            RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,       \
                               (unsigned)slot);                               \
            bk->hash[slot] = key;                                             \
            rix_hash_mrsw_bucket_valid_set(bk, (unsigned)slot);               \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = name##_kickout(buckets, mask, bk0, RIX_HASH_FOLLOW_DEPTH);      \
        if (pos >= 0)                                                         \
            bki = bk0;                                                        \
        else {                                                                \
            pos = name##_kickout(buckets, mask, bk1, RIX_HASH_FOLLOW_DEPTH);  \
            if (pos < 0)                                                      \
                return elm;                                                   \
            bki = bk1;                                                        \
        }                                                                     \
        struct rix_hash_bucket_s *bk = buckets + bki;                         \
        bk->idx[pos] = elm_idx;                                               \
        RIX_HASH_MRSW_HOOK(#name, "insert_idx", head, buckets, bki,           \
                           (unsigned)pos);                                    \
        bk->hash[pos] = key;                                                  \
        rix_hash_mrsw_bucket_valid_set(bk, (unsigned)pos);                    \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        return NULL;                                                          \
    }                                                                         \
}                                                                             \
attr type *                                                                   \
name##_remove(struct name *head, struct rix_hash_bucket_s *buckets,           \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned node_idx = name##_hidx(base, elm);                               \
    u32 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);             \
    unsigned bks[2] = { h.val32[0] & mask, h.val32[1] & mask };               \
    for (int i = 0; i < 2; i++) {                                             \
        struct rix_hash_bucket_s *b = buckets + bks[i];                       \
        u32 valid = rix_hash_mrsw_bucket_valid_load(b, memory_order_relaxed); \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            if (b->idx[s] == (u32)node_idx &&                                 \
                name##_remove_at_inner(head, buckets, bks[i], s) !=           \
                    (unsigned)RIX_NIL)                                        \
                return elm;                                                   \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}

/* ---- U64 variant generator -------------------------------------------- *
 * Parallel to U32 but with u64 keys, find_u64x16, hash_u64, and the 192 B  *
 * struct rix_hash64_bucket_s.  Ctrl/reserved aliasing keeps 15 usable      *
 * slots on the same memory layout as pure U64.                             */
#  define RIX_HASH_MRSW_GENERATE_U64_INTERNAL(name, type, key_field, attr)    \
attr void                                                                     \
name##_init(struct name *head, struct rix_hash64_bucket_s *buckets,           \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    rix_hash_mrsw_u64_buckets_init(buckets, nb_bk);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(type *base, const type *p)                                        \
{                                                                             \
    return RIX_IDX_FROM_PTR(base, (type *)(uintptr_t)p);                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_hptr(type *base, unsigned i)                                           \
{                                                                             \
    return (type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_keys(struct rix_hash64_bucket_s *bk, u64 key, u32 valid)   \
{                                                                             \
    u32 hits = rix_hash_arch->find_u64x16(bk->key, key);                      \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_find_empty(struct rix_hash64_bucket_s *buckets, unsigned bk_idx)       \
{                                                                             \
    struct rix_hash64_bucket_s *bk = buckets + bk_idx;                        \
    u32 valid = rix_hash_mrsw_u64_bucket_valid_load(bk,                       \
                                                    memory_order_relaxed);    \
    u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                          \
    return empty ? (int)__builtin_ctz(empty) : -1;                            \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_u64_find_ctx_s *ctx,              \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash64_bucket_s *buckets,                   \
                       u64 key,                                               \
                       unsigned hash_mask __attribute__((unused)),             \
                       unsigned bk_mask)                                      \
{                                                                             \
    union rix_hash_hash_u h = rix_hash_arch->hash_u64(key, bk_mask);          \
    ctx->key     = key;                                                       \
    ctx->buckets = buckets;                                                   \
    ctx->bk_mask = bk_mask;                                                   \
    ctx->bk[0]   = buckets + (h.val32[0] & bk_mask);                          \
    ctx->bk[1]   = buckets + (h.val32[1] & bk_mask);                          \
    __builtin_prefetch(ctx->bk[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_u64_find_ctx_s *ctx,                     \
                struct name *head, struct rix_hash64_bucket_s *buckets,      \
                u64 key)                                                      \
{                                                                             \
    name##_hash_key_masked(ctx, head, buckets, key, head->rhh_mask,           \
                           head->rhh_mask);                                   \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_u64_find_ctx_s *ctx,                      \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash64_bucket_s *buckets __attribute__((unused)))   \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_u64_bucket_read_begin(ctx->bk[0]);           \
    ctx->hits[0] = name##_scan_bucket_keys(ctx->bk[0], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[0]));               \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->hits[1] = 0u;                                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_u64_find_ctx_s *ctx, type *base)    \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 h = ctx->hits[i];                                                 \
        while (h) {                                                           \
            unsigned bit = (unsigned)__builtin_ctz(h);                        \
            h &= h - 1u;                                                      \
            u32 idx = ctx->bk[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                type *node = name##_hptr(base, idx);                          \
                if (node)                                                     \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD type *              \
name##_cmp_key_bk_once(struct rix_hash_mrsw_u64_find_ctx_s *ctx, type *base,  \
                       unsigned which)                                        \
{                                                                             \
    u32 hits = ctx->hits[which];                                              \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        return name##_hptr(base, idx);                                        \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk0_once(struct rix_hash_mrsw_u64_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 0u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_bk1(struct rix_hash_mrsw_u64_find_ctx_s *ctx)                 \
{                                                                             \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_u64_find_ctx_s *ctx)                     \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_u64_bucket_read_begin(ctx->bk[1]);           \
    ctx->hits[1] = name##_scan_bucket_keys(ctx->bk[1], ctx->key,              \
                                            rix_hash_mrsw_ctrl_valid(         \
                                                ctx->ctrl[1]));               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_bk1_once(struct rix_hash_mrsw_u64_find_ctx_s *ctx, type *base) \
{                                                                             \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key_once(struct rix_hash_mrsw_u64_find_ctx_s *ctx, type *base)     \
{                                                                             \
    type *r = name##_cmp_key_bk0_once(ctx, base);                             \
    if (r != NULL)                                                            \
        return r;                                                             \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk1_once(ctx, base);                                \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_miss_retry(struct rix_hash_mrsw_u64_find_ctx_s *ctx)                   \
{                                                                             \
    int retry0 = rix_hash_mrsw_u64_bucket_read_retry(ctx->bk[0], ctx->ctrl[0]); \
    int retry1 = rix_hash_mrsw_u64_bucket_read_retry(ctx->bk[1], ctx->ctrl[1]); \
    return retry0 || retry1;                                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE type *                                     \
name##_cmp_key(struct rix_hash_mrsw_u64_find_ctx_s *ctx, type *base)          \
{                                                                             \
    for (;;) {                                                                \
        type *r = name##_cmp_key_once(ctx, base);                             \
        if (r != NULL)                                                        \
            return r;                                                         \
        int retry0 = rix_hash_mrsw_u64_bucket_read_retry(ctx->bk[0],          \
                                                         ctx->ctrl[0]);       \
        int retry1 = rix_hash_mrsw_u64_bucket_read_retry(ctx->bk[1],          \
                                                         ctx->ctrl[1]);       \
        if (!retry0 && !retry1)                                               \
            return NULL;                                                      \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_u64_find_ctx_s *ctx,            \
                         unsigned n, struct name *head,                       \
                         struct rix_hash64_bucket_s *buckets,                 \
                         const u64 *keys,                                     \
                         unsigned hash_mask, unsigned bk_mask)                \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_u64_find_ctx_s *ctx, unsigned n,       \
                  struct name *head, struct rix_hash64_bucket_s *buckets,     \
                  const u64 *keys)                                            \
{                                                                             \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, head->rhh_mask,     \
                             head->rhh_mask);                                 \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_u64_find_ctx_s *ctx, unsigned n,        \
                 struct name *head, struct rix_hash64_bucket_s *buckets)      \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_u64_find_ctx_s *ctx, unsigned n,  \
                       type *base)                                            \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_u64_find_ctx_s *ctx, unsigned n,        \
                 type *base, type **results)                                  \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr type *                                                                   \
name##_find(struct name *head, struct rix_hash64_bucket_s *buckets,           \
            type *base, u64 key)                                              \
{                                                                             \
    struct rix_hash_mrsw_u64_find_ctx_s ctx;                                  \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head, struct rix_hash64_bucket_s *buckets,\
                       unsigned bk, unsigned slot)                            \
{                                                                             \
    struct rix_hash64_bucket_s *b = buckets + bk;                             \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_u64_bucket_valid_load(b, memory_order_relaxed); \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_u64_bucket_valid_clear(b, slot);                            \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head, struct rix_hash64_bucket_s *buckets,      \
                 unsigned bk, unsigned slot)                                  \
{                                                                             \
    return name##_remove_at_inner(head, buckets, bk, slot);                   \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head, struct rix_hash64_bucket_s *buckets,           \
            type *base, int (*cb)(type *, void *), void *arg)                 \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash64_bucket_s *bk = buckets + b;                         \
        u32 ctrl = rix_hash_mrsw_u64_bucket_read_begin(bk);                   \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 idx = bk->idx[s];                                             \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            type *node = name##_hptr(base, idx);                              \
            int rc = cb(node, arg);                                           \
            if (rc)                                                           \
                return rc;                                                    \
        }                                                                     \
        if (rix_hash_mrsw_u64_bucket_read_retry(bk, ctrl))                    \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_flipflop(struct rix_hash64_bucket_s *buckets, unsigned mask,           \
                unsigned bk_idx, unsigned slot)                               \
{                                                                             \
    struct rix_hash64_bucket_s *bk = buckets + bk_idx;                        \
    u64 key = bk->key[slot];                                                  \
    u32 idx = bk->idx[slot];                                                  \
    if (idx == (u32)RIX_NIL)                                                  \
        return -1;                                                            \
    union rix_hash_hash_u h = rix_hash_arch->hash_u64(key, mask);             \
    unsigned b0 = h.val32[0] & mask;                                          \
    unsigned b1 = h.val32[1] & mask;                                          \
    unsigned ab = (bk_idx == b0) ? b1 : b0;                                   \
    int alt_slot = name##_find_empty(buckets, ab);                            \
    if (alt_slot < 0)                                                         \
        return -1;                                                            \
    struct rix_hash64_bucket_s *alt = buckets + ab;                           \
    alt->idx[alt_slot] = idx;                                                 \
    alt->key[alt_slot] = key;                                                 \
    rix_hash_mrsw_u64_bucket_valid_set(alt, (unsigned)alt_slot);              \
    rix_hash_mrsw_u64_bucket_valid_clear(bk, slot);                           \
    return (int)slot;                                                         \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_kickout(struct rix_hash64_bucket_s *buckets, unsigned mask,            \
               unsigned bk_idx, int depth)                                    \
{                                                                             \
    if (depth <= 0)                                                           \
        return -1;                                                            \
    struct rix_hash64_bucket_s *bk = buckets + bk_idx;                        \
    u32 valid = rix_hash_mrsw_u64_bucket_valid_load(bk, memory_order_relaxed);\
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        if (name##_flipflop(buckets, mask, bk_idx, s) >= 0)                   \
            return (int)s;                                                    \
    }                                                                         \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u64 key = bk->key[s];                                                 \
        u32 si = bk->idx[s];                                                  \
        if (si == (u32)RIX_NIL)                                               \
            continue;                                                         \
        union rix_hash_hash_u h = rix_hash_arch->hash_u64(key, mask);         \
        unsigned b0 = h.val32[0] & mask;                                      \
        unsigned b1 = h.val32[1] & mask;                                      \
        unsigned ab = (bk_idx == b0) ? b1 : b0;                               \
        if (name##_kickout(buckets, mask, ab, depth - 1) >= 0) {              \
            u64 now_key = bk->key[s];                                         \
            u32 now_idx = bk->idx[s];                                         \
            u32 now_valid = rix_hash_mrsw_u64_bucket_valid_load(bk,           \
                                                                memory_order_relaxed); \
            if (now_key != key || now_idx != si ||                            \
                (now_valid & (UINT32_C(1) << s)) == 0u) {                     \
                int fs = name##_find_empty(buckets, bk_idx);                  \
                if (fs >= 0)                                                  \
                    return fs;                                                \
                valid = now_valid;                                            \
                continue;                                                     \
            }                                                                 \
            name##_flipflop(buckets, mask, bk_idx, s);                        \
            return (int)s;                                                    \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
attr type *                                                                   \
name##_insert(struct name *head, struct rix_hash64_bucket_s *buckets,         \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u64 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u64(key, mask);             \
    unsigned bk0 = h.val32[0] & mask;                                         \
    unsigned bk1 = h.val32[1] & mask;                                         \
    struct rix_hash64_bucket_s *bks[2] = { buckets + bk0, buckets + bk1 };    \
    u32 hits_v[2];                                                            \
    int empty_slot_v[2];                                                      \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_u64_bucket_valid_load(bks[i],               \
                                                        memory_order_acquire);\
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        hits_v[i] = name##_scan_bucket_keys(bks[i], key, valid);              \
        empty_slot_v[i] = empty ? (int)__builtin_ctz(empty) : -1;             \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = hits_v[i];                                                 \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = bks[i]->idx[bit];                                       \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            return name##_hptr(base, idx);                                    \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        int slot = empty_slot_v[i];                                           \
        if (slot >= 0) {                                                      \
            unsigned bki = (i == 0) ? bk0 : bk1;                              \
            struct rix_hash64_bucket_s *bk = bks[i];                          \
            (void)bki;                                                        \
            bk->idx[slot] = elm_idx;                                          \
            bk->key[slot] = key;                                              \
            rix_hash_mrsw_u64_bucket_valid_set(bk, (unsigned)slot);           \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = name##_kickout(buckets, mask, bk0, RIX_HASH_FOLLOW_DEPTH);      \
        if (pos >= 0)                                                         \
            bki = bk0;                                                        \
        else {                                                                \
            pos = name##_kickout(buckets, mask, bk1, RIX_HASH_FOLLOW_DEPTH);  \
            if (pos < 0)                                                      \
                return elm;                                                   \
            bki = bk1;                                                        \
        }                                                                     \
        struct rix_hash64_bucket_s *bk = buckets + bki;                       \
        bk->idx[pos] = elm_idx;                                               \
        bk->key[pos] = key;                                                   \
        rix_hash_mrsw_u64_bucket_valid_set(bk, (unsigned)pos);                \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        return NULL;                                                          \
    }                                                                         \
}                                                                             \
attr type *                                                                   \
name##_remove(struct name *head, struct rix_hash64_bucket_s *buckets,         \
              type *base, type *elm)                                          \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned node_idx = name##_hidx(base, elm);                               \
    u64 key = elm->key_field;                                                 \
    union rix_hash_hash_u h = rix_hash_arch->hash_u64(key, mask);             \
    unsigned bks[2] = { h.val32[0] & mask, h.val32[1] & mask };               \
    for (int i = 0; i < 2; i++) {                                             \
        struct rix_hash64_bucket_s *b = buckets + bks[i];                     \
        u32 valid = rix_hash_mrsw_u64_bucket_valid_load(b,                    \
                                                        memory_order_relaxed);\
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            if (b->idx[s] == (u32)node_idx &&                                 \
                name##_remove_at_inner(head, buckets, bks[i], s) !=           \
                    (unsigned)RIX_NIL)                                        \
                return elm;                                                   \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}

/* ---- SLOT_EXTRA variant generator ------------------------------------- *
 * Structurally the SLOT MRSW pipeline with extra[] writes added on the    *
 * writer side; reader path leaves extra[] untouched (caller-controlled    *
 * extra_get / extra_set).  Bucket type is rix_hash_bucket_extra_s         *
 * (192 B, 3 cache lines) shared with pure SLOT_EXTRA.                      */
#  define RIX_HASH_MRSW_GENERATE_SLOT_EXTRA_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
attr void                                                                     \
name##_init(struct name *head,                                                \
            struct rix_hash_bucket_extra_s *buckets,                          \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    atomic_init(&head->rhh_nb, 0u);                                           \
    rix_hash_mrsw_extra_buckets_init(buckets, nb_bk);                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p)                          \
{                                                                             \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i)                                    \
{                                                                             \
    return (struct type *)rix_ptr_from_idx_valid_(base, sizeof(*base), i);    \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD u32                 \
name##_scan_bucket_hashes(struct rix_hash_bucket_extra_s *bk, u32 fp,         \
                          u32 valid)                                          \
{                                                                             \
    u32 hits = RIX_HASH_FIND_U32X16(bk->hash, fp);                            \
    return hits & valid;                                                      \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE int                                        \
name##_find_empty(struct rix_hash_bucket_extra_s *buckets, unsigned bk_idx)   \
{                                                                             \
    struct rix_hash_bucket_extra_s *bk = buckets + bk_idx;                    \
    u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bk,                     \
                                                      memory_order_relaxed);  \
    u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                          \
    return empty ? (int)__builtin_ctz(empty) : -1;                            \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_masked(struct rix_hash_mrsw_extra_find_ctx_s *ctx,            \
                       struct name *head __attribute__((unused)),             \
                       struct rix_hash_bucket_extra_s *buckets,               \
                       const RIX_HASH_KEY_TYPE(type, key_field) *key,         \
                       unsigned hash_mask, unsigned bk_mask)                  \
{                                                                             \
    union rix_hash_hash_u h = hash_fn(key, hash_mask);                        \
    unsigned bk0, bk1;                                                        \
    u32 fp = rix_hash_fp(h, bk_mask, &bk0, &bk1);                             \
    ctx->hash = h;                                                            \
    ctx->fp = fp;                                                             \
    ctx->key = (const void *)key;                                             \
    ctx->buckets = buckets;                                                   \
    ctx->hash_mask = hash_mask;                                               \
    ctx->bk_mask = bk_mask;                                                   \
    ctx->bk[0] = buckets + bk0;                                               \
    ctx->bk[1] = buckets + bk1;                                               \
    __builtin_prefetch(ctx->bk[0], 0, 1);                                     \
    __builtin_prefetch(ctx->bk[1], 0, 1);                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_mrsw_extra_find_ctx_s *ctx,                   \
                struct name *head,                                            \
                struct rix_hash_bucket_extra_s *buckets,                      \
                const RIX_HASH_KEY_TYPE(type, key_field) *key)                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_masked(ctx, head, buckets, key, mask, mask);              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_mrsw_extra_find_ctx_s *ctx,                    \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_extra_s *buckets __attribute__((unused))) \
{                                                                             \
    ctx->ctrl[0] = rix_hash_mrsw_extra_bucket_read_begin(ctx->bk[0]);         \
    ctx->fp_hits[0] = name##_scan_bucket_hashes(ctx->bk[0], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[0]));           \
    ctx->ctrl[1] = 0u;                                                        \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD void                \
name##_prefetch_node(struct rix_hash_mrsw_extra_find_ctx_s *ctx,              \
                     struct type *base)                                       \
{                                                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = ctx->fp_hits[i];                                           \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 idx = ctx->bk[i]->idx[bit];                                   \
            if (idx != (u32)RIX_NIL) {                                        \
                struct type *node = name##_hptr(base, idx);                   \
                if (node)                                                     \
                    rix_hash_prefetch_entry_of(node);                         \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE RIX_NO_SANITIZE_THREAD struct type *       \
name##_cmp_key_bk_once(struct rix_hash_mrsw_extra_find_ctx_s *ctx,            \
                       struct type *base, unsigned which)                     \
{                                                                             \
    u32 hits = ctx->fp_hits[which];                                           \
    while (hits) {                                                            \
        unsigned bit = (unsigned)__builtin_ctz(hits);                         \
        hits &= hits - 1u;                                                    \
        u32 idx = ctx->bk[which]->idx[bit];                                   \
        if (idx == (u32)RIX_NIL)                                              \
            continue;                                                         \
        struct type *node = name##_hptr(base, idx);                           \
        if (cmp_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,      \
                   &node->key_field) == 0)                                    \
            return node;                                                      \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk1(struct rix_hash_mrsw_extra_find_ctx_s *ctx)                   \
{                                                                             \
    ctx->ctrl[1] = rix_hash_mrsw_extra_bucket_read_begin(ctx->bk[1]);         \
    ctx->fp_hits[1] = name##_scan_bucket_hashes(ctx->bk[1], ctx->fp,          \
                                                rix_hash_mrsw_ctrl_valid(     \
                                                    ctx->ctrl[1]));           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key_once(struct rix_hash_mrsw_extra_find_ctx_s *ctx,               \
                    struct type *base)                                        \
{                                                                             \
    struct type *r = name##_cmp_key_bk_once(ctx, base, 0u);                   \
    if (r != NULL)                                                            \
        return r;                                                             \
    name##_scan_bk1(ctx);                                                     \
    return name##_cmp_key_bk_once(ctx, base, 1u);                             \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_mrsw_extra_find_ctx_s *ctx, struct type *base) \
{                                                                             \
    for (;;) {                                                                \
        struct type *r = name##_cmp_key_once(ctx, base);                      \
        if (r != NULL)                                                        \
            return r;                                                         \
        int retry0 = rix_hash_mrsw_extra_bucket_read_retry(ctx->bk[0],        \
                                                           ctx->ctrl[0]);     \
        int retry1 = rix_hash_mrsw_extra_bucket_read_retry(ctx->bk[1],        \
                                                           ctx->ctrl[1]);     \
        if (!retry0 && !retry1)                                               \
            return NULL;                                                      \
        name##_scan_bk(ctx, NULL, NULL);                                      \
    }                                                                         \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n_masked(struct rix_hash_mrsw_extra_find_ctx_s *ctx,          \
                         unsigned n, struct name *head,                       \
                         struct rix_hash_bucket_extra_s *buckets,             \
                         const RIX_HASH_KEY_TYPE(type, key_field) * const *keys, \
                         unsigned hash_mask, unsigned bk_mask)                \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_hash_key_masked(&ctx[i], head, buckets, keys[i],               \
                               hash_mask, bk_mask);                           \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_mrsw_extra_find_ctx_s *ctx, unsigned n,     \
                  struct name *head, struct rix_hash_bucket_extra_s *buckets, \
                  const RIX_HASH_KEY_TYPE(type, key_field) * const *keys)     \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, mask, mask);        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_mrsw_extra_find_ctx_s *ctx, unsigned n,      \
                 struct name *head, struct rix_hash_bucket_extra_s *buckets)  \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_mrsw_extra_find_ctx_s *ctx,            \
                       unsigned n, struct type *base)                         \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_mrsw_extra_find_ctx_s *ctx, unsigned n,      \
                 struct type *base, struct type **results)                   \
{                                                                             \
    for (unsigned i = 0u; i < n; i++)                                         \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
attr struct type *                                                            \
name##_find(struct name *head, struct rix_hash_bucket_extra_s *buckets,       \
            struct type *base,                                                \
            const RIX_HASH_KEY_TYPE(type, key_field) *key)                    \
{                                                                             \
    struct rix_hash_mrsw_extra_find_ctx_s ctx;                                \
    name##_hash_key(&ctx, head, buckets, key);                                \
    name##_scan_bk(&ctx, head, buckets);                                      \
    return name##_cmp_key(&ctx, base);                                        \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_remove_at_inner(struct name *head,                                     \
                       struct rix_hash_bucket_extra_s *buckets,               \
                       unsigned bk, unsigned slot)                            \
{                                                                             \
    struct rix_hash_bucket_extra_s *b = buckets + bk;                         \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 valid = rix_hash_mrsw_extra_bucket_valid_load(b,                      \
                                                      memory_order_relaxed);  \
    if ((valid & (UINT32_C(1) << slot)) == 0u)                                \
        return (unsigned)RIX_NIL;                                             \
    u32 idx = b->idx[slot];                                                   \
    if (idx == (u32)RIX_NIL)                                                  \
        return (unsigned)RIX_NIL;                                             \
    rix_hash_mrsw_extra_bucket_valid_clear(b, slot);                          \
    atomic_fetch_sub_explicit(&head->rhh_nb, 1u, memory_order_relaxed);       \
    return idx;                                                               \
}                                                                             \
attr unsigned                                                                 \
name##_remove_at(struct name *head, struct rix_hash_bucket_extra_s *buckets,  \
                 unsigned bk, unsigned slot)                                  \
{                                                                             \
    return name##_remove_at_inner(head, buckets, bk, slot);                   \
}                                                                             \
attr int                                                                      \
name##_walk(struct name *head, struct rix_hash_bucket_extra_s *buckets,       \
            struct type *base,                                                \
            int (*cb)(struct type *, void *), void *arg)                      \
{                                                                             \
    for (unsigned b = 0u; b <= head->rhh_mask; b++) {                         \
        struct rix_hash_bucket_extra_s *bk = buckets + b;                     \
        u32 ctrl = rix_hash_mrsw_extra_bucket_read_begin(bk);                 \
        u32 valid = rix_hash_mrsw_ctrl_valid(ctrl);                           \
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {       \
            if ((valid & (UINT32_C(1) << s)) == 0u)                           \
                continue;                                                     \
            u32 idx = bk->idx[s];                                             \
            if (idx == (u32)RIX_NIL)                                          \
                continue;                                                     \
            struct type *node = name##_hptr(base, idx);                       \
            int rc = cb(node, arg);                                           \
            if (rc)                                                           \
                return rc;                                                    \
        }                                                                     \
        if (rix_hash_mrsw_extra_bucket_read_retry(bk, ctrl))                  \
            return -1;                                                        \
    }                                                                         \
    return 0;                                                                 \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_flipflop(struct rix_hash_bucket_extra_s *buckets,                      \
                struct type *base,                                            \
                unsigned mask, unsigned bk_idx, unsigned slot)                \
{                                                                             \
    struct rix_hash_bucket_extra_s *bk = buckets + bk_idx;                    \
    u32 fp = bk->hash[slot];                                                  \
    u32 idx = bk->idx[slot];                                                  \
    u32 extra_save = bk->extra[slot];                                         \
    if (fp == 0u || idx == (u32)RIX_NIL)                                      \
        return -1;                                                            \
    struct type *nd = name##_hptr(base, idx);                                 \
    unsigned ab = (fp ^ nd->hash_field) & mask;                               \
    int alt_slot = name##_find_empty(buckets, ab);                            \
    if (alt_slot < 0)                                                         \
        return -1;                                                            \
    struct rix_hash_bucket_extra_s *alt = buckets + ab;                       \
    nd->hash_field ^= fp;                                                     \
    nd->slot_field =                                                          \
        (RIX_HASH_SLOT_TYPE(type, slot_field))(unsigned)alt_slot;             \
    alt->idx[alt_slot] = idx;                                                 \
    alt->extra[alt_slot] = extra_save;                                        \
    alt->hash[alt_slot] = fp;                                                 \
    rix_hash_mrsw_extra_bucket_valid_set(alt, (unsigned)alt_slot);            \
    rix_hash_mrsw_extra_bucket_valid_clear(bk, slot);                         \
    return (int)slot;                                                         \
}                                                                             \
static RIX_UNUSED RIX_NO_SANITIZE_THREAD int                                  \
name##_kickout(struct rix_hash_bucket_extra_s *buckets, struct type *base,    \
               unsigned mask, unsigned bk_idx, int depth)                     \
{                                                                             \
    if (depth <= 0)                                                           \
        return -1;                                                            \
    struct rix_hash_bucket_extra_s *bk = buckets + bk_idx;                    \
    u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bk,                     \
                                                      memory_order_relaxed);  \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        if (name##_flipflop(buckets, base, mask, bk_idx, s) >= 0)             \
            return (int)s;                                                    \
    }                                                                         \
    for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {           \
        if ((valid & (UINT32_C(1) << s)) == 0u)                               \
            continue;                                                         \
        u32 fp = bk->hash[s];                                                 \
        u32 si = bk->idx[s];                                                  \
        if (si == (u32)RIX_NIL)                                               \
            continue;                                                         \
        struct type *sn = name##_hptr(base, si);                              \
        unsigned ab = (fp ^ sn->hash_field) & mask;                           \
        if (name##_kickout(buckets, base, mask, ab, depth - 1) >= 0) {        \
            u32 now_fp = bk->hash[s];                                         \
            u32 now_idx = bk->idx[s];                                         \
            u32 now_valid = rix_hash_mrsw_extra_bucket_valid_load(bk,         \
                                                                  memory_order_relaxed); \
            if (now_fp != fp || now_idx != si ||                              \
                (now_valid & (UINT32_C(1) << s)) == 0u) {                     \
                int fs = name##_find_empty(buckets, bk_idx);                  \
                if (fs >= 0)                                                  \
                    return fs;                                                \
                valid = now_valid;                                            \
                continue;                                                     \
            }                                                                 \
            name##_flipflop(buckets, base, mask, bk_idx, s);                  \
            return (int)s;                                                    \
        }                                                                     \
    }                                                                         \
    return -1;                                                                \
}                                                                             \
attr struct type *                                                            \
name##_insert(struct name *head, struct rix_hash_bucket_extra_s *buckets,     \
              struct type *base, struct type *elm, u32 extra)                 \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u h =                                                 \
        hash_fn((const RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field,  \
                mask);                                                        \
    unsigned bk0, bk1;                                                        \
    u32 elm_idx = name##_hidx(base, elm);                                     \
    u32 fp = rix_hash_fp(h, mask, &bk0, &bk1);                                \
    struct rix_hash_bucket_extra_s *bks[2] = {                                \
        buckets + bk0, buckets + bk1 };                                       \
    u32 fp_hits_v[2];                                                         \
    int empty_slot_v[2];                                                      \
    elm->hash_field = h.val32[0];                                             \
    for (int i = 0; i < 2; i++) {                                             \
        u32 valid = rix_hash_mrsw_extra_bucket_valid_load(bks[i],             \
                                                          memory_order_acquire); \
        u32 empty = (~valid) & RIX_HASH_MRSW_VALID_MASK;                      \
        fp_hits_v[i] = name##_scan_bucket_hashes(bks[i], fp, valid);          \
        empty_slot_v[i] = empty ? (int)__builtin_ctz(empty) : -1;             \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        u32 hits = fp_hits_v[i];                                              \
        while (hits) {                                                        \
            unsigned bit = (unsigned)__builtin_ctz(hits);                     \
            hits &= hits - 1u;                                                \
            u32 node_idx = bks[i]->idx[bit];                                  \
            if (node_idx == (u32)RIX_NIL)                                     \
                continue;                                                     \
            struct type *node = name##_hptr(base, node_idx);                  \
            if (cmp_fn(&elm->key_field, &node->key_field) == 0)               \
                return node;                                                  \
        }                                                                     \
    }                                                                         \
    for (int i = 0; i < 2; i++) {                                             \
        int slot = empty_slot_v[i];                                           \
        if (slot >= 0) {                                                      \
            unsigned bki = (i == 0) ? bk0 : bk1;                              \
            struct rix_hash_bucket_extra_s *bk = bks[i];                      \
            (void)bki;                                                        \
            if (i == 1)                                                       \
                elm->hash_field = h.val32[1];                                 \
            elm->slot_field =                                                 \
                (RIX_HASH_SLOT_TYPE(type, slot_field))(unsigned)slot;         \
            bk->idx[slot] = elm_idx;                                          \
            bk->extra[slot] = extra;                                          \
            bk->hash[slot] = fp;                                              \
            rix_hash_mrsw_extra_bucket_valid_set(bk, (unsigned)slot);         \
            atomic_fetch_add_explicit(&head->rhh_nb, 1u,                      \
                                      memory_order_relaxed);                  \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        int pos;                                                              \
        unsigned bki;                                                         \
        pos = name##_kickout(buckets, base, mask, bk0, RIX_HASH_FOLLOW_DEPTH);\
        if (pos >= 0)                                                         \
            bki = bk0;                                                        \
        else {                                                                \
            pos = name##_kickout(buckets, base, mask, bk1,                    \
                                 RIX_HASH_FOLLOW_DEPTH);                      \
            if (pos < 0)                                                      \
                return elm;                                                   \
            bki = bk1;                                                        \
            elm->hash_field = h.val32[1];                                     \
        }                                                                     \
        struct rix_hash_bucket_extra_s *bk = buckets + bki;                   \
        elm->slot_field =                                                     \
            (RIX_HASH_SLOT_TYPE(type, slot_field))(unsigned)pos;              \
        bk->idx[pos] = elm_idx;                                               \
        bk->extra[pos] = extra;                                               \
        bk->hash[pos] = fp;                                                   \
        rix_hash_mrsw_extra_bucket_valid_set(bk, (unsigned)pos);              \
        atomic_fetch_add_explicit(&head->rhh_nb, 1u, memory_order_relaxed);   \
        return NULL;                                                          \
    }                                                                         \
}                                                                             \
attr struct type *                                                            \
name##_remove(struct name *head, struct rix_hash_bucket_extra_s *buckets,     \
              struct type *base, struct type *elm)                            \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned bk = (unsigned)(elm->hash_field & head->rhh_mask);               \
    unsigned slot = (unsigned)elm->slot_field;                                \
    struct rix_hash_bucket_extra_s *b = buckets + bk;                         \
    if (slot >= RIX_HASH_MRSW_BUCKET_ENTRY_SZ)                                \
        return NULL;                                                          \
    if (b->idx[slot] != (u32)node_idx)                                        \
        return NULL;                                                          \
    if (name##_remove_at_inner(head, buckets, bk, slot) ==                    \
        (unsigned)RIX_NIL)                                                    \
        return NULL;                                                          \
    return elm;                                                               \
}

/* ---- convenience macro API --------------------------------------------- */
#  define RIX_HASH_MRSW_INIT(name, head, buckets, nb_bk)                     \
    name##_init(head, buckets, nb_bk)

#  define RIX_HASH_MRSW_FIND(name, head, buckets, base, key)                 \
    name##_find(head, buckets, base, key)

#  define RIX_HASH_MRSW_INSERT(name, head, buckets, base, elm)               \
    name##_insert(head, buckets, base, elm)

#  define RIX_HASH_MRSW_REMOVE(name, head, buckets, base, elm)               \
    name##_remove(head, buckets, base, elm)

#  define RIX_HASH_MRSW_REMOVE_AT(name, head, buckets, bk, slot)             \
    name##_remove_at(head, buckets, bk, slot)

#  define RIX_HASH_MRSW_WALK(name, head, buckets, base, cb, arg)             \
    name##_walk(head, buckets, base, cb, arg)

#  define RIX_HASH_MRSW_HASH_KEY(name, ctx, head, buckets, key)              \
    name##_hash_key(ctx, head, buckets, key)

#  define RIX_HASH_MRSW_HASH_KEY_MASKED(name, ctx, head, buckets, key,       \
                                        hash_mask, bk_mask)                  \
    name##_hash_key_masked(ctx, head, buckets, key, hash_mask, bk_mask)

#  define RIX_HASH_MRSW_SCAN_BK(name, ctx, head, buckets)                    \
    name##_scan_bk(ctx, head, buckets)

#  define RIX_HASH_MRSW_PREFETCH_NODE(name, ctx, base)                       \
    name##_prefetch_node(ctx, base)

#  define RIX_HASH_MRSW_CMP_KEY(name, ctx, base)                             \
    name##_cmp_key(ctx, base)

#  define RIX_HASH_MRSW_HASH_KEY2(name, ctx, head, buckets, keys)            \
    name##_hash_key_n(ctx, 2, head, buckets, keys)

#  define RIX_HASH_MRSW_SCAN_BK2(name, ctx, head, buckets)                   \
    name##_scan_bk_n(ctx, 2, head, buckets)

#  define RIX_HASH_MRSW_PREFETCH_NODE2(name, ctx, base)                      \
    name##_prefetch_node_n(ctx, 2, base)

#  define RIX_HASH_MRSW_CMP_KEY2(name, ctx, base, results)                   \
    name##_cmp_key_n(ctx, 2, base, results)

#  define RIX_HASH_MRSW_HASH_KEY4(name, ctx, head, buckets, keys)            \
    name##_hash_key_n(ctx, 4, head, buckets, keys)

#  define RIX_HASH_MRSW_SCAN_BK4(name, ctx, head, buckets)                   \
    name##_scan_bk_n(ctx, 4, head, buckets)

#  define RIX_HASH_MRSW_PREFETCH_NODE4(name, ctx, base)                      \
    name##_prefetch_node_n(ctx, 4, base)

#  define RIX_HASH_MRSW_CMP_KEY4(name, ctx, base, results)                   \
    name##_cmp_key_n(ctx, 4, base, results)

#  define RIX_HASH_MRSW_HASH_KEY_N(name, ctx, n, head, buckets, keys)        \
    name##_hash_key_n(ctx, n, head, buckets, keys)

#  define RIX_HASH_MRSW_HASH_KEY_N_MASKED(name, ctx, n, head, buckets, keys, \
                                          hash_mask, bk_mask)                \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, hash_mask, bk_mask)

#  define RIX_HASH_MRSW_SCAN_BK_N(name, ctx, n, head, buckets)               \
    name##_scan_bk_n(ctx, n, head, buckets)

#  define RIX_HASH_MRSW_PREFETCH_NODE_N(name, ctx, n, base)                  \
    name##_prefetch_node_n(ctx, n, base)

#  define RIX_HASH_MRSW_CMP_KEY_N(name, ctx, n, base, results)               \
    name##_cmp_key_n(ctx, n, base, results)

#endif /* _RIX_HASH_MRSW_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
