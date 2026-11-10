/*
 * frame.c -- the non-client area of top-level windows: Platinum-style
 * frames and pinstriped title bars with the Windows 95 boxes (system,
 * minimize, maximize/restore, close), minimized tiles, maximize/restore.
 */
#include "uipriv.h"

#define frame_pressed_box (S->frame_pressed_box)
#define frame_pressed_wnd (S->frame_pressed_wnd)

bool frame_has(struct wnd *w)
{
    return !w->parent && (w->style & WS_CAPTION) == WS_CAPTION;
}

/* With a shell (a taskbar) minimized windows leave the screen; without
 * one they become tiles along the bottom. */
bool frame_shown(struct wnd *w)
{
    return w->parent || w->state != WS_STATE_MIN || !S->shell_hook;
}

void frame_outer(struct wnd *w, RECT *r)
{
    if (!w->parent && w->state == WS_STATE_MIN && S->shell_hook) {
        *r = (RECT){ 0, 0, 0, 0 };
        return;
    }
    if (!w->parent && w->state == WS_STATE_MIN) {
        int x = 4 + w->min_slot * (TILE_W + 4), y = S->scr_h - TILE_H - 4;
        *r = (RECT){ x, y, x + TILE_W, y + TILE_H };
        return;
    }
    *r = (RECT){ w->x, w->y, w->x + w->w, w->y + w->h };
}

void frame_client(struct wnd *w, RECT *r)
{
    if (!frame_has(w)) {
        frame_outer(w, r);
        return;
    }
    int top = w->y + CLIENT_TOP + (w->menu ? MENUBAR_H + 1 : 0);
    *r = (RECT){ w->x + FRAME, top, w->x + w->w - FRAME, w->y + w->h - FRAME };
}

bool frame_menubar(struct wnd *w, RECT *r)
{
    if (!frame_has(w) || !w->menu || w->state == WS_STATE_MIN)
        return false;
    *r = (RECT){ w->x + FRAME, w->y + CLIENT_TOP, w->x + w->w - FRAME, w->y + CLIENT_TOP + MENUBAR_H };
    return true;
}

static RECT title_rect(struct wnd *w)
{
    RECT o;
    frame_outer(w, &o);
    return (RECT){ o.left + FRAME, o.top + FRAME, o.right - FRAME, o.top + FRAME + TITLE_H };
}

RECT frame_box(struct wnd *w, int which)
{
    RECT t = title_rect(w);
    int y = t.top + (TITLE_H - BOX) / 2;
    int close = t.right - 3 - BOX, x;
    switch (which) {
    case HT_SYSMENU:   x = t.left + 3; break;
    case HT_CLOSE:     x = close; break;
    case HT_MAXBUTTON: x = close - BOX - 3; break;
    default:           x = close - 2 * BOX - 5; break;
    }
    return (RECT){ x, y, x + BOX, y + BOX };
}

static bool has_box(struct wnd *w, int which)
{
    switch (which) {
    case HT_SYSMENU:   return w->style & WS_SYSMENU;
    case HT_CLOSE:     return w->style & WS_SYSMENU;
    case HT_MAXBUTTON: return w->style & WS_MAXIMIZEBOX;
    case HT_MINBUTTON: return w->style & WS_MINIMIZEBOX;
    }
    return false;
}

int frame_hit(struct wnd *w, int x, int y, int *edges)
{
    RECT o;
    frame_outer(w, &o);
    *edges = 0;
    if (!rect_contains(&o, x, y))
        return HT_NOWHERE;
    if (w->state == WS_STATE_MIN)
        return HT_TILE;
    if (!frame_has(w))
        return HT_CLIENT;
    static const int boxes[] = { HT_SYSMENU, HT_MINBUTTON, HT_MAXBUTTON, HT_CLOSE };
    for (int i = 0; i < 4; i++) {
        RECT b = frame_box(w, boxes[i]);
        if (has_box(w, boxes[i]) && rect_contains(&b, x, y))
            return boxes[i];
    }
    if ((w->style & WS_THICKFRAME) && w->state == WS_STATE_NORMAL) {
        const int corner = 14;
        bool top = y < o.top + FRAME - 1, bottom = y >= o.bottom - FRAME;
        if (x < o.left + FRAME || (x < o.left + corner && (top || bottom)))     *edges |= E_L;
        if (x >= o.right - FRAME || (x >= o.right - corner && (top || bottom))) *edges |= E_R;
        if (top)    *edges |= E_T;
        if (bottom) *edges |= E_B;
        RECT c;                                     /* the size grip */
        frame_client(w, &c);
        if (x >= c.right - 14 && y >= c.bottom - 14 && x < c.right && y < c.bottom)
            *edges = E_R | E_B;
        if (*edges)
            return HT_BORDER;
    }
    RECT t = title_rect(w), bar;
    if (rect_contains(&t, x, y))
        return HT_CAPTION;
    if (frame_menubar(w, &bar) && rect_contains(&bar, x, y))
        return HT_MENU;
    return HT_CLIENT;
}

