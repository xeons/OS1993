/*
 * user/apps/paint.c -- PAINT.BIN, a bitmap painter.
 *
 * A 400x280 canvas in the 16 theme colours with a toolbox (pencil, brush,
 * eraser, fill, line, rectangle, filled rectangle, ellipse) and a palette:
 * the left button paints in the foreground colour, the right button in the
 * background colour (and picks it on the palette). Shapes rubber-band while
 * the button is held. The picture lives in the process's own memory, one
 * byte per pixel, and reaches the screen through DrawPixels.
 */
#include "os1993.h"

#define CW      400                     /* canvas */
#define CH      280
#define CX      62                      /* where it sits in the client area */
#define CY      6
#define TOOL    24
#define SWATCH  16
#define PAL_Y   (CY + CH + 8)

enum { T_PENCIL, T_BRUSH, T_ERASER, T_FILL, T_LINE, T_RECT, T_FRECT, T_ELLIPSE, T_COUNT };
enum { ID_NEW = 100, ID_EXIT, ID_CLEAR, ID_ABOUT };

static const char *const tool_names[T_COUNT] = {
    "pencil", "brush", "eraser", "fill", "line", "rectangle", "filled rectangle", "ellipse",
};

static const uint16_t tool_glyph[T_COUNT][16] = {
    [T_PENCIL]  = { 0, 0x0006, 0x000F, 0x001E, 0x003C, 0x0078, 0x00F0, 0x01E0, 0x03C0, 0x0780,
                    0x0F00, 0x1E00, 0x3C00, 0x7000, 0x6000, 0 },
    [T_BRUSH]   = { 0, 0x0003, 0x0007, 0x000E, 0x001C, 0x0038, 0x0070, 0x00E0, 0x01C0, 0x0380,
                    0x0F00, 0x1F80, 0x3F80, 0x3F00, 0x7E00, 0x7800 },
    [T_ERASER]  = { 0, 0, 0x01FE, 0x0306, 0x060A, 0x0C14, 0x1828, 0x3050, 0x7FA0, 0x40C0, 0x4180,
                    0x7F00, 0, 0, 0, 0 },
    [T_FILL]    = { 0, 0x0300, 0x0480, 0x0840, 0x1020, 0x2010, 0x4008, 0x8004, 0x400A, 0x2016,
                    0x1026, 0x0846, 0x0486, 0x0306, 0x0006, 0x0006 },
    [T_LINE]    = { 0, 0x0003, 0x0006, 0x000C, 0x0018, 0x0030, 0x0060, 0x00C0, 0x0180, 0x0300,
                    0x0600, 0x0C00, 0x1800, 0x3000, 0x6000, 0 },
    [T_RECT]    = { 0, 0, 0, 0x7FFE, 0x4002, 0x4002, 0x4002, 0x4002, 0x4002, 0x4002, 0x4002,
                    0x4002, 0x7FFE, 0, 0, 0 },
    [T_FRECT]   = { 0, 0, 0, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE,
                    0x7FFE, 0x7FFE, 0, 0, 0 },
    [T_ELLIPSE] = { 0, 0, 0x07E0, 0x1818, 0x2004, 0x4002, 0x4002, 0x4002, 0x4002, 0x4002,
                    0x2004, 0x1818, 0x07E0, 0, 0, 0 },
};

static uint8_t canvas[CW * CH];
static HWND paint_wnd;
static int tool = T_PENCIL;
static uint8_t fg = C_BLACK, bg = C_WHITE;
static bool drawing;
static uint8_t ink;                     /* this stroke's colour */
static int sx, sy, lx, ly;              /* stroke start, last point (canvas) */

/* ---- the picture ---------------------------------------------------------------- */

static void put(int x, int y, uint8_t c)
{
    if (x >= 0 && y >= 0 && x < CW && y < CH)
        canvas[y * CW + x] = c;
}

