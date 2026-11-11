/*
 * paint.c -- invalidation, painting, and drawing through an HDC.
 *
 * Every top-level window has its own update region (a few rectangles,
 * screen coordinates); invalidating part of the screen adds to the region
 * of each window there, and wakes the processes they belong to. A process paints
 * only its own windows, each clipped to its visible region -- what's left
 * of it after taking away the windows above -- so processes can paint in
 * any order: per region rectangle, the frame, the client area cleared to
 * its class background and a WM_PAINT, then the same for its children.
 * Screen areas no window covers (the desktop, until a shell provides one)
 * are filled by whichever process paints next. Painting goes into the
 * off-screen copy of the screen in the shared arena, and each finished
 * rectangle is copied to video memory (double buffering).
 */
#include "uipriv.h"

#define REGION_MAX 48

struct gfx ui_gfx;                          /* this process's view of the back buffer */
static struct dc paint_dc;
static struct wnd *painting;

struct region { int n; RECT r[REGION_MAX]; };

/* ---- regions ------------------------------------------------------------- */

static void region_add(struct region *g, RECT r)
{
    if (r.left < r.right && r.top < r.bottom && g->n < REGION_MAX)
        g->r[g->n++] = r;
}

/* Take x away from every rectangle (each splits into at most four). */
static void region_subtract(struct region *g, RECT x)
{
    struct region out = { 0 };
    for (int i = 0; i < g->n; i++) {
        RECT a = g->r[i], in;
        if (!rect_intersect(a, x, &in)) {
            region_add(&out, a);
            continue;
        }
        region_add(&out, (RECT){ a.left, a.top, a.right, in.top });         /* above */
        region_add(&out, (RECT){ a.left, in.bottom, a.right, a.bottom });   /* below */
        region_add(&out, (RECT){ a.left, in.top, in.left, in.bottom });     /* left */
        region_add(&out, (RECT){ in.right, in.top, a.right, in.bottom });   /* right */
    }
    *g = out;
}

/* Does a window cover the screen at frame_outer? (Tiles sit below every
 * normal window; hidden minimized windows don't exist on screen.) */
static bool covers(struct wnd *t)
{
    return t->visible && frame_shown(t);
}

/* Windows above t, in t's region */
static void subtract_above(struct region *g, struct wnd *t)
{
    bool tile = t->state == WS_STATE_MIN;
    bool after = false;
    for (int i = 0; i < S->ntops; i++) {
        struct wnd *o = S->tops[i];
        if (o == t) { after = true; continue; }
        if (!covers(o))
            continue;
        bool otile = o->state == WS_STATE_MIN;
        bool above = tile ? (!otile || after) : (!otile && after);
        if (above) {
            RECT r;
            frame_outer(o, &r);
            region_subtract(g, r);
        }
    }
}

/* ---- invalidation ---------------------------------------------------------- */

static int area(RECT r) { return (r.right - r.left) * (r.bottom - r.top); }

static RECT joined(RECT a, RECT b) { rect_union(&a, b); return a; }

/* Add r: into a rectangle it overlaps or nearly fills out, else a new one;
 * when they run out, the two whose union wastes least merge. */
static void dirty_add(struct dirty *d, RECT r)
{
    for (int i = 0; i < d->n; i++) {
        RECT u = joined(d->r[i], r), in;
        if (rect_intersect(d->r[i], r, &in) || area(u) <= (area(d->r[i]) + area(r)) * 5 / 4) {
            d->r[i] = u;                            /* ... and anything it now overlaps */
            for (int j = 0; j < d->n; j++)
                if (j != i && rect_intersect(d->r[i], d->r[j], &in)) {
                    rect_union(&d->r[i], d->r[j]);
                    d->r[j] = d->r[--d->n];
                    if (i == d->n) i = j;
                    j = -1;
                }
            return;
        }
    }
    if (d->n == DIRTY_MAX) {
        int bi = 0, bj = 1, best = 1 << 30;
        for (int i = 0; i < d->n; i++)
            for (int j = i + 1; j < d->n; j++) {
                int waste = area(joined(d->r[i], d->r[j])) - area(d->r[i]) - area(d->r[j]);
                if (waste < best) { best = waste; bi = i; bj = j; }
            }
        rect_union(&d->r[bi], d->r[bj]);
        d->r[bj] = d->r[--d->n];
    }
    d->r[d->n++] = r;
}

