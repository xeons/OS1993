/*
 * printf.c -- formatted output: the engine, and the string versions.
 *
 * Conversions: d i u o x X c s p %, with the flags - + space # 0, a width
 * and a precision (numbers or *), and the lengths hh h l ll z t j. There
 * is no floating point (no FPU on a 486SX) and no %n: it writes through a
 * pointer, the classic format-string hole, so it is a broken constraint
 * here, as in the Annex K functions. 64-bit numbers are divided 16 bits at
 * a time, so there is no call to libgcc's 64-bit division.
 */
#include "crt.h"
#include <stdio.h>
#include <string.h>

/* v / base, returning the remainder (base <= 16). */
static unsigned divmod64(uint64_t *v, unsigned base)
{
    uint32_t hi = (uint32_t)(*v >> 32), lo = (uint32_t)*v;
    uint32_t parts[4] = { hi >> 16, hi & 0xFFFF, lo >> 16, lo & 0xFFFF }, rem = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t cur = rem << 16 | parts[i];
        parts[i] = cur / base;
        rem = cur % base;
    }
    *v = (uint64_t)(parts[0] << 16 | parts[1]) << 32 | (parts[2] << 16 | parts[3]);
    return rem;
}

static void put_pad(struct fmt_out *o, char c, int n)
{
    char buf[16];
    memset(buf, c, sizeof buf);
    for (; n > 0; n -= 16)
        o->put(o, buf, n < 16 ? (size_t)n : 16);
}

enum { F_LEFT = 1, F_PLUS = 2, F_SPACE = 4, F_ALT = 8, F_ZERO = 16 };

int __crt_format(struct fmt_out *o, const char *fmt, va_list ap, unsigned mode)
{
    o->count = 0;
    while (*fmt) {
        const char *lit = fmt;
        while (*fmt && *fmt != '%')
            fmt++;
        if (fmt > lit)
            o->put(o, lit, (size_t)(fmt - lit));
        if (!*fmt)
            break;
        fmt++;                                      /* the '%' */

        unsigned flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_ALT;
            else if (*fmt == '0') flags |= F_ZERO;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_LEFT; width = width == -2147483647 - 1 ? 0 : -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width < 100000 ? width * 10 + (*fmt++ - '0') : (fmt++, width);
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec < 100000 ? prec * 10 + (*fmt++ - '0') : (fmt++, prec);
            }
        }
        int len = 0;                                /* 1 h, 2 hh, 3 l, 4 ll, 5 z/t, 6 j */
        if (*fmt == 'h') { len = 1; if (*++fmt == 'h') { len = 2; fmt++; } }
        else if (*fmt == 'l') { len = 3; if (*++fmt == 'l') { len = 4; fmt++; } }
        else if (*fmt == 'z' || *fmt == 't') { len = 5; fmt++; }
        else if (*fmt == 'j') { len = 6; fmt++; }

        char conv = *fmt ? *fmt++ : '\0';
        char buf[24], *digits = buf + sizeof buf, sign = 0;
        const char *s = NULL, *prefix = "";
        size_t n = 0;
        switch (conv) {
        case '%':
            o->put(o, "%", 1);
            continue;
        case 'c':
            buf[0] = (char)va_arg(ap, int);
            s = buf;
            n = 1;
            prec = -1;
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) {
                if (mode & FMT_SECURE) {
                    __crt_constraint("printf_s: %s with a null pointer", EINVAL);
                    return -1;
                }
                s = "(null)";
            }
            n = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
            prec = -1;
            break;
        case 'n':
            __crt_constraint("printf: %n is not supported", EINVAL);
            return -1;
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            uint64_t v;
            bool is_signed = conv == 'd' || conv == 'i';
            if (conv == 'p') {
                v = (uintptr_t)va_arg(ap, void *);
                flags |= F_ALT;
                if (prec < 0) prec = 8;
            } else if (len == 4 || len == 6) {
                v = va_arg(ap, unsigned long long);
            } else {
                uint32_t w = va_arg(ap, unsigned);
                if (len == 1) w = is_signed ? (uint32_t)(int16_t)w : (uint16_t)w;
                if (len == 2) w = is_signed ? (uint32_t)(int8_t)w : (uint8_t)w;
                v = is_signed ? (uint64_t)(int64_t)(int32_t)w : w;
            }
            if (is_signed && (int64_t)v < 0) {
                sign = '-';
                v = -v;
            } else if (is_signed && (flags & (F_PLUS | F_SPACE))) {
                sign = flags & F_PLUS ? '+' : ' ';
            }
            unsigned base = conv == 'o' ? 8 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : 10;
            const char *set = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            bool zero = v == 0;
            while (v)
                *--digits = set[divmod64(&v, base)];
            n = (size_t)(buf + sizeof buf - digits);
            if (prec >= 0)
                flags &= ~F_ZERO;                   /* a precision overrides '0' */
            else
                prec = 1;
            if ((flags & F_ALT) && base == 16 && (!zero || conv == 'p'))
                prefix = conv == 'X' ? "0X" : "0x";
            if ((flags & F_ALT) && base == 8 && (size_t)prec <= n)
                prec = (int)n + 1;                  /* a leading 0 */
            s = digits;
            break;
        }
        default:                                    /* unknown: show it as it was */
            o->put(o, "%", 1);
            if (conv)
                o->put(o, fmt - 1, 1);
            continue;
        }

        int zeros = prec > (int)n ? prec - (int)n : 0;
        int body = (int)n + zeros + (sign != 0) + (int)strlen(prefix);
        int pad = width > body ? width - body : 0;
        if (!(flags & (F_LEFT | F_ZERO)))
            put_pad(o, ' ', pad);
        if (sign)
            o->put(o, &sign, 1);
        o->put(o, prefix, strlen(prefix));
        if ((flags & (F_LEFT | F_ZERO)) == F_ZERO)
            put_pad(o, '0', pad);
        put_pad(o, '0', zeros);
        o->put(o, s, n);
        if (flags & F_LEFT)
            put_pad(o, ' ', pad);
    }
    return o->count > 0x7FFFFFFF ? -1 : (int)o->count;
}

