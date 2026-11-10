/*
 * string.c -- strings and memory.
 *
 * Nothing here calls another module, so any module can link it statically
 * (KERNEL.DLL and GFX.DLL do).
 */
#include "crt.h"
#include <string.h>

void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (n >= 8 && !(((uintptr_t)dp ^ (uintptr_t)sp) & 3)) {    /* same alignment: words */
        while ((uintptr_t)dp & 3) { *dp++ = *sp++; n--; }
        uint32_t *dw = (uint32_t *)dp;
        const uint32_t *sw = (const uint32_t *)sp;
        for (; n >= 4; n -= 4)
            *dw++ = *sw++;
        dp = (uint8_t *)dw;
        sp = (const uint8_t *)sw;
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp == sp || !n)
        return d;
    if (dp < sp || dp >= sp + n)
        return memcpy(d, s, n);
    while (n--)                                 /* overlapping, upwards: from the end */
        dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    if (n >= 8) {
        while ((uintptr_t)p & 3) { *p++ = (uint8_t)c; n--; }
        uint32_t w = (uint8_t)c * 0x01010101u, *pw = (uint32_t *)p;
        for (; n >= 4; n -= 4)
            *pw++ = w;
        p = (uint8_t *)pw;
    }
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

void explicit_bzero(void *d, size_t n)
{
    memset(d, 0, n);
    __asm__ volatile("" : : "r"(d) : "memory");     /* the zeros must be stored */
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n--; x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

int timingsafe_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    int res = 0, done = 0;
    for (size_t i = 0; i < n; i++) {            /* the first difference, without branching */
        int lt = (x[i] - y[i]) >> 8;            /* -1 if x < y, else 0 */
        int gt = (y[i] - x[i]) >> 8;            /* -1 if x > y, else 0 */
        res |= (lt - gt) & ~done;
        done |= lt | gt;
    }
    return res;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    for (; n--; p++)
        if (*p == (uint8_t)c)
            return (void *)p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (uint8_t)*a - (uint8_t)*b;
        if (!*a)
            break;
    }
    return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower((uint8_t)*a) == lower((uint8_t)*b))
        a++, b++;
    return lower((uint8_t)*a) - lower((uint8_t)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = lower((uint8_t)*a), y = lower((uint8_t)*b);
        if (x != y)
            return x - y;
        if (!x)
            break;
    }
    return 0;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

char *strcpy(char *restrict d, const char *restrict s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *strncpy(char *restrict d, const char *restrict s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = '\0';
    return d;
}

char *strcat(char *restrict d, const char *restrict s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *restrict d, const char *restrict s, size_t n)
{
    char *p = d + strlen(d);
    while (n-- && *s)
        *p++ = *s++;
    *p = '\0';
    return d;
}

size_t strlcpy(char *restrict d, const char *restrict s, size_t size)
{
    size_t n = strlen(s);
    if (size) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(d, s, k);
        d[k] = '\0';
    }
    return n;
}

size_t strlcat(char *restrict d, const char *restrict s, size_t size)
{
    size_t have = strnlen(d, size);
    if (have == size)                           /* not terminated within size */
        return size + strlen(s);
    return have + strlcpy(d + have, s, size - have);
}

char *strncpy0(char *d, const char *s, size_t size)
{
    strlcpy(d, s, size);
    return d;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n))
            return (char *)hay;
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *restrict s, const char *restrict delim, char **restrict save)
{
    if (!s)
        s = *save;
    if (!s)
        return NULL;
    s += strspn(s, delim);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    char *end = s + strcspn(s, delim);
    if (*end)
        *end++ = '\0';
    else
        end = NULL;
    *save = end;
    return s;
}

char *strtok(char *restrict s, const char *restrict delim)
{
    static char *save;
    return strtok_r(s, delim, &save);
}
