/*
 * wm.c -- the window manager (ring 3).
 *
 * Windows live in a z-order (order[0] is the bottom). Anything that
 * changes on screen is invalidated into one dirty rectangle, and
 * wm_flush() repaints it back to front -- desktop, minimized tiles,
 * windows -- clipped to that rectangle, into an off-screen copy of the
 * screen (wm_gfx), then presents just that rectangle: double buffering, so
 * no half-drawn layer is ever visible. The mouse cursor and the XOR drag
 * outlines live only on the real screen (screen_gfx); the cursor is hidden
 * only when the presented rectangle touches it.
 */
#include "wm.h"
#include "os.h"
#include "theme.h"
#include "ui.h"

struct gfx wm_gfx;                          /* the off-screen buffer */
static struct gfx screen_gfx;               /* the real screen */
static uint8_t back[4 * GFX_PLANE_BYTES];

static struct window wins[WIN_MAX];
static struct window *order[WIN_MAX];
static int norder;

static bool dirty;
static int dx0, dy0, dx1, dy1;
static bool background;                     /* a DOS session has the screen */

/* ---- cursor --------------------------------------------------------------- */

static const uint16_t arrow[16] = {
    0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100,
    0x8080, 0x8040, 0x8780, 0x9400, 0xB400, 0xCA00, 0x0A00, 0x0400,
};
static int cur_x = 320, cur_y = 240;
static bool cur_on;

static void cursor(bool on)
{
    if (background)
        return;
    if (on != cur_on) {
        gfx_xor_bitmap(&screen_gfx, cur_x, cur_y, arrow, 16);
        cur_on = on;
    }
}

/* ---- geometry -------------------------------------------------------------- */

struct rect { int x, y, w, h; };

static bool contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static struct rect outer(const struct window *w)
{
    if (w->state == WS_MIN)
        return (struct rect){ 4 + w->min_slot * (TILE_W + 4), 480 - TILE_H - 4, TILE_W, TILE_H };
    return (struct rect){ w->x, w->y, w->w, w->h };
}

static struct rect title_rect(const struct window *w)
{
    struct rect r = outer(w);
    return (struct rect){ r.x + FRAME, r.y + FRAME, r.w - 2 * FRAME, TITLE_H };
}

void wm_client_rect(const struct window *w, int *x, int *y, int *cw, int *ch)
{
    struct rect r = outer(w);
    *x = r.x + FRAME;
    *y = r.y + CLIENT_TOP;
    *cw = r.w - 2 * FRAME;
    *ch = r.h - CLIENT_TOP - FRAME;
}

enum { H_NONE, H_CLIENT, H_TITLE, H_SYS, H_MIN, H_MAX, H_CLOSE, H_TILE, H_EDGE };
#define E_L 1
#define E_R 2
#define E_T 4
#define E_B 8

static struct rect box_rect(const struct window *w, int which)
{
    struct rect t = title_rect(w);
    int y = t.y + (TITLE_H - BOX) / 2;
    int close = t.x + t.w - 3 - BOX;
    switch (which) {
    case H_SYS:   return (struct rect){ t.x + 3, y, BOX, BOX };
    case H_CLOSE: return (struct rect){ close, y, BOX, BOX };
    case H_MAX:   return (struct rect){ close - BOX - 3, y, BOX, BOX };
    default:      return (struct rect){ close - 2 * BOX - 5, y, BOX, BOX };
    }
}

static int hit(const struct window *w, int x, int y, int *edges)
{
    struct rect r = outer(w);
    *edges = 0;
    if (!contains(r, x, y))
        return H_NONE;
    if (w->state == WS_MIN)
        return H_TILE;
    static const int boxes[] = { H_SYS, H_MIN, H_MAX, H_CLOSE };
    for (int i = 0; i < 4; i++)
        if (contains(box_rect(w, boxes[i]), x, y))
            return boxes[i];
    if ((w->flags & WF_RESIZABLE) && w->state == WS_NORMAL) {
        const int corner = 14;
        if (x < r.x + FRAME || (x < r.x + corner && (y < r.y + FRAME || y >= r.y + r.h - FRAME))) *edges |= E_L;
        if (x >= r.x + r.w - FRAME || (x >= r.x + r.w - corner && (y < r.y + FRAME || y >= r.y + r.h - FRAME))) *edges |= E_R;
        if (y < r.y + FRAME - 1) *edges |= E_T;
        if (y >= r.y + r.h - FRAME) *edges |= E_B;
        int cx, cy, cw, ch;                         /* the size grip */
        wm_client_rect(w, &cx, &cy, &cw, &ch);
        if (x >= cx + cw - 14 && y >= cy + ch - 14 && x < cx + cw && y < cy + ch)
            *edges = E_R | E_B;
        if (*edges)
            return H_EDGE;
    }
    if (contains(title_rect(w), x, y))
        return H_TITLE;
    return H_CLIENT;
}

