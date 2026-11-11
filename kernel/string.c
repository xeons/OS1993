#include "kernel.h"

/* REP STOSD / MOVSD for the bulk (fine on a 386), bytes for the rest. */
void *memset(void *d, int c, size_t n)
{
    uint32_t v = (uint8_t)c * 0x01010101u;
    void *p = d;
    size_t dwords = n / 4, bytes = n % 4;
    __asm__ volatile("cld\n\trep stosl\n\tmov %3, %%ecx\n\trep stosb"
                     : "+D"(p), "+c"(dwords) : "a"(v), "r"(bytes) : "memory");
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    void *dp = d;
    const void *sp = s;
    size_t dwords = n / 4, bytes = n % 4;
    __asm__ volatile("cld\n\trep movsl\n\tmov %3, %%ecx\n\trep movsb"
                     : "+D"(dp), "+S"(sp), "+c"(dwords) : "r"(bytes) : "memory");
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else {
        dp += n; sp += n;
        while (n--) *--dp = *--sp;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    return 0;
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c)
            r = s;
        if (!*s)
            return (char *)r;
    }
}
