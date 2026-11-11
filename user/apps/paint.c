/*
 * user/apps/paint.c -- PAINT.BIN, a bitmap painter.
 *
 * A canvas (400x280 to start; Image > Attributes sets the size, up to
 * 1024x768) in the 16 theme colours with a toolbox (pencil, brush,
 * eraser, fill, line, rectangle, filled rectangle, ellipse, select) and a
 * palette: the left button paints in the foreground colour, the right
 * button in the background colour (and picks it on the palette). Shapes
 * rubber-band while the button is held.
 *
 * The select tool marks a rectangle; dragging inside it lifts the pixels
 * and moves them (the hole filled with the background colour) until they
 * are dropped by a click outside or another tool. Cut, Copy and Paste go
 * through the clipboard as a picture (Paste: as a moving selection, the
 * canvas growing to fit if it must). View > Zoom magnifies 2, 4 or 8
 * times; scroll bars appear when the canvas is bigger than the window.
 * Edit > Undo takes back the last change (and undoes the undo).
 *
 * The picture lives in the process's own memory, one byte per pixel, and
 * reaches the screen through DrawPixels. Pictures are saved as 16-colour
 * BMP files and 16- or 256-colour BMPs can be opened (their colours mapped
 * to the nearest of the 16).
 */
#include "os1993.h"
#include <stdlib.h>

#define MAX_W   1024
#define MAX_H   768
#define CX      62                      /* the viewport's corner in the client area */
#define CY      6
#define TOOL    24
#define SWATCH  16
#define PAL_H   (2 * SWATCH + 2)
#define SCROLL_W 16                     /* (as UI.DLL draws them) */

enum { T_PENCIL, T_BRUSH, T_ERASER, T_FILL, T_LINE, T_RECT, T_FRECT, T_ELLIPSE, T_SELECT, T_COUNT };
enum { ID_NEW = 100, ID_EXIT, ID_CLEAR, ID_ABOUT, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_UNDO, ID_CUT, ID_COPY,
       ID_PASTE, ID_DELETE, ID_SELALL, ID_ATTR, ID_ZOOM1, ID_ZOOM2, ID_ZOOM4, ID_ZOOM8, ID_ZOOMIN,
       ID_ZOOMOUT, ID_HBAR, ID_VBAR, ID_W, ID_H, ID_WSPIN, ID_HSPIN };

static const char *const tool_names[T_COUNT] = {
    "pencil", "brush", "eraser", "fill", "line", "rectangle", "filled rectangle", "ellipse", "select",
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
    [T_SELECT]  = { 0, 0, 0x6DB6, 0, 0x4002, 0x4002, 0, 0x4002, 0x4002, 0, 0x4002, 0x4002, 0,
                    0x6DB6, 0, 0 },
};

static uint8_t *canvas;                 /* cw x chh, one byte a pixel */
static int cw = 400, chh = 280;
static uint8_t *undo_px;                /* the last state, for Undo */
static int undo_w, undo_h;
static bool can_undo;
static HWND paint_wnd, hbar, vbar;
static int tool = T_PENCIL;
static uint8_t fg = C_BLACK, bg = C_WHITE;
static bool drawing;
static uint8_t ink;                     /* this stroke's colour */
static int sx, sy, lx, ly;              /* stroke start, last point (canvas) */
static int zoom = 1, scroll_x, scroll_y;
static RECT view;                       /* the viewport (client coordinates) */
static int pal_y;                       /* the palette's top */

/* the selection (canvas coordinates) */
static bool sel_on, sel_float, sel_drag, selecting;
static RECT sel;
static uint8_t *sel_px;                 /* floating: its pixels, sel's size */
static int drag_dx, drag_dy;

static bool dirty;                      /* changed since it was saved */
static char file_path[128];             /* "" while untitled */

/* ---- the picture ---------------------------------------------------------------- */

static void put(int x, int y, uint8_t c)
{
    if (x >= 0 && y >= 0 && x < cw && y < chh)
        canvas[y * cw + x] = c;
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
    uint8_t old = canvas[y * cw + x];
    if (old == c)
        return;
    static int16_t stack[8192][2];
    int n = 0;
    stack[n][0] = (int16_t)x; stack[n][1] = (int16_t)y; n++;
    while (n) {
        n--;
        int px = stack[n][0], py = stack[n][1];
        if (canvas[py * cw + px] != old)
            continue;
        int l = px, r = px;
        while (l > 0 && canvas[py * cw + l - 1] == old) l--;
        while (r < cw - 1 && canvas[py * cw + r + 1] == old) r++;
        for (int i = l; i <= r; i++)
            canvas[py * cw + i] = c;
        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = py + dir;
            if (ny < 0 || ny >= chh)
                continue;
            for (int i = l; i <= r; i++)
                if (canvas[ny * cw + i] == old && (i == l || canvas[ny * cw + i - 1] != old) &&
                    n < 8192) {
                    stack[n][0] = (int16_t)i; stack[n][1] = (int16_t)ny; n++;
                }
        }
    }
}