/* ---- z-order and state ----------------------------------------------------- */

struct window *wm_active(void)
{
    for (int i = norder - 1; i >= 0; i--)
        if (order[i]->state != WS_MIN)
            return order[i];
    return NULL;
}

void wm_invalidate(int x, int y, int w, int h)
{
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

void wm_invalidate_window(const struct window *w)
{
    struct rect r = outer(w);
    wm_invalidate(r.x, r.y, r.w, r.h);
}

static void raise(struct window *w)
{
    int i = 0;
    while (i < norder && order[i] != w)
        i++;
    for (; i < norder - 1; i++)
        order[i] = order[i + 1];
    order[norder - 1] = w;
}

void wm_activate(struct window *w)
{
    struct window *old = wm_active();
    if (old == w)
        return;
    raise(w);
    if (old)
        wm_invalidate_window(old);                  /* frame goes inactive */
    wm_invalidate_window(w);
    ulog("activate %s", w->title);
}

struct window *wm_create(const char *title, int x, int y, int w, int h, unsigned flags)
{
    struct window *win = NULL;
    for (int i = 0; i < WIN_MAX; i++)
        if (!wins[i].used) { win = &wins[i]; break; }
    if (!win)
        return NULL;
    memset(win, 0, sizeof *win);
    win->used = true;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->flags = flags;
    win->focus = -1;
    size_t n = strlen(title);
    if (n >= sizeof win->title)
        n = sizeof win->title - 1;
    memcpy(win->title, title, n);
    win->title[n] = '\0';
    struct window *old = wm_active();
    order[norder++] = win;
    if (old)
        wm_invalidate_window(old);
    wm_invalidate_window(win);
    return win;
}

void wm_close(struct window *w)
{
    wm_invalidate_window(w);
    if (w->on_close)
        w->on_close(w);
    int i = 0;
    while (i < norder && order[i] != w)
        i++;
    for (; i < norder - 1; i++)
        order[i] = order[i + 1];
    norder--;
    w->used = false;
    ulog("closed %s", w->title);
    struct window *a = wm_active();
    if (a)
        wm_invalidate_window(a);
}

static void minimize(struct window *w)
{
    wm_invalidate_window(w);
    unsigned used = 0;
    for (int i = 0; i < norder; i++)
        if (order[i]->state == WS_MIN)
            used |= 1u << order[i]->min_slot;
    int slot = 0;
    while (used & (1u << slot))
        slot++;
    w->min_slot = slot;
    if (w->state == WS_MAX) {                       /* remember the normal size */
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
    }
    w->state = WS_MIN;
    wm_invalidate_window(w);
    struct window *a = wm_active();
    if (a)
        wm_invalidate_window(a);
    ulog("minimized %s", w->title);
}

static void restore_tile(struct window *w)
{
    wm_invalidate_window(w);                        /* the tile */
    w->state = WS_NORMAL;
    struct window *old = wm_active();
    raise(w);
    if (old)
        wm_invalidate_window(old);
    wm_invalidate_window(w);
    ulog("restored %s", w->title);
}

static void toggle_maximize(struct window *w)
{
    wm_invalidate_window(w);
    if (w->state == WS_MAX) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->state = WS_NORMAL;
        ulog("restored %s", w->title);
    } else {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = 640; w->h = 480;
        w->state = WS_MAX;
        ulog("maximized %s", w->title);
    }
    wm_invalidate_window(w);
}

/* ---- painting -------------------------------------------------------------- */

static const uint16_t glyph_sys[9]     = { 0, 0, 0xFF80, 0, 0xFF80, 0, 0xFF80, 0, 0 };
static const uint16_t glyph_min[9]     = { 0, 0, 0, 0, 0, 0, 0, 0xFF80, 0xFF80 };
static const uint16_t glyph_max[9]     = { 0xFF80, 0xFF80, 0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0x8080, 0xFF80 };
static const uint16_t glyph_restore[9] = { 0x3F80, 0x3F80, 0x2080, 0xFE80, 0xFE80, 0x8380, 0x8200, 0x8200, 0xFE00 };
static const uint16_t glyph_close[9]   = { 0xC180, 0x6300, 0x3600, 0x1C00, 0x0800, 0x1C00, 0x3600, 0x6300, 0xC180 };