/* ---- state changes --------------------------------------------------------- */

static void notify_size(struct wnd *w)
{
    RECT c;
    GetClientRect(w, &c);
    wnd_notify(w, WM_SIZE, 0, MAKELPARAM(c.right, c.bottom));
    ui_shell_notify(HSHELL_REDRAW, w);
}

void frame_minimize(struct wnd *w)
{
    if (w->parent || w->state == WS_STATE_MIN)
        return;
    ui_invalidate_wnd(w);
    struct wnd *list[WND_MAX];
    int n = wnd_toplevels(list);
    unsigned used = 0;
    for (int i = 0; i < n; i++)
        if (list[i]->state == WS_STATE_MIN)
            used |= 1u << list[i]->min_slot;
    int slot = 0;
    while (used & (1u << slot))
        slot++;
    w->min_slot = slot;
    if (w->state == WS_STATE_MAX) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
    }
    w->state = WS_STATE_MIN;
    ui_invalidate_wnd(w);
    if (wnd_active() == w)                          /* activate the next one */
        wnd_activate_next(w);
    ui_shell_notify(HSHELL_REDRAW, w);
    DebugLog("minimized %s", w->text);
}

void frame_maximize(struct wnd *w)
{
    if (w->parent || w->state == WS_STATE_MAX)
        return;
    ui_invalidate_wnd(w);
    if (w->state == WS_STATE_NORMAL) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
    }
    RECT a = S->work;                               /* the work area (not the taskbar) */
    w->x = a.left; w->y = a.top; w->w = a.right - a.left; w->h = a.bottom - a.top;
    w->state = WS_STATE_MAX;
    ui_invalidate_wnd(w);
    wnd_activate(w);
    notify_size(w);
    DebugLog("maximized %s", w->text);
}

void frame_restore(struct wnd *w)
{
    if (w->parent || w->state == WS_STATE_NORMAL)
        return;
    ui_invalidate_wnd(w);
    if (w->state == WS_STATE_MAX) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
    }
    w->state = WS_STATE_NORMAL;
    ui_invalidate_wnd(w);
    wnd_activate(w);
    notify_size(w);
    DebugLog("restored %s", w->text);
}

/* ---- painting ----------------------------------------------------------------- */

static const uint16_t glyph_sys[9]     = { 0, 0, 0xFF80, 0, 0xFF80, 0, 0xFF80, 0, 0 };
static const uint16_t glyph_min[9]     = { 0, 0, 0, 0, 0, 0, 0, 0xFF80, 0xFF80 };
static const uint16_t glyph_max[9]     = { 0xFF80, 0xFF80, 0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0xFF80 };
static const uint16_t glyph_restore[9] = { 0x3F80, 0x3F80, 0x2080, 0xFE80, 0xFE80, 0x8380, 0x8200, 0x8200, 0xFE00 };
static const uint16_t glyph_close[9]   = { 0xC180, 0x6300, 0x3600, 0x1C00, 0x0800, 0x1C00, 0x3600, 0x6300, 0xC180 };

static void paint_box(RECT b, const uint16_t *glyph, bool pressed)
{
    struct gfx *g = &ui_gfx;
    int x = b.left, y = b.top, s = b.right - b.left;
    gfx_rect(g, x, y, s, s, C_GRAY2);
    gfx_fill(g, x + 1, y + 1, s - 2, s - 2, pressed ? C_GRAY4 : C_FACE);
    if (!pressed) {
        gfx_hline(g, x + 1, y + 1, s - 2, C_WHITE);
        gfx_vline(g, x + 1, y + 1, s - 2, C_WHITE);
        gfx_hline(g, x + 1, y + s - 2, s - 2, C_GRAY5);
        gfx_vline(g, x + s - 2, y + 1, s - 2, C_GRAY5);
    }
    gfx_bitmap(g, x + 2, y + 2, glyph, 9, pressed ? C_WHITE : C_BLACK);
}

