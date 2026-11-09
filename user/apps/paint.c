/*
 * user/apps/paint.c -- PAINT.BIN, a bitmap painter.
 *
 * A 400x280 canvas in the 16 theme colours with a toolbox (pencil, brush,
 * eraser, fill, line, rectangle, filled rectangle, ellipse) and a palette:
 * the left button paints in the foreground colour, the right button in the
 * background colour (and picks it on the palette). Shapes rubber-band while
 * the button is held. The picture lives in the process's own memory, one
 * byte per pixel, and reaches the screen through DrawPixels. Pictures are
 * saved as 16-colour BMP files and 16- or 256-colour BMPs can be opened
 * (their colours mapped to the nearest of the 16).
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
enum { ID_NEW = 100, ID_EXIT, ID_CLEAR, ID_ABOUT, ID_OPEN, ID_SAVE, ID_SAVEAS };

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

static bool dirty;                      /* changed since it was saved */
static char file_path[128];             /* "" while untitled */

static void clear_canvas(void)
{
    memset(canvas, C_WHITE, sizeof canvas);
    RECT r = { CX, CY, CX + CW, CY + CH };
    InvalidateRect(paint_wnd, &r);
}

/* ---- BMP files ------------------------------------------------------------------ */

/* The theme's colours (6-bit DAC values, as UI.DLL loads them) */
static const uint8_t theme[16][3] = {
    {  0,  0,  0 }, { 13, 13, 13 }, { 21, 21, 21 }, { 30, 30, 30 }, { 38, 38, 38 }, { 46, 46, 46 },
    { 55, 55, 55 }, { 59, 59, 59 }, { 63, 63, 63 }, { 26, 26, 50 }, { 50, 50, 63 }, { 17, 30, 40 },
    { 52,  8,  8 }, {  8, 40, 12 }, { 63, 56, 16 }, { 16, 16, 40 },
};

#define BMP_HEADER 118                  /* file and info headers, 16 palette entries */
#define ROW4       ((CW * 4 + 31) / 32 * 4)
static uint8_t file_buf[1078 + CW * CH + 1024];     /* big enough for 256 colours */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

static const char *base_name(const char *path)
{
    const char *b = path;
    for (; *path; path++)
        if (*path == '\\' || *path == ':') b = path + 1;
    return b;
}

static void set_title(void)
{
    char title[64];
    strncpy0(title, file_path[0] ? base_name(file_path) : "untitled", sizeof title);
    strncpy0(title + strlen(title), " - Paint", sizeof title - strlen(title));
    SetWindowText(paint_wnd, title);
}

static void error_box(const char *what, const char *path)
{
    char text[160];
    strncpy0(text, what, sizeof text);
    strncpy0(text + strlen(text), path, sizeof text - strlen(text));
    MessageBox(paint_wnd, text, "Paint", MB_OK | MB_ICONERROR);
}

static bool save(const char *path)
{
    uint32_t size = BMP_HEADER + ROW4 * CH;
    memset(file_buf, 0, BMP_HEADER);
    file_buf[0] = 'B'; file_buf[1] = 'M';
    put32(file_buf + 2, size);
    put32(file_buf + 10, BMP_HEADER);
    put32(file_buf + 14, 40);                           /* BITMAPINFOHEADER */
    put32(file_buf + 18, CW);
    put32(file_buf + 22, CH);
    put16(file_buf + 26, 1);
    put16(file_buf + 28, 4);
    put32(file_buf + 34, ROW4 * CH);
    put32(file_buf + 38, 2835);                         /* 72 dpi */
    put32(file_buf + 42, 2835);
    put32(file_buf + 46, 16);
    put32(file_buf + 50, 16);
    for (int i = 0; i < 16; i++) {                      /* B, G, R, 0 */
        uint8_t *q = file_buf + 54 + i * 4;
        q[0] = (uint8_t)(theme[i][2] * 255 / 63);
        q[1] = (uint8_t)(theme[i][1] * 255 / 63);
        q[2] = (uint8_t)(theme[i][0] * 255 / 63);
    }
    for (int y = 0; y < CH; y++) {                      /* bottom row first */
        uint8_t *row = file_buf + BMP_HEADER + (CH - 1 - y) * ROW4;
        for (int x = 0; x < CW; x += 2)
            row[x / 2] = (uint8_t)(canvas[y * CW + x] << 4 | canvas[y * CW + x + 1]);
    }
    HANDLE f = CreateFile(path, GENERIC_WRITE, CREATE_ALWAYS);
    uint32_t done = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, file_buf, size, &done) && done == size;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (!ok) {
        DebugLog("paint could not save %s (error %d)", path, (int)GetLastError());
        error_box("Could not save ", path);
        return false;
    }
    strncpy0(file_path, path, sizeof file_path);
    dirty = false;
    set_title();
    DebugLog("paint saved %s", path);
    return true;
}

