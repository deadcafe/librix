/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash_mr_core.h - multi-reader hash table core protocol.
 *
 * This is an independent hash-table variant.  It reuses the normal bucket
 * layouts through ctrl/reserved aliases: MRSW readers only use slots 0..14
 * and reserve physical slot 15 for a packed seq/valid control word that lets
 * readers reject observations that overlap a writer touching that bucket.
 *
 * MRSW concurrency contract:
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
 *   - ctrl is the only ordered atomic object observed by readers.  MRMW adds
 *     a writer-only bucket lock in the reserved word, but readers never load
 *     it.  hash[]/idx[] are ordinary payload words so the hot hash scan can
 *     use the same SIMD find_u32x16 dispatch as the normal hash table.
 *     Miss-side ctrl verify discards overlapped payload observations that
 *     could otherwise become a false negative.
 *   - MRMW insert_slow is a writer-only relocation path.  It serializes
 *     relocations with rhh_kickout_lock, locks bucket writer locks in a
 *     deterministic order, finds an empty slot by BFS over the cuckoo graph,
 *     and replays the path with the same publish-before-unpublish ordering as
 *     MRSW kickout.  Readers remain lockless and may observe the intentional
 *     transient duplicate created during a move.
 *
 * Bucket layout:
 *   MRSW keeps the classic 128 B / 2 cache-line bucket envelope by using
 *   slot 15 of the hash line for the packed seq/valid control word.  The
 *   spare u32 after idx[15] is reserved; MRMW reuses it as a writer lock.
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

#ifndef _RIX_HASH_MR_CORE_H_
#  define _RIX_HASH_MR_CORE_H_

#  include "rix_hash_common.h"

#  include <stdatomic.h>
#  include <stdlib.h>

/*
 * Multi-reader variants reuse struct rix_hash_bucket_s defined in
 * rix_hash_common.h.  The unified bucket exposes ctrl/reserved through
 * anonymous unions so MRSW/MRMW can access the control word atomically while
 * pure FP/SLOT/keyonly variants still see 16-slot hash[]/idx[] arrays.
 * MRSW/MRMW restrict themselves to slots 0..14 (15 usable entries) so that
 * hash[15]/idx[15] remain ctrl/reserved.
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

#  define RIX_HASH_MRMW_HEAD(name)                                           \
    struct name {                                                             \
        unsigned         rhh_mask;                                            \
        _Atomic unsigned rhh_nb;                                              \
        _Atomic u32      rhh_kickout_lock;                                    \
        unsigned        *rhh_kickout_scratch;                                 \
    }

/* Scratch buffer required by MRMW insert_slow.  The caller allocates it and
 * passes it to name##_init; insert_slow uses it under rhh_kickout_lock so a
 * single buffer per table is sufficient (no concurrent slow paths).  Element
 * count is fixed at 3 * nb_bk: a BFS queue plus parent_bk / parent_slot back
 * pointers. */
#  define RIX_HASH_MRMW_KICKOUT_SCRATCH_NITEMS(nb_bk)                         \
    (3u * (unsigned)(nb_bk))

#  define RIX_HASH_MR_HEAD_INIT_MRSW(head)                                   \
    do {                                                                      \
        (void)(head);                                                         \
    } while (0)

#  define RIX_HASH_MR_HEAD_INIT_MRMW(head)                                   \
    do {                                                                      \
        atomic_init(&(head)->rhh_kickout_lock, 0u);                           \
        (head)->rhh_kickout_scratch = NULL;                                   \
    } while (0)

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

static RIX_FORCE_INLINE void
rix_hash_mr_pause(void)
{
#  if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#  else
    atomic_signal_fence(memory_order_seq_cst);
#  endif
}

#  define RIX_HASH_MRMW_LOCK_NEXT_INC UINT32_C(0x00010000)
#  define RIX_HASH_MRMW_LOCK_OWNER_MASK UINT32_C(0x0000ffff)
#  define RIX_HASH_MRMW_LOCK_NEXT_SHIFT 16u

/* Per-bucket MRMW writer lock.  Readers never observe this word; it orders
 * only writer-vs-writer access before the existing ctrl release/acquire
 * publication protocol.  The ticket form gives bounded FIFO progress for a
 * locked bucket and avoids random backoff in correctness-sensitive paths. */
static RIX_FORCE_INLINE void
rix_hash_mrmw_lock(_Atomic u32 *lock)
{
    u32 old = atomic_fetch_add_explicit(lock, RIX_HASH_MRMW_LOCK_NEXT_INC,
                                        memory_order_acquire);
    u32 ticket = old >> RIX_HASH_MRMW_LOCK_NEXT_SHIFT;

    for (;;) {
        u32 cur = atomic_load_explicit(lock, memory_order_acquire);
        if ((cur & RIX_HASH_MRMW_LOCK_OWNER_MASK) == ticket)
            return;
        rix_hash_mr_pause();
    }
}

