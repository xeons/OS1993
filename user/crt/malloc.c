/*
 * malloc.c -- the C heap: KERNEL.DLL's process heap (LocalAlloc), with
 * checks around every block.
 *
 *     [ size | cookie | state | 0 ] [ the caller's bytes ] [ guard ]
 *
 * The cookie mixes the size and the block's address with a secret the
 * kernel's random numbers choose when the heap is first used; the guard
 * after the bytes is derived the same way. free and realloc check both,
 * and the state: freeing a block twice, or one whose header or guard was
 * overwritten (a heap overrun), or a pointer malloc never returned, ends
 * the program rather than corrupting the heap further. Freed bytes are
 * overwritten (0xDD), so what they held can't leak into a later block.
 */
#include "crt.h"
#include <stdlib.h>
#include <string.h>
#include "os1993.h"

#define LIVE  0xA110CA7Eu
#define FREED 0xF7EEF7EEu

struct hdr {
    uint32_t size;
    uint32_t cookie;
    uint32_t state;
    uint32_t zero;
};

static uint32_t secret;

static void init_secret(void)
{
    while (!secret)
        if (!GetRandomBytes(&secret, sizeof secret))
            secret = GetTickCount() * 0x9E3779B1u ^ 0x5EC2E7u;
}

static uint32_t cookie_of(const struct hdr *h, uint32_t size, uint32_t state)
{
    uint32_t v = size ^ (uint32_t)(uintptr_t)h ^ state ^ secret;
    return (v << 13 | v >> 19) * 0x85EBCA6Bu;
}

static uint32_t guard_of(const struct hdr *h)
{
    return ((uint32_t)(uintptr_t)h + 0x7F4A7C15u) * 0xC2B2AE35u ^ secret;
}

void *malloc(size_t n)
{
    if (n > RSIZE_MAX - sizeof(struct hdr) - 4) {
        errno = ENOMEM;
        return NULL;
    }
    __crt_lock(CRT_LOCK_HEAP);
    init_secret();
    struct hdr *h = LocalAlloc(0, (uint32_t)(sizeof *h + n + 4));
    __crt_unlock(CRT_LOCK_HEAP);
    if (!h) {
        errno = ENOMEM;
        return NULL;
    }
    h->size = (uint32_t)n;
    h->state = LIVE;
    h->zero = 0;
    h->cookie = cookie_of(h, h->size, LIVE);
    uint32_t g = guard_of(h);
    memcpy((uint8_t *)(h + 1) + n, &g, 4);         /* may be unaligned */
    return h + 1;
}

/* The block behind p, checked; a bad one ends the program. */
static struct hdr *check(void *p, const char *fn)
{
    struct hdr *h = (struct hdr *)p - 1;
    if ((uintptr_t)p & 3)
        __crt_fail(FAIL_HEAP_CORRUPT, fn);
    if (h->state == FREED && h->cookie == cookie_of(h, h->size, FREED))
        __crt_fail(FAIL_HEAP_CORRUPT, "double free");
    if (h->state != LIVE || h->zero || h->cookie != cookie_of(h, h->size, LIVE))
        __crt_fail(FAIL_HEAP_CORRUPT, "a block's header was overwritten (or not from malloc)");
    uint32_t g;
    memcpy(&g, (uint8_t *)p + h->size, 4);
    if (g != guard_of(h))
        __crt_fail(FAIL_HEAP_CORRUPT, "a write ran past the end of a block");
    return h;
}

void free(void *p)
{
    if (!p)
        return;
    __crt_lock(CRT_LOCK_HEAP);
    struct hdr *h = check(p, "free: a misaligned pointer");
    memset(p, 0xDD, h->size + 4);
    h->state = FREED;
    h->cookie = cookie_of(h, h->size, FREED);
    LocalFree(h);
    __crt_unlock(CRT_LOCK_HEAP);
}

size_t malloc_usable_size(void *p)
{
    return p ? check(p, "malloc_usable_size: a misaligned pointer")->size : 0;
}

void *calloc(size_t count, size_t size)
{
    if (size && count > RSIZE_MAX / size) {         /* the multiplication would overflow */
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    struct hdr *h = check(p, "realloc: a misaligned pointer");
    if (!n) {                                       /* as free (and nothing to use) */
        free(p);
        return NULL;
    }
    void *q = malloc(n);
    if (!q)
        return NULL;                                /* p is still good */
    memcpy(q, p, h->size < n ? h->size : n);
    free(p);
    return q;
}

void *reallocarray(void *p, size_t count, size_t size)
{
    if (size && count > RSIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, count * size);
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    return d ? memcpy(d, s, n) : NULL;
}

char *strndup(const char *s, size_t max)
{
    size_t n = strnlen(s, max);
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}
