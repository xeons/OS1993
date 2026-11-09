/*
 * ui.c -- controls: the Windows 95 set, drawn Platinum-style.
 *
 * Push buttons are rounded and bevelled (the default one wears a heavy
 * ring), fields are inset with an accent focus ring, scroll bar thumbs are
 * pale lavender with grip lines, and selection is lavender blue.
 */
#include "ui.h"
#include "os.h"
#include "theme.h"

#define G (&wm_gfx)
#define ITEM_H 16

static void origin(const struct window *w, const struct ctl *c, int *x, int *y)
{
    int cx, cy, cw, ch;
    wm_client_rect(w, &cx, &cy, &cw, &ch);
    *x = cx + c->x;
    *y = cy + c->y;
}

static void raised(int x, int y, int w, int h)
{
    gfx_hline(G, x, y, w, C_WHITE);
    gfx_vline(G, x, y, h, C_WHITE);
    gfx_hline(G, x, y + h - 1, w, C_GRAY4);
    gfx_vline(G, x + w - 1, y, h, C_GRAY4);
}

static void sunken(int x, int y, int w, int h)
{
    gfx_hline(G, x, y, w, C_GRAY4);
    gfx_vline(G, x, y, h, C_GRAY4);
    gfx_hline(G, x, y + h - 1, w, C_WHITE);
    gfx_vline(G, x + w - 1, y, h, C_WHITE);
}

/* A rounded rectangle outline (corners cut by one pixel) */
static void round_rect(int x, int y, int w, int h, uint8_t c)
{
    gfx_hline(G, x + 2, y, w - 4, c);
    gfx_hline(G, x + 2, y + h - 1, w - 4, c);
    gfx_vline(G, x, y + 2, h - 4, c);
    gfx_vline(G, x + w - 1, y + 2, h - 4, c);
    gfx_pixel(G, x + 1, y + 1, c);
    gfx_pixel(G, x + w - 2, y + 1, c);
    gfx_pixel(G, x + 1, y + h - 2, c);
    gfx_pixel(G, x + w - 2, y + h - 2, c);
}

static void text_centered(int x, int y, int w, int h, const char *s, uint8_t c)
{
    int tw = (int)strlen(s) * 8;
    gfx_text(G, x + (w - tw) / 2, y + (h - 16) / 2, s, c, GFX_TRANSPARENT);
}

static bool focusable(const struct ctl *c)
{
    return !(c->flags & CF_DISABLED) && c->type != CT_LABEL && c->type != CT_GROUP &&
           c->type != CT_PROGRESS;
}

static bool has_focus(const struct window *w, const struct ctl *c)
{
    return w == wm_active() && w->focus >= 0 && &w->ctls[w->focus] == c;
}

/* ---- glyphs ---------------------------------------------------------------- */

static const uint16_t g_check[9] = { 0x0080, 0x0180, 0x0300, 0x8600, 0xCC00, 0x7800, 0x3000, 0, 0 };
static const uint16_t g_ring[13] = { 0x0F80, 0x3060, 0x4010, 0x4010, 0x8008, 0x8008, 0x8008,
                                     0x8008, 0x8008, 0x4010, 0x4010, 0x3060, 0x0F80 };
static const uint16_t g_disc[13] = { 0, 0x0F80, 0x3FE0, 0x3FE0, 0x7FF0, 0x7FF0, 0x7FF0,
                                     0x7FF0, 0x7FF0, 0x3FE0, 0x3FE0, 0x0F80, 0 };
static const uint16_t g_dot[13]  = { 0, 0, 0, 0, 0x0700, 0x0F80, 0x0F80, 0x0F80, 0x0700, 0, 0, 0, 0 };
static const uint16_t g_up[7]    = { 0x1000, 0x3800, 0x7C00, 0xFE00, 0, 0, 0 };
static const uint16_t g_down[7]  = { 0xFE00, 0x7C00, 0x3800, 0x1000, 0, 0, 0 };
static const uint16_t g_left[7]  = { 0x1000, 0x3000, 0x7000, 0xF000, 0x7000, 0x3000, 0x1000 };
static const uint16_t g_right[7] = { 0x8000, 0xC000, 0xE000, 0xF000, 0xE000, 0xC000, 0x8000 };
static const uint16_t g_slider[14] = { 0xFFE0, 0x8020, 0x8020, 0x8020, 0x8020, 0x8020, 0x8020,
                                       0x8020, 0x8020, 0x4040, 0x2080, 0x1100, 0x0A00, 0x0400 };