static void paint_grip(int x, int y)                /* bottom-right corner */
{
    for (int i = 0; i < 3; i++) {
        int len = 4 + i * 4;
        for (int k = 0; k < len; k++) {
            gfx_pixel(&ui_gfx, x - len + k, y - k, C_GRAY4);
            gfx_pixel(&ui_gfx, x - len + k + 1, y - k, C_WHITE);
        }
    }
}

/* Frame, title bar and boxes (the client area is painted separately). */
void frame_paint(struct wnd *w, bool active)
{
    struct gfx *g = &ui_gfx;
    RECT o;
    frame_outer(w, &o);
    int ox = o.left, oy = o.top, ow = o.right - o.left, oh = o.bottom - o.top;
    bool tile = w->state == WS_STATE_MIN;

    if (!frame_has(w)) {                            /* plain popup: a border */
        gfx_rect(g, ox, oy, ow, oh, C_BLACK);
        return;
    }
    RECT t = title_rect(w);
    int tw_ = t.right - t.left;
    gfx_fill(g, ox, oy, ow, oh, C_FACE);
    gfx_rect(g, ox, oy, ow, oh, active ? C_BLACK : C_GRAY3);
    gfx_hline(g, ox + 1, oy + 1, ow - 2, C_WHITE);
    gfx_vline(g, ox + 1, oy + 1, oh - 2, C_WHITE);
    gfx_hline(g, ox + 1, oy + oh - 2, ow - 2, C_GRAY4);
    gfx_vline(g, ox + ow - 2, oy + 1, oh - 2, C_GRAY4);

    if (active)                                     /* pinstripes */
        for (int row = 2; row < TITLE_H - 3; row += 2) {
            gfx_hline(g, t.left + 1, t.top + row, tw_ - 2, C_GRAY4);
            gfx_hline(g, t.left + 1, t.top + row + 1, tw_ - 2, C_WHITE);
        }

    int reserve = tile ? BOX + 10 : 3 * BOX + 18;
    int room = (tw_ - 2 * reserve) / 8;
    char title[TEXT_MAX];
    int n = (int)strlen(w->text);
    if (n > room) n = room < 0 ? 0 : room;
    memcpy(title, w->text, (size_t)n);
    title[n] = '\0';
    int textw = n * 8, tx = t.left + (tw_ - textw) / 2;
    if (n) {
        gfx_fill(g, tx - 6, t.top + 1, textw + 12, TITLE_H - 2, C_FACE);
        gfx_text(g, tx, t.top + 2, title, active || tile ? C_BLACK : C_GRAY3, GFX_TRANSPARENT);
    }

    bool pressed_here = frame_pressed_wnd == w;
    if ((active || tile) && has_box(w, HT_SYSMENU))
        paint_box(frame_box(w, HT_SYSMENU), glyph_sys, pressed_here && frame_pressed_box == HT_SYSMENU);
    if (active && !tile) {
        if (has_box(w, HT_MINBUTTON))
            paint_box(frame_box(w, HT_MINBUTTON), glyph_min, pressed_here && frame_pressed_box == HT_MINBUTTON);
        if (has_box(w, HT_MAXBUTTON))
            paint_box(frame_box(w, HT_MAXBUTTON), w->state == WS_STATE_MAX ? glyph_restore : glyph_max,
                      pressed_here && frame_pressed_box == HT_MAXBUTTON);
        if (has_box(w, HT_CLOSE))
            paint_box(frame_box(w, HT_CLOSE), glyph_close, pressed_here && frame_pressed_box == HT_CLOSE);
    }
    if (tile)
        return;

    RECT c, bar;
    frame_client(w, &c);
    int top = c.top;
    if (frame_menubar(w, &bar)) {
        top = bar.top;
        menubar_paint(w);
        gfx_hline(g, c.left, bar.bottom, c.right - c.left, active ? C_GRAY2 : C_GRAY4);
    }
    gfx_rect(g, c.left - 1, top - 1, c.right - c.left + 2, c.bottom - top + 2,
             active ? C_GRAY2 : C_GRAY4);
}

/* After the client area is painted: the size grip goes on top of it. */
void frame_paint_grip(struct wnd *w)
{
    if (!frame_has(w) || !(w->style & WS_THICKFRAME) || w->state != WS_STATE_NORMAL)
        return;
    RECT c;
    frame_client(w, &c);
    paint_grip(c.right - 2, c.bottom - 2);
}
