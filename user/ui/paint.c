/*
 * paint.c -- invalidation, the paint cycle, and drawing through an HDC.
 *
 * Invalidated areas collect in one dirty rectangle (screen coordinates).
 * When the message queue runs dry, ui_paint() repaints it back to front --
 * desktop, minimized tiles, windows: each frame, then the client area
 * cleared to its class background and a WM_PAINT to the window, then the
 * same for its children, every one clipped -- into the off-screen buffer,
 * and presents just that rectangle (double buffering).
 */
#include "uipriv.h"

struct gfx ui_gfx;                          /* the off-screen buffer */
static uint8_t back[4 * GFX_PLANE_BYTES];

static bool dirty;
static int dx0, dy0, dx1, dy1;

static struct dc paint_dc;
static struct wnd *painting;

void ui_screen_init(void)
{
    const struct user_info *info = GetStartupInfo();
    gfx_init(&ui_screen, (volatile uint8_t *)info->vga, (const uint8_t *)info->font);
    gfx_init_mem(&ui_gfx, back, (const uint8_t *)info->font);
}

void ui_invalidate_screen(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 640) w = 640 - x;
    if (y + h > 480) h = 480 - y;
    if (w <= 0 || h <= 0)
        return;
    if (!dirty) {
        dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h;
        dirty = true;
        return;
    }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
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

bool ui_paint_pending(void) { return dirty && !ui_background; }

void ui_set_background(bool background)
{
    ui_background = background;
    if (!background)                            /* the VDD restored an old image */
        ui_invalidate_screen(0, 0, 640, 480);
}

/* ---- the paint cycle ------------------------------------------------------- */

static bool intersect(RECT a, RECT b, RECT *out)
{
    out->left = a.left > b.left ? a.left : b.left;
    out->top = a.top > b.top ? a.top : b.top;
    out->right = a.right < b.right ? a.right : b.right;
    out->bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    return out->left < out->right && out->top < out->bottom;
}

static void clip_to(RECT r)
{
    gfx_clip(&ui_gfx, r.left, r.top, r.right - r.left, r.bottom - r.top);
}

static void send_paint(struct wnd *w, RECT clip)
{
    int ox, oy;
    wnd_client_screen(w, &ox, &oy);
    paint_dc = (struct dc){ w, ox, oy, clip.left, clip.top, clip.right, clip.bottom };
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
        if (!intersect(r, clip, &cc))
            continue;
        clip_to(cc);                            /* erase to the parent's background */
        gfx_fill(&ui_gfx, cc.left, cc.top, cc.right - cc.left, cc.bottom - cc.top,
                 p->cls->background);
        send_paint(c, cc);
        paint_children(c, cc);
    }
}

void ui_paint(void)
{
    if (!dirty || ui_background)
        return;
    RECT d = { dx0, dy0, dx1, dy1 };
    dirty = false;
    struct gfx *g = &ui_gfx;
    clip_to(d);
    gfx_fill(g, d.left, d.top, d.right - d.left, d.bottom - d.top, C_DESKTOP);

    struct wnd *list[WND_MAX];
    int n = wnd_toplevels(list);
    struct wnd *active = wnd_active();
    for (int pass = 0; pass < 2; pass++)        /* tiles, then windows */
        for (int i = 0; i < n; i++) {
            struct wnd *t = list[i];
            if (!t->visible || (t->state == WS_STATE_MIN) != (pass == 0))
                continue;
            RECT o, oc, c, cc;
            frame_outer(t, &o);
            if (!intersect(o, d, &oc))
                continue;
            clip_to(oc);
            frame_paint(t, t == active);
            if (t->state == WS_STATE_MIN)
                continue;
            frame_client(t, &c);
            if (!intersect(c, d, &cc))
                continue;
            clip_to(cc);
            gfx_fill(g, cc.left, cc.top, cc.right - cc.left, cc.bottom - cc.top, t->cls->background);
            send_paint(t, cc);
            paint_children(t, cc);
            clip_to(cc);
            frame_paint_grip(t);
        }
    gfx_noclip(g);

    /* Present whole bytes; hide the cursor only if they overlap it. */
    int px0 = d.left & ~7, px1 = (d.right + 7) & ~7;
    bool hit = ui_cursor_overlaps(px0, d.top, px1, d.bottom);
    if (hit)
        ui_cursor(false);
    gfx_present(g, ui_screen.fb, d.left, d.top, d.right - d.left, d.bottom - d.top);
    ui_cursor(true);
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
        paint_dc = (struct dc){ w, ox, oy, 0, 0, 0, 0 };
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

void TextOut(HDC dc, int x, int y, const char *s, uint8_t fg, int bg)
{
    dc_apply(dc);
    gfx_text(&ui_gfx, dc->ox + x, dc->oy + y, s, fg, bg < 0 ? GFX_TRANSPARENT : (uint8_t)bg);
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

int TextWidth(const char *s) { return (int)strlen(s) * 8; }
