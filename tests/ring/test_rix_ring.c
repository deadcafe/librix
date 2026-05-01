/* test_rix_ring.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/rix/rix_ring.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

static void
check_state(const struct rix_ring *ring,
            u32 count,
            u32 capacity,
            const char *tag)
{
    if (rix_ring_count(ring) != count)
        FAILF("%s count: got=%u want=%u", tag, rix_ring_count(ring), count);
    if (rix_ring_free_count(ring) != capacity - count)
        FAILF("%s free_count: got=%u want=%u", tag,
              rix_ring_free_count(ring), capacity - count);
    if (!!rix_ring_empty(ring) != (count == 0u))
        FAILF("%s empty mismatch count=%u", tag, count);
    if (!!rix_ring_full(ring) != (count == capacity))
        FAILF("%s full mismatch count=%u capacity=%u", tag, count, capacity);
}

static void
check_vec(const u32 *got,
          const u32 *want,
          u32 count,
          const char *tag)
{
    for (u32 i = 0u; i < count; i++) {
        if (got[i] != want[i])
            FAILF("%s[%u]: got=%u want=%u", tag, i, got[i], want[i]);
    }
}

static void
test_zero_capacity(void)
{
    struct rix_ring ring;
    u32 out = 99u;
    u32 in = 1u;

    rix_ring_init(&ring, NULL, 0u);
    check_state(&ring, 0u, 0u, "zero init");

    if (rix_ring_enqueue_burst(&ring, &in, 1u) != 0u)
        FAIL("zero enqueue should move nothing");
    if (rix_ring_dequeue_burst(&ring, &out, 1u) != 0u)
        FAIL("zero dequeue should move nothing");
    if (rix_ring_push_burst(&ring, &in, 1u) != 0u)
        FAIL("zero push should move nothing");
    if (rix_ring_pop_burst(&ring, &out, 1u) != 0u)
        FAIL("zero pop should move nothing");

    rix_ring_enqueue_seq(&ring, 1u, 0u);
    check_state(&ring, 0u, 0u, "zero enqueue_seq");
    rix_ring_push_seq(&ring, 1u, 0u);
    check_state(&ring, 0u, 0u, "zero push_seq");

    if (rix_ring_enqueue_burst(&ring, NULL, 0u) != 0u)
        FAIL("zero null enqueue should move nothing");
    if (rix_ring_dequeue_burst(&ring, NULL, 0u) != 0u)
        FAIL("zero null dequeue should move nothing");
    if (rix_ring_push_burst(&ring, NULL, 0u) != 0u)
        FAIL("zero null push should move nothing");
    if (rix_ring_pop_burst(&ring, NULL, 0u) != 0u)
        FAIL("zero null pop should move nothing");
}

static void
test_fifo_wrap_and_partial(void)
{
    struct rix_ring ring;
    u32 storage[5];
    u32 out[8];
    u32 in[] = { 20u, 21u, 22u, 23u };
    const u32 want_first[] = { 10u, 11u, 12u };
    const u32 want_wrap[] = { 13u, 20u, 21u, 22u, 23u };
    const u32 want_partial[] = { 31u, 32u, 20u, 21u, 22u };
    u32 n;

    rix_ring_init(&ring, storage, RIX_COUNT_OF(storage));
    rix_ring_enqueue_seq(&ring, 10u, 4u);
    check_state(&ring, 4u, RIX_COUNT_OF(storage), "fifo seq");

    n = rix_ring_dequeue_burst(&ring, out, 3u);
    if (n != 3u)
        FAILF("fifo first dequeue count: got=%u", n);
    check_vec(out, want_first, n, "fifo first dequeue");
    check_state(&ring, 1u, RIX_COUNT_OF(storage), "fifo after first dequeue");

    n = rix_ring_enqueue_burst(&ring, in, RIX_COUNT_OF(in));
    if (n != RIX_COUNT_OF(in))
        FAILF("fifo wrap enqueue count: got=%u", n);
    check_state(&ring, 5u, RIX_COUNT_OF(storage), "fifo full wrap");

    n = rix_ring_enqueue_burst(&ring, in, RIX_COUNT_OF(in));
    if (n != 0u)
        FAILF("fifo full enqueue count: got=%u", n);

    n = rix_ring_dequeue_burst(&ring, out, RIX_COUNT_OF(out));
    if (n != RIX_COUNT_OF(want_wrap))
        FAILF("fifo wrap dequeue count: got=%u", n);
    check_vec(out, want_wrap, n, "fifo wrap dequeue");
    check_state(&ring, 0u, RIX_COUNT_OF(storage), "fifo empty after wrap");

    rix_ring_enqueue_seq(&ring, 30u, 3u);
    n = rix_ring_dequeue_burst(&ring, out, 1u);
    if (n != 1u || out[0] != 30u)
        FAIL("fifo partial setup dequeue mismatch");
    n = rix_ring_enqueue_burst(&ring, in, RIX_COUNT_OF(in));
    if (n != 3u)
        FAILF("fifo partial enqueue count: got=%u", n);
    n = rix_ring_dequeue_burst(&ring, out, RIX_COUNT_OF(out));
    if (n != RIX_COUNT_OF(want_partial))
        FAILF("fifo partial dequeue count: got=%u", n);
    check_vec(out, want_partial, n, "fifo partial dequeue");
}

static void
test_lifo_partial_and_reset(void)
{
    struct rix_ring ring;
    u32 storage[4];
    u32 out[8];
    u32 in_full[] = { 1u, 2u, 3u, 4u, 5u, 6u };
    u32 in_more[] = { 7u, 8u, 9u };
    const u32 want_top[] = { 4u, 3u };
    const u32 want_final[] = { 8u, 7u, 2u, 1u };
    const u32 want_seq[] = { 40u, 41u, 42u, 43u };
    u32 n;

    rix_ring_init(&ring, storage, RIX_COUNT_OF(storage));
    n = rix_ring_push_burst(&ring, in_full, RIX_COUNT_OF(in_full));
    if (n != RIX_COUNT_OF(storage))
        FAILF("lifo partial push count: got=%u", n);
    check_state(&ring, 4u, RIX_COUNT_OF(storage), "lifo full");

    n = rix_ring_push_burst(&ring, in_more, RIX_COUNT_OF(in_more));
    if (n != 0u)
        FAILF("lifo full push count: got=%u", n);

    n = rix_ring_pop_burst(&ring, out, 2u);
    if (n != RIX_COUNT_OF(want_top))
        FAILF("lifo top pop count: got=%u", n);
    check_vec(out, want_top, n, "lifo top pop");
    check_state(&ring, 2u, RIX_COUNT_OF(storage), "lifo after top pop");

    n = rix_ring_push_burst(&ring, in_more, RIX_COUNT_OF(in_more));
    if (n != 2u)
        FAILF("lifo second partial push count: got=%u", n);

    n = rix_ring_pop_burst(&ring, out, RIX_COUNT_OF(out));
    if (n != RIX_COUNT_OF(want_final))
        FAILF("lifo final pop count: got=%u", n);
    check_vec(out, want_final, n, "lifo final pop");
    check_state(&ring, 0u, RIX_COUNT_OF(storage), "lifo empty");

    rix_ring_push_seq(&ring, 40u, RIX_COUNT_OF(storage));
    n = rix_ring_pop_burst(&ring, out, RIX_COUNT_OF(out));
    if (n != RIX_COUNT_OF(want_seq))
        FAILF("lifo push_seq pop count: got=%u", n);
    check_vec(out, want_seq, n, "lifo push_seq pop");

    rix_ring_push_seq(&ring, 50u, 3u);
    rix_ring_reset(&ring);
    check_state(&ring, 0u, RIX_COUNT_OF(storage), "lifo reset");
    n = rix_ring_pop_burst(&ring, out, RIX_COUNT_OF(out));
    if (n != 0u)
        FAILF("lifo pop after reset count: got=%u", n);
}

int
main(void)
{
    test_zero_capacity();
    test_fifo_wrap_and_partial();
    test_lifo_partial_and_reset();
    puts("rix_ring tests passed");
    return 0;
}