void ui_invalidate_screen(int x, int y, int w, int h)
{
    RECT r = { x, y, x + w, y + h }, scr = { 0, 0, S->scr_w, S->scr_h }, in;
    if (!rect_intersect(r, scr, &r))
        return;
    for (int i = 0; i < S->ntops; i++) {
        struct wnd *t = S->tops[i];
        RECT o;
        if (!covers(t))
            continue;
        frame_outer(t, &o);
        if (!rect_intersect(r, o, &in))
            continue;
        if (!t->has_upd)
            t->upd.n = 0;
        dirty_add(&t->upd, in);
        t->has_upd = true;
        if (t->pid != ui_pid)
            WakeProcess(t->pid);
    }
    if (!S->desk_dirty)
        S->desk.n = 0;
    dirty_add(&S->desk, r);
    S->desk_dirty = true;
}

void ui_invalidate_wnd(struct wnd *w)
{
    RECT r;
    wnd_screen_rect(w, &r);
    ui_invalidate_screen(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

bool InvalidateRect(HWND h, const RECT *r)
{
    struct wnd *w = wnd_from(h);
    if (!w)
        return false;
    int ox, oy;
    wnd_client_screen(w, &ox, &oy);
    RECT c;
    GetClientRect(h, &c);
    if (r)
        c = *r;
    ui_invalidate_screen(ox + c.left, oy + c.top, c.right - c.left, c.bottom - c.top);
    return true;
}

bool ui_paint_pending(void)
{
    if (S->background)
        return false;
    if (S->desk_dirty)
        return true;
    for (int i = 0; i < S->ntops; i++)
        if (S->tops[i]->has_upd && wnd_mine(S->tops[i]))
            return true;
    return false;
}

void ui_set_background(bool background)
{
    S->background = background;
    if (!background)                            /* the VDD restored an old image */
        ui_invalidate_screen(0, 0, S->scr_w, S->scr_h);
}

/* ---- presenting ------------------------------------------------------------- */

void ui_present(int x0, int y0, int x1, int y1)
{
    if (S->background)
        return;
    int px0 = x0 & ~7, px1 = (x1 + 7) & ~7;     /* whole bytes */
    bool cur = ui_cursor_overlaps(px0, y0, px1, y1);
    bool outl = S->in.outline_on;
    if (ui_screen.bpp != 4 && !outl && S->in.cur_on) {  /* the sprite: refreshed, never off */
        gfx_present(&ui_gfx, &ui_screen, x0, y0, x1 - x0, y1 - y0);
        if (cur)
            ui_cursor_refresh();
        return;
    }
    if (cur) ui_cursor(false);
    if (outl) ui_outline(false);
    gfx_present(&ui_gfx, &ui_screen, x0, y0, x1 - x0, y1 - y0);
    if (outl) ui_outline(true);
    ui_cursor(true);                            /* also after a repaint from scratch */
}

/* ---- painting ------------------------------------------------------------------ */

static void clip_to(RECT r)
{
    gfx_clip(&ui_gfx, r.left, r.top, r.right - r.left, r.bottom - r.top);
}

static void send_paint(struct wnd *w, RECT clip)
{
    int ox, oy;
    wnd_client_screen(w, &ox, &oy);
    paint_dc = (struct dc){ .w = w, .ox = ox, .oy = oy, .cx0 = clip.left, .cy0 = clip.top,
                            .cx1 = clip.right, .cy1 = clip.bottom };
    painting = w;
    SendMessage(w, WM_PAINT, 0, 0);
    painting = NULL;
}

static void paint_children(struct wnd *p, RECT clip)
{
    for (struct wnd *c = p->first; c; c = c->next) {
        if (!c->visible)
            continue;
        RECT r, cc;
        wnd_screen_rect(c, &r);
        if (!rect_intersect(r, clip, &cc))
            continue;
        clip_to(cc);                            /* erase to the parent's background */
        gfx_fill(&ui_gfx, cc.left, cc.top, cc.right - cc.left, cc.bottom - cc.top,
                 p->cls->background);
        send_paint(c, cc);
        if (!wnd_from(p))
            return;
        paint_children(c, cc);
    }
}

static void paint_rect(struct wnd *t, RECT oc)
{
    clip_to(oc);
    frame_paint(t, t == S->active);
    if (t->state != WS_STATE_MIN) {
        RECT c, cc;
        frame_client(t, &c);
        if (rect_intersect(c, oc, &cc)) {
            clip_to(cc);
            gfx_fill(&ui_gfx, cc.left, cc.top, cc.right - cc.left, cc.bottom - cc.top,
                     t->cls->background);
            send_paint(t, cc);
            if (wnd_from(t)) {
                paint_children(t, cc);
                clip_to(cc);
                frame_paint_grip(t);
            }
        }
    }
    gfx_noclip(&ui_gfx);
    ui_present(oc.left, oc.top, oc.right, oc.bottom);
}

static void paint_window(struct wnd *t)
{
    RECT o, u;
    t->has_upd = false;
    frame_outer(t, &o);
    if (!covers(t))
        return;
    struct region g = { 0 };
    for (int i = 0; i < t->upd.n; i++)
        if (rect_intersect(t->upd.r[i], o, &u))
            region_add(&g, u);
    t->upd.n = 0;
    subtract_above(&g, t);
    for (int i = 0; i < g.n && wnd_from(t); i++)
        paint_rect(t, g.r[i]);
}

static void paint_desktop(void)
{
    struct region g = { 0 };
    for (int i = 0; i < S->desk.n; i++)
        region_add(&g, S->desk.r[i]);
    S->desk.n = 0;
    S->desk_dirty = false;
    for (int i = 0; i < S->ntops; i++)
        if (covers(S->tops[i])) {
            RECT r;
            frame_outer(S->tops[i], &r);
            region_subtract(&g, r);
        }
    for (int i = 0; i < g.n; i++) {
        RECT r = g.r[i];
        gfx_noclip(&ui_gfx);
        gfx_fill(&ui_gfx, r.left, r.top, r.right - r.left, r.bottom - r.top, C_DESKTOP);
        ui_present(r.left, r.top, r.right, r.bottom);
    }
}

void ui_paint(void)
{
    if (S->background)
        return;
    struct wnd *list[WND_MAX];                  /* a WM_PAINT may change the list */
    int n = wnd_toplevels(list);
    for (int i = 0; i < n; i++)
        if (wnd_from(list[i]) && list[i]->has_upd && wnd_mine(list[i]))
            paint_window(list[i]);
    if (S->desk_dirty)
        paint_desktop();
}

void ui_paint_now(struct wnd *w)
{
    if (wnd_mine(w) && w->has_upd && !S->background)
        paint_window(w);
}

bool UpdateWindow(HWND h)
{
    struct wnd *w = wnd_top(wnd_from(h));
    if (!w) return false;
    ui_paint_now(w);
    return true;
}

/* ---- device contexts ----------------------------------------------------------- */

HDC BeginPaint(HWND h, PAINTSTRUCT *ps)
{
    struct wnd *w = wnd_from(h);
    if (!w)
        return NULL;
    if (painting != w) {                        /* outside WM_PAINT: draws nothing */
        int ox, oy;
        wnd_client_screen(w, &ox, &oy);
        paint_dc = (struct dc){ .w = w, .ox = ox, .oy = oy };
    }
    if (ps) {
        ps->hdc = &paint_dc;
        ps->rcPaint = (RECT){ paint_dc.cx0 - paint_dc.ox, paint_dc.cy0 - paint_dc.oy,
                              paint_dc.cx1 - paint_dc.ox, paint_dc.cy1 - paint_dc.oy };
    }
    return &paint_dc;
}

bool EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    (void)h; (void)ps;
    gfx_noclip(&ui_gfx);
    return true;
}

void dc_apply(HDC dc)
{
    ui_gfx.cx0 = dc->cx0; ui_gfx.cy0 = dc->cy0;
    ui_gfx.cx1 = dc->cx1; ui_gfx.cy1 = dc->cy1;
}

void FillRect(HDC dc, const RECT *r, uint8_t color)
{
    dc_apply(dc);
    gfx_fill(&ui_gfx, dc->ox + r->left, dc->oy + r->top, r->right - r->left, r->bottom - r->top, color);
}

void FrameRect(HDC dc, const RECT *r, uint8_t color)
{
    dc_apply(dc);
    gfx_rect(&ui_gfx, dc->ox + r->left, dc->oy + r->top, r->right - r->left, r->bottom - r->top, color);
}

void DrawLine(HDC dc, int x, int y, int len, bool vertical, uint8_t color)
{
    dc_apply(dc);
    if (vertical) gfx_vline(&ui_gfx, dc->ox + x, dc->oy + y, len, color);
    else          gfx_hline(&ui_gfx, dc->ox + x, dc->oy + y, len, color);
}

void SetPixel(HDC dc, int x, int y, uint8_t color)
{
    dc_apply(dc);
    gfx_pixel(&ui_gfx, dc->ox + x, dc->oy + y, color);
}

void DrawLineEx(HDC dc, int x0, int y0, int x1, int y1, uint8_t color)
{
    dc_apply(dc);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {                                  /* Bresenham */
        gfx_pixel(&ui_gfx, dc->ox + x0, dc->oy + y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint ellipse inside r */
void DrawEllipse(HDC dc, const RECT *r, uint8_t color, bool filled)
{
    dc_apply(dc);
    int w = r->right - r->left - 1, h = r->bottom - r->top - 1;
    if (w < 0 || h < 0)
        return;
    int ox = dc->ox + r->left, oy = dc->oy + r->top;
    int a = w / 2, b = h / 2, cx = ox + a, cy = oy + b, ex = w & 1, ey = h & 1;
    int32_t a2 = (int32_t)a * a, b2 = (int32_t)b * b;
    int x = 0, y = b;
    int32_t d1 = b2 - a2 * b + a2 / 4;
    #define PLOT4(px, py) do { \
        if (filled) { \
            gfx_hline(&ui_gfx, cx - (px), cy - (py), 2 * (px) + 1 + ex, color); \
            gfx_hline(&ui_gfx, cx - (px), cy + (py) + ey, 2 * (px) + 1 + ex, color); \
        } else { \
            gfx_pixel(&ui_gfx, cx + (px) + ex, cy + (py) + ey, color); \
            gfx_pixel(&ui_gfx, cx - (px), cy + (py) + ey, color); \
            gfx_pixel(&ui_gfx, cx + (px) + ex, cy - (py), color); \
            gfx_pixel(&ui_gfx, cx - (px), cy - (py), color); \
        } } while (0)
    while (b2 * x < a2 * y) {
        PLOT4(x, y);
        if (d1 < 0) {
            d1 += b2 * (2 * x + 3);
        } else {
            d1 += b2 * (2 * x + 3) + a2 * (2 - 2 * y);
            y--;
        }
        x++;
    }
    int32_t d2 = b2 * x * x + b2 * x + b2 / 4 + a2 * (y - 1) * (y - 1) - a2 * b2;
    while (y >= 0) {
        PLOT4(x, y);
        if (d2 > 0) {
            d2 += a2 * (3 - 2 * y);
        } else {
            d2 += b2 * (2 * x + 2) + a2 * (3 - 2 * y);
            x++;
        }
        y--;
    }
    #undef PLOT4
}

void DrawPixels(HDC dc, int x, int y, int w, int h, const uint8_t *pixels, int stride)
{
    dc_apply(dc);
    gfx_blit8(&ui_gfx, dc->ox + x, dc->oy + y, w, h, pixels, stride);
}

void TextOut(HDC dc, int x, int y, const char *s, uint8_t fg, int bg)
{
    dc_apply(dc);
    gfx_text_font(&ui_gfx, dc->font, dc->ox + x, dc->oy + y, s, -1, fg, bg < 0 ? GFX_TRANSPARENT : (uint8_t)bg);
}

int SelectFont(HDC dc, int font)
{
    int old = dc->font;
    dc->font = font == FONT_GUI ? FONT_GUI : FONT_FIXED;
    return old;
}

int TextWidthFont(int font, const char *s, int n)
{
    return gfx_text_width(&ui_gfx, font, s, n);
}

int GetTextWidth(HDC dc, const char *s, int n) { return gfx_text_width(&ui_gfx, dc->font, s, n); }

int SetTextColor(HDC dc, int color)
{
    int old = dc->has_fg ? dc->fg : -1;
    dc->has_fg = color >= 0;
    dc->fg = (uint8_t)color;
    return old;
}

int SetBkColor(HDC dc, int color)
{
    int old = dc->has_bg ? dc->bg : -1;
    dc->has_bg = color >= 0;
    dc->bg = (uint8_t)color;
    return old;
}

/* One line of DrawText: the characters shown (prefixes taken out) and
 * where the mnemonic is (-1 none). */
static int strip_prefix(const char *s, int n, unsigned flags, char *out, int size, int *mn)
{
    int k = 0;
    *mn = -1;
    for (int i = 0; i < n && k < size - 1; i++) {
        if (s[i] == '&' && !(flags & DT_NOPREFIX)) {
            if (i + 1 < n && s[i + 1] == '&') { out[k++] = '&'; i++; continue; }
            if (i + 1 < n) *mn = k;
            continue;
        }
        out[k++] = s[i] == '\t' ? ' ' : s[i];
    }
    out[k] = '\0';
    return k;
}

int DrawText(HDC dc, const char *s, int n, RECT *r, unsigned flags)
{
    if (!s || !r)
        return 0;
    if (n < 0)
        n = (int)strlen(s);
    int width = r->right - r->left, font = dc->font;
    int starts[64], lens[64], nl = 0;
    if (flags & DT_SINGLELINE) {
        starts[0] = 0;
        lens[0] = n;
        nl = 1;
    } else {                                    /* lines, wrapped at spaces */
        int i = 0;
        while (i <= n && nl < 64) {
            int start = i, brk = -1, w = 0;
            while (i < n && s[i] != '\n') {
                char c = s[i] == '\t' ? ' ' : s[i];
                if (c == '&' && !(flags & DT_NOPREFIX)) { i++; continue; }
                int cw = gfx_text_width(&ui_gfx, font, &c, 1);
                if ((flags & DT_WORDBREAK) && w + cw > width && i > start) {
                    if (brk > start) i = brk;
                    break;
                }
                if (c == ' ') brk = i + 1;
                w += cw;
                i++;
            }
            int end = i;
            while (end > start && s[end - 1] == ' ' && i < n && s[i] != '\n') end--;
            starts[nl] = start;
            lens[nl] = end - start;
            nl++;
            if (i < n && s[i] == '\n') i++;
            else if (i >= n) break;
            while (i < n && s[i] == ' ' && s[i - 1] != '\n') i++;  /* a wrapped line's spaces */
        }
    }
    int h = nl * 16, maxw = 0;
    char line[256];
    int mn;
    for (int k = 0; k < nl; k++) {
        int len = strip_prefix(s + starts[k], lens[k], flags, line, sizeof line, &mn);
        int w = gfx_text_width(&ui_gfx, font, line, len);
        if (w > maxw) maxw = w;
    }
    if (flags & DT_CALCRECT) {
        r->right = r->left + ((flags & DT_END_ELLIPSIS) && maxw > width ? width : maxw);
        r->bottom = r->top + h;
        return h;
    }
    RECT saved;
    if (!(flags & DT_NOCLIP))
        dc_clip(dc, *r, &saved);
    int y = r->top;
    if (flags & DT_SINGLELINE) {
        if (flags & DT_VCENTER) y = r->top + (r->bottom - r->top - 16) / 2;
        else if (flags & DT_BOTTOM) y = r->bottom - 16;
    }
    uint8_t fg = dc->has_fg ? dc->fg : C_BLACK;
    for (int k = 0; k < nl; k++, y += 16) {
        int len = strip_prefix(s + starts[k], lens[k], flags, line, sizeof line, &mn);
        int w = gfx_text_width(&ui_gfx, font, line, len);
        if ((flags & DT_END_ELLIPSIS) && w > width) {
            int dots = gfx_text_width(&ui_gfx, font, "...", 3);
            while (len > 0 && gfx_text_width(&ui_gfx, font, line, len) + dots > width)
                len--;
            memcpy(line + len, "...", 4);
            if (mn >= len) mn = -1;
            len += 3;
            w = gfx_text_width(&ui_gfx, font, line, len);
        }
        int x = r->left;
        if (flags & DT_CENTER) x = r->left + (width - w) / 2;
        else if (flags & DT_RIGHT) x = r->right - w;
        dc_apply(dc);
        if (dc->has_bg)
            gfx_fill(&ui_gfx, dc->ox + x, dc->oy + y, w, 16, dc->bg);
        gfx_text_font(&ui_gfx, font, dc->ox + x, dc->oy + y, line, len, fg, GFX_TRANSPARENT);
        if (mn >= 0 && !(flags & DT_HIDEPREFIX)) {
            int ux = x + gfx_text_width(&ui_gfx, font, line, mn);
            int uw = gfx_text_width(&ui_gfx, font, line + mn, 1) - (font == FONT_GUI ? 1 : 0);
            gfx_hline(&ui_gfx, dc->ox + ux, dc->oy + y + 14, uw > 0 ? uw : 1, fg);
        }
    }
    if (!(flags & DT_NOCLIP))
        dc_unclip(dc, &saved);
    return h;
}

void DrawBitmap(HDC dc, int x, int y, const uint16_t *rows, int h, uint8_t color)
{
    dc_apply(dc);
    gfx_bitmap(&ui_gfx, dc->ox + x, dc->oy + y, rows, h, color);
}

void DrawBevel(HDC dc, const RECT *r, bool raised)
{
    uint8_t hi = raised ? C_WHITE : C_GRAY4, lo = raised ? C_GRAY4 : C_WHITE;
    int w = r->right - r->left, h = r->bottom - r->top;
    DrawLine(dc, r->left, r->top, w, false, hi);
    DrawLine(dc, r->left, r->top, h, true, hi);
    DrawLine(dc, r->left, r->bottom - 1, w, false, lo);
    DrawLine(dc, r->right - 1, r->top, h, true, lo);
}

void DrawRoundRect(HDC dc, const RECT *r, uint8_t c)
{
    int x = r->left, y = r->top, w = r->right - r->left, h = r->bottom - r->top;
    DrawLine(dc, x + 2, y, w - 4, false, c);
    DrawLine(dc, x + 2, y + h - 1, w - 4, false, c);
    DrawLine(dc, x, y + 2, h - 4, true, c);
    DrawLine(dc, x + w - 1, y + 2, h - 4, true, c);
    SetPixel(dc, x + 1, y + 1, c);
    SetPixel(dc, x + w - 2, y + 1, c);
    SetPixel(dc, x + 1, y + h - 2, c);
    SetPixel(dc, x + w - 2, y + h - 2, c);
}

void FillRectRGB(HDC dc, const RECT *r, uint8_t red, uint8_t green, uint8_t blue)
{
    dc_apply(dc);
    gfx_fill_rgb(&ui_gfx, dc->ox + r->left, dc->oy + r->top, r->right - r->left, r->bottom - r->top,
                 red, green, blue);
}

int TextWidth(const char *s) { return (int)strlen(s) * 8; }

COLORREF GetThemeColor(int i)
{
    const uint8_t *c = theme_palette[i & 15];
    return RGB(c[0] * 255 / 63, c[1] * 255 / 63, c[2] * 255 / 63);
}
