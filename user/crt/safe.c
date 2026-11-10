/*
 * safe.c -- C11 Annex K's bounds-checked string and memory functions, and
 * the runtime-constraint handler.
 *
 * A broken constraint (a null pointer, a size of 0 or over RSIZE_MAX, a
 * destination too small, overlapping buffers) empties the destination
 * where there is one, then calls the handler; the default,
 * abort_handler_s, ends the program. ignore_handler_s returns, and the
 * function returns a nonzero errno_t.
 */
#include "crt.h"
#include <stdlib.h>
#include <string.h>

static constraint_handler_t handler;            /* NULL: abort_handler_s */

constraint_handler_t set_constraint_handler_s(constraint_handler_t h)
{
    constraint_handler_t old = handler ? handler : abort_handler_s;
    handler = h;
    return old;
}

void abort_handler_s(const char *restrict msg, void *restrict ptr, errno_t error)
{
    (void)ptr; (void)error;
    __crt_fail(FAIL_CONSTRAINT, msg);
}

void ignore_handler_s(const char *restrict msg, void *restrict ptr, errno_t error)
{
    (void)msg; (void)ptr; (void)error;
}

void __crt_constraint(const char *msg, errno_t e)
{
    (handler ? handler : abort_handler_s)(msg, NULL, e);
}

static bool overlap(const void *a, size_t an, const void *b, size_t bn)
{
    const char *x = a, *y = b;
    return x < y + bn && y < x + an;
}

errno_t memcpy_s(void *restrict d, rsize_t dmax, const void *restrict s, rsize_t n)
{
    const char *why = !d ? "memcpy_s: d is null" : dmax > RSIZE_MAX ? "memcpy_s: dmax > RSIZE_MAX" :
                      !s ? "memcpy_s: s is null" : n > dmax ? "memcpy_s: n > dmax" :
                      overlap(d, n, s, n) ? "memcpy_s: overlap" : NULL;
    if (why) {
        if (d && dmax <= RSIZE_MAX)
            memset(d, 0, dmax);
        __crt_constraint(why, EINVAL);
        return EINVAL;
    }
    memcpy(d, s, n);
    return 0;
}

errno_t memmove_s(void *d, rsize_t dmax, const void *s, rsize_t n)
{
    const char *why = !d ? "memmove_s: d is null" : dmax > RSIZE_MAX ? "memmove_s: dmax > RSIZE_MAX" :
                      !s ? "memmove_s: s is null" : n > dmax ? "memmove_s: n > dmax" : NULL;
    if (why) {
        if (d && dmax <= RSIZE_MAX)
            memset(d, 0, dmax);
        __crt_constraint(why, EINVAL);
        return EINVAL;
    }
    memmove(d, s, n);
    return 0;
}

errno_t memset_s(void *d, rsize_t dmax, int c, rsize_t n)
{
    const char *why = !d ? "memset_s: d is null" : dmax > RSIZE_MAX ? "memset_s: dmax > RSIZE_MAX" :
                      n > RSIZE_MAX ? "memset_s: n > RSIZE_MAX" : n > dmax ? "memset_s: n > dmax" : NULL;
    if (d && dmax <= RSIZE_MAX) {               /* as much as fits, even if it breaks a rule */
        memset(d, c, n < dmax ? n : dmax);
        __asm__ volatile("" : : "r"(d) : "memory");     /* never optimized away */
    }
    if (why) {
        __crt_constraint(why, EINVAL);
        return EINVAL;
    }
    return 0;
}

/* The checks every string copy shares; on failure d[0] = '\0' if allowed. */
static errno_t str_bad(char *d, rsize_t dmax, const char *s, const char *what)
{
    const char *why = !d ? "d is null" : !dmax ? "dmax is 0" : dmax > RSIZE_MAX ? "dmax > RSIZE_MAX" :
                      !s ? "s is null" : NULL;
    if (!why)
        return 0;
    if (d && dmax && dmax <= RSIZE_MAX)
        d[0] = '\0';
    char msg[48];
    strlcpy(msg, what, sizeof msg);
    strlcat(msg, ": ", sizeof msg);
    strlcat(msg, why, sizeof msg);
    __crt_constraint(msg, EINVAL);
    return EINVAL;
}

