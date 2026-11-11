/*
 * user/apps/calc.c -- CALC.BIN, the Calculator.
 *
 * The standard calculator: the four operations, square root, percent,
 * reciprocal, sign, and a memory, by mouse or keyboard (digits, + - * /,
 * Enter or =, Esc clears, Backspace, % and @ for square root). The 386 has
 * no FPU on a 486SX, so numbers are 64-bit fixed point with six decimals and up
 * to twelve digits before the point -- with our own 64-bit division, since
 * ring-3 code has no libgcc either.
 *
 * View > Scientific adds sin, cos, tan (degrees or radians), ln, log, e^x,
 * x^y, x^2, x^3, n!, pi, Int, Mod, the bitwise And, Or, Xor and Not, and
 * hexadecimal, octal and binary for whole numbers. The functions work in
 * fixed point too, with nine decimals inside: series after reducing the
 * argument (sines on a quarter turn, logarithms by powers of two).
 */
#include "os1993.h"

#define SCALE   1000000LL               /* six decimals */
#define MAXINT  999999999999LL          /* largest integer part */
#define DIGITS  12

enum { ID_DIGIT = 100, ID_POINT = 116, ID_SIGN, ID_ADD, ID_SUB, ID_MUL, ID_DIV, ID_EQ,
       ID_SQRT, ID_PCT, ID_INV, ID_BACK, ID_CE, ID_C, ID_MC, ID_MR, ID_MS, ID_MPLUS,
       ID_SIN, ID_COS, ID_TAN, ID_FACT, ID_LN, ID_LOG, ID_EXP, ID_POW, ID_SQR, ID_CUBE, ID_PI,
       ID_INT, ID_MOD, ID_AND, ID_OR, ID_XOR, ID_NOT, ID_HEX, ID_DEC, ID_OCT, ID_BIN, ID_DEG, ID_RAD,
       ID_ABOUT = 200, ID_STANDARD, ID_SCIENTIFIC };

#define SCI_X   230                     /* the scientific keys' width */

static HWND calc;

/* ---- arithmetic -------------------------------------------------------------- */