static const uint16_t g_slider_fill[14] = { 0, 0x7FC0, 0x7FC0, 0x7FC0, 0x7FC0, 0x7FC0, 0x7FC0,
                                            0x7FC0, 0x7FC0, 0x3F80, 0x1F00, 0x0E00, 0x0400, 0 };

/* ---- scroll bar geometry (shared with list boxes) -------------------------- */

struct sbar { bool vert; int x, y, w, h; int value, min, max, page; };
enum { SB_NONE, SB_DEC, SB_INC, SB_PAGE_DEC, SB_PAGE_INC, SB_THUMB };

static void sb_thumb(const struct sbar *s, int *pos, int *len, int *track, int *tlen)
{
    int along = s->vert ? s->h : s->w;
    *track = (s->vert ? s->y : s->x) + SCROLL_W;
    *tlen = along - 2 * SCROLL_W;
    int range = s->max - s->min;
    if (range <= 0) { *pos = *track; *len = 0; return; }
    int l = *tlen * s->page / (range + s->page);
    if (l < 12) l = 12;
    if (l > *tlen) l = *tlen;
    *len = l;
    *pos = *track + (*tlen - l) * (s->value - s->min) / range;
}

static void draw_arrow_box(int x, int y, const uint16_t *glyph, bool pressed, bool enabled)
{
    gfx_rect(G, x, y, SCROLL_W, SCROLL_W, C_GRAY2);
    gfx_fill(G, x + 1, y + 1, SCROLL_W - 2, SCROLL_W - 2, pressed ? C_GRAY4 : C_FACE);
    if (!pressed)
        raised(x + 1, y + 1, SCROLL_W - 2, SCROLL_W - 2);
    gfx_bitmap(G, x + 4, y + 5, glyph, 7, enabled ? C_BLACK : C_GRAY4);
}

static int sb_pressed_part;                       /* while tracking */

static void sb_draw(const struct sbar *s, bool tracking)
{
    int pos, len, track, tlen;
    sb_thumb(s, &pos, &len, &track, &tlen);
    bool enabled = s->max > s->min;
    if (s->vert) {
        gfx_fill(G, s->x, track, SCROLL_W, tlen, C_GRAY5);
        gfx_vline(G, s->x, track, tlen, C_GRAY2);
        gfx_vline(G, s->x + SCROLL_W - 1, track, tlen, C_GRAY2);
        gfx_vline(G, s->x + 1, track, tlen, C_GRAY4);
        draw_arrow_box(s->x, s->y, g_up, tracking && sb_pressed_part == SB_DEC, enabled);
        draw_arrow_box(s->x, s->y + s->h - SCROLL_W, g_down, tracking && sb_pressed_part == SB_INC, enabled);
        if (len) {
            gfx_rect(G, s->x, pos, SCROLL_W, len, C_GRAY2);
            gfx_fill(G, s->x + 1, pos + 1, SCROLL_W - 2, len - 2, C_ACCENT_LT);
            raised(s->x + 1, pos + 1, SCROLL_W - 2, len - 2);
            for (int i = -2; i <= 2; i += 2) {          /* grip */
                gfx_hline(G, s->x + 4, pos + len / 2 + i, 8, C_ACCENT);
                gfx_hline(G, s->x + 5, pos + len / 2 + i + 1, 8, C_WHITE);
            }
        }
    } else {
        gfx_fill(G, track, s->y, tlen, SCROLL_W, C_GRAY5);
        gfx_hline(G, track, s->y, tlen, C_GRAY2);
        gfx_hline(G, track, s->y + SCROLL_W - 1, tlen, C_GRAY2);
        gfx_hline(G, track, s->y + 1, tlen, C_GRAY4);
        draw_arrow_box(s->x, s->y, g_left, tracking && sb_pressed_part == SB_DEC, enabled);
        draw_arrow_box(s->x + s->w - SCROLL_W, s->y, g_right, tracking && sb_pressed_part == SB_INC, enabled);
        if (len) {
            gfx_rect(G, pos, s->y, len, SCROLL_W, C_GRAY2);
            gfx_fill(G, pos + 1, s->y + 1, len - 2, SCROLL_W - 2, C_ACCENT_LT);
            raised(pos + 1, s->y + 1, len - 2, SCROLL_W - 2);
            for (int i = -2; i <= 2; i += 2) {
                gfx_vline(G, pos + len / 2 + i, s->y + 4, 8, C_ACCENT);
                gfx_vline(G, pos + len / 2 + i + 1, s->y + 5, 8, C_WHITE);
            }
        }
    }
}