static errno_t str_fail(char *d, const char *why, errno_t e)
{
    d[0] = '\0';
    __crt_constraint(why, e);
    return e;
}

errno_t strcpy_s(char *restrict d, rsize_t dmax, const char *restrict s)
{
    if (str_bad(d, dmax, s, "strcpy_s"))
        return EINVAL;
    size_t n = strnlen(s, dmax);
    if (n == dmax)
        return str_fail(d, "strcpy_s: s doesn't fit", ERANGE);
    if (overlap(d, dmax, s, n + 1))
        return str_fail(d, "strcpy_s: overlap", EINVAL);
    memcpy(d, s, n + 1);
    return 0;
}

errno_t strncpy_s(char *restrict d, rsize_t dmax, const char *restrict s, rsize_t n)
{
    if (str_bad(d, dmax, s, "strncpy_s"))
        return EINVAL;
    if (n > RSIZE_MAX)
        return str_fail(d, "strncpy_s: n > RSIZE_MAX", EINVAL);
    size_t len = strnlen(s, n);
    if (len >= dmax)
        return str_fail(d, "strncpy_s: s doesn't fit", ERANGE);
    if (overlap(d, dmax, s, len))
        return str_fail(d, "strncpy_s: overlap", EINVAL);
    memcpy(d, s, len);
    d[len] = '\0';
    return 0;
}

errno_t strcat_s(char *restrict d, rsize_t dmax, const char *restrict s)
{
    if (str_bad(d, dmax, s, "strcat_s"))
        return EINVAL;
    size_t have = strnlen(d, dmax);
    if (have == dmax)
        return str_fail(d, "strcat_s: d isn't terminated", EINVAL);
    size_t n = strnlen(s, dmax - have);
    if (have + n == dmax)
        return str_fail(d, "strcat_s: s doesn't fit", ERANGE);
    if (overlap(d, dmax, s, n + 1))
        return str_fail(d, "strcat_s: overlap", EINVAL);
    memcpy(d + have, s, n + 1);
    return 0;
}

errno_t strncat_s(char *restrict d, rsize_t dmax, const char *restrict s, rsize_t n)
{
    if (str_bad(d, dmax, s, "strncat_s"))
        return EINVAL;
    if (n > RSIZE_MAX)
        return str_fail(d, "strncat_s: n > RSIZE_MAX", EINVAL);
    size_t have = strnlen(d, dmax);
    if (have == dmax)
        return str_fail(d, "strncat_s: d isn't terminated", EINVAL);
    size_t len = strnlen(s, n);
    if (have + len >= dmax)
        return str_fail(d, "strncat_s: s doesn't fit", ERANGE);
    if (overlap(d, dmax, s, len))
        return str_fail(d, "strncat_s: overlap", EINVAL);
    memcpy(d + have, s, len);
    d[have + len] = '\0';
    return 0;
}

size_t strnlen_s(const char *s, size_t max)
{
    return s ? strnlen(s, max) : 0;
}

char *strtok_s(char *restrict s, rsize_t *restrict smax, const char *restrict delim, char **restrict save)
{
    if (!smax || !delim || !save || (!s && !*save) || *smax > RSIZE_MAX) {
        __crt_constraint("strtok_s: a null argument or bad smax", EINVAL);
        return NULL;
    }
    char *p = s ? s : *save;
    rsize_t left = *smax;
    while (left && *p && strchr(delim, *p)) {    /* skip delimiters */
        p++;
        left--;
    }
    if (!left) {
        __crt_constraint("strtok_s: s isn't terminated within smax", EINVAL);
        return NULL;
    }
    if (!*p) {
        *save = p;
        *smax = left;
        return NULL;
    }
    char *tok = p;
    while (left && *p && !strchr(delim, *p)) {
        p++;
        left--;
    }
    if (!left) {
        __crt_constraint("strtok_s: s isn't terminated within smax", EINVAL);
        return NULL;
    }
    if (*p) {
        *p++ = '\0';
        left--;
    }
    *save = p;
    *smax = left;
    return tok;
}
