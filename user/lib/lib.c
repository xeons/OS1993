/* user/lib/lib.c -- the small C runtime every module links its own copy of. */
#include "os1993.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strncpy0(char *d, const char *s, size_t size)
{
    size_t i = 0;
    if (!size)
        return d;
    for (; i < size - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = '\0';
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
