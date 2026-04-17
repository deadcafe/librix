/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */
#ifndef _RIX_RING_H_
#  define _RIX_RING_H_

#  include <string.h>

#  include "rix_defs_private.h"

/*
 * Caller-supplied ring/stack buffer.
 *
 * Choose semantics by API:
 *   enqueue/dequeue = FIFO
 *   push/pop        = LIFO
 *
 * Intended for 1-origin indices. RIX_NIL is treated as programmer error and is
 * guarded with RIX_ASSERT in insertion paths.
 */

struct rix_ring {
    u32 *data;
    u32  capacity;
    u32  head;
    u32  tail;
    u32  count;
};

static RIX_UNUSED RIX_FORCE_INLINE void
rix_ring_init(struct rix_ring *ring, u32 *storage, u32 capacity)
{
    RIX_ASSERT(ring != NULL);
    RIX_ASSERT(storage != NULL || capacity == 0u);
    ring->data = storage;
    ring->capacity = capacity;
    ring->head = 0u;
    ring->tail = 0u;
    ring->count = 0u;
}

static RIX_UNUSED RIX_FORCE_INLINE void
rix_ring_reset(struct rix_ring *ring)
{
    RIX_ASSERT(ring != NULL);
    ring->head = 0u;
    ring->tail = 0u;
    ring->count = 0u;
}

static RIX_UNUSED RIX_FORCE_INLINE u32
rix_ring_count(const struct rix_ring *ring)
{
    RIX_ASSERT(ring != NULL);
    return ring->count;
}

static RIX_UNUSED RIX_FORCE_INLINE u32
rix_ring_free_count(const struct rix_ring *ring)
{
    RIX_ASSERT(ring != NULL);
    return ring->capacity - ring->count;
}

static RIX_UNUSED RIX_FORCE_INLINE int
rix_ring_empty(const struct rix_ring *ring)
{
    return rix_ring_count(ring) == 0u;
}

static RIX_UNUSED RIX_FORCE_INLINE int
rix_ring_full(const struct rix_ring *ring)
{
    RIX_ASSERT(ring != NULL);
    return ring->count == ring->capacity;
}

static RIX_UNUSED RIX_FORCE_INLINE u32
rix_ring_enqueue_burst(struct rix_ring *ring, const u32 *idxv, u32 count)
{
    u32 n;
    u32 first;

    RIX_ASSERT(ring != NULL);
    RIX_ASSERT(idxv != NULL || count == 0u);
    n = RIX_MIN(count, rix_ring_free_count(ring));
    if (n == 0u)
        return 0u;
    first = RIX_MIN(n, ring->capacity - ring->tail);
    for (u32 i = 0u; i < n; i++)
        RIX_ASSERT(idxv[i] != RIX_NIL);
    memcpy(&ring->data[ring->tail], idxv, (size_t)first * sizeof(*idxv));
    if (n > first)
        memcpy(ring->data, idxv + first, (size_t)(n - first) * sizeof(*idxv));
    ring->tail += n;
    if (ring->tail >= ring->capacity)
        ring->tail -= ring->capacity;
    ring->count += n;
    return n;
}

static RIX_UNUSED RIX_FORCE_INLINE u32
rix_ring_dequeue_burst(struct rix_ring *ring, u32 *idxv, u32 count)
{
    u32 n;
    u32 first;

    RIX_ASSERT(ring != NULL);
    RIX_ASSERT(idxv != NULL || count == 0u);
    n = RIX_MIN(count, ring->count);
    if (n == 0u)
        return 0u;
    first = RIX_MIN(n, ring->capacity - ring->head);
    memcpy(idxv, &ring->data[ring->head], (size_t)first * sizeof(*idxv));
    if (n > first)
        memcpy(idxv + first, ring->data, (size_t)(n - first) * sizeof(*idxv));
    ring->head += n;
    if (ring->head >= ring->capacity)
        ring->head -= ring->capacity;
    ring->count -= n;
    return n;
}

static RIX_UNUSED RIX_FORCE_INLINE void
rix_ring_enqueue_seq(struct rix_ring *ring, u32 first_idx, u32 count)
{
    RIX_ASSERT(ring != NULL);
    RIX_ASSERT(first_idx != RIX_NIL || count == 0u);
    RIX_ASSERT(count <= ring->capacity);
    ring->head = 0u;
    ring->tail = count;
    ring->count = count;
    for (u32 i = 0u; i < count; i++)
        ring->data[i] = first_idx + i;
    if (ring->tail == ring->capacity)
        ring->tail = 0u;
}

static RIX_UNUSED RIX_FORCE_INLINE u32
rix_ring_push_burst(struct rix_ring *ring, const u32 *idxv, u32 count)
{
    u32 n;

    RIX_ASSERT(ring != NULL);
    RIX_ASSERT(idxv != NULL || count == 0u);
    n = RIX_MIN(count, rix_ring_free_count(ring));
    if (n == 0u)
        return 0u;
    for (u32 i = 0u; i < n; i++)
        RIX_ASSERT(idxv[i] != RIX_NIL);
    memcpy(&ring->data[ring->count], idxv, (size_t)n * sizeof(*idxv));
    ring->count += n;
    return n;
}

static RIX_UNUSED RIX_FORCE_INLINE u32
rix_ring_pop_burst(struct rix_ring *ring, u32 *idxv, u32 count)
{
    u32 n;

    RIX_ASSERT(ring != NULL);
    RIX_ASSERT(idxv != NULL || count == 0u);
    n = RIX_MIN(count, ring->count);
    for (u32 i = 0u; i < n; i++)
        idxv[i] = ring->data[ring->count - 1u - i];
    ring->count -= n;
    return n;
}

static RIX_UNUSED RIX_FORCE_INLINE void
rix_ring_push_seq(struct rix_ring *ring, u32 first_idx, u32 count)
{
    RIX_ASSERT(ring != NULL);
    RIX_ASSERT(first_idx != RIX_NIL || count == 0u);
    RIX_ASSERT(count <= ring->capacity);
    ring->head = 0u;
    ring->tail = 0u;
    ring->count = count;
    for (u32 i = 0u; i < count; i++)
        ring->data[count - 1u - i] = first_idx + i;
}

#endif /* _RIX_RING_H_ */