/* Remember the picture before a change (Undo). */
static void checkpoint(void)
{
    memcpy(undo_px, canvas, (size_t)cw * chh);
    undo_w = cw;
    undo_h = chh;
    can_undo = true;
}

static uint8_t *scratch(void)                           /* a picture-sized work buffer */
{
    static uint8_t *tmp;
    if (!tmp) tmp = malloc((size_t)MAX_W * MAX_H);
    return tmp;
}

/* A new size, the picture kept at the top left, new room in white. */
static bool resize_canvas(int w, int h)
{
    w = w < 1 ? 1 : w > MAX_W ? MAX_W : w;
    h = h < 1 ? 1 : h > MAX_H ? MAX_H : h;
    uint8_t *tmp = scratch();
    if (!tmp)
        return false;
    memset(tmp, C_WHITE, (size_t)w * h);
    for (int y = 0; y < h && y < chh; y++)
        memcpy(tmp + y * w, canvas + y * cw, (size_t)(w < cw ? w : cw));
    memcpy(canvas, tmp, (size_t)w * h);
    cw = w;
    chh = h;
    return true;
}

/* ---- the view: zoom and scrolling --------------------------------------------- */

static int to_x(int cx) { return view.left + (cx - scroll_x) * zoom; }     /* canvas -> client */
static int to_y(int cy) { return view.top + (cy - scroll_y) * zoom; }
static int vis_w(void) { return (view.right - view.left) / zoom; }         /* canvas pixels shown */
static int vis_h(void) { return (view.bottom - view.top) / zoom; }

static void invalidate_view(void) { InvalidateRect(paint_wnd, &view); }

static void invalidate_canvas(RECT r)                   /* canvas coordinates */
{
    RECT s = { to_x(r.left) - 4 * zoom, to_y(r.top) - 4 * zoom, to_x(r.right) + 4 * zoom,
               to_y(r.bottom) + 4 * zoom };
    if (s.left < view.left) s.left = view.left;
    if (s.top < view.top) s.top = view.top;
    if (s.right > view.right) s.right = view.right;
    if (s.bottom > view.bottom) s.bottom = view.bottom;
    if (s.left < s.right && s.top < s.bottom)
        InvalidateRect(paint_wnd, &s);
}

static void scroll_to(int x, int y)
{
    int mx = cw - vis_w(), my = chh - vis_h();
    x = x > mx ? mx : x;
    y = y > my ? my : y;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    if (x != scroll_x || y != scroll_y) {
        scroll_x = x;
        scroll_y = y;
        invalidate_view();
    }
    SendMessage(hbar, SBM_SETPOS, (WPARAM)scroll_x, 0);
    SendMessage(vbar, SBM_SETPOS, (WPARAM)scroll_y, 0);
}

/* The viewport, the scroll bars (only when the canvas doesn't fit) and the
 * palette for the client area's size. */
static void layout(void)
{
    RECT c;
    GetClientRect(paint_wnd, &c);
    pal_y = c.bottom - PAL_H - 8;
    int w = c.right - CX - 8, h = pal_y - 8 - CY;
    bool hb = false, vb = false;
    for (int k = 0; k < 2; k++) {               /* one bar can make the other needed */
        hb = cw * zoom > w - (vb ? SCROLL_W : 0);
        vb = chh * zoom > h - (hb ? SCROLL_W : 0);
    }
    int vw = w - (vb ? SCROLL_W : 0), vh = h - (hb ? SCROLL_W : 0);
    if (vw > cw * zoom) vw = cw * zoom;
    if (vh > chh * zoom) vh = chh * zoom;
    if (vw < 8) vw = 8;
    if (vh < 8) vh = 8;
    view = (RECT){ CX, CY, CX + vw, CY + vh };
    MoveWindow(hbar, CX, CY + vh, vw, SCROLL_W, true);
    MoveWindow(vbar, CX + vw, CY, SCROLL_W, vh, true);
    ShowWindow(hbar, hb ? SW_SHOW : SW_HIDE);
    ShowWindow(vbar, vb ? SW_SHOW : SW_HIDE);
    SendMessage(hbar, SBM_SETRANGE, 0, cw - vis_w() > 0 ? cw - vis_w() : 0);
    SendMessage(hbar, SBM_SETPAGE, (WPARAM)vis_w(), 0);
    SendMessage(vbar, SBM_SETRANGE, 0, chh - vis_h() > 0 ? chh - vis_h() : 0);
    SendMessage(vbar, SBM_SETPAGE, (WPARAM)vis_h(), 0);
    scroll_to(scroll_x, scroll_y);
    InvalidateRect(paint_wnd, NULL);
}

