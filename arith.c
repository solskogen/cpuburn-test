#include "kernel.h"

/* Keep compiler helpers on the same i486 baseline as the kernel, not the
   host distribution's libgcc (which may contain newer instructions). */
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    if (!d) {
        __asm__ volatile("divl %0" :: "r"(0) : "eax", "edx", "cc");
        __builtin_unreachable();
    }
    for (int i = 63; i >= 0; i--) {
        u32 carry = (u32)(r >> 63);
        r = (r << 1) | ((n >> i) & 1);
        if (carry || r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, NULL); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }
s64 __divdi3(s64 n, s64 d) {
    u64 a = n < 0 ? 0ull - (u64)n : (u64)n;
    u64 b = d < 0 ? 0ull - (u64)d : (u64)d;
    u64 q = __udivdi3(a, b);
    return (s64)((n < 0) != (d < 0) ? 0ull - q : q);
}
s64 __moddi3(s64 n, s64 d) {
    u64 a = n < 0 ? 0ull - (u64)n : (u64)n;
    u64 b = d < 0 ? 0ull - (u64)d : (u64)d;
    u64 r = __umoddi3(a, b);
    return (s64)(n < 0 ? 0ull - r : r);
}

/* Compilers may emit these for aggregate initialisation/copies even in
   freestanding mode (clang does for large zero-initialised locals). String
   instructions keep them i486-safe and prevent idiom recognition from
   turning a byte loop back into a recursive call. */
void *memset(void *d, int c, unsigned long n) {
    void *p = d;
    __asm__ volatile("cld; rep stosb" : "+D"(p), "+c"(n) : "a"(c) : "memory", "cc");
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    void *p = d;
    __asm__ volatile("cld; rep movsb" : "+D"(p), "+S"(s), "+c"(n) :: "memory", "cc");
    return d;
}
void *memmove(void *d, const void *s, unsigned long n) {
    if ((unsigned long)d - (unsigned long)s >= n) return memcpy(d, s, n);
    void *p = (char *)d + n - 1;
    s = (const char *)s + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(p), "+S"(s), "+c"(n) :: "memory", "cc");
    return d;
}
int memcmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}
