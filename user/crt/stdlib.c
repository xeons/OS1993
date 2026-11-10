/*
 * stdlib.c -- numbers from text, arithmetic, random numbers, sorting and
 * searching, the environment.
 */
#include "crt.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "os1993.h"

/* ---- numbers from text ---------------------------------------------------- */

/* The digits of s in base (0: by prefix), up to limit; overflow saturates
 * (ERANGE). No division: each step is checked with 32-bit halves. */
static unsigned long long parse(const char *s, char **end, int base, bool *neg, bool *over)
{
    const char *p = s;
    *neg = *over = false;
    if (end)
        *end = (char *)s;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        return 0;
    }
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+' || *p == '-')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    bool any = false;
    for (;; p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10 : 99;
        if (d >= base)
            break;
        any = true;
        if (*over)
            continue;
        uint64_t lo = (uint64_t)(uint32_t)v * (uint32_t)base;
        uint64_t hi = (uint64_t)(uint32_t)(v >> 32) * (uint32_t)base + (lo >> 32);
        if (hi >> 32) {
            *over = true;
            continue;
        }
        uint64_t nv = (hi << 32 | (uint32_t)lo) + (uint32_t)d;
        if (nv < (hi << 32 | (uint32_t)lo))
            *over = true;
        else
            v = nv;
    }
    if (any && end)
        *end = (char *)p;
    return v;
}

unsigned long long strtoull(const char *restrict s, char **restrict end, int base)
{
    bool neg, over;
    unsigned long long v = parse(s, end, base, &neg, &over);
    if (over) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return neg ? -v : v;
}

long long strtoll(const char *restrict s, char **restrict end, int base)
{
    bool neg, over;
    unsigned long long v = parse(s, end, base, &neg, &over);
    if (over || v > (neg ? (unsigned long long)LLONG_MAX + 1 : (unsigned long long)LLONG_MAX)) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? (long long)-v : (long long)v;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base)
{
    bool neg, over;
    unsigned long long v = parse(s, end, base, &neg, &over);
    if (over || v > ULONG_MAX) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return neg ? -(unsigned long)v : (unsigned long)v;
}

long strtol(const char *restrict s, char **restrict end, int base)
{
    bool neg, over;
    unsigned long long v = parse(s, end, base, &neg, &over);
    if (over || v > (neg ? (unsigned long long)LONG_MAX + 1 : (unsigned long long)LONG_MAX)) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? (long)-v : (long)v;
}

int       atoi(const char *s)  { return (int)strtol(s, NULL, 10); }
long      atol(const char *s)  { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* ---- arithmetic ------------------------------------------------------------- */

int       abs(int v)          { return v < 0 ? -v : v; }
long      labs(long v)        { return v < 0 ? -v : v; }
long long llabs(long long v)  { return v < 0 ? -v : v; }
div_t     div(int a, int b)   { return (div_t){ a / b, a % b }; }
ldiv_t    ldiv(long a, long b) { return (ldiv_t){ a / b, a % b }; }

/* ---- random numbers -------------------------------------------------------------- */

static uint32_t rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)(rand_state >> 16 & RAND_MAX);
}

void srand(unsigned seed) { rand_state = seed; }

int getrandom_bytes(void *buf, size_t n)
{
    if (!buf && n) {
        __crt_constraint("getrandom_bytes: buf is null", EINVAL);
        return EINVAL;
    }
    for (uint8_t *p = buf; n; ) {
        uint32_t take = n < 256 ? (uint32_t)n : 256;
        if (!GetRandomBytes(p, take))
            return EIO;
        p += take;
        n -= take;
    }
    return 0;
}

int rand_s(unsigned *out)
{
    if (!out) {
        __crt_constraint("rand_s: out is null", EINVAL);
        return EINVAL;
    }
    return getrandom_bytes(out, sizeof *out);
}

/* ---- sorting and searching ------------------------------------------------------- */

static void swap(char *a, char *b, size_t size)
{
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Heapsort: O(n log n) whatever the input, no recursion, no memory -- an
 * attacker choosing the data can't make it slow or run out of stack. */
static void heapsort_ctx(char *base, size_t n, size_t size,
                         int (*cmp)(const void *, const void *, void *), void *ctx)
{
    if (n < 2)
        return;
    for (size_t start = n / 2; start-- > 0; ) {
        for (size_t root = start; root * 2 + 1 < n; ) {
            size_t child = root * 2 + 1;
            if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size, ctx) < 0)
                child++;
            if (cmp(base + root * size, base + child * size, ctx) >= 0)
                break;
            swap(base + root * size, base + child * size, size);
            root = child;
        }
    }
    for (size_t end = n - 1; end > 0; end--) {
        swap(base, base + end * size, size);
        for (size_t root = 0; root * 2 + 1 < end; ) {
            size_t child = root * 2 + 1;
            if (child + 1 < end && cmp(base + child * size, base + (child + 1) * size, ctx) < 0)
                child++;
            if (cmp(base + root * size, base + child * size, ctx) >= 0)
                break;
            swap(base + root * size, base + child * size, size);
            root = child;
        }
    }
}

static int plain_cmp(const void *a, const void *b, void *ctx)
{
    return ((int (*)(const void *, const void *))ctx)(a, b);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    heapsort_ctx(base, n, size, plain_cmp, (void *)cmp);
}

errno_t qsort_s(void *base, rsize_t n, rsize_t size, int (*cmp)(const void *, const void *, void *), void *ctx)
{
    if (n > RSIZE_MAX || size > RSIZE_MAX || (n && (!base || !cmp))) {
        __crt_constraint("qsort_s: a null argument or a size over RSIZE_MAX", EINVAL);
        return EINVAL;
    }
    heapsort_ctx(base, n, size, cmp, ctx);
    return 0;
}

static void *search(const void *key, const char *base, size_t n, size_t size,
                    int (*cmp)(const void *, const void *, void *), void *ctx)
{
    while (n) {
        const char *mid = base + (n / 2) * size;
        int c = cmp(key, mid, ctx);
        if (!c)
            return (void *)mid;
        if (c > 0) {
            base = mid + size;
            n -= n / 2 + 1;
        } else {
            n /= 2;
        }
    }
    return NULL;
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    return search(key, base, n, size, plain_cmp, (void *)cmp);
}

void *bsearch_s(const void *key, const void *base, rsize_t n, rsize_t size,
                int (*cmp)(const void *, const void *, void *), void *ctx)
{
    if (n > RSIZE_MAX || size > RSIZE_MAX || (n && (!key || !base || !cmp))) {
        __crt_constraint("bsearch_s: a null argument or a size over RSIZE_MAX", EINVAL);
        return NULL;
    }
    return search(key, base, n, size, cmp, ctx);
}

/* ---- the environment ----------------------------------------------------------- */

char *getenv(const char *name) { (void)name; return NULL; }

errno_t getenv_s(size_t *restrict len, char *restrict d, rsize_t dmax, const char *restrict name)
{
    if (!name || dmax > RSIZE_MAX || (dmax && !d)) {
        if (len) *len = 0;
        __crt_constraint("getenv_s: name is null, or a bad d/dmax", EINVAL);
        return EINVAL;
    }
    if (len) *len = 0;
    if (d && dmax) d[0] = '\0';
    return ENOENT;                                  /* there is no environment */
}