static void set_zoom(int z)
{
    if (z < 1) z = 1;
    if (z > 8) z = 8;
    if (z == zoom) return;
    int cxm = scroll_x + vis_w() / 2, cym = scroll_y + vis_h() / 2;    /* keep the middle */
    zoom = z;
    layout();
    scroll_to(cxm - vis_w() / 2, cym - vis_h() / 2);
    CheckMenuRadioItem(GetMenu(paint_wnd), ID_ZOOM1, ID_ZOOM8,
                       z == 1 ? ID_ZOOM1 : z == 2 ? ID_ZOOM2 : z == 4 ? ID_ZOOM4 : ID_ZOOM8, MF_BYCOMMAND);
    DebugLog("paint zoom %dx", zoom);
}

/* ---- the selection --------------------------------------------------------------- */

static int sel_w(void) { return sel.right - sel.left; }
static int sel_h(void) { return sel.bottom - sel.top; }

static void invalidate_sel(void)
{
    RECT r = { sel.left - 1, sel.top - 1, sel.right + 1, sel.bottom + 1 };
    invalidate_canvas(r);
}

/* Drop a moving selection where it is. */
static void drop(void)
{
    if (sel_float) {
        for (int y = 0; y < sel_h(); y++)
            for (int x = 0; x < sel_w(); x++)
                put(sel.left + x, sel.top + y, sel_px[y * sel_w() + x]);
        free(sel_px);
        sel_px = NULL;
        sel_float = false;
        dirty = true;
        DebugLog("paint dropped the selection at %d,%d", (int)sel.left, (int)sel.top);
    }
    if (sel_on) invalidate_sel();
    sel_on = false;
}

/* Lift the selected pixels to move them (the hole: background colour). */
static bool lift(void)
{
    if (sel_float) return true;
    int w = sel_w(), h = sel_h();
    if (!(sel_px = malloc((size_t)w * h)))
        return false;
    checkpoint();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            sel_px[y * w + x] = canvas[(sel.top + y) * cw + sel.left + x];
            canvas[(sel.top + y) * cw + sel.left + x] = bg;
        }
    sel_float = true;
    dirty = true;
    return true;
}

static void copy_sel(void)
{
    if (!sel_on) return;
    int w = sel_w(), h = sel_h();
    uint8_t *px = sel_float ? sel_px : malloc((size_t)w * h);
    if (!px) return;
    if (!sel_float)
        for (int y = 0; y < h; y++)
            memcpy(px + y * w, canvas + (sel.top + y) * cw + sel.left, (size_t)w);
    bool ok = SetClipboardBitmap(w, h, px);
    if (!sel_float) free(px);
    DebugLog("paint copied %dx%d%s", w, h, ok ? "" : " (too big for the clipboard)");
}

static void delete_sel(void)
{
    if (!sel_on) return;
    if (sel_float) {                            /* the hole is already filled */
        free(sel_px);
        sel_px = NULL;
        sel_float = false;
    } else {
        checkpoint();
        rect(sel, bg, true);
    }
    dirty = true;
    invalidate_sel();
    sel_on = false;
}

static void paste(void)
{
    int w, h;
    if (!GetClipboardBitmap(&w, &h, NULL, 0)) {
        MessageBox(paint_wnd, "The clipboard has no picture.", "Paint", MB_OK | MB_ICONINFORMATION);
        return;
    }
    drop();
    uint8_t *px = malloc((size_t)w * h);
    if (!px) return;
    GetClipboardBitmap(&w, &h, px, w * h);
    checkpoint();
    if (w > cw || h > chh) {                    /* the canvas grows to take it */
        resize_canvas(w > cw ? w : cw, h > chh ? h : chh);
        layout();
    }
    int x = scroll_x, y = scroll_y;             /* at the top left of the view */
    if (x + w > cw) x = cw - w;
    if (y + h > chh) y = chh - h;
    sel = (RECT){ x, y, x + w, y + h };
    sel_px = px;
    sel_on = sel_float = true;
    dirty = true;
    tool = T_SELECT;
    InvalidateRect(paint_wnd, NULL);
    DebugLog("paint pasted %dx%d", w, h);
}

/* ---- BMP files ------------------------------------------------------------------ */

/* The theme's colours (6-bit DAC values, as UI.DLL loads them) */
static const uint8_t theme[16][3] = {
    {  0,  0,  0 }, { 13, 13, 13 }, { 21, 21, 21 }, { 30, 30, 30 }, { 38, 38, 38 }, { 46, 46, 46 },
    { 55, 55, 55 }, { 59, 59, 59 }, { 63, 63, 63 }, { 26, 26, 50 }, { 50, 50, 63 }, { 17, 30, 40 },
    { 52,  8,  8 }, {  8, 40, 12 }, { 63, 56, 16 }, { 16, 16, 40 },
};