static void dab(int x, int y, uint8_t c, int size)      /* a square brush */
{
    for (int dy = 0; dy < size; dy++)
        for (int dx = 0; dx < size; dx++)
            put(x - size / 2 + dx, y - size / 2 + dy, c);
}

static void line(int x0, int y0, int x1, int y1, uint8_t c, int size)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, stx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sty = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        dab(x0, y0, c, size);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += stx; }
        if (e2 <= dx) { err += dx; y0 += sty; }
    }
}

static RECT norm(int x0, int y0, int x1, int y1)
{
    return (RECT){ x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, (x0 > x1 ? x0 : x1) + 1, (y0 > y1 ? y0 : y1) + 1 };
}

static void rect(RECT r, uint8_t c, bool filled)
{
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++)
            if (filled || y == r.top || y == r.bottom - 1 || x == r.left || x == r.right - 1)
                put(x, y, c);
}

static void ellipse(RECT r, uint8_t c)                  /* midpoint, as in UI.DLL */
{
    int w = r.right - r.left - 1, h = r.bottom - r.top - 1;
    int a = w / 2, b = h / 2, cx = r.left + a, cy = r.top + b, ex = w & 1, ey = h & 1;
    int32_t a2 = (int32_t)a * a, b2 = (int32_t)b * b;
    int x = 0, y = b;
    int32_t d1 = b2 - a2 * b + a2 / 4;
    #define P4(px, py) do { put(cx + (px) + ex, cy + (py) + ey, c); put(cx - (px), cy + (py) + ey, c); \
                            put(cx + (px) + ex, cy - (py), c); put(cx - (px), cy - (py), c); } while (0)
    while (b2 * x < a2 * y) {
        P4(x, y);
        if (d1 < 0) d1 += b2 * (2 * x + 3);
        else { d1 += b2 * (2 * x + 3) + a2 * (2 - 2 * y); y--; }
        x++;
    }
    int32_t d2 = b2 * x * x + b2 * x + b2 / 4 + a2 * (y - 1) * (y - 1) - a2 * b2;
    while (y >= 0) {
        P4(x, y);
        if (d2 > 0) d2 += a2 * (3 - 2 * y);
        else { d2 += b2 * (2 * x + 2) + a2 * (3 - 2 * y); x++; }
        y--;
    }
    #undef P4
}

static void flood(int x, int y, uint8_t c)              /* scanline fill */
{
    uint8_t old = canvas[y * CW + x];
    if (old == c)
        return;
    static int16_t stack[8192][2];
    int n = 0;
    stack[n][0] = (int16_t)x; stack[n][1] = (int16_t)y; n++;
    while (n) {
        n--;
        int px = stack[n][0], py = stack[n][1];
        if (canvas[py * CW + px] != old)
            continue;
        int l = px, r = px;
        while (l > 0 && canvas[py * CW + l - 1] == old) l--;
        while (r < CW - 1 && canvas[py * CW + r + 1] == old) r++;
        for (int i = l; i <= r; i++)
            canvas[py * CW + i] = c;
        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = py + dir;
            if (ny < 0 || ny >= CH)
                continue;
            for (int i = l; i <= r; i++)
                if (canvas[ny * CW + i] == old && (i == l || canvas[ny * CW + i - 1] != old) &&
                    n < 8192) {
                    stack[n][0] = (int16_t)i; stack[n][1] = (int16_t)ny; n++;
                }
        }
    }
}

static void clear_canvas(void)
{
    memset(canvas, C_WHITE, sizeof canvas);
    RECT r = { CX, CY, CX + CW, CY + CH };
    InvalidateRect(paint_wnd, &r);
}

/* ---- the window ------------------------------------------------------------------ */

static RECT tool_rect(int t)  { int x = 6 + (t % 2) * (TOOL + 2), y = CY + (t / 2) * (TOOL + 2);
                                return (RECT){ x, y, x + TOOL, y + TOOL }; }
static RECT swatch_rect(int c) { int x = 50 + (c % 8) * (SWATCH + 2), y = PAL_Y + (c / 8) * (SWATCH + 2);
                                 return (RECT){ x, y, x + SWATCH, y + SWATCH }; }