static uint64_t udiv(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {             /* shift and subtract */
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

static const char *error;               /* set by an operation that failed */

static uint64_t mag(int64_t v) { return v < 0 ? (uint64_t)-v : (uint64_t)v; }

static int64_t fix(uint64_t m, bool neg)
{
    if (m > (uint64_t)MAXINT * SCALE + (SCALE - 1)) {
        error = "Overflow";
        return 0;
    }
    return neg ? -(int64_t)m : (int64_t)m;
}

static int64_t mul(int64_t a, int64_t b)
{
    uint64_t ua = mag(a), ub = mag(b), al, bl;
    uint64_t ah = udiv(ua, SCALE, &al), bh = udiv(ub, SCALE, &bl);
    if (ah && bh > udiv(MAXINT, ah, NULL)) {
        error = "Overflow";
        return 0;
    }
    uint64_t m = ah * bh * SCALE + ah * bl + al * bh + udiv(al * bl, SCALE, NULL);
    return fix(m, (a < 0) != (b < 0));
}

static int64_t divide(int64_t a, int64_t b)
{
    if (!b) {
        error = "Cannot divide by zero";
        return 0;
    }
    uint64_t ua = mag(a), ub = mag(b), r;
    uint64_t q = udiv(ua, ub, &r);
    if (q > (uint64_t)MAXINT) {
        error = "Overflow";
        return 0;
    }
    uint64_t frac = 0;
    for (int i = 0; i < 7; i++) {               /* six decimals and one to round */
        uint64_t d = udiv(r * 10, ub, &r);
        frac = frac * 10 + d;
    }
    frac = udiv(frac + 5, 10, NULL);
    return fix(q * SCALE + frac, (a < 0) != (b < 0));
}

static int64_t root(int64_t a)
{
    if (a < 0) {
        error = "Invalid input";
        return 0;
    }
    if (!a)
        return 0;
    int64_t x = a > SCALE ? a : SCALE;
    for (int i = 0; i < 60; i++) {              /* Newton's method */
        int64_t next = (x + divide(a, x)) / 2;
        if (next == x || next == x - 1 || next == x + 1) {
            x = next;
            break;
        }
        x = next;
    }
    return x;
}

/* ---- the scientific functions: nine decimals inside (Q) ----------------------- */

#define Q        1000000000LL
#define PI_Q     3141592654LL
#define TWO_PI_Q 6283185307LL
#define LN2_Q    693147181LL
#define LN10_Q   2302585093LL

static int64_t sdiv(int64_t a, uint64_t d)              /* a / d, rounded, signed */
{
    uint64_t q = udiv(mag(a) + d / 2, d, NULL);
    return a < 0 ? -(int64_t)q : (int64_t)q;
}

static int64_t qmul(int64_t a, int64_t b)               /* a * b / Q (|a * b| < 9e18) */
{
    uint64_t p = mag(a) * mag(b);
    uint64_t r = udiv(p + Q / 2, Q, NULL);
    return (a < 0) != (b < 0) ? -(int64_t)r : (int64_t)r;
}

static int64_t qdiv(int64_t a, int64_t b)               /* a * Q / b, any size */
{
    uint64_t ua = mag(a), ub = mag(b), r;
    uint64_t q = udiv(ua, ub, &r), frac = 0;
    for (int i = 0; i < 9; i++) {
        uint64_t d = udiv(r * 10, ub, &r);
        frac = frac * 10 + d;
    }
    int64_t v = (int64_t)(q * Q + frac);
    return (a < 0) != (b < 0) ? -v : v;
}

static int64_t from_q(int64_t v) { return sdiv(v, 1000); }   /* Q -> SCALE */

static bool deg = true;                 /* angles in degrees, else radians */
static int base = 10;                   /* how whole numbers show */

/* The sine of x (Q radians in [-pi, pi]): Taylor on [-pi/2, pi/2]. */
static int64_t qsin(int64_t x)
{
    if (x > PI_Q / 2) x = PI_Q - x;
    if (x < -PI_Q / 2) x = -PI_Q - x;
    int64_t x2 = qmul(x, x), term = x, sum = x;
    for (int n = 1; n <= 9; n++) {
        term = sdiv(qmul(term, x2), (uint64_t)(2 * n) * (2 * n + 1));
        sum += n & 1 ? -term : term;
    }
    return sum;
}

/* An angle (SCALE, degrees or radians) -> Q radians in [-pi, pi]. */
static int64_t angle(int64_t v)
{
    int64_t r;
    if (deg) {
        uint64_t rem;
        udiv(mag(v), 360ULL * SCALE, &rem);             /* a whole turn off */
        int64_t a = v < 0 ? -(int64_t)rem : (int64_t)rem;
        r = (int64_t)udiv(mag(a) * (uint64_t)PI_Q, 180ULL * SCALE, NULL);
        if (a < 0) r = -r;
    } else {
        uint64_t rem;
        udiv(mag(v) * 1000, (uint64_t)TWO_PI_Q, &rem);
        r = v < 0 ? -(int64_t)rem : (int64_t)rem;
    }
    if (r > PI_Q) r -= TWO_PI_Q;
    if (r < -PI_Q) r += TWO_PI_Q;
    return r;
}

static int64_t f_sin(int64_t v) { return from_q(qsin(angle(v))); }
static int64_t f_cos(int64_t v)
{
    int64_t a = angle(v) + PI_Q / 2;
    if (a > PI_Q) a -= TWO_PI_Q;
    return from_q(qsin(a));
}
static int64_t f_tan(int64_t v)
{
    int64_t a = angle(v), c = a + PI_Q / 2;
    if (c > PI_Q) c -= TWO_PI_Q;
    int64_t sn = qsin(a), cs = qsin(c);
    if (mag(cs) < 1000) {                               /* 90 degrees: no tangent */
        error = "Invalid input";
        return 0;
    }
    int64_t t = qdiv(sn, cs);
    return fix(mag(from_q(t)), t < 0);
}

static int64_t qln(int64_t v)                           /* v (SCALE) > 0 -> ln v (Q) */
{
    int k = 0;
    uint64_t m = (uint64_t)v;
    while (m >= 2 * (uint64_t)SCALE) { m >>= 1; k++; }   /* m in [1, 2) */
    while (m < (uint64_t)SCALE) { m <<= 1; k--; }
    int64_t mq = (int64_t)m * 1000;
    int64_t z = qdiv(mq - Q, mq + Q), z2 = qmul(z, z), term = z, sum = z;
    for (int n = 3; n <= 31; n += 2) {                  /* 2 atanh z */
        term = qmul(term, z2);
        sum += sdiv(term, (uint64_t)n);
    }
    return 2 * sum + k * LN2_Q;
}

static int64_t f_ln(int64_t v)
{
    if (v <= 0) {
        error = "Invalid input";
        return 0;
    }
    return from_q(qln(v));
}

static int64_t f_log(int64_t v)
{
    if (v <= 0) {
        error = "Invalid input";
        return 0;
    }
    return from_q(qdiv(qln(v), LN10_Q));
}

static int64_t f_exp(int64_t v)                         /* e^v, v in SCALE */
{
    if (v > 28 * SCALE) {
        error = "Overflow";
        return 0;
    }
    if (v < -15 * SCALE)
        return 0;
    int64_t x = v * 1000;                               /* Q */
    int k = 0;
    while (x >= LN2_Q) { x -= LN2_Q; k++; }
    while (x < 0) { x += LN2_Q; k--; }
    int64_t term = Q, sum = Q;
    for (int n = 1; n <= 16; n++) {
        term = sdiv(qmul(term, x), (uint64_t)n);
        sum += term;
    }
    uint64_t r = (uint64_t)sum;                         /* e^x in Q, x in [0, ln 2) */
    if (k < 0) {                                        /* halved in Q, then rounded */
        r = -k < 63 ? (r + (1ULL << (-k - 1))) >> -k : 0;
        return (int64_t)udiv(r + 500, 1000, NULL);
    }
    while (k > 0 && r < (1ULL << 61)) {                 /* doubled in Q while it fits */
        r <<= 1;
        k--;
    }
    r = udiv(r + 500, 1000, NULL);
    for (; k > 0; k--) {
        r <<= 1;
        if (r > (uint64_t)MAXINT * SCALE) {
            error = "Overflow";
            return 0;
        }
    }
    if (r > (uint64_t)MAXINT * SCALE) {
        error = "Overflow";
        return 0;
    }
    return (int64_t)r;
}

static bool whole(int64_t v) { uint64_t r; udiv(mag(v), SCALE, &r); return r == 0; }
static int64_t trunc_int(int64_t v) { int64_t n = (int64_t)udiv(mag(v), SCALE, NULL); return v < 0 ? -n : n; }

static int64_t f_pow(int64_t a, int64_t b)
{
    if (whole(b) && mag(b) <= 64ULL * SCALE) {          /* by multiplying (any sign of a) */
        int n = (int)trunc_int(b);
        int64_t r = SCALE;
        for (int i = 0; i < (n < 0 ? -n : n) && !error; i++)
            r = mul(r, a);
        return n < 0 && !error ? divide(SCALE, r) : r;
    }
    if (a <= 0) {
        error = a ? "Invalid input" : NULL;
        return 0;
    }
    int64_t l = from_q(qln(a));
    return f_exp(mul(b, l));
}

static int64_t f_fact(int64_t v)
{
    if (v < 0 || !whole(v) || v > 20 * SCALE) {
        error = "Invalid input";
        return 0;
    }
    int64_t r = SCALE;
    for (int i = 2; i <= (int)trunc_int(v) && !error; i++)
        r = mul(r, (int64_t)i * SCALE);
    return r;
}

static int64_t f_mod(int64_t a, int64_t b)
{
    if (!b) {
        error = "Cannot divide by zero";
        return 0;
    }
    uint64_t r;
    udiv(mag(a), mag(b), &r);
    return a < 0 ? -(int64_t)r : (int64_t)r;
}

static int64_t bits(int64_t a, int op, int64_t b)       /* on the whole parts */
{
    int64_t x = trunc_int(a), y = trunc_int(b), r = 0;
    switch (op) {
    case ID_AND: r = x & y; break;
    case ID_OR:  r = x | y; break;
    case ID_XOR: r = x ^ y; break;
    }
    return fix(mag(r) * SCALE, r < 0);
}

static int64_t apply(int64_t a, int op, int64_t b)
{
    switch (op) {
    case ID_ADD: return fix(mag(a + b), a + b < 0);
    case ID_SUB: return fix(mag(a - b), a - b < 0);
    case ID_MUL: return mul(a, b);
    case ID_DIV: return divide(a, b);
    case ID_POW: return f_pow(a, b);
    case ID_MOD: return f_mod(a, b);
    case ID_AND: case ID_OR: case ID_XOR: return bits(a, op, b);
    }
    return b;
}

/* "123.", "-0.5", "3.141593" -- with a trailing point for whole numbers;
 * in hex, octal or binary the whole part alone. */
static void format(int64_t v, char *out)
{
    if (base != 10) {
        uint64_t t = udiv(mag(v), SCALE, NULL);
        char digits[48];
        int n = 0, k = 0;
        do {
            uint64_t d;
            t = udiv(t, (uint64_t)base, &d);
            digits[n++] = "0123456789ABCDEF"[d];
        } while (t && n < 47);
        if (v < 0) out[k++] = '-';
        while (n) out[k++] = digits[--n];
        out[k] = '\0';
        return;
    }
    uint64_t fr, t = udiv(mag(v), SCALE, &fr);
    char digits[24];
    int n = 0;
    do {
        uint64_t d;
        t = udiv(t, 10, &d);
        digits[n++] = (char)('0' + d);
    } while (t);
    int k = 0;
    if (v < 0) out[k++] = '-';
    while (n) out[k++] = digits[--n];
    out[k++] = '.';
    char fd[6];
    for (int i = 5; i >= 0; i--) {
        uint64_t d;
        fr = udiv(fr, 10, &d);
        fd[i] = (char)('0' + d);
    }
    int last = 5;
    while (last >= 0 && fd[last] == '0') last--;
    for (int i = 0; i <= last; i++) out[k++] = fd[i];
    out[k] = '\0';
}

/* ---- the calculator ---------------------------------------------------------- */

static char entry[24] = "0";            /* what is being typed */
static bool typing;
static int64_t shown, acc, memory, last_operand;
static int op, last_op;
static bool has_memory;

static int64_t parse(const char *s)
{
    bool neg = *s == '-';
    if (neg) s++;
    if (base != 10) {
        uint64_t n = 0;
        for (; *s; s++) {
            int d = *s >= 'A' ? *s - 'A' + 10 : *s - '0';
            n = n * (uint64_t)base + (uint64_t)d;
            if (n > (uint64_t)MAXINT) { error = "Overflow"; return 0; }
        }
        return fix(n * SCALE, neg);
    }
    uint64_t ip = 0, fr = 0;
    int fd = 0;
    for (; *s && *s != '.'; s++)
        ip = ip * 10 + (uint64_t)(*s - '0');
    if (*s == '.')
        for (s++; *s && fd < 6; s++, fd++)
            fr = fr * 10 + (uint64_t)(*s - '0');
    for (; fd < 6; fd++)
        fr *= 10;
    return fix(ip * SCALE + fr, neg);
}

static int64_t current(void) { return typing ? parse(entry) : shown; }

static bool scientific;

static void refresh(void)
{
    RECT r = { 0, 0, 270 + SCI_X, 40 };
    InvalidateRect(calc, &r);
}

static void show(int64_t v)
{
    shown = v;
    typing = false;
    refresh();
}

static void clear_all(void)
{
    error = NULL;
    acc = shown = last_operand = 0;
    op = last_op = 0;
    typing = false;
    strncpy0(entry, "0", sizeof entry);
    refresh();
}

static void type_digit(char c)
{
    if (!typing) {
        strncpy0(entry, "0", sizeof entry);
        typing = true;
    }
    int n = (int)strlen(entry), ints = 0, decs = -1;
    for (int i = 0; i < n; i++) {
        if (entry[i] == '.') decs = 0;
        else if (entry[i] != '-') { if (decs >= 0) decs++; else ints++; }
    }
    int dv = c >= 'A' ? c - 'A' + 10 : c - '0';
    if (c != '.' && dv >= base)
        return;                                 /* not a digit in this base */
    if (base != 10) {
        int limit = base == 16 ? 10 : base == 8 ? 13 : 39;
        if (n == 1 && entry[0] == '0') entry[0] = c;
        else if (ints < limit && c != '.') { entry[n] = c; entry[n + 1] = '\0'; }
        refresh();
        return;
    }
    if (c == '.') {
        if (decs < 0 && n < (int)sizeof entry - 1) { entry[n] = '.'; entry[n + 1] = '\0'; }
    } else if (decs < 0 && ints == 1 && entry[n - 1] == '0') {
        entry[n - 1] = c;                       /* replace a lone 0 */
    } else if ((decs < 0 && ints < DIGITS) || (decs >= 0 && decs < 6)) {
        entry[n] = c;
        entry[n + 1] = '\0';
    }
    refresh();
}

static void command(int id)
{
    if (error && id != ID_C && id != ID_CE)
        return;
    if (id >= ID_DIGIT && id < ID_DIGIT + 16) {
        int d = id - ID_DIGIT;
        type_digit((char)(d < 10 ? '0' + d : 'A' + d - 10));
        return;
    }
    char text[32];
    switch (id) {
    case ID_POINT: type_digit('.'); return;
    case ID_BACK:
        if (typing) {
            int n = (int)strlen(entry);
            entry[n - 1] = '\0';
            if (!entry[0] || !strcmp(entry, "-"))
                strncpy0(entry, "0", sizeof entry);
            refresh();
        }
        return;
    case ID_CE:
        error = NULL;
        strncpy0(entry, "0", sizeof entry);
        typing = true;
        refresh();
        return;
    case ID_C:
        clear_all();
        return;
    case ID_SIGN:
        if (typing) {
            int n = (int)strlen(entry);
            if (entry[0] == '-')
                memmove(entry, entry + 1, (size_t)n);
            else if (strcmp(entry, "0") && n < (int)sizeof entry - 1) {
                memmove(entry + 1, entry, (size_t)n + 1);
                entry[0] = '-';
            }
            refresh();
        } else {
            show(-shown);
        }
        return;
    case ID_ADD: case ID_SUB: case ID_MUL: case ID_DIV:
    case ID_POW: case ID_MOD: case ID_AND: case ID_OR: case ID_XOR: {
        int64_t v = current();
        if (!op)
            acc = v;
        else if (typing)                        /* 2 + 3 + ...: the running total */
            acc = apply(acc, op, v);
        op = id;                                /* else just a change of mind */
        show(acc);
        break;
    }
    case ID_EQ: {
        int64_t v = current();
        if (op) {
            last_op = op;
            last_operand = v;
            acc = apply(acc, op, v);
            op = 0;
        } else if (last_op) {
            acc = apply(v, last_op, last_operand);
        } else {
            acc = v;
        }
        show(acc);
        format(acc, text);
        DebugLog("calc = %s", error ? error : text);
        break;
    }
    case ID_SQRT: show(root(current())); break;
    case ID_SIN:  show(f_sin(current())); break;
    case ID_COS:  show(f_cos(current())); break;
    case ID_TAN:  show(f_tan(current())); break;
    case ID_LN:   show(f_ln(current())); break;
    case ID_LOG:  show(f_log(current())); break;
    case ID_EXP:  show(f_exp(current())); break;
    case ID_SQR:  { int64_t v = current(); show(mul(v, v)); break; }
    case ID_CUBE: { int64_t v = current(); show(mul(mul(v, v), v)); break; }
    case ID_FACT: show(f_fact(current())); break;
    case ID_PI:   show(3141593); break;
    case ID_INT:  show(trunc_int(current()) * SCALE); break;
    case ID_NOT:  { int64_t n = ~trunc_int(current()); show(fix(mag(n) * SCALE, n < 0)); break; }
    case ID_HEX: case ID_DEC: case ID_OCT: case ID_BIN: {
        int64_t v = current();
        base = id == ID_HEX ? 16 : id == ID_OCT ? 8 : id == ID_BIN ? 2 : 10;
        if (base != 10) v = trunc_int(v) * SCALE;
        show(v);
        for (int d = 0; d < 16; d++) {          /* only the digits of the base */
            HWND k = GetDlgItem(calc, ID_DIGIT + d);
            if (k) EnableWindow(k, d < base);
        }
        EnableWindow(GetDlgItem(calc, ID_POINT), base == 10);
        DebugLog("calc base %d", base);
        return;
    }
    case ID_DEG: deg = true; return;
    case ID_RAD: deg = false; return;
    case ID_INV:  show(divide(SCALE, current())); break;
    case ID_PCT:  show(divide(mul(acc, current()), 100 * SCALE)); break;
    case ID_MC:   memory = 0; has_memory = false; refresh(); break;
    case ID_MR:   show(memory); break;
    case ID_MS:   memory = current(); has_memory = memory != 0; typing = false; shown = memory; refresh(); break;
    case ID_MPLUS: memory = apply(memory, ID_ADD, current()); has_memory = memory != 0;
                   shown = current(); typing = false; refresh(); break;
    }
    if (error) {
        DebugLog("calc error: %s", error);
        refresh();
    }
}

/* ---- the window -------------------------------------------------------------- */

struct key { int id; const char *text; int col, row; };

static const struct key keys[] = {
    { ID_MC, "MC", -1, 1 },  { ID_DIGIT + 7, "7", 0, 1 }, { ID_DIGIT + 8, "8", 1, 1 },
    { ID_DIGIT + 9, "9", 2, 1 }, { ID_DIV, "/", 3, 1 }, { ID_SQRT, "sqrt", 4, 1 },
    { ID_MR, "MR", -1, 2 },  { ID_DIGIT + 4, "4", 0, 2 }, { ID_DIGIT + 5, "5", 1, 2 },
    { ID_DIGIT + 6, "6", 2, 2 }, { ID_MUL, "*", 3, 2 }, { ID_PCT, "%", 4, 2 },
    { ID_MS, "MS", -1, 3 },  { ID_DIGIT + 1, "1", 0, 3 }, { ID_DIGIT + 2, "2", 1, 3 },
    { ID_DIGIT + 3, "3", 2, 3 }, { ID_SUB, "-", 3, 3 }, { ID_INV, "1/x", 4, 3 },
    { ID_MPLUS, "M+", -1, 4 }, { ID_DIGIT + 0, "0", 0, 4 }, { ID_SIGN, "+/-", 1, 4 },
    { ID_POINT, ".", 2, 4 }, { ID_ADD, "+", 3, 4 }, { ID_EQ, "=", 4, 4 },
};

struct skey { int id; const char *text; int x, y, w; };
static const struct skey sci_keys[] = {
    { ID_SIN, "sin", 10, 76, 40 }, { ID_LN, "ln", 52, 76, 40 }, { ID_SQR, "x^2", 94, 76, 40 },
    { ID_MOD, "Mod", 136, 76, 40 }, { ID_NOT, "Not", 178, 76, 40 },
    { ID_COS, "cos", 10, 108, 40 }, { ID_LOG, "log", 52, 108, 40 }, { ID_CUBE, "x^3", 94, 108, 40 },
    { ID_AND, "And", 136, 108, 40 }, { ID_PI, "pi", 178, 108, 40 },
    { ID_TAN, "tan", 10, 140, 40 }, { ID_EXP, "e^x", 52, 140, 40 }, { ID_FACT, "n!", 94, 140, 40 },
    { ID_OR, "Or", 136, 140, 40 }, { ID_INT, "Int", 178, 140, 40 },
    { ID_POW, "x^y", 52, 172, 40 }, { ID_XOR, "Xor", 136, 172, 40 },
    { ID_DIGIT + 10, "A", 10, 204, 32 }, { ID_DIGIT + 11, "B", 46, 204, 32 },
    { ID_DIGIT + 12, "C", 82, 204, 32 }, { ID_DIGIT + 13, "D", 118, 204, 32 },
    { ID_DIGIT + 14, "E", 154, 204, 32 }, { ID_DIGIT + 15, "F", 190, 204, 32 },
};

static void create_sci(HWND h)
{
    for (unsigned i = 0; i < sizeof sci_keys / sizeof sci_keys[0]; i++) {
        const struct skey *k = &sci_keys[i];
        CreateWindow(WC_BUTTON, k->text, WS_CHILD | (k->id >= ID_DIGIT + 10 && k->id < ID_DIGIT + 16 ? WS_DISABLED : 0),
                     k->x, k->y, k->w, 28, h, k->id, NULL);
    }
    static const struct { int id; const char *t; int x, y; } radios[] = {
        { ID_HEX, "Hex", 10, 44 }, { ID_DEC, "Dec", 60, 44 }, { ID_OCT, "Oct", 110, 44 }, { ID_BIN, "Bin", 160, 44 },
        { ID_DEG, "Degrees", 10, 172 - 2 }, { ID_RAD, "Radians", 10, 186 },
    };
    for (unsigned i = 0; i < sizeof radios / sizeof radios[0]; i++)
        CreateWindow(WC_BUTTON, radios[i].t, WS_CHILD | BS_AUTORADIOBUTTON | ((i == 0 || i == 4) ? WS_GROUP : 0),
                     radios[i].x, radios[i].y, i >= 4 ? 44 : 48, i >= 4 ? 14 : 20, h, radios[i].id, NULL);
    CheckDlgButton(h, ID_DEC, BST_CHECKED);
    CheckDlgButton(h, ID_DEG, BST_CHECKED);
}

static void set_view(HWND h, bool sci)
{
    if (sci == scientific)
        return;
    int dx = sci ? SCI_X : -SCI_X;
    scientific = sci;
    for (int id = ID_DIGIT; id <= ID_MPLUS; id++) {    /* the standard keys move over */
        if (id >= ID_DIGIT + 10 && id < ID_DIGIT + 16)
            continue;
        HWND k = GetDlgItem(h, id);
        RECT r;
        if (!k) continue;
        GetWindowRect(k, &r);
        POINT p = { r.left, r.top };
        ScreenToClient(h, &p);
        MoveWindow(k, p.x + dx, p.y, r.right - r.left, r.bottom - r.top, true);
    }
    for (unsigned i = 0; i < sizeof sci_keys / sizeof sci_keys[0]; i++)
        ShowWindow(GetDlgItem(h, sci_keys[i].id), sci ? SW_SHOW : SW_HIDE);
    for (int id = ID_HEX; id <= ID_RAD; id++)
        ShowWindow(GetDlgItem(h, id), sci ? SW_SHOW : SW_HIDE);
    if (!sci && base != 10)
        command(ID_DEC), CheckDlgButton(h, ID_HEX, 0), CheckDlgButton(h, ID_OCT, 0),
        CheckDlgButton(h, ID_BIN, 0), CheckDlgButton(h, ID_DEC, BST_CHECKED);
    RECT w;
    GetWindowRect(h, &w);
    MoveWindow(h, w.left, w.top, 270 + 10 + (sci ? SCI_X : 0), (sci ? 236 : 204) + 25 + 21 + 5, true);
    CheckMenuRadioItem(GetMenu(h), ID_STANDARD, ID_SCIENTIFIC, sci ? ID_SCIENTIFIC : ID_STANDARD, MF_BYCOMMAND);
    InvalidateRect(h, NULL);
    DebugLog("calc %s view", sci ? "scientific" : "standard");
}

static void create_keys(HWND h)
{
    CreateWindow(WC_BUTTON, "Back", WS_CHILD | WS_VISIBLE, 56, 44, 64, 28, h, ID_BACK, NULL);
    CreateWindow(WC_BUTTON, "CE", WS_CHILD | WS_VISIBLE, 126, 44, 64, 28, h, ID_CE, NULL);
    CreateWindow(WC_BUTTON, "C", WS_CHILD | WS_VISIBLE, 196, 44, 64, 28, h, ID_C, NULL);
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const struct key *k = &keys[i];
        int x = k->col < 0 ? 10 : 56 + k->col * 42, y = 44 + k->row * 32;
        CreateWindow(WC_BUTTON, k->text, WS_CHILD | WS_VISIBLE, x, y, 36, 28, h, k->id, NULL);
    }
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    int ox = scientific ? SCI_X : 0;
    RECT d = { 10, 8, 260 + ox, 34 }, in = { 12, 10, 258 + ox, 32 };
    DrawBevel(dc, &d, false);
    FrameRect(dc, &in, C_GRAY1);
    RECT white = { 13, 11, 257 + ox, 31 };
    FillRect(dc, &white, C_WHITE);
    char text[32];
    if (error) strncpy0(text, error, sizeof text);
    else if (typing) {
        strncpy0(text, entry, sizeof text);
        bool dot = false;
        for (char *p = text; *p; p++)
            dot |= *p == '.';
        if (!dot) { size_t n = strlen(text); text[n] = '.'; text[n + 1] = '\0'; }
    } else format(shown, text);
    TextOut(dc, d.right - 8 - TextWidth(text), 13, text, C_BLACK, -1);
    RECT m = { 10 + ox, 44, 46 + ox, 72 };      /* the memory indicator */
    DrawBevel(dc, &m, false);
    if (has_memory)
        TextOut(dc, 24 + ox, 50, "M", C_BLACK, -1);
    EndPaint(h, &ps);
}

static LRESULT calc_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        calc = h;
        create_keys(h);
        create_sci(h);
        return 0;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ABOUT)
            ShellAbout(h, "Calculator", NULL);
        else if (LOWORD(wp) == ID_STANDARD || LOWORD(wp) == ID_SCIENTIFIC)
            set_view(h, LOWORD(wp) == ID_SCIENTIFIC);
        else
            command(LOWORD(wp));
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* Keys typed anywhere in the calculator */
static bool calc_key(const MSG *m)
{
    int id = 0;
    if (m->message == WM_CHAR) {
        char c = (char)m->wParam;
        if (c >= '0' && c <= '9') id = ID_DIGIT + (c - '0');
        else if (base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            id = ID_DIGIT + 10 + ((c | 0x20) - 'a');
        else switch (c) {
            case '.': case ',': id = ID_POINT; break;
            case '+': id = ID_ADD; break;
            case '-': id = ID_SUB; break;
            case '*': id = ID_MUL; break;
            case '/': id = ID_DIV; break;
            case '=': case 13: id = ID_EQ; break;
            case '%': id = ID_PCT; break;
            case '@': id = ID_SQRT; break;
            case 'r': case 'R': id = ID_INV; break;
            case 's': case 'S': if (scientific) id = ID_SIN; break;
            case 'o': case 'O': if (scientific) id = ID_COS; break;
            case 't': case 'T': if (scientific) id = ID_TAN; break;
            case 'n': case 'N': if (scientific) id = ID_LN; break;
            case 'l': case 'L': if (scientific) id = ID_LOG; break;
            case 'y': case 'Y': if (scientific) id = ID_POW; break;
            case '!': if (scientific) id = ID_FACT; break;
            case 'p': case 'P': if (scientific) id = ID_PI; break;
            case '^': if (scientific) id = ID_XOR; break;
            case '&': if (scientific) id = ID_AND; break;
            case '|': if (scientific) id = ID_OR; break;
            case '~': if (scientific) id = ID_NOT; break;
            case 8: id = ID_BACK; break;
            case 27: id = ID_C; break;
        }
    } else if (m->message == WM_KEYDOWN && m->wParam == VK_DELETE) {
        id = ID_CE;
    } else if (m->message == WM_KEYDOWN && m->wParam == VK_RETURN) {
        return true;                            /* WM_CHAR 13 does it */
    }
    if (id)
        command(id);
    return id != 0;
}

int main(void)
{
    InitUI();
    WNDCLASS wc = { .name = "CalcWindow", .proc = calc_proc, .background = C_FACE, .icon = ICON_CALC };
    RegisterClass(&wc);
    HMENU help = CreatePopupMenu(), view = CreatePopupMenu(), bar = CreateMenu();
    AppendMenu(view, MF_STRING | MF_CHECKED | MFT_RADIOCHECK, ID_STANDARD, "&Standard");
    AppendMenu(view, MF_STRING, ID_SCIENTIFIC, "S&cientific");
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About Calculator");
    AppendMenu(bar, MF_POPUP, (uintptr_t)view, "&View");
    AppendMenu(bar, MF_POPUP, (uintptr_t)help, "&Help");
    HWND h = CreateWindow("CalcWindow", "Calculator", WS_DIALOG, 180, 90,
                          270 + 10, 204 + 25 + 21 + 5, NULL, 0, NULL);
    SetMenu(h, bar);
    ShowWindow(h, SW_SHOW);
    DebugLog("calculator ready");

    MSG msg;
    while (GetMessage(&msg)) {
        if (GetActiveWindow() == h && calc_key(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
