/* Copyright (c) 2026 Zhuozhao Xia.
 * Released under the same terms as Redis.
 */
#include "rvv_optim.h"

#include <riscv_vector.h>
#include <stdint.h>

size_t redisRvvVectorBytes(void) {
    return __riscv_vsetvlmax_e8m1();
}

const void *redisRvvMemchrImpl(const void *src, int value, size_t len) {
    const unsigned char *p = src;
    size_t left = len;

    while (left) {
        size_t vl = __riscv_vsetvl_e8m1(left);
        vuint8m1_t bytes = __riscv_vle8_v_u8m1(p, vl);
        vbool8_t matches = __riscv_vmseq_vx_u8m1_b8(bytes, (uint8_t)value, vl);
        long first = __riscv_vfirst_m_b8(matches, vl);
        if (first >= 0) return p + first;
        p += vl;
        left -= vl;
    }
    return NULL;
}

size_t redisRvvCommonPrefixImpl(const void *lhs, const void *rhs, size_t len) {
    const unsigned char *a = lhs;
    const unsigned char *b = rhs;
    size_t pos = 0;

    while (pos < len) {
        size_t vl = __riscv_vsetvl_e8m1(len - pos);
        vuint8m1_t va = __riscv_vle8_v_u8m1(a + pos, vl);
        vuint8m1_t vb = __riscv_vle8_v_u8m1(b + pos, vl);
        vbool8_t differs = __riscv_vmsne_vv_u8m1_b8(va, vb, vl);
        long first = __riscv_vfirst_m_b8(differs, vl);
        if (first >= 0) return pos + first;
        pos += vl;
    }
    return len;
}

/* LZF matches are bounded at 264 bytes. LMUL=8 covers them with one or two
 * comparisons on common VLENs, while the general string comparator keeps
 * LMUL=1 to reduce register pressure in database hot paths. */
size_t redisRvvCommonPrefixWideImpl(const void *lhs, const void *rhs, size_t len) {
    const unsigned char *a = lhs;
    const unsigned char *b = rhs;
    size_t pos = 0;

    while (pos < len) {
        size_t vl = __riscv_vsetvl_e8m8(len - pos);
        vuint8m8_t va = __riscv_vle8_v_u8m8(a + pos, vl);
        vuint8m8_t vb = __riscv_vle8_v_u8m8(b + pos, vl);
        vbool1_t differs = __riscv_vmsne_vv_u8m8_b1(va, vb, vl);
        long first = __riscv_vfirst_m_b1(differs, vl);
        if (first >= 0) return pos + first;
        pos += vl;
    }
    return len;
}

int redisRvvMemcmpImpl(const void *lhs, const void *rhs, size_t len) {
    const unsigned char *a = lhs;
    const unsigned char *b = rhs;
    size_t first = redisRvvCommonPrefixImpl(lhs, rhs, len);
    return first == len ? 0 : (int)a[first] - (int)b[first];
}

void *redisRvvMemcpyImpl(void *dst, const void *src, size_t len) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t left = len;

    while (left) {
        size_t vl = __riscv_vsetvl_e8m1(left);
        vuint8m1_t bytes = __riscv_vle8_v_u8m1(s, vl);
        __riscv_vse8_v_u8m1(d, bytes, vl);
        s += vl;
        d += vl;
        left -= vl;
    }
    return dst;
}

void *redisRvvMemsetImpl(void *dst, int value, size_t len) {
    unsigned char *d = dst;
    size_t left = len;

    while (left) {
        size_t vl = __riscv_vsetvl_e8m1(left);
        __riscv_vse8_v_u8m1(d, __riscv_vmv_v_x_u8m1((uint8_t)value, vl), vl);
        d += vl;
        left -= vl;
    }
    return dst;
}