static RIX_FORCE_INLINE void
rix_hash_mrmw_unlock(_Atomic u32 *lock)
{
    atomic_fetch_add_explicit(lock, 1u, memory_order_release);
}

static RIX_FORCE_INLINE void
rix_hash_mrmw_lock2(_Atomic u32 *a, _Atomic u32 *b)
{
    if (a == b) {
        rix_hash_mrmw_lock(a);
    } else if ((uintptr_t)a < (uintptr_t)b) {
        rix_hash_mrmw_lock(a);
        rix_hash_mrmw_lock(b);
    } else {
        rix_hash_mrmw_lock(b);
        rix_hash_mrmw_lock(a);
    }
}

static RIX_FORCE_INLINE void
rix_hash_mrmw_unlock2(_Atomic u32 *a, _Atomic u32 *b)
{
    if (a == b) {
        rix_hash_mrmw_unlock(a);
    } else if ((uintptr_t)a < (uintptr_t)b) {
        rix_hash_mrmw_unlock(b);
        rix_hash_mrmw_unlock(a);
    } else {
        rix_hash_mrmw_unlock(a);
        rix_hash_mrmw_unlock(b);
    }
}

#  define RIX_HASH_MR_BK_LOCK_MRSW(bk)                                       \
    do {                                                                      \
        (void)(bk);                                                           \
    } while (0)
#  define RIX_HASH_MR_BK_UNLOCK_MRSW(bk)                                     \
    do {                                                                      \
        (void)(bk);                                                           \
    } while (0)
#  define RIX_HASH_MR_BK_LOCK2_MRSW(a, b)                                    \
    do {                                                                      \
        (void)(a);                                                            \
        (void)(b);                                                            \
    } while (0)
#  define RIX_HASH_MR_BK_UNLOCK2_MRSW(a, b)                                  \
    do {                                                                      \
        (void)(a);                                                            \
        (void)(b);                                                            \
    } while (0)

#  define RIX_HASH_MR_BK_LOCK_MRMW(bk)                                       \
    rix_hash_mrmw_lock(&(bk)->wlock)
#  define RIX_HASH_MR_BK_UNLOCK_MRMW(bk)                                     \
    rix_hash_mrmw_unlock(&(bk)->wlock)
#  define RIX_HASH_MR_BK_LOCK2_MRMW(a, b)                                    \
    rix_hash_mrmw_lock2(&(a)->wlock, &(b)->wlock)
#  define RIX_HASH_MR_BK_UNLOCK2_MRMW(a, b)                                  \
    rix_hash_mrmw_unlock2(&(a)->wlock, &(b)->wlock)

#  define RIX_HASH_MR_KICKOUT_LOCK_MRMW(head)                                \
    rix_hash_mrmw_lock(&(head)->rhh_kickout_lock)
#  define RIX_HASH_MR_KICKOUT_UNLOCK_MRMW(head)                              \
    rix_hash_mrmw_unlock(&(head)->rhh_kickout_lock)

/* Conservative MRMW relocation lock set.  The slow path is rare and writer
 * only: readers never observe these locks.  Taking all bucket writer locks
 * gives a stable cuckoo graph for BFS relocation and keeps the slow algorithm
 * independent from the fast-path duplicate/empty-slot code. */
#  define RIX_HASH_MR_BK_LOCK_ALL_MRMW(buckets, nb_bk)                       \
    do {                                                                      \
        for (unsigned _rix_hash_mr_b = 0u; _rix_hash_mr_b < (nb_bk);          \
             _rix_hash_mr_b++)                                                \
            RIX_HASH_MR_BK_LOCK_MRMW(&(buckets)[_rix_hash_mr_b]);             \
    } while (0)