static int sb_hit(const struct sbar *s, int x, int y)
{
    int pos, len, track, tlen;
    sb_thumb(s, &pos, &len, &track, &tlen);
    int a = s->vert ? y : x;
    int start = s->vert ? s->y : s->x, along = s->vert ? s->h : s->w;
    if (a < start + SCROLL_W) return SB_DEC;
    if (a >= start + along - SCROLL_W) return SB_INC;
    if (!len) return SB_NONE;
    if (a < pos) return SB_PAGE_DEC;
    if (a >= pos + len) return SB_PAGE_INC;
    return SB_THUMB;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- per-control helpers ---------------------------------------------------- */

static struct sbar list_bar(const struct window *w, const struct ctl *c)
{
    int x, y;
    origin(w, c, &x, &y);
    int visible = (c->h - 4) / ITEM_H;
    int max = c->nitems - visible;
    return (struct sbar){ true, x + c->w - SCROLL_W - 1, y + 1, SCROLL_W, c->h - 2,
                          c->top, 0, max < 0 ? 0 : max, visible };
}

static struct sbar ctl_bar(const struct window *w, const struct ctl *c)
{
    int x, y;
    origin(w, c, &x, &y);
    return (struct sbar){ c->type == CT_VSCROLL, x, y, c->w, c->h, c->value, c->min, c->max, c->page };
}

static int slider_x(const struct ctl *c, int x)     /* thumb centre */
{
    int range = c->max - c->min;
    return x + 6 + (range ? (c->w - 12) * (c->value - c->min) / range : 0);
}

/* ---- drawing ----------------------------------------------------------------- */

static struct ctl *tracking;                        /* control being clicked */

void ui_draw(struct window *w, struct ctl *c)
{
    int x, y;
    origin(w, c, &x, &y);
    bool dis = c->flags & CF_DISABLED;
    bool focus = has_focus(w, c);
    uint8_t bg = (w->flags & WF_DOCUMENT) ? C_WHITE : C_FACE;

    switch (c->type) {
    case CT_LABEL:
        gfx_text(G, x, y, c->text, dis ? C_GRAY3 : C_BLACK, GFX_TRANSPARENT);
        break;

    case CT_BUTTON: {
        bool down = c->pressed;
        if (c->flags & CF_DEFAULT) {                /* the heavy default ring */
            round_rect(x - 4, y - 4, c->w + 8, c->h + 8, C_GRAY1);
            round_rect(x - 3, y - 3, c->w + 6, c->h + 6, C_GRAY1);
            round_rect(x - 2, y - 2, c->w + 4, c->h + 4, C_GRAY1);
        }
        gfx_fill(G, x + 2, y + 1, c->w - 4, c->h - 2, down ? C_GRAY3 : C_FACE);
        gfx_fill(G, x + 1, y + 2, c->w - 2, c->h - 4, down ? C_GRAY3 : C_FACE);
        round_rect(x, y, c->w, c->h, dis ? C_GRAY4 : C_BLACK);
        if (!down) {
            gfx_hline(G, x + 2, y + 1, c->w - 4, C_WHITE);
            gfx_vline(G, x + 1, y + 2, c->h - 4, C_WHITE);
            gfx_hline(G, x + 2, y + c->h - 2, c->w - 4, C_GRAY4);
            gfx_vline(G, x + c->w - 2, y + 2, c->h - 4, C_GRAY4);
        }
        text_centered(x, y, c->w, c->h, c->text, dis ? C_GRAY3 : down ? C_WHITE : C_BLACK);
        if (focus)
            gfx_rect(G, x + 3, y + 3, c->w - 6, c->h - 6, C_ACCENT);
        break;
    }

    case CT_CHECK:
    case CT_RADIO: {
        int by = y + (c->h - CHECK) / 2;
        bool down = c->pressed;
        if (c->type == CT_CHECK) {
            gfx_rect(G, x, by, CHECK, CHECK, dis ? C_GRAY4 : C_BLACK);
            gfx_fill(G, x + 1, by + 1, CHECK - 2, CHECK - 2, down ? C_GRAY5 : C_WHITE);
            gfx_hline(G, x + 1, by + 1, CHECK - 2, C_GRAY5);
            gfx_vline(G, x + 1, by + 1, CHECK - 2, C_GRAY5);
            if (c->value)
                gfx_bitmap(G, x + 2, by + 2, g_check, 9, dis ? C_GRAY4 : C_BLACK);
        } else {
            gfx_bitmap(G, x, by, g_disc, 13, down ? C_GRAY5 : C_WHITE);
            gfx_bitmap(G, x, by, g_ring, 13, dis ? C_GRAY4 : C_BLACK);
            if (c->value)
                gfx_bitmap(G, x, by, g_dot, 13, dis ? C_GRAY4 : C_BLACK);
        }
        int tx = x + CHECK + 6, ty = y + (c->h - 16) / 2;
        gfx_text(G, tx, ty, c->text, dis ? C_GRAY3 : C_BLACK, GFX_TRANSPARENT);
        if (focus)
            gfx_rect(G, tx - 2, ty, (int)strlen(c->text) * 8 + 4, 16, C_ACCENT);
        break;
    }

    case CT_GROUP: {
        gfx_rect(G, x, y + 8, c->w - 1, c->h - 9, C_GRAY4);
        gfx_rect(G, x + 1, y + 9, c->w - 1, c->h - 9, C_WHITE);
        int tw = (int)strlen(c->text) * 8;
        gfx_fill(G, x + 8, y, tw + 8, 16, bg);
        gfx_text(G, x + 12, y, c->text, C_BLACK, GFX_TRANSPARENT);
        break;
    }

    case CT_EDIT: {
        if (focus) {                                /* accent focus ring */
            gfx_rect(G, x - 3, y - 3, c->w + 6, c->h + 6, C_ACCENT);
            gfx_rect(G, x - 2, y - 2, c->w + 4, c->h + 4, C_ACCENT);
        }
        sunken(x, y, c->w, c->h);
        gfx_rect(G, x + 1, y + 1, c->w - 2, c->h - 2, C_GRAY1);
        gfx_fill(G, x + 2, y + 2, c->w - 4, c->h - 4, dis ? C_FACE : C_WHITE);
        int room = (c->w - 10) / 8, len = (int)strlen(c->buf);
        int first = c->caret > room ? c->caret - room : 0;
        gfx_text(G, x + 5, y + (c->h - 16) / 2, c->buf + first, C_BLACK, GFX_TRANSPARENT);
        if (focus && len >= 0)
            gfx_vline(G, x + 5 + (c->caret - first) * 8, y + 4, c->h - 8, C_BLACK);
        break;
    }

    case CT_LIST: {
        if (focus) {
            gfx_rect(G, x - 3, y - 3, c->w + 6, c->h + 6, C_ACCENT);
            gfx_rect(G, x - 2, y - 2, c->w + 4, c->h + 4, C_ACCENT);
        }
        gfx_rect(G, x, y, c->w, c->h, C_GRAY1);
        int iw = c->w - SCROLL_W - 2;
        gfx_fill(G, x + 1, y + 1, iw, c->h - 2, C_WHITE);
        int visible = (c->h - 4) / ITEM_H;
        for (int i = 0; i < visible && c->top + i < c->nitems; i++) {
            int item = c->top + i, iy = y + 2 + i * ITEM_H;
            bool sel = item == c->value;
            if (sel)
                gfx_fill(G, x + 1, iy, iw, ITEM_H, C_ACCENT);
            gfx_text(G, x + 6, iy, c->items[item], sel ? C_WHITE : C_BLACK, GFX_TRANSPARENT);
        }
        struct sbar s = list_bar(w, c);
        sb_draw(&s, tracking == c);
        break;
    }

    case CT_VSCROLL:
    case CT_HSCROLL: {
        struct sbar s = ctl_bar(w, c);
        sb_draw(&s, tracking == c);
        break;
    }

    case CT_SLIDER: {
        int ty = y + 6;
        sunken(x, ty, c->w, 5);
        gfx_fill(G, x + 1, ty + 1, c->w - 2, 3, C_WHITE);
        for (int i = 0; i <= 10; i++)               /* tick marks */
            gfx_vline(G, x + 6 + (c->w - 12) * i / 10, y + c->h - 4, 3, C_GRAY3);
        int tx = slider_x(c, x) - 5;
        gfx_bitmap(G, tx, y, g_slider_fill, 14, c->pressed ? C_ACCENT : C_ACCENT_LT);
        gfx_bitmap(G, tx, y, g_slider, 14, C_BLACK);
        if (focus)
            gfx_rect(G, x - 2, y - 2, c->w + 4, c->h + 4, C_ACCENT);
        break;
    }

    case CT_PROGRESS: {
        gfx_rect(G, x, y, c->w, c->h, C_GRAY1);
        sunken(x + 1, y + 1, c->w - 2, c->h - 2);
        gfx_fill(G, x + 2, y + 2, c->w - 4, c->h - 4, C_GRAY5);
        int range = c->max - c->min;
        int fw = range ? (c->w - 4) * (c->value - c->min) / range : 0;
        if (fw > 0) {
            gfx_fill(G, x + 2, y + 2, fw, c->h - 4, C_ACCENT);
            gfx_hline(G, x + 2, y + 3, fw, C_ACCENT_LT);
            for (int sx = x + 2 + 6; sx < x + 2 + fw; sx += 8)    /* bands */
                gfx_vline(G, sx, y + 4, c->h - 6, C_ACCENT_DK);
        }
        break;
    }
    }
}

void ui_draw_all(struct window *w)
{
    for (int i = 0; i < w->nctl; i++)
        ui_draw(w, &w->ctls[i]);
}

void ui_redraw(struct window *w, struct ctl *c)
{
    int x, y;
    origin(w, c, &x, &y);
    wm_invalidate(x - 5, y - 5, c->w + 10, c->h + 10);  /* rings and focus too */
}

/* ---- behaviour ------------------------------------------------------------- */

static struct window *track_win;
static int track_part, grab;

static void command(struct window *w, struct ctl *c)
{
    if (w->on_command)
        w->on_command(w, c->id);
}

static void set_focus(struct window *w, struct ctl *c)
{
    int idx = (int)(c - w->ctls);
    if (w->focus == idx)
        return;
    if (w->focus >= 0)
        ui_redraw(w, &w->ctls[w->focus]);
    w->focus = idx;
    ui_redraw(w, c);
}

static void activate(struct window *w, struct ctl *c)
{
    switch (c->type) {
    case CT_BUTTON:
        command(w, c);
        break;
    case CT_CHECK:
        c->value = !c->value;
        ui_redraw(w, c);
        command(w, c);
        break;
    case CT_RADIO:
        for (int i = 0; i < w->nctl; i++) {
            struct ctl *o = &w->ctls[i];
            if (o->type == CT_RADIO && o->group == c->group && o->value) {
                o->value = 0;
                ui_redraw(w, o);
            }
        }
        c->value = 1;
        ui_redraw(w, c);
        command(w, c);
        break;
    default:
        break;
    }
}

static void scroll_to(struct window *w, struct ctl *c, int v)
{
    if (c->type == CT_LIST) {
        struct sbar s = list_bar(w, c);
        v = clampi(v, 0, s.max);
        if (v != c->top) { c->top = v; ui_redraw(w, c); }
        return;
    }
    v = clampi(v, c->min, c->max);
    if (v != c->value) {
        c->value = v;
        ui_redraw(w, c);
        command(w, c);
    }
}

static bool in_ctl(const struct window *w, const struct ctl *c, int px, int py)
{
    int x, y;
    origin(w, c, &x, &y);
    return px >= x && py >= y && px < x + c->w && py < y + c->h;
}

static void slider_set(struct window *w, struct ctl *c, int px)
{
    int x, y;
    origin(w, c, &x, &y);
    int range = c->max - c->min, span = c->w - 12;
    int v = c->min + (span ? ((px - x - 6) * range + span / 2) / span : 0);
    scroll_to(w, c, v);
}

static void sb_press(struct window *w, struct ctl *c, struct sbar *s, int px, int py)
{
    int cur = c->type == CT_LIST ? c->top : c->value;
    int step = 1, page = s->page > 0 ? s->page : 1;
    track_part = sb_hit(s, px, py);
    sb_pressed_part = track_part;
    switch (track_part) {
    case SB_DEC:      scroll_to(w, c, cur - step); break;
    case SB_INC:      scroll_to(w, c, cur + step); break;
    case SB_PAGE_DEC: scroll_to(w, c, cur - page); break;
    case SB_PAGE_INC: scroll_to(w, c, cur + page); break;
    case SB_THUMB: {
        int pos, len, track, tlen;
        sb_thumb(s, &pos, &len, &track, &tlen);
        grab = (s->vert ? py : px) - pos;
        break;
    }
    }
    ui_redraw(w, c);
}

static void sb_drag(struct window *w, struct ctl *c, struct sbar *s, int px, int py)
{
    if (track_part != SB_THUMB)
        return;
    int pos, len, track, tlen;
    sb_thumb(s, &pos, &len, &track, &tlen);
    int span = tlen - len, range = s->max - s->min;
    if (span <= 0)
        return;
    int p = (s->vert ? py : px) - grab - track;
    scroll_to(w, c, s->min + (p * range + span / 2) / span);
}

bool ui_mouse_down(struct window *w, int px, int py)
{
    for (int i = w->nctl - 1; i >= 0; i--) {
        struct ctl *c = &w->ctls[i];
        if (!in_ctl(w, c, px, py) || !focusable(c))
            continue;
        set_focus(w, c);
        tracking = c;
        track_win = w;
        switch (c->type) {
        case CT_BUTTON: case CT_CHECK: case CT_RADIO:
            c->pressed = true;
            ui_redraw(w, c);
            break;
        case CT_EDIT: {
            int x, y;
            origin(w, c, &x, &y);
            int room = (c->w - 10) / 8, len = (int)strlen(c->buf);
            int first = c->caret > room ? c->caret - room : 0;
            c->caret = clampi(first + (px - x - 5 + 4) / 8, 0, len);
            ui_redraw(w, c);
            break;
        }
        case CT_LIST: {
            struct sbar s = list_bar(w, c);
            if (px >= s.x) {
                sb_press(w, c, &s, px, py);
            } else {
                int x, y;
                origin(w, c, &x, &y);
                int item = c->top + (py - y - 2) / ITEM_H;
                if (item >= 0 && item < c->nitems && item != c->value) {
                    c->value = item;
                    ui_redraw(w, c);
                    command(w, c);
                }
                track_part = SB_NONE;
            }
            break;
        }
        case CT_VSCROLL: case CT_HSCROLL: {
            struct sbar s = ctl_bar(w, c);
            sb_press(w, c, &s, px, py);
            break;
        }
        case CT_SLIDER:
            c->pressed = true;
            slider_set(w, c, px);
            ui_redraw(w, c);
            break;
        }
        return true;
    }
    return false;
}

void ui_mouse_move(int px, int py)
{
    struct ctl *c = tracking;
    struct window *w = track_win;
    if (!c)
        return;
    switch (c->type) {
    case CT_BUTTON: case CT_CHECK: case CT_RADIO: {
        bool in = in_ctl(w, c, px, py);
        if (in != c->pressed) { c->pressed = in; ui_redraw(w, c); }
        break;
    }
    case CT_LIST: {
        struct sbar s = list_bar(w, c);
        sb_drag(w, c, &s, px, py);
        break;
    }
    case CT_VSCROLL: case CT_HSCROLL: {
        struct sbar s = ctl_bar(w, c);
        sb_drag(w, c, &s, px, py);
        break;
    }
    case CT_SLIDER:
        slider_set(w, c, px);
        break;
    }
}

void ui_mouse_up(int px, int py)
{
    struct ctl *c = tracking;
    struct window *w = track_win;
    if (!c)
        return;
    tracking = NULL;
    sb_pressed_part = SB_NONE;
    if (c->pressed) {
        c->pressed = false;
        ui_redraw(w, c);
        if (in_ctl(w, c, px, py))
            activate(w, c);
    }
    ui_redraw(w, c);
}

void ui_focus_next(struct window *w, int dir)
{
    if (!w->nctl)
        return;
    int i = w->focus;
    for (int n = 0; n < w->nctl; n++) {
        i = (i + dir + w->nctl) % w->nctl;
        if (focusable(&w->ctls[i])) {
            set_focus(w, &w->ctls[i]);
            return;
        }
    }
}

bool ui_key(struct window *w, int sc, char ch)
{
    struct ctl *f = w->focus >= 0 ? &w->ctls[w->focus] : NULL;

    if (f && f->type == CT_EDIT) {
        int len = (int)strlen(f->buf);
        if (ch >= 32 && ch < 127 && len < f->buflen - 1) {
            for (int i = len; i >= f->caret; i--)
                f->buf[i + 1] = f->buf[i];
            f->buf[f->caret++] = ch;
        } else if (ch == '\b' && f->caret > 0) {
            for (int i = f->caret - 1; i < len; i++)
                f->buf[i] = f->buf[i + 1];
            f->caret--;
        } else if (sc == 0x53 && f->caret < len) {          /* Delete */
            for (int i = f->caret; i < len; i++)
                f->buf[i] = f->buf[i + 1];
        } else if (sc == 0x4B && f->caret > 0)  f->caret--; /* Left */
        else if (sc == 0x4D && f->caret < len)  f->caret++; /* Right */
        else if (sc == 0x47)                    f->caret = 0;
        else if (sc == 0x4F)                    f->caret = len;
        else if (ch != '\n')
            return false;
        if (ch != '\n') {
            ui_redraw(w, f);
            return true;
        }
    }

    if (ch == '\n') {                                       /* Enter: default button */
        for (int i = 0; i < w->nctl; i++)
            if (w->ctls[i].type == CT_BUTTON && (w->ctls[i].flags & CF_DEFAULT)) {
                activate(w, &w->ctls[i]);
                return true;
            }
        return false;
    }
    if (!f)
        return false;
    if (ch == ' ' && (f->type == CT_BUTTON || f->type == CT_CHECK || f->type == CT_RADIO)) {
        activate(w, f);
        return true;
    }
    int d = (sc == 0x48 || sc == 0x4B) ? -1 : (sc == 0x50 || sc == 0x4D) ? 1 : 0;
    if (!d)
        return false;
    if (f->type == CT_LIST) {
        int v = clampi(f->value + d, 0, f->nitems - 1);
        if (v != f->value) {
            f->value = v;
            int visible = (f->h - 4) / ITEM_H;
            if (v < f->top) f->top = v;
            if (v >= f->top + visible) f->top = v - visible + 1;
            ui_redraw(w, f);
            command(w, f);
        }
        return true;
    }
    if (f->type == CT_SLIDER || f->type == CT_VSCROLL || f->type == CT_HSCROLL) {
        scroll_to(w, f, f->value + d);
        return true;
    }
    return false;
}