static int box_pressed;                             /* H_* being pressed, or 0 */
static struct window *box_window;

static void paint_box(struct rect b, const uint16_t *glyph, bool pressed)
{
    struct gfx *g = &wm_gfx;
    gfx_rect(g, b.x, b.y, b.w, b.h, C_GRAY2);
    gfx_fill(g, b.x + 1, b.y + 1, b.w - 2, b.h - 2, pressed ? C_GRAY4 : C_FACE);
    if (!pressed) {
        gfx_hline(g, b.x + 1, b.y + 1, b.w - 2, C_WHITE);
        gfx_vline(g, b.x + 1, b.y + 1, b.h - 2, C_WHITE);
        gfx_hline(g, b.x + 1, b.y + b.h - 2, b.w - 2, C_GRAY5);
        gfx_vline(g, b.x + b.w - 2, b.y + 1, b.h - 2, C_GRAY5);
    }
    gfx_bitmap(g, b.x + 2, b.y + 2, glyph, 9, pressed ? C_WHITE : C_BLACK);
}

static void paint_grip(int x, int y)                /* bottom-right corner x,y */
{
    for (int i = 0; i < 3; i++) {
        int len = 4 + i * 4;
        for (int k = 0; k < len; k++) {
            gfx_pixel(&wm_gfx, x - len + k, y - k, C_GRAY4);
            gfx_pixel(&wm_gfx, x - len + k + 1, y - k, C_WHITE);
        }
    }
}

static void paint_window(struct window *w, bool active)
{
    struct gfx *g = &wm_gfx;
    struct rect r = outer(w), t = title_rect(w);

    gfx_fill(g, r.x, r.y, r.w, r.h, C_FACE);
    gfx_rect(g, r.x, r.y, r.w, r.h, active ? C_BLACK : C_GRAY3);
    gfx_hline(g, r.x + 1, r.y + 1, r.w - 2, C_WHITE);
    gfx_vline(g, r.x + 1, r.y + 1, r.h - 2, C_WHITE);
    gfx_hline(g, r.x + 1, r.y + r.h - 2, r.w - 2, C_GRAY4);
    gfx_vline(g, r.x + r.w - 2, r.y + 1, r.h - 2, C_GRAY4);

    if (active)                                     /* pinstripes */
        for (int row = 2; row < TITLE_H - 3; row += 2) {
            gfx_hline(g, t.x + 1, t.y + row, t.w - 2, C_GRAY4);
            gfx_hline(g, t.x + 1, t.y + row + 1, t.w - 2, C_WHITE);
        }

    /* Title on a plain plate, truncated to the room between the boxes */
    bool tile = w->state == WS_MIN;
    int reserve = tile ? BOX + 10 : 3 * BOX + 18;
    int room = (t.w - 2 * reserve) / 8;
    char title[sizeof w->title];
    int n = (int)strlen(w->title);
    if (n > room) n = room < 0 ? 0 : room;
    memcpy(title, w->title, (size_t)n);
    title[n] = '\0';
    int tw = n * 8, tx = t.x + (t.w - tw) / 2;
    if (n) {
        gfx_fill(g, tx - 6, t.y + 1, tw + 12, TITLE_H - 2, C_FACE);
        gfx_text(g, tx, t.y + 2, title, active || tile ? C_BLACK : C_GRAY3, GFX_TRANSPARENT);
    }

    if (active || tile)
        paint_box(box_rect(w, H_SYS), glyph_sys, false);
    if (active && !tile) {
        paint_box(box_rect(w, H_MIN), glyph_min, box_window == w && box_pressed == H_MIN);
        paint_box(box_rect(w, H_MAX), w->state == WS_MAX ? glyph_restore : glyph_max,
                  box_window == w && box_pressed == H_MAX);
        paint_box(box_rect(w, H_CLOSE), glyph_close, box_window == w && box_pressed == H_CLOSE);
    }
    if (tile)
        return;

    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    gfx_rect(g, cx - 1, cy - 1, cw + 2, ch + 2, active ? C_GRAY2 : C_GRAY4);
    gfx_fill(g, cx, cy, cw, ch, (w->flags & WF_DOCUMENT) ? C_WHITE : C_FACE);

    int sx0 = g->cx0, sy0 = g->cy0, sx1 = g->cx1, sy1 = g->cy1;   /* clip to client */
    int nx0 = cx > sx0 ? cx : sx0, ny0 = cy > sy0 ? cy : sy0;
    int nx1 = cx + cw < sx1 ? cx + cw : sx1, ny1 = cy + ch < sy1 ? cy + ch : sy1;
    if (nx0 < nx1 && ny0 < ny1) {
        g->cx0 = nx0; g->cy0 = ny0; g->cx1 = nx1; g->cy1 = ny1;
        ui_draw_all(w);
        if ((w->flags & WF_RESIZABLE) && w->state == WS_NORMAL)
            paint_grip(cx + cw - 2, cy + ch - 2);
    }
    g->cx0 = sx0; g->cy0 = sy0; g->cx1 = sx1; g->cy1 = sy1;
}

