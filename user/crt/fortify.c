/*
 * fortify.c -- _FORTIFY_SOURCE's checks. With it on, the compiler turns a
 * copy into an object whose size it knows into one of these (when it can't
 * prove the copy fits); one that would overrun ends the program.
 */
#include "crt.h"
#include <string.h>

__attribute__((noreturn, cold))
static void overrun(const char *fn)
{
    __crt_fail(FAIL_BUFFER_OVERRUN, fn);
}

void *__memcpy_chk(void *d, const void *s, size_t n, size_t dsize)
{
    if (n > dsize) overrun("memcpy");
    return memcpy(d, s, n);
}

void *__memmove_chk(void *d, const void *s, size_t n, size_t dsize)
{
    if (n > dsize) overrun("memmove");
    return memmove(d, s, n);
}

void *__memset_chk(void *d, int c, size_t n, size_t dsize)
{
    if (n > dsize) overrun("memset");
    return memset(d, c, n);
}

char *__strcpy_chk(char *d, const char *s, size_t dsize)
{
    if (strlen(s) >= dsize) overrun("strcpy");
    return strcpy(d, s);
}

char *__strncpy_chk(char *d, const char *s, size_t n, size_t dsize)
{
    if (n > dsize) overrun("strncpy");
    return strncpy(d, s, n);
}

char *__strcat_chk(char *d, const char *s, size_t dsize)
{
    size_t have = strnlen(d, dsize);
    if (have == dsize || strlen(s) >= dsize - have) overrun("strcat");
    return strcat(d, s);
}

char *__strncat_chk(char *d, const char *s, size_t n, size_t dsize)
{
    size_t have = strnlen(d, dsize);
    if (have == dsize || strnlen(s, n) >= dsize - have) overrun("strncat");
    return strncat(d, s, n);
}

void __chk_fail(void) { overrun("a fortified call"); }
