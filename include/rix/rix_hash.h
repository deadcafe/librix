/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * rix_hash.h - index-based cuckoo hash table, C11
 *
 * Umbrella header: includes all fingerprint-variant sub-headers and
 * provides the convenience macro API.
 *
 * Three variants, all sharing the same bucket layout and staged-find
 * pipeline (hash_key -> scan_bk -> prefetch_node -> cmp_key):
 *
 *   rix_hash_fp.h      - fingerprint variant (hash_field in node)
 *   rix_hash_slot.h    - slot variant (hash_field + slot_field in node)
 *   rix_hash_keyonly.h  - key-only variant (no auxiliary fields in node)
 *
 * Usage:
 *   #include <rix/rix_hash.h>            // all three variants
 *   #include <rix/rix_hash_fp.h>         // fp only
 *   #include <rix/rix_hash_keyonly.h>    // keyonly only
 */

#ifndef _RIX_HASH_H_
#  define _RIX_HASH_H_

#  include "rix_hash_fp.h"
#  include "rix_hash_slot.h"
#  include "rix_hash_keyonly.h"
#  include "rix_hash32.h"
#  include "rix_hash64.h"

/*===========================================================================
 * Convenience macro API
 *
 * Wraps the generated name##_xxx() functions with BSD queue-style macros
 * that embed the table-name prefix explicitly.  The name argument must
 * match the tag passed to RIX_HASH_HEAD / RIX_HASH_GENERATE.
 *
 * Single-shot ops:
 *   RIX_HASH_INIT        (name, head, nb_bk)
 *   RIX_HASH_FIND        (name, head, buckets, base, key)
 *   RIX_HASH_INSERT      (name, head, buckets, base, elm)
 *   RIX_HASH_REMOVE      (name, head, buckets, base, elm)
 *   RIX_HASH_WALK        (name, head, buckets, base, cb, arg)
 *
 * Staged find - x1:
 *   RIX_HASH_HASH_KEY    (name, ctx, head, buckets, key) key: const key_type *
 *   RIX_HASH_HASH_KEY_MASKED(name, ctx, head, buckets, key, hash_mask, bk_mask)
 *   RIX_HASH_SCAN_BK     (name, ctx, head, buckets)
 *   RIX_HASH_PREFETCH_NODE(name, ctx, base)
 *   RIX_HASH_CMP_KEY     (name, ctx, base)
 *
 * Staged find - x2 / x4  (named shorthands; expand to _n internally):
 *   RIX_HASH_HASH_KEY2   (name, ctx, head, buckets, keys)
 *   RIX_HASH_SCAN_BK2    (name, ctx, head, buckets)
 *   RIX_HASH_PREFETCH_NODE2(name, ctx, base)
 *   RIX_HASH_CMP_KEY2    (name, ctx, base, results)
 *
 *   RIX_HASH_HASH_KEY4   (name, ctx, head, buckets, keys)
 *   RIX_HASH_SCAN_BK4    (name, ctx, head, buckets)
 *   RIX_HASH_PREFETCH_NODE4(name, ctx, base)
 *   RIX_HASH_CMP_KEY4    (name, ctx, base, results)
 *
 * Staged find - xN  (arbitrary n; FORCE_INLINE + constant n -> unrolled):
 *   RIX_HASH_HASH_KEY_N  (name, ctx, n, head, buckets, keys)
 *   RIX_HASH_HASH_KEY_N_MASKED(name, ctx, n, head, buckets, keys,
 *                              hash_mask, bk_mask)
 *   RIX_HASH_SCAN_BK_N   (name, ctx, n, head, buckets)
 *   RIX_HASH_PREFETCH_NODE_N(name, ctx, n, base)
 *   RIX_HASH_CMP_KEY_N   (name, ctx, n, base, results)
 *===========================================================================*/

/* ---- single-shot ops ---------------------------------------------------- */
#  define RIX_HASH_INIT(name, head, nb_bk)                                     \
    name##_init(head, nb_bk)