static bool overlaps(struct rect r)
{
    return r.x < dx1 && r.y < dy1 && r.x + r.w > dx0 && r.y + r.h > dy0;
}

void wm_flush(void)
{
    if (!dirty || background)
        return;
    struct gfx *g = &wm_gfx;
    gfx_clip(g, dx0, dy0, dx1 - dx0, dy1 - dy0);
    gfx_fill(g, dx0, dy0, dx1 - dx0, dy1 - dy0, C_DESKTOP);
    struct window *active = wm_active();
    for (int pass = 0; pass < 2; pass++)            /* tiles, then windows */
        for (int i = 0; i < norder; i++) {
            struct window *w = order[i];
            if ((w->state == WS_MIN) == (pass == 0) && overlaps(outer(w)))
                paint_window(w, w == active);
        }
    gfx_noclip(g);
    dirty = false;

    /* Present whole bytes; hide the cursor only if they overlap it. */
    int px0 = dx0 & ~7, px1 = (dx1 + 7) & ~7;
    bool hit = cur_on && cur_x < px1 && cur_x + 16 > px0 && cur_y < dy1 && cur_y + 16 > dy0;
    if (hit)
        cursor(false);
    gfx_present(g, screen_gfx.fb, dx0, dy0, dx1 - dx0, dy1 - dy0);
    if (hit || !cur_on)
        cursor(true);
}

void wm_screen(bool ours)
{
    background = !ours;
    if (ours) {                             /* the VDD restored an old image: */
        cur_on = false;                     /* repaint everything, cursor too */
        wm_invalidate(0, 0, 640, 480);
        wm_flush();
    }
}

void wm_init(void)
{
    screen_gfx = wm_gfx;                    /* ui_init set up the screen target */
    gfx_init_mem(&wm_gfx, back, screen_gfx.font);
    norder = 0;
    wm_invalidate(0, 0, 640, 480);
}

/* ---- mouse ----------------------------------------------------------------- */

static enum { M_IDLE, M_MOVE, M_SIZE, M_BOX, M_CTL } mode;
static struct window *mw;
static struct rect outline;
static int grab_x, grab_y, edges_held;
static uint8_t last_buttons;
static uint32_t last_click_tick;
static struct window *last_click_win;

#define MIN_W 160
#define MIN_H 100

static void xor_outline(void) { if (!background) gfx_xor_frame(&screen_gfx, outline.x, outline.y, outline.w, outline.h, 2); }

static void press(int x, int y, uint32_t tick)
{
    struct window *w = NULL;
    int part = H_NONE, edges = 0;
    for (int pass = 0; pass < 2 && !w; pass++)      /* windows over tiles */
        for (int i = norder - 1; i >= 0; i--) {
            struct window *c = order[i];
            if ((c->state == WS_MIN) == (pass == 1) && (part = hit(c, x, y, &edges)) != H_NONE) {
                w = c;
                break;
            }
        }
    if (!w)
        return;
    if (part == H_TILE) {
        restore_tile(w);
        return;
    }
    wm_activate(w);

    switch (part) {
    case H_MIN: case H_MAX: case H_CLOSE:
        mode = M_BOX;
        mw = box_window = w;
        box_pressed = part;
        wm_invalidate_window(w);
        break;
    case H_TITLE:
    case H_SYS:
        if (last_click_win == w && tick - last_click_tick <= 9) {   /* double-click */
            last_click_win = NULL;
            if (part == H_SYS)
                wm_close(w);                        /* as in Windows 95 */
            else
                toggle_maximize(w);
            break;
        }
        last_click_win = w;
        last_click_tick = tick;
        if (part == H_TITLE && w->state == WS_NORMAL) {
            mode = M_MOVE;
            mw = w;
            outline = outer(w);
            grab_x = x - outline.x;
            grab_y = y - outline.y;
            wm_flush();
            xor_outline();
        }
        break;
    case H_EDGE:
        mode = M_SIZE;
        mw = w;
        outline = outer(w);
        edges_held = edges;
        grab_x = x;
        grab_y = y;
        wm_flush();
        xor_outline();
        break;
    case H_CLIENT:
        if (ui_mouse_down(w, x, y))
            mode = M_CTL;
        break;
    }
}