#define BMP_HEADER 118                  /* file and info headers, 16 palette entries */

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
    drop();
    uint32_t row4 = ((uint32_t)cw * 4 + 31) / 32 * 4, size = BMP_HEADER + row4 * (uint32_t)chh;
    uint8_t *buf = calloc(1, size);
    if (!buf) {
        error_box("Not enough memory to save ", path);
        return false;
    }
    buf[0] = 'B'; buf[1] = 'M';
    put32(buf + 2, size);
    put32(buf + 10, BMP_HEADER);
    put32(buf + 14, 40);                                /* BITMAPINFOHEADER */
    put32(buf + 18, (uint32_t)cw);
    put32(buf + 22, (uint32_t)chh);
    put16(buf + 26, 1);
    put16(buf + 28, 4);
    put32(buf + 34, row4 * (uint32_t)chh);
    put32(buf + 38, 2835);                              /* 72 dpi */
    put32(buf + 42, 2835);
    put32(buf + 46, 16);
    put32(buf + 50, 16);
    for (int i = 0; i < 16; i++) {                      /* B, G, R, 0 */
        uint8_t *q = buf + 54 + i * 4;
        q[0] = (uint8_t)(theme[i][2] * 255 / 63);
        q[1] = (uint8_t)(theme[i][1] * 255 / 63);
        q[2] = (uint8_t)(theme[i][0] * 255 / 63);
    }
    for (int y = 0; y < chh; y++) {                     /* bottom row first */
        uint8_t *row = buf + BMP_HEADER + (uint32_t)(chh - 1 - y) * row4;
        for (int x = 0; x < cw; x += 2)
            row[x / 2] = (uint8_t)(canvas[y * cw + x] << 4 | (x + 1 < cw ? canvas[y * cw + x + 1] : 0));
    }
    HANDLE f = CreateFile(path, GENERIC_WRITE, CREATE_ALWAYS);
    uint32_t done = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, buf, size, &done) && done == size;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    free(buf);
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
    uint8_t *buf = size >= 54 && size <= 1078 + MAX_W * MAX_H ? malloc(size) : NULL;
    bool ok = buf && ReadFile(f, buf, size, &got) && got == size;
    CloseHandle(f);
    int32_t w = 0, h = 0;
    uint32_t bpp = 0, off = 0, colours = 0;
    if (ok) {
        w = (int32_t)get32(buf + 18);
        h = (int32_t)get32(buf + 22);
        bpp = get16(buf + 28);
        off = get32(buf + 10);
        colours = get32(buf + 46);
        if (!colours) colours = 1u << bpp;
        ok = buf[0] == 'B' && buf[1] == 'M' && get32(buf + 30) == 0 &&
             (bpp == 4 || bpp == 8) && w > 0 && w <= MAX_W && h != 0 && (h < 0 ? -h : h) <= MAX_H &&
             colours <= (1u << bpp) && 54 + colours * 4 <= off;
    }
    uint32_t stride = ((uint32_t)w * bpp + 31) / 32 * 4;
    int rows = h < 0 ? -h : h;
    if (ok && off + stride * (uint32_t)rows > size)
        ok = false;
    if (!ok) {
        free(buf);
        error_box("Paint can open 16- and 256-colour BMP files up to 1024x768.\n", path);
        return false;
    }
    drop();
    uint8_t map[256];
    for (uint32_t i = 0; i < colours; i++)
        map[i] = nearest(buf + 14 + get32(buf + 14) + i * 4);
    cw = w;
    chh = rows;
    memset(canvas, C_WHITE, (size_t)cw * chh);
    for (int y = 0; y < rows; y++) {
        const uint8_t *row = buf + off + (uint32_t)(h < 0 ? y : rows - 1 - y) * stride;
        for (int x = 0; x < w; x++) {
            uint32_t v = bpp == 8 ? row[x] : (x & 1 ? row[x / 2] & 15 : row[x / 2] >> 4);
            canvas[y * cw + x] = v < colours ? map[v] : C_WHITE;
        }
    }
    free(buf);
    can_undo = false;
    strncpy0(file_path, path, sizeof file_path);
    dirty = false;
    scroll_x = scroll_y = 0;
    set_title();
    layout();
    DebugLog("paint opened %s (%dx%d)", path, (int)w, rows);
    return true;
}

static bool save_as(void)
{
    char path[128];
    strncpy0(path, file_path[0] ? file_path : "untitled.bmp", sizeof path);
    OPENFILENAME ofn = { paint_wnd, "*.BMP", path, sizeof path, NULL, "BMP", OFN_OVERWRITEPROMPT,
                         "Bitmaps (*.BMP)\0*.BMP\0All files (*.*)\0*.*\0", 1 };
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
    OPENFILENAME ofn = { paint_wnd, "*.BMP", path, sizeof path, NULL, "BMP", OFN_FILEMUSTEXIST,
                         "Bitmaps (*.BMP)\0*.BMP\0All files (*.*)\0*.*\0", 1 };
    if (GetOpenFileName(&ofn))
        open_bmp(path);
}

/* ---- Image > Attributes: the canvas size ----------------------------------------- */

