/* Standalone correctness test for src/rvv_optim.c.
 * Build this test for RV64GCV and run it at multiple emulated VLENs. */
#include "../../src/rvv_optim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SIZE 2064

static int signum(int value) {
    return (value > 0) - (value < 0);
}

int main(void) {
    unsigned char *a = malloc(TEST_SIZE);
    unsigned char *b = malloc(TEST_SIZE);
    unsigned char *dst = malloc(TEST_SIZE);
    unsigned char *ref = malloc(TEST_SIZE);
    if (!a || !b || !dst || !ref) return 2;

    uint32_t state = 0x2603;
    for (size_t i = 0; i < TEST_SIZE; i++) {
        state = state * 1664525u + 1013904223u;
        a[i] = state >> 24;
        b[i] = a[i];
    }

    for (size_t offset = 0; offset < 8; offset++) {
        for (size_t len = 0; len <= 2048; len++) {
            memcpy(dst, a, TEST_SIZE);
            memcpy(ref, a, TEST_SIZE);
            redisRvvMemcpy(dst + offset, b + 7, len);
            memcpy(ref + offset, b + 7, len);
            if (memcmp(dst, ref, TEST_SIZE) != 0) return 10;

            redisRvvMemset(dst + offset, 0xa5, len);
            memset(ref + offset, 0xa5, len);
            if (memcmp(dst, ref, TEST_SIZE) != 0) return 11;

            int needle = a[offset + (len ? len / 2 : 0)];
            const void *got = redisRvvMemchr(a + offset, needle, len);
            const void *expected = memchr(a + offset, needle, len);
            if (got != expected) return 12;

            memcpy(b, a, TEST_SIZE);
            if (len) b[offset + len - 1] ^= 0x80;
            int got_cmp = redisRvvMemcmp(a + offset, b + offset, len);
            int expected_cmp = memcmp(a + offset, b + offset, len);
            if (signum(got_cmp) != signum(expected_cmp)) return 13;

            size_t expected_prefix = len ? len - 1 : 0;
            if (redisRvvCommonPrefix(a + offset, b + offset, len) != expected_prefix)
                return 14;
            if (redisRvvCommonPrefixWide(a + offset, b + offset, len) != expected_prefix)
                return 15;
        }
    }

    printf("RVV primitives passed (VLEN=%zu bits)\n", redisRvvVectorBytes() * 8);
    free(a);
    free(b);
    free(dst);
    free(ref);
    return 0;
}