#  define RIX_HASH_FIND(name, head, buckets, base, key)                        \
    name##_find(head, buckets, base, key)

#  define RIX_HASH_INSERT(name, head, buckets, base, elm)                      \
    name##_insert(head, buckets, base, elm)

#  define RIX_HASH_REMOVE(name, head, buckets, base, elm)                      \
    name##_remove(head, buckets, base, elm)

#  define RIX_HASH_REMOVE_AT(name, head, buckets, bk, slot)                    \
    name##_remove_at(head, buckets, bk, slot)

#  define RIX_HASH_WALK(name, head, buckets, base, cb, arg)                    \
    name##_walk(head, buckets, base, cb, arg)

/* ---- staged find - x1 --------------------------------------------------- */
#  define RIX_HASH_HASH_KEY(name, ctx, head, buckets, key)                     \
    name##_hash_key(ctx, head, buckets, key)

#  define RIX_HASH_HASH_KEY_MASKED(name, ctx, head, buckets, key,             \
                                   hash_mask, bk_mask)                        \
    name##_hash_key_masked(ctx, head, buckets, key, hash_mask, bk_mask)

#  define RIX_HASH_SCAN_BK(name, ctx, head, buckets)                           \
    name##_scan_bk(ctx, head, buckets)

#  define RIX_HASH_PREFETCH_NODE(name, ctx, base)                              \
    name##_prefetch_node(ctx, base)

#  define RIX_HASH_CMP_KEY(name, ctx, base)                                    \
    name##_cmp_key(ctx, base)

/* ---- staged find - x2 --------------------------------------------------- */
#  define RIX_HASH_HASH_KEY2(name, ctx, head, buckets, keys)                   \
    name##_hash_key_n(ctx, 2, head, buckets, keys)

#  define RIX_HASH_SCAN_BK2(name, ctx, head, buckets)                          \
    name##_scan_bk_n(ctx, 2, head, buckets)

#  define RIX_HASH_PREFETCH_NODE2(name, ctx, base)                             \
    name##_prefetch_node_n(ctx, 2, base)

#  define RIX_HASH_CMP_KEY2(name, ctx, base, results)                          \
    name##_cmp_key_n(ctx, 2, base, results)

/* ---- staged find - x4 --------------------------------------------------- */
#  define RIX_HASH_HASH_KEY4(name, ctx, head, buckets, keys)                   \
    name##_hash_key_n(ctx, 4, head, buckets, keys)

#  define RIX_HASH_SCAN_BK4(name, ctx, head, buckets)                          \
    name##_scan_bk_n(ctx, 4, head, buckets)

#  define RIX_HASH_PREFETCH_NODE4(name, ctx, base)                             \
    name##_prefetch_node_n(ctx, 4, base)

#  define RIX_HASH_CMP_KEY4(name, ctx, base, results)                          \
    name##_cmp_key_n(ctx, 4, base, results)

/* ---- staged find - xN (arbitrary n) ------------------------------------- */
#  define RIX_HASH_HASH_KEY_N(name, ctx, n, head, buckets, keys)               \
    name##_hash_key_n(ctx, n, head, buckets, keys)

#  define RIX_HASH_HASH_KEY_N_MASKED(name, ctx, n, head, buckets, keys,       \
                                     hash_mask, bk_mask)                      \
    name##_hash_key_n_masked(ctx, n, head, buckets, keys, hash_mask, bk_mask)

#  define RIX_HASH_SCAN_BK_N(name, ctx, n, head, buckets)                      \
    name##_scan_bk_n(ctx, n, head, buckets)

#  define RIX_HASH_PREFETCH_NODE_N(name, ctx, n, base)                         \
    name##_prefetch_node_n(ctx, n, base)

#  define RIX_HASH_CMP_KEY_N(name, ctx, n, base, results)                      \
    name##_cmp_key_n(ctx, n, base, results)

#endif /* _RIX_HASH_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