void wm_mouse(int x, int y, uint8_t buttons, uint32_t tick)
{
    bool down = (buttons & MB_LEFT) && !(last_buttons & MB_LEFT);
    bool up = !(buttons & MB_LEFT) && (last_buttons & MB_LEFT);
    last_buttons = buttons;
    if (x != cur_x || y != cur_y) {
        cursor(false);
        cur_x = x;
        cur_y = y;
        cursor(true);
    }

    switch (mode) {
    case M_IDLE:
        if (down)
            press(x, y, tick);
        break;
    case M_MOVE: {
        xor_outline();
        outline.x = x - grab_x;
        outline.y = y - grab_y;
        if (outline.y < 0) outline.y = 0;
        if (outline.y > 480 - TILE_H) outline.y = 480 - TILE_H;
        if (up) {
            if (outline.x != mw->x || outline.y != mw->y) {
                wm_invalidate_window(mw);
                mw->x = outline.x;
                mw->y = outline.y;
                wm_invalidate_window(mw);
                ulog("moved %s to %d,%d", mw->title, mw->x, mw->y);
            }
            mode = M_IDLE;
        } else {
            xor_outline();
        }
        break;
    }
    case M_SIZE: {
        xor_outline();
        struct rect o = outer(mw);
        int ddx = x - grab_x, ddy = y - grab_y;
        outline = o;
        if (edges_held & E_L) { outline.x = o.x + ddx; outline.w = o.w - ddx; }
        if (edges_held & E_R) outline.w = o.w + ddx;
        if (edges_held & E_T) { outline.y = o.y + ddy; outline.h = o.h - ddy; }
        if (edges_held & E_B) outline.h = o.h + ddy;
        if (outline.w < MIN_W) { if (edges_held & E_L) outline.x -= MIN_W - outline.w; outline.w = MIN_W; }
        if (outline.h < MIN_H) { if (edges_held & E_T) outline.y -= MIN_H - outline.h; outline.h = MIN_H; }
        if (up) {
            wm_invalidate_window(mw);
            mw->x = outline.x; mw->y = outline.y; mw->w = outline.w; mw->h = outline.h;
            wm_invalidate_window(mw);
            ulog("resized %s to %dx%d", mw->title, mw->w, mw->h);
            mode = M_IDLE;
        } else {
            xor_outline();
        }
        break;
    }
    case M_BOX: {
        int e;
        bool inside = hit(mw, x, y, &e) == box_pressed ||
                      contains(box_rect(mw, box_pressed), x, y);
        if (!up) {
            int want = inside ? box_pressed : 0;
            if ((box_window == mw) != (want != 0)) {
                box_window = want ? mw : NULL;
                struct rect b = box_rect(mw, box_pressed);
                wm_invalidate(b.x, b.y, b.w, b.h);
            }
            break;
        }
        int which = box_pressed;
        box_window = NULL;
        box_pressed = 0;
        mode = M_IDLE;
        wm_invalidate_window(mw);
        if (inside) {
            if (which == H_CLOSE)     wm_close(mw);
            else if (which == H_MAX)  toggle_maximize(mw);
            else                      minimize(mw);
        }
        break;
    }
    case M_CTL:
        if (up) {
            ui_mouse_up(x, y);
            mode = M_IDLE;
        } else {
            ui_mouse_move(x, y);
        }
        break;
    }
    wm_flush();
}

/* ---- keyboard -------------------------------------------------------------- */

static bool alt;

void wm_key(int sc)
{
    if (sc == 0x38) alt = true;
    if (sc == 0xB8) alt = false;
    char ch = kbd_ascii(sc);
    struct window *w = wm_active();
    if (w && !(sc & 0x80)) {
        if (alt && sc == 0x3E)                      /* Alt+F4 */
            wm_close(w);
        else if (sc == 0x0F)                        /* Tab, Shift+Tab */
            ui_focus_next(w, kbd_shift() ? -1 : 1);
        else
            ui_key(w, sc, ch);
    }
    wm_flush();
}