#define SPIN (UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS)
static const DLGITEM attr_items[] = {
    { WC_STATIC, "&Width:", 0, 0, 12, 14, 60, 16, 0 },
    { WC_EDIT, "", WS_TABSTOP | ES_NUMBER, 0, 76, 8, 64, 28, ID_W },
    { WC_UPDOWN, "", SPIN, 0, 0, 0, 16, 22, ID_WSPIN },
    { WC_STATIC, "&Height:", 0, 0, 12, 46, 60, 16, 0 },
    { WC_EDIT, "", WS_TABSTOP | ES_NUMBER, 0, 76, 40, 64, 28, ID_H },
    { WC_UPDOWN, "", SPIN, 0, 0, 0, 16, 22, ID_HSPIN },
    { WC_STATIC, "pixels", 0, 0, 166, 30, 60, 16, 0 },
    { WC_BUTTON, "OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 50, 80, 84, 30, IDOK },
    { WC_BUTTON, "Cancel", WS_TABSTOP, 0, 144, 84, 76, 22, IDCANCEL },
};
static const DLGTEMPLATE attr_dlg = { "Attributes", 0, 0, CW_USEDEFAULT, 0, 232, 120,
                                      sizeof attr_items / sizeof attr_items[0], attr_items, 0 };

static intptr_t attr_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessage(h, ID_WSPIN, UDM_SETRANGE32, 1, MAX_W);
        SendDlgItemMessage(h, ID_HSPIN, UDM_SETRANGE32, 1, MAX_H);
        SendDlgItemMessage(h, ID_WSPIN, UDM_SETPOS32, 0, cw);
        SendDlgItemMessage(h, ID_HSPIN, UDM_SETPOS32, 0, chh);
        return 1;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            bool okw, okh;
            int w = GetDlgItemInt(h, ID_W, &okw, false), hh = GetDlgItemInt(h, ID_H, &okh, false);
            if (!okw || !okh || w < 1 || hh < 1 || w > MAX_W || hh > MAX_H) {
                MessageBox(h, "The width can be 1 to 1024 pixels, the height 1 to 768.", "Attributes",
                           MB_OK | MB_ICONWARNING);
                return 1;
            }
            EndDialog(h, MAKELONG(w, hh));
            return 1;
        }
        return 0;
    }
    return 0;
}

static void attributes(void)
{
    int r = DialogBox(&attr_dlg, paint_wnd, attr_proc, 0);
    if (r <= 0)
        return;
    int w = LOWORD(r), h = HIWORD(r);
    if (w == cw && h == chh)
        return;
    drop();
    checkpoint();
    resize_canvas(w, h);
    dirty = true;
    layout();
    DebugLog("paint canvas %dx%d", cw, chh);
}

/* ---- the window ------------------------------------------------------------------ */

static RECT tool_rect(int t)  { int x = 6 + (t % 2) * (TOOL + 2), y = CY + (t / 2) * (TOOL + 2);
                                return (RECT){ x, y, x + TOOL, y + TOOL }; }
static RECT swatch_rect(int c) { int x = 50 + (c % 8) * (SWATCH + 2), y = pal_y + (c / 8) * (SWATCH + 2);
                                 return (RECT){ x, y, x + SWATCH, y + SWATCH }; }
static RECT colours_rect(void) { return (RECT){ 6, pal_y, 42, pal_y + PAL_H }; }

static bool is_shape(int t) { return t == T_LINE || t == T_RECT || t == T_FRECT || t == T_ELLIPSE; }

static void dotted(HDC dc, RECT r)                      /* client coordinates */
{
    for (int x = r.left; x < r.right; x++) {
        uint8_t c = ((x - r.left) / 3) & 1 ? C_WHITE : C_BLACK;
        SetPixel(dc, x, r.top, c);
        SetPixel(dc, x, r.bottom - 1, c);
    }
    for (int y = r.top; y < r.bottom; y++) {
        uint8_t c = ((y - r.top) / 3) & 1 ? C_WHITE : C_BLACK;
        SetPixel(dc, r.left, y, c);
        SetPixel(dc, r.right - 1, y, c);
    }
}