static void invalidate_canvas(RECT r)                   /* canvas coordinates */
{
    RECT s = { CX + r.left - 4, CY + r.top - 4, CX + r.right + 4, CY + r.bottom + 4 };
    InvalidateRect(paint_wnd, &s);
}

static bool is_shape(int t) { return t == T_LINE || t == T_RECT || t == T_FRECT || t == T_ELLIPSE; }

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    for (int t = 0; t < T_COUNT; t++) {                 /* the toolbox */
        RECT r = tool_rect(t), in = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
        bool on = t == tool;
        FrameRect(dc, &r, C_GRAY2);
        FillRect(dc, &in, on ? C_GRAY5 : C_FACE);
        DrawBevel(dc, &in, !on);
        DrawBitmap(dc, r.left + 4 + on, r.top + 4 + on, tool_glyph[t], 16, C_BLACK);
    }
    RECT frame = { CX - 2, CY - 2, CX + CW + 2, CY + CH + 2 };      /* the canvas */
    DrawBevel(dc, &frame, false);
    DrawPixels(dc, CX, CY, CW, CH, canvas, CW);
    if (drawing && is_shape(tool)) {                    /* rubber band */
        RECT r = norm(sx, sy, lx, ly);
        RECT s = { CX + r.left, CY + r.top, CX + r.right, CY + r.bottom };
        if (tool == T_LINE)         DrawLineEx(dc, CX + sx, CY + sy, CX + lx, CY + ly, ink);
        else if (tool == T_RECT)    FrameRect(dc, &s, ink);
        else if (tool == T_FRECT)   FillRect(dc, &s, ink);
        else                        DrawEllipse(dc, &s, ink, false);
    }
    RECT cur = { 6, PAL_Y, 42, PAL_Y + 2 * SWATCH + 2 };            /* current colours */
    DrawBevel(dc, &cur, false);
    RECT b = { 20, PAL_Y + 14, 38, PAL_Y + 32 }, f = { 10, PAL_Y + 4, 28, PAL_Y + 22 };
    FillRect(dc, &b, bg); FrameRect(dc, &b, C_BLACK);
    FillRect(dc, &f, fg); FrameRect(dc, &f, C_BLACK);
    for (int c = 0; c < 16; c++) {                      /* the palette */
        RECT r = swatch_rect(c), in = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
        FrameRect(dc, &r, C_BLACK);
        FillRect(dc, &in, (uint8_t)c);
    }
    EndPaint(h, &ps);
}

static void canvas_point(LPARAM lp, int *x, int *y)
{
    int px = GET_X_LPARAM(lp) - CX, py = GET_Y_LPARAM(lp) - CY;
    *x = px < 0 ? 0 : px >= CW ? CW - 1 : px;
    *y = py < 0 ? 0 : py >= CH ? CH - 1 : py;
}

static void stroke_to(int x, int y)
{
    int size = tool == T_BRUSH ? 3 : tool == T_ERASER ? 8 : 1;
    line(lx, ly, x, y, ink, size);
    RECT r = norm(lx, ly, x, y);
    lx = x;
    ly = y;
    invalidate_canvas(r);
}

