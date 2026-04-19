/* test_rix_hash_extra.c - slot_extra variant tests */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "rix_hash.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

static void
test_bucket_layout(void)
{
    printf("[T] bucket layout\n");
    if (sizeof(struct rix_hash_bucket_extra_s) != 192u)
        FAILF("bucket size expected 192, got %zu",
              sizeof(struct rix_hash_bucket_extra_s));
    if (_Alignof(struct rix_hash_bucket_extra_s) != 64u)
        FAILF("bucket align expected 64, got %zu",
              _Alignof(struct rix_hash_bucket_extra_s));
    if (offsetof(struct rix_hash_bucket_extra_s, extra) != 128u)
        FAILF("extra offset expected 128, got %zu",
              offsetof(struct rix_hash_bucket_extra_s, extra));
}

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    test_bucket_layout();
    printf("PASS\n");
    return 0;
}
