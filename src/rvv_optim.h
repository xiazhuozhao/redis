/* RVV 1.0 primitives used by Redis hot paths.
 *
 * The scalar aliases keep normal upstream builds unchanged. BUILD_RVV=yes
 * enables the out-of-line RVV implementation without allowing the compiler
 * to auto-vectorize the rest of Redis.
 */
#ifndef REDIS_RVV_OPTIM_H
#define REDIS_RVV_OPTIM_H

#include <stddef.h>
#include <string.h>

#ifdef REDIS_RVV
#define REDIS_RVV_BYTE_THRESHOLD 64

void *redisRvvMemcpyImpl(void *dst, const void *src, size_t len);
void *redisRvvMemsetImpl(void *dst, int value, size_t len);
const void *redisRvvMemchrImpl(const void *src, int value, size_t len);
int redisRvvMemcmpImpl(const void *lhs, const void *rhs, size_t len);
size_t redisRvvCommonPrefixImpl(const void *lhs, const void *rhs, size_t len);
size_t redisRvvCommonPrefixWideImpl(const void *lhs, const void *rhs, size_t len);
size_t redisRvvVectorBytes(void);

static inline void *redisRvvMemcpy(void *dst, const void *src, size_t len) {
    return len < REDIS_RVV_BYTE_THRESHOLD ? memcpy(dst, src, len) :
                                            redisRvvMemcpyImpl(dst, src, len);
}

static inline void *redisRvvMemset(void *dst, int value, size_t len) {
    return len < REDIS_RVV_BYTE_THRESHOLD ? memset(dst, value, len) :
                                            redisRvvMemsetImpl(dst, value, len);
}

static inline const void *redisRvvMemchr(const void *src, int value, size_t len) {
    return len < REDIS_RVV_BYTE_THRESHOLD ? memchr(src, value, len) :
                                            redisRvvMemchrImpl(src, value, len);
}

static inline int redisRvvMemcmp(const void *lhs, const void *rhs, size_t len) {
    return len < REDIS_RVV_BYTE_THRESHOLD ? memcmp(lhs, rhs, len) :
                                            redisRvvMemcmpImpl(lhs, rhs, len);
}

static inline size_t redisRvvCommonPrefix(const void *lhs, const void *rhs, size_t len) {
    if (len >= REDIS_RVV_BYTE_THRESHOLD)
        return redisRvvCommonPrefixImpl(lhs, rhs, len);
    const unsigned char *a = lhs;
    const unsigned char *b = rhs;
    size_t pos = 0;
    while (pos < len && a[pos] == b[pos]) pos++;
    return pos;
}

static inline size_t redisRvvCommonPrefixWide(const void *lhs, const void *rhs, size_t len) {
    return len < REDIS_RVV_BYTE_THRESHOLD ? redisRvvCommonPrefix(lhs, rhs, len) :
                                            redisRvvCommonPrefixWideImpl(lhs, rhs, len);
}
#else
#define redisRvvMemcpy(dst, src, len) memcpy((dst), (src), (len))
#define redisRvvMemset(dst, value, len) memset((dst), (value), (len))
#define redisRvvMemchr(src, value, len) memchr((src), (value), (len))
#define redisRvvMemcmp(lhs, rhs, len) memcmp((lhs), (rhs), (len))

static inline size_t redisRvvCommonPrefix(const void *lhs, const void *rhs, size_t len) {
    const unsigned char *a = lhs;
    const unsigned char *b = rhs;
    size_t pos = 0;
    while (pos < len && a[pos] == b[pos]) pos++;
    return pos;
}

#define redisRvvCommonPrefixWide(lhs, rhs, len) redisRvvCommonPrefix((lhs), (rhs), (len))

static inline size_t redisRvvVectorBytes(void) {
    return 0;
}
#endif

#endif