static void begin(HWND h, LPARAM lp, bool right)
{
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    for (int t = 0; t < T_COUNT; t++) {
        RECT r = tool_rect(t);
        if (PtInRect(&r, x, y)) {
            tool = t;
            DebugLog("paint tool %s", tool_names[t]);
            RECT all = { 0, 0, CX - 4, CY + 4 * (TOOL + 2) };
            InvalidateRect(h, &all);
            return;
        }
    }
    for (int c = 0; c < 16; c++) {
        RECT r = swatch_rect(c);
        if (PtInRect(&r, x, y)) {
            if (right) bg = (uint8_t)c; else fg = (uint8_t)c;
            DebugLog("paint %s colour %d", right ? "background" : "foreground", c);
            RECT cur = { 6, PAL_Y, 42, PAL_Y + 2 * SWATCH + 2 };
            InvalidateRect(h, &cur);
            return;
        }
    }
    RECT canvas_r = { CX, CY, CX + CW, CY + CH };
    if (!PtInRect(&canvas_r, x, y))
        return;
    canvas_point(lp, &sx, &sy);
    lx = sx;
    ly = sy;
    ink = tool == T_ERASER ? bg : right ? bg : fg;
    if (tool == T_FILL) {
        flood(sx, sy, ink);
        DebugLog("paint fill at %d,%d", sx, sy);
        InvalidateRect(h, &canvas_r);
        return;
    }
    drawing = true;
    SetCapture(h);
    if (!is_shape(tool))
        stroke_to(sx, sy);
}

static void end(void)
{
    if (!drawing)
        return;
    drawing = false;
    ReleaseCapture();
    RECT r = norm(sx, sy, lx, ly);
    switch (tool) {
    case T_LINE:    line(sx, sy, lx, ly, ink, 1); break;
    case T_RECT:    rect(r, ink, false); break;
    case T_FRECT:   rect(r, ink, true); break;
    case T_ELLIPSE: ellipse(r, ink); break;
    }
    DebugLog("paint %s from %d,%d to %d,%d", tool_names[tool], sx, sy, lx, ly);
    invalidate_canvas(r);
}

static LRESULT paint_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_LBUTTONDOWN: begin(h, lp, false); return 0;
    case WM_RBUTTONDOWN: begin(h, lp, true); return 0;
    case WM_MOUSEMOVE:
        if (drawing) {
            int x, y;
            canvas_point(lp, &x, &y);
            if (is_shape(tool)) {
                RECT old = norm(sx, sy, lx, ly);
                lx = x;
                ly = y;
                invalidate_canvas(old);
                invalidate_canvas(norm(sx, sy, lx, ly));
            } else {
                stroke_to(x, y);
            }
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        end();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_NEW:
        case ID_CLEAR: clear_canvas(); DebugLog("paint cleared"); break;
        case ID_EXIT:  PostQuitMessage(0); break;
        case ID_ABOUT: ShellAbout(h, "Paint", NULL); break;
        }
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

int main(void)
{
    InitUI();
    memset(canvas, C_WHITE, sizeof canvas);
    WNDCLASS wc = { .name = "PaintWindow", .proc = paint_proc, .background = C_FACE, .icon = ICON_PAINT };
    RegisterClass(&wc);
    HMENU file = CreatePopupMenu(), image = CreatePopupMenu(), help = CreatePopupMenu(), bar = CreateMenu();
    AppendMenu(file, MF_STRING, ID_NEW, "&New\tCtrl+N");
    AppendMenu(file, MF_SEPARATOR, 0, NULL);
    AppendMenu(file, MF_STRING, ID_EXIT, "E&xit");
    AppendMenu(image, MF_STRING, ID_CLEAR, "&Clear Image");
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About Paint");
    AppendMenu(bar, MF_POPUP, (uintptr_t)file, "&File");
    AppendMenu(bar, MF_POPUP, (uintptr_t)image, "&Image");
    AppendMenu(bar, MF_POPUP, (uintptr_t)help, "&Help");
    paint_wnd = CreateWindow("PaintWindow", "untitled - Paint", WS_OVERLAPPEDWINDOW, 30, 24,
                             CX + CW + 8 + 10, PAL_Y + 2 * SWATCH + 10 + 25 + 21 + 5, NULL, 0, NULL);
    SetMenu(paint_wnd, bar);
    ShowWindow(paint_wnd, SW_SHOW);
    static const ACCEL keys[] = { { FVIRTKEY | FCONTROL, 'N', ID_NEW } };
    HACCEL acc = CreateAcceleratorTable(keys, 1);
    DebugLog("paint ready");

    MSG msg;
    while (GetMessage(&msg)) {
        if (TranslateAccelerator(paint_wnd, acc, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