/* ---- into a string -------------------------------------------------------- */

struct str_out {
    struct fmt_out o;
    char  *s;
    size_t cap;                                     /* room, the terminator included */
};

static void str_put(struct fmt_out *o, const char *s, size_t n)
{
    struct str_out *so = (struct str_out *)o;
    if (so->cap && o->count < so->cap - 1) {
        size_t room = so->cap - 1 - o->count;
        memcpy(so->s + o->count, s, n < room ? n : room);
    }
    o->count += n;
}

static int to_string(char *s, size_t cap, const char *fmt, va_list ap, unsigned mode)
{
    struct str_out so = { { str_put, 0 }, s, cap };
    int r = __crt_format(&so.o, fmt, ap, mode);
    if (cap)
        s[so.o.count < cap ? so.o.count : cap - 1] = '\0';
    return r;
}

int vsnprintf(char *restrict s, size_t n, const char *restrict fmt, va_list ap)
{
    if (n > RSIZE_MAX) {
        __crt_constraint("vsnprintf: n > RSIZE_MAX", EINVAL);
        return -1;
    }
    return to_string(s, n, fmt, ap, 0);
}

int snprintf(char *restrict s, size_t n, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

/* sprintf can't know the size: at most RSIZE_MAX, which only stops a
 * wrapped-around length. Use snprintf. */
int vsprintf(char *restrict s, const char *restrict fmt, va_list ap)
{
    return to_string(s, RSIZE_MAX, fmt, ap, 0);
}

int sprintf(char *restrict s, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(s, fmt, ap);
    va_end(ap);
    return r;
}

static bool s_args_bad(const char *fn, char *s, rsize_t n, const char *fmt)
{
    const char *why = !s ? "s is null" : !n ? "n is 0" : n > RSIZE_MAX ? "n > RSIZE_MAX" :
                      !fmt ? "format is null" : NULL;
    if (!why)
        return false;
    if (s && n && n <= RSIZE_MAX)
        s[0] = '\0';
    char msg[48];
    strlcpy(msg, fn, sizeof msg);
    strlcat(msg, ": ", sizeof msg);
    strlcat(msg, why, sizeof msg);
    __crt_constraint(msg, EINVAL);
    return true;
}

int vsnprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, va_list ap)
{
    if (s_args_bad("vsnprintf_s", s, n, fmt))
        return -1;
    int r = to_string(s, n, fmt, ap, FMT_SECURE);
    if (r < 0)
        s[0] = '\0';
    return r;
}

int snprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf_s(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, va_list ap)
{
    if (s_args_bad("vsprintf_s", s, n, fmt))
        return -1;
    int r = to_string(s, n, fmt, ap, FMT_SECURE);
    if (r < 0) {
        s[0] = '\0';
        return -1;
    }
    if ((size_t)r >= n) {                           /* didn't fit: a broken constraint */
        s[0] = '\0';
        __crt_constraint("sprintf_s: the output doesn't fit", ERANGE);
        return 0;
    }
    return r;
}

int sprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf_s(s, n, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- _FORTIFY_SOURCE ---------------------------------------------------------- */

int __vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, va_list ap)
{
    (void)flag;
    if (n > slen)
        __crt_fail(FAIL_BUFFER_OVERRUN, "snprintf");
    return vsnprintf(s, n, fmt, ap);
}

int __snprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __vsnprintf_chk(s, n, flag, slen, fmt, ap);
    va_end(ap);
    return r;
}

int __vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list ap)
{
    (void)flag;
    if (slen > RSIZE_MAX)
        slen = RSIZE_MAX;
    int r = to_string(s, slen, fmt, ap, 0);
    if (r >= 0 && (size_t)r >= slen)
        __crt_fail(FAIL_BUFFER_OVERRUN, "sprintf");
    return r;
}

int __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __vsprintf_chk(s, flag, slen, fmt, ap);
    va_end(ap);
    return r;
}