/* Part of the canvas (and a moving selection over it), scaled, row by row. */
static void paint_canvas(HDC dc, RECT c)
{
    if (c.left < view.left) c.left = view.left;
    if (c.top < view.top) c.top = view.top;
    if (c.right > view.right) c.right = view.right;
    if (c.bottom > view.bottom) c.bottom = view.bottom;
    if (c.left >= c.right || c.top >= c.bottom)
        return;
    int w = c.right - c.left;
    static uint8_t row[MAX_W * 8];
    for (int y = c.top; y < c.bottom; y++) {
        int cy = scroll_y + (y - view.top) / zoom;
        for (int x = c.left; x < c.right; x++) {
            int cx = scroll_x + (x - view.left) / zoom;
            uint8_t p = cx < cw && cy < chh ? canvas[cy * cw + cx] : C_WHITE;
            if (sel_float && cx >= sel.left && cx < sel.right && cy >= sel.top && cy < sel.bottom)
                p = sel_px[(cy - sel.top) * sel_w() + cx - sel.left];
            row[x - c.left] = p;
        }
        DrawPixels(dc, c.left, y, w, 1, row, w);
    }
}

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
    RECT frame = { view.left - 2, view.top - 2, view.right + 2, view.bottom + 2 };      /* the canvas */
    DrawBevel(dc, &frame, false);
    if (zoom == 1 && !sel_float) {
        int w = view.right - view.left, hh = view.bottom - view.top;
        DrawPixels(dc, view.left, view.top, w, hh, canvas + scroll_y * cw + scroll_x, cw);
    } else {
        paint_canvas(dc, ps.rcPaint);
    }
    if (drawing && is_shape(tool)) {                    /* rubber band */
        RECT r = norm(sx, sy, lx, ly);
        RECT s = { to_x(r.left), to_y(r.top), to_x(r.right), to_y(r.bottom) };
        int hz = zoom / 2;
        if (tool == T_LINE)         DrawLineEx(dc, to_x(sx) + hz, to_y(sy) + hz, to_x(lx) + hz, to_y(ly) + hz, ink);
        else if (tool == T_RECT)    FrameRect(dc, &s, ink);
        else if (tool == T_FRECT)   FillRect(dc, &s, ink);
        else                        DrawEllipse(dc, &s, ink, false);
    }
    if (sel_on || selecting) {                          /* the selection's dotted edge */
        RECT r = selecting ? norm(sx, sy, lx, ly) : sel;
        RECT s = { to_x(r.left), to_y(r.top), to_x(r.right), to_y(r.bottom) };
        if (s.right > s.left && s.bottom > s.top)
            dotted(dc, s);
    }
    RECT cur = colours_rect();                          /* current colours */
    DrawBevel(dc, &cur, false);
    RECT b = { 20, pal_y + 14, 38, pal_y + 32 }, f = { 10, pal_y + 4, 28, pal_y + 22 };
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
    int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
    int px = mx < view.left ? scroll_x - 1 : (mx - view.left) / zoom + scroll_x;
    int py = my < view.top ? scroll_y - 1 : (my - view.top) / zoom + scroll_y;
    *x = px < 0 ? 0 : px >= cw ? cw - 1 : px;
    *y = py < 0 ? 0 : py >= chh ? chh - 1 : py;
}

static bool in_view(int x, int y) { return PtInRect(&view, x, y); }

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

static void pick_tool(HWND h, int t)
{
    if (t != T_SELECT) drop();
    tool = t;
    DebugLog("paint tool %s", tool_names[t]);
    RECT all = { 0, 0, CX - 4, CY + 5 * (TOOL + 2) };
    InvalidateRect(h, &all);
}

static void begin(HWND h, LPARAM lp, bool right)
{
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    for (int t = 0; t < T_COUNT; t++) {
        RECT r = tool_rect(t);
        if (PtInRect(&r, x, y)) {
            pick_tool(h, t);
            return;
        }
    }
    for (int c = 0; c < 16; c++) {
        RECT r = swatch_rect(c);
        if (PtInRect(&r, x, y)) {
            if (right) bg = (uint8_t)c; else fg = (uint8_t)c;
            DebugLog("paint %s colour %d", right ? "background" : "foreground", c);
            RECT cur = colours_rect();
            InvalidateRect(h, &cur);
            return;
        }
    }
    if (!in_view(x, y))
        return;
    SetFocus(h);
    canvas_point(lp, &sx, &sy);
    lx = sx;
    ly = sy;
    if (tool == T_SELECT) {
        SetCapture(h);
        if (sel_on && sx >= sel.left && sx < sel.right && sy >= sel.top && sy < sel.bottom && !right) {
            if (lift()) {                       /* drag the selection along */
                sel_drag = true;
                drag_dx = sx - sel.left;
                drag_dy = sy - sel.top;
            }
            return;
        }
        drop();
        selecting = true;
        return;
    }
    drop();
    ink = tool == T_ERASER ? bg : right ? bg : fg;
    checkpoint();
    if (tool == T_FILL) {
        dirty = true;
        flood(sx, sy, ink);
        DebugLog("paint fill at %d,%d", sx, sy);
        invalidate_view();
        return;
    }
    drawing = true;
    SetCapture(h);
    if (!is_shape(tool))
        stroke_to(sx, sy);
}

