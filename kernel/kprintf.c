/*
 * kprintf.c -- minimal printf: %c %s %d %i %u %x %X %p %%, with '-', '0'
 * and width. 32-bit only, so no libgcc helpers are needed.
 */
#include "kernel.h"
#include "console.h"
#include "serial.h"

struct out { char *buf; size_t size, len; };

static void emit(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void emit_padded(struct out *o, const char *s, size_t n,
                        int width, bool left, char pad)
{
    int fill = width > (int)n ? width - (int)n : 0;
    if (!left)
        while (fill-- > 0) emit(o, pad);
    while (n--) emit(o, *s++);
    if (left)
        while (fill-- > 0) emit(o, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    char tmp[12];

    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        bool left = false;
        char pad = ' ';
        int width = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            emit_padded(&o, tmp, 1, width, left, ' ');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            emit_padded(&o, s, strlen(s), width, left, ' ');
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p': {
            uint32_t v;
            bool neg = false;
            unsigned base = 10;
            const char *digits = "0123456789abcdef";
            if (*fmt == 'd' || *fmt == 'i') {
                int32_t sv = va_arg(ap, int32_t);
                neg = sv < 0;
                v = neg ? -(uint32_t)sv : (uint32_t)sv;
            } else {
                v = va_arg(ap, uint32_t);
                if (*fmt != 'u') base = 16;
                if (*fmt == 'X') digits = "0123456789ABCDEF";
                if (*fmt == 'p') { pad = '0'; width = 8; digits = "0123456789ABCDEF"; }
            }
            size_t n = 0;
            do { tmp[sizeof tmp - 1 - n++] = digits[v % base]; v /= base; } while (v);
            if (neg) {
                if (pad == '0') { emit(&o, '-'); if (width) width--; }
                else tmp[sizeof tmp - 1 - n++] = '-';
            }
            emit_padded(&o, tmp + sizeof tmp - n, n, width, left, pad);
            break;
        }
        case '%':
            emit(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            emit(&o, '%');
            emit(&o, *fmt);
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_puts(buf);
    serial_puts(buf);
}

void klog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    serial_puts(buf);
}
