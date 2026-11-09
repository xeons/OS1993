/* user/lib.c -- the few C library functions ring-3 code needs. */
#include "os.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
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

#include <stdarg.h>

/* Debug log line through SYS_PRINT: %s, %d and %c only. */
void ulog(const char *fmt, ...)
{
    char buf[160];
    size_t n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt && n < sizeof buf - 12; fmt++) {
        if (*fmt != '%') { buf[n++] = *fmt; continue; }
        fmt++;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s && n < sizeof buf - 12) buf[n++] = *s++;
        } else if (*fmt == 'c') {
            buf[n++] = (char)va_arg(ap, int);
        } else if (*fmt == 'd') {
            int v = va_arg(ap, int);
            char d[12];
            int m = 0;
            unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
            do { d[m++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) buf[n++] = '-';
            while (m) buf[n++] = d[--m];
        } else if (!*fmt) {
            break;
        }
    }
    va_end(ap);
    buf[n] = '\0';
    sys_print(buf);
}