static uint8_t nearest(const uint8_t *bgr)              /* the closest theme colour */
{
    int best = 0;
    int32_t best_d = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        int32_t d = 0;
        for (int c = 0; c < 3; c++) {
            int32_t diff = (int32_t)bgr[2 - c] - theme[i][c] * 255 / 63;
            d += diff * diff;
        }
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

static bool open_bmp(const char *path)
{
    HANDLE f = CreateFile(path, GENERIC_READ, OPEN_EXISTING);
    if (f == INVALID_HANDLE_VALUE) {
        error_box("Cannot open ", path);
        return false;
    }
    uint32_t size = GetFileSize(f), got = 0;
    bool ok = size >= 54 && size <= sizeof file_buf && ReadFile(f, file_buf, size, &got) && got == size;
    CloseHandle(f);
    int32_t w = 0, h = 0;
    uint32_t bpp = 0, off = 0, colours = 0;
    if (ok) {
        w = (int32_t)get32(file_buf + 18);
        h = (int32_t)get32(file_buf + 22);
        bpp = get16(file_buf + 28);
        off = get32(file_buf + 10);
        colours = get32(file_buf + 46);
        if (!colours) colours = 1u << bpp;
        ok = file_buf[0] == 'B' && file_buf[1] == 'M' && get32(file_buf + 30) == 0 &&
             (bpp == 4 || bpp == 8) && w > 0 && w <= CW && h != 0 && (h < 0 ? -h : h) <= CH &&
             colours <= (1u << bpp) && 54 + colours * 4 <= off;
    }
    uint32_t stride = ((uint32_t)w * bpp + 31) / 32 * 4;
    int rows = h < 0 ? -h : h;
    if (ok && off + stride * (uint32_t)rows > size)
        ok = false;
    if (!ok) {
        error_box("Paint can open 16- and 256-colour BMP files up to 400x280.\n", path);
        return false;
    }
    uint8_t map[256];
    for (uint32_t i = 0; i < colours; i++)
        map[i] = nearest(file_buf + 14 + get32(file_buf + 14) + i * 4);
    memset(canvas, C_WHITE, sizeof canvas);
    for (int y = 0; y < rows; y++) {
        const uint8_t *row = file_buf + off + (uint32_t)(h < 0 ? y : rows - 1 - y) * stride;
        for (int x = 0; x < w; x++) {
            uint32_t v = bpp == 8 ? row[x] : (x & 1 ? row[x / 2] & 15 : row[x / 2] >> 4);
            canvas[y * CW + x] = v < colours ? map[v] : C_WHITE;
        }
    }
    strncpy0(file_path, path, sizeof file_path);
    dirty = false;
    set_title();
    RECT r = { CX, CY, CX + CW, CY + CH };
    InvalidateRect(paint_wnd, &r);
    DebugLog("paint opened %s (%dx%d)", path, (int)w, rows);
    return true;
}

static bool save_as(void)
{
    char path[128];
    strncpy0(path, file_path[0] ? file_path : "untitled.bmp", sizeof path);
    OPENFILENAME ofn = { paint_wnd, "*.BMP", path, sizeof path, NULL, "BMP", OFN_OVERWRITEPROMPT };
    return GetSaveFileName(&ofn) && save(path);
}

static bool save_current(void) { return file_path[0] ? save(file_path) : save_as(); }

/* Unsaved changes: save, discard, or (false) cancel what was asked for. */
static bool may_discard(void)
{
    if (!dirty)
        return true;
    char text[96] = "Save changes to ";
    strncpy0(text + strlen(text), file_path[0] ? base_name(file_path) : "untitled",
             sizeof text - strlen(text));
    strncpy0(text + strlen(text), "?", sizeof text - strlen(text));
    int r = MessageBox(paint_wnd, text, "Paint", MB_YESNOCANCEL | MB_ICONWARNING);
    return r == IDNO || (r == IDYES && save_current());
}

static void open_file(void)
{
    if (!may_discard())
        return;
    char path[128] = "";
    OPENFILENAME ofn = { paint_wnd, "*.BMP", path, sizeof path, NULL, "BMP", OFN_FILEMUSTEXIST };
    if (GetOpenFileName(&ofn))
        open_bmp(path);
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
    dirty = true;
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
        dirty = true;
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
    if (is_shape(tool))
        dirty = true;
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
            if (may_discard()) {
                clear_canvas();
                file_path[0] = '\0';
                dirty = false;
                set_title();
                DebugLog("paint new");
            }
            break;
        case ID_CLEAR:  clear_canvas(); dirty = true; DebugLog("paint cleared"); break;
        case ID_OPEN:   open_file(); break;
        case ID_SAVE:   save_current(); break;
        case ID_SAVEAS: save_as(); break;
        case ID_EXIT:   SendMessage(h, WM_CLOSE, 0, 0); break;
        case ID_ABOUT: ShellAbout(h, "Paint", NULL); break;
        }
        return 0;
    case WM_CLOSE:
        if (may_discard())
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
    AppendMenu(file, MF_STRING, ID_OPEN, "&Open...\tCtrl+O");
    AppendMenu(file, MF_STRING, ID_SAVE, "&Save\tCtrl+S");
    AppendMenu(file, MF_STRING, ID_SAVEAS, "Save &As...");
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
    static const ACCEL keys[] = { { FVIRTKEY | FCONTROL, 'N', ID_NEW },
                                  { FVIRTKEY | FCONTROL, 'O', ID_OPEN },
                                  { FVIRTKEY | FCONTROL, 'S', ID_SAVE } };
    HACCEL acc = CreateAcceleratorTable(keys, 3);
    const char *arg = GetStartupInfo()->args;           /* PAINT.BIN picture.bmp */
    if (arg[0])
        open_bmp(arg);
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
