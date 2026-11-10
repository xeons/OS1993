/*
 * user/apps/calc.c -- CALC.BIN, the Calculator.
 *
 * The standard calculator: the four operations, square root, percent,
 * reciprocal, sign, and a memory, by mouse or keyboard (digits, + - * /,
 * Enter or =, Esc clears, Backspace, % and @ for square root). The 386 has
 * no FPU on a 486SX, so numbers are 64-bit fixed point with six decimals and up
 * to twelve digits before the point -- with our own 64-bit division, since
 * ring-3 code has no libgcc either.
 */
#include "os1993.h"

#define SCALE   1000000LL               /* six decimals */
#define MAXINT  999999999999LL          /* largest integer part */
#define DIGITS  12

enum { ID_DIGIT = 100, ID_POINT = 110, ID_SIGN, ID_ADD, ID_SUB, ID_MUL, ID_DIV, ID_EQ,
       ID_SQRT, ID_PCT, ID_INV, ID_BACK, ID_CE, ID_C, ID_MC, ID_MR, ID_MS, ID_MPLUS,
       ID_ABOUT = 200 };

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

static int64_t apply(int64_t a, int op, int64_t b)
{
    switch (op) {
    case ID_ADD: return fix(mag(a + b), a + b < 0);
    case ID_SUB: return fix(mag(a - b), a - b < 0);
    case ID_MUL: return mul(a, b);
    case ID_DIV: return divide(a, b);
    }
    return b;
}

/* "123.", "-0.5", "3.141593" -- with a trailing point for whole numbers */
static void format(int64_t v, char *out)
{
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

static void refresh(void)
{
    RECT r = { 0, 0, 270, 40 };
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
    if (id >= ID_DIGIT && id < ID_DIGIT + 10) { type_digit((char)('0' + id - ID_DIGIT)); return; }
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
    case ID_ADD: case ID_SUB: case ID_MUL: case ID_DIV: {
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
    RECT d = { 10, 8, 260, 34 }, in = { 12, 10, 258, 32 };
    DrawBevel(dc, &d, false);
    FrameRect(dc, &in, C_GRAY1);
    RECT white = { 13, 11, 257, 31 };
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
    TextOut(dc, 252 - TextWidth(text), 13, text, C_BLACK, -1);
    RECT m = { 10, 44, 46, 72 };                /* the memory indicator */
    DrawBevel(dc, &m, false);
    if (has_memory)
        TextOut(dc, 24, 50, "M", C_BLACK, -1);
    EndPaint(h, &ps);
}

static LRESULT calc_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        calc = h;
        create_keys(h);
        return 0;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ABOUT)
            ShellAbout(h, "Calculator", NULL);
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
    HMENU help = CreatePopupMenu(), bar = CreateMenu();
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About Calculator");
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
