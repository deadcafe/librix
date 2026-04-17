/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _LIBRIX_H_
#  define _LIBRIX_H_

/*
 * librix -- Relative Index Library
 *
 * Index-based data structures for shared memory / mmapped regions.
 * All heads and nodes store unsigned indices (1-origin, RIX_NIL=0)
 * instead of raw pointers, enabling:
 *
 *   - Relocatable structures across processes and after remapping
 *   - Direct placement in shared memory and file-backed regions
 *   - Mixed 32/64-bit process interoperability
 *   - Zero-initialization produces valid empty state
 *
 * Each API takes a `base` pointer (element array) for index-to-pointer
 * conversion at call time.
 *
 * -----------------------------------------------------------------------
 * Data Structures
 * -----------------------------------------------------------------------
 *
 *   Queues (rix/rix_queue.h)
 *     RIX_SLIST   -- singly-linked list
 *     RIX_LIST    -- doubly-linked list
 *     RIX_STAILQ  -- singly-linked tail queue
 *     RIX_TAILQ   -- doubly-linked tail queue
 *     RIX_CIRCLEQ -- circular queue
 *
 *   Index Containers (rix/rix_ring.h)
 *     rix_ring -- caller-supplied index buffer
 *                enqueue/dequeue = FIFO, push/pop = LIFO
 *
 *   Tree (rix/rix_tree.h)
 *     RIX_RB      -- red-black tree
 *
 *   Hash Tables (rix/rix_hash.h, rix/rix_hash_32.h, rix/rix_hash_64.h)
 *     RIX_HASH (fp)      -- cuckoo hash, 16-way bucket, fingerprint
 *                           in bucket, variable-length key in node.
 *                           O(1) remove via hash_field.
 *     RIX_HASH (slot)    -- fp + slot_field; O(1) remove without
 *                           scanning idx[16].
 *     RIX_HASH (keyonly)  -- fp without hash_field/slot_field in node;
 *                           smaller node, remove requires re-hash.
 *     RIX_HASH32         -- cuckoo hash, u32 key stored in bucket
 *     RIX_HASH64         -- cuckoo hash, u64 key stored in bucket
 *
 *     All hash variants provide:
 *       - Runtime SIMD dispatch (Generic / AVX2 / AVX-512)
 *       - Pipelined staged find (hash_key, scan_bk,
 *         prefetch_node, cmp_key) for DRAM latency hiding
 *       - init, find, insert, remove, walk
 *
 * -----------------------------------------------------------------------
 * Quick Start
 * -----------------------------------------------------------------------
 *
 *   #include "librix.h"
 *
 *   // Optional per source file: enable the best SIMD level.
 *   // If omitted, hash tables stay on the Generic path.
 *   rix_hash_arch_init(RIX_HASH_ARCH_AUTO);   // or 0 to force Generic
 *
 * -----------------------------------------------------------------------
 * Requirements
 * -----------------------------------------------------------------------
 *
 *   C11 (gnu11), single-header-per-module, no external dependencies.
 *   Optional: -mavx2 / -mavx512f for SIMD-accelerated hash operations.
 */

#  include <rix/rix_queue.h>
#  include <rix/rix_ring.h>
#  include <rix/rix_tree.h>
#  include <rix/rix_hash.h>

#endif /* _LIBRIX_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