#  define RIX_HASH_MR_BK_UNLOCK_ALL_MRMW(buckets, nb_bk)                     \
    do {                                                                      \
        unsigned _rix_hash_mr_b = (nb_bk);                                    \
        while (_rix_hash_mr_b-- > 0u)                                         \
            RIX_HASH_MR_BK_UNLOCK_MRMW(&(buckets)[_rix_hash_mr_b]);           \
    } while (0)

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
        atomic_init(&bk->wlock, 0u);
        for (unsigned s = 0u; s < RIX_HASH_MRSW_BUCKET_ENTRY_SZ; s++) {
            bk->hash[s] = 0u;
            bk->idx [s] = (u32)RIX_NIL;
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

/* MRMW exposes the same reader/staged API shape as MRSW.  These aliases keep
 * call sites from depending on MRSW names when the generated table uses
 * bucket writer locks. */
#  define RIX_HASH_MRMW_INIT(name, head, buckets, nb_bk)                     \
    RIX_HASH_MRSW_INIT(name, head, buckets, nb_bk)

#  define RIX_HASH_MRMW_FIND(name, head, buckets, base, key)                 \
    RIX_HASH_MRSW_FIND(name, head, buckets, base, key)

#  define RIX_HASH_MRMW_INSERT(name, head, buckets, base, elm)               \
    RIX_HASH_MRSW_INSERT(name, head, buckets, base, elm)

#  define RIX_HASH_MRMW_REMOVE(name, head, buckets, base, elm)               \
    RIX_HASH_MRSW_REMOVE(name, head, buckets, base, elm)

#  define RIX_HASH_MRMW_REMOVE_AT(name, head, buckets, bk, slot)             \
    RIX_HASH_MRSW_REMOVE_AT(name, head, buckets, bk, slot)

#  define RIX_HASH_MRMW_WALK(name, head, buckets, base, cb, arg)             \
    RIX_HASH_MRSW_WALK(name, head, buckets, base, cb, arg)

#  define RIX_HASH_MRMW_HASH_KEY(name, ctx, head, buckets, key)              \
    RIX_HASH_MRSW_HASH_KEY(name, ctx, head, buckets, key)

#  define RIX_HASH_MRMW_HASH_KEY_MASKED(name, ctx, head, buckets, key,       \
                                        hash_mask, bk_mask)                  \
    RIX_HASH_MRSW_HASH_KEY_MASKED(name, ctx, head, buckets, key,             \
                                  hash_mask, bk_mask)

#  define RIX_HASH_MRMW_SCAN_BK(name, ctx, head, buckets)                    \
    RIX_HASH_MRSW_SCAN_BK(name, ctx, head, buckets)

#  define RIX_HASH_MRMW_PREFETCH_NODE(name, ctx, base)                       \
    RIX_HASH_MRSW_PREFETCH_NODE(name, ctx, base)

#  define RIX_HASH_MRMW_CMP_KEY(name, ctx, base)                             \
    RIX_HASH_MRSW_CMP_KEY(name, ctx, base)

#  define RIX_HASH_MRMW_HASH_KEY2(name, ctx, head, buckets, keys)            \
    RIX_HASH_MRSW_HASH_KEY2(name, ctx, head, buckets, keys)

#  define RIX_HASH_MRMW_SCAN_BK2(name, ctx, head, buckets)                   \
    RIX_HASH_MRSW_SCAN_BK2(name, ctx, head, buckets)

#  define RIX_HASH_MRMW_PREFETCH_NODE2(name, ctx, base)                      \
    RIX_HASH_MRSW_PREFETCH_NODE2(name, ctx, base)

#  define RIX_HASH_MRMW_CMP_KEY2(name, ctx, base, results)                   \
    RIX_HASH_MRSW_CMP_KEY2(name, ctx, base, results)

#  define RIX_HASH_MRMW_HASH_KEY4(name, ctx, head, buckets, keys)            \
    RIX_HASH_MRSW_HASH_KEY4(name, ctx, head, buckets, keys)

#  define RIX_HASH_MRMW_SCAN_BK4(name, ctx, head, buckets)                   \
    RIX_HASH_MRSW_SCAN_BK4(name, ctx, head, buckets)

#  define RIX_HASH_MRMW_PREFETCH_NODE4(name, ctx, base)                      \
    RIX_HASH_MRSW_PREFETCH_NODE4(name, ctx, base)

#  define RIX_HASH_MRMW_CMP_KEY4(name, ctx, base, results)                   \
    RIX_HASH_MRSW_CMP_KEY4(name, ctx, base, results)

#  define RIX_HASH_MRMW_HASH_KEY_N(name, ctx, n, head, buckets, keys)        \
    RIX_HASH_MRSW_HASH_KEY_N(name, ctx, n, head, buckets, keys)

#  define RIX_HASH_MRMW_HASH_KEY_N_MASKED(name, ctx, n, head, buckets, keys, \
                                          hash_mask, bk_mask)                \
    RIX_HASH_MRSW_HASH_KEY_N_MASKED(name, ctx, n, head, buckets, keys,       \
                                    hash_mask, bk_mask)

#  define RIX_HASH_MRMW_SCAN_BK_N(name, ctx, n, head, buckets)               \
    RIX_HASH_MRSW_SCAN_BK_N(name, ctx, n, head, buckets)

#  define RIX_HASH_MRMW_PREFETCH_NODE_N(name, ctx, n, base)                  \
    RIX_HASH_MRSW_PREFETCH_NODE_N(name, ctx, n, base)

#  define RIX_HASH_MRMW_CMP_KEY_N(name, ctx, n, base, results)               \
    RIX_HASH_MRSW_CMP_KEY_N(name, ctx, n, base, results)


#endif /* _RIX_HASH_MR_CORE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