static void end(void)
{
    if (sel_drag || selecting) {
        ReleaseCapture();
        if (selecting) {
            selecting = false;
            RECT r = norm(sx, sy, lx, ly);
            sel_on = r.right - r.left > 1 || r.bottom - r.top > 1;
            sel = r;
            if (sel_on)
                DebugLog("paint selected %d,%d %dx%d", (int)r.left, (int)r.top, sel_w(), sel_h());
            invalidate_canvas(r);
        }
        sel_drag = false;
        return;
    }
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

static void moved(LPARAM lp)
{
    int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
    bool over = in_view(mx, my);
    int x, y;
    canvas_point(lp, &x, &y);
    bool in_sel = sel_on && tool == T_SELECT && x >= sel.left && x < sel.right && y >= sel.top && y < sel.bottom;
    int shape = sel_drag || (over && in_sel && !selecting) ? IDC_SIZEALL :
                drawing || selecting || over ? IDC_CROSS : IDC_ARROW;
    SetCursor(LoadCursor(NULL, shape));
    if (sel_drag) {
        invalidate_sel();
        int nx = x - drag_dx, ny = y - drag_dy;
        sel = (RECT){ nx, ny, nx + sel_w(), ny + sel_h() };
        invalidate_sel();
        return;
    }
    if (selecting) {
        RECT o = norm(sx, sy, lx, ly);
        lx = x;
        ly = y;
        RECT n = norm(sx, sy, lx, ly);
        invalidate_canvas(o);
        invalidate_canvas(n);
        return;
    }
    if (!drawing)
        return;
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

static void undo(void)
{
    if (!can_undo) return;
    drop();
    uint8_t *tmp = scratch();
    if (!tmp) return;
    int w = cw, h = chh;
    memcpy(tmp, canvas, (size_t)w * h);
    memcpy(canvas, undo_px, (size_t)undo_w * undo_h);
    memcpy(undo_px, tmp, (size_t)w * h);
    cw = undo_w;
    chh = undo_h;
    undo_w = w;
    undo_h = h;
    dirty = true;
    layout();
    DebugLog("paint undo");
}

static void on_scroll(HWND bar, int code, int pos)
{
    bool v = bar == vbar;
    int cur = v ? scroll_y : scroll_x, page = v ? vis_h() : vis_w(), to = cur;
    switch (code) {
    case SB_LINEUP:        to = cur - 8; break;
    case SB_LINEDOWN:      to = cur + 8; break;
    case SB_PAGEUP:        to = cur - page; break;
    case SB_PAGEDOWN:      to = cur + page; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: to = pos; break;
    default: return;
    }
    if (v) scroll_to(scroll_x, to); else scroll_to(to, scroll_y);
}

static LRESULT paint_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        hbar = CreateWindow(WC_SCROLLBAR, "", WS_CHILD | SBS_HORZ, 0, 0, 10, SCROLL_W, h, ID_HBAR, NULL);
        vbar = CreateWindow(WC_SCROLLBAR, "", WS_CHILD | SBS_VERT, 0, 0, SCROLL_W, 10, h, ID_VBAR, NULL);
        return 0;
    case WM_SIZE:
        if (hbar) layout();
        return 0;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL:
        on_scroll((HWND)lp, LOWORD(wp), (int16_t)HIWORD(wp));
        return 0;
    case WM_LBUTTONDOWN: begin(h, lp, false); return 0;
    case WM_RBUTTONDOWN: begin(h, lp, true); return 0;
    case WM_MOUSEMOVE:   moved(lp); return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        end();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && (sel_on || selecting)) {
            if (selecting) { selecting = false; ReleaseCapture(); invalidate_view(); }
            else drop();
        } else if (wp == VK_DELETE) {
            delete_sel();
        }
        return 0;
    case WM_INITMENUPOPUP: {
        HMENU m = (HMENU)wp;
        EnableMenuItem(m, ID_UNDO, can_undo ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(m, ID_CUT, sel_on ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(m, ID_COPY, sel_on ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(m, ID_DELETE, sel_on ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(m, ID_PASTE, IsClipboardFormatAvailable(CF_BITMAP) ? MF_ENABLED : MF_GRAYED);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_NEW:
            if (may_discard()) {
                drop();
                cw = 400;
                chh = 280;
                memset(canvas, C_WHITE, (size_t)cw * chh);
                can_undo = false;
                file_path[0] = '\0';
                dirty = false;
                scroll_x = scroll_y = 0;
                set_title();
                layout();
                DebugLog("paint new");
            }
            break;
        case ID_CLEAR:
            drop();
            checkpoint();
            memset(canvas, C_WHITE, (size_t)cw * chh);
            dirty = true;
            invalidate_view();
            DebugLog("paint cleared");
            break;
        case ID_OPEN:    open_file(); break;
        case ID_SAVE:    save_current(); break;
        case ID_SAVEAS:  save_as(); break;
        case ID_EXIT:    SendMessage(h, WM_CLOSE, 0, 0); break;
        case ID_ABOUT:   ShellAbout(h, "Paint", NULL); break;
        case ID_UNDO:    undo(); break;
        case ID_CUT:     copy_sel(); delete_sel(); break;
        case ID_COPY:    copy_sel(); break;
        case ID_PASTE:   paste(); break;
        case ID_DELETE:  delete_sel(); break;
        case ID_SELALL:
            drop();
            pick_tool(h, T_SELECT);
            sel = (RECT){ 0, 0, cw, chh };
            sel_on = true;
            invalidate_view();
            break;
        case ID_ATTR:    attributes(); break;
        case ID_ZOOM1:   set_zoom(1); break;
        case ID_ZOOM2:   set_zoom(2); break;
        case ID_ZOOM4:   set_zoom(4); break;
        case ID_ZOOM8:   set_zoom(8); break;
        case ID_ZOOMIN:  set_zoom(zoom * 2); break;
        case ID_ZOOMOUT: set_zoom(zoom / 2); break;
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
    canvas = malloc((size_t)MAX_W * MAX_H);
    undo_px = malloc((size_t)MAX_W * MAX_H);
    if (!canvas || !undo_px) {
        MessageBox(NULL, "Not enough memory for Paint.", "Paint", MB_OK | MB_ICONERROR);
        return 1;
    }
    memset(canvas, C_WHITE, (size_t)cw * chh);
    WNDCLASS wc = { .name = "PaintWindow", .proc = paint_proc, .background = C_FACE, .icon = ICON_PAINT };
    RegisterClass(&wc);
    HMENU file = CreatePopupMenu(), edit = CreatePopupMenu(), viewm = CreatePopupMenu(),
          zoomm = CreatePopupMenu(), image = CreatePopupMenu(), help = CreatePopupMenu(), bar = CreateMenu();
    AppendMenu(file, MF_STRING, ID_NEW, "&New\tCtrl+N");
    AppendMenu(file, MF_STRING, ID_OPEN, "&Open...\tCtrl+O");
    AppendMenu(file, MF_STRING, ID_SAVE, "&Save\tCtrl+S");
    AppendMenu(file, MF_STRING, ID_SAVEAS, "Save &As...");
    AppendMenu(file, MF_SEPARATOR, 0, NULL);
    AppendMenu(file, MF_STRING, ID_EXIT, "E&xit");
    AppendMenu(edit, MF_STRING, ID_UNDO, "&Undo\tCtrl+Z");
    AppendMenu(edit, MF_SEPARATOR, 0, NULL);
    AppendMenu(edit, MF_STRING, ID_CUT, "Cu&t\tCtrl+X");
    AppendMenu(edit, MF_STRING, ID_COPY, "&Copy\tCtrl+C");
    AppendMenu(edit, MF_STRING, ID_PASTE, "&Paste\tCtrl+V");
    AppendMenu(edit, MF_STRING, ID_DELETE, "C&lear Selection\tDel");
    AppendMenu(edit, MF_STRING, ID_SELALL, "Select &All\tCtrl+A");
    AppendMenu(zoomm, MF_STRING | MF_CHECKED | MFT_RADIOCHECK, ID_ZOOM1, "&Normal Size\tCtrl+PgUp");
    AppendMenu(zoomm, MF_STRING, ID_ZOOM2, "&2x");
    AppendMenu(zoomm, MF_STRING, ID_ZOOM4, "&4x");
    AppendMenu(zoomm, MF_STRING, ID_ZOOM8, "&8x\tCtrl+PgDn");
    AppendMenu(viewm, MF_POPUP, (uintptr_t)zoomm, "&Zoom");
    AppendMenu(image, MF_STRING, ID_ATTR, "&Attributes...\tCtrl+E");
    AppendMenu(image, MF_STRING, ID_CLEAR, "&Clear Image");
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About Paint");
    AppendMenu(bar, MF_POPUP, (uintptr_t)file, "&File");
    AppendMenu(bar, MF_POPUP, (uintptr_t)edit, "&Edit");
    AppendMenu(bar, MF_POPUP, (uintptr_t)viewm, "&View");
    AppendMenu(bar, MF_POPUP, (uintptr_t)image, "&Image");
    AppendMenu(bar, MF_POPUP, (uintptr_t)help, "&Help");
    paint_wnd = CreateWindow("PaintWindow", "untitled - Paint", WS_OVERLAPPEDWINDOW, 30, 24,
                             CX + 400 + 8 + 10, CY + 280 + 8 + PAL_H + 8 + 25 + 21 + 5, NULL, 0, NULL);
    SetMenu(paint_wnd, bar);
    layout();
    ShowWindow(paint_wnd, SW_SHOW);
    static const ACCEL keys[] = { { FVIRTKEY | FCONTROL, 'N', ID_NEW },
                                  { FVIRTKEY | FCONTROL, 'O', ID_OPEN },
                                  { FVIRTKEY | FCONTROL, 'S', ID_SAVE },
                                  { FVIRTKEY | FCONTROL, 'Z', ID_UNDO },
                                  { FVIRTKEY | FCONTROL, 'X', ID_CUT },
                                  { FVIRTKEY | FCONTROL, 'C', ID_COPY },
                                  { FVIRTKEY | FCONTROL, 'V', ID_PASTE },
                                  { FVIRTKEY | FCONTROL, 'A', ID_SELALL },
                                  { FVIRTKEY | FCONTROL, 'E', ID_ATTR },
                                  { FVIRTKEY | FCONTROL, VK_PRIOR, ID_ZOOMOUT },
                                  { FVIRTKEY | FCONTROL, VK_NEXT, ID_ZOOMIN } };
    HACCEL acc = CreateAcceleratorTable(keys, sizeof keys / sizeof keys[0]);
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
