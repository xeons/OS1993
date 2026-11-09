/*
 * scroll.c -- the SCROLLBAR class, and scroll bar geometry shared with
 * list boxes. Platinum style: grey track, bevelled arrow boxes, a pale
 * lavender thumb with grip lines, proportional to the page size.
 *
 * As in Windows, a scroll bar doesn't move itself: it tells its parent
 * (WM_HSCROLL / WM_VSCROLL with SB_* codes) and the parent sets the new
 * position with SBM_SETPOS. Only while the thumb is dragged does it draw
 * the thumb where the pointer is.
 */
#include "uipriv.h"

static const uint16_t g_up[7]    = { 0x1000, 0x3800, 0x7C00, 0xFE00, 0, 0, 0 };
static const uint16_t g_down[7]  = { 0xFE00, 0x7C00, 0x3800, 0x1000, 0, 0, 0 };
static const uint16_t g_left[7]  = { 0x1000, 0x3000, 0x7000, 0xF000, 0x7000, 0x3000, 0x1000 };
static const uint16_t g_right[7] = { 0x8000, 0xC000, 0xE000, 0xF000, 0xE000, 0xC000, 0x8000 };

void sbar_thumb(const struct sbar *s, int *pos, int *len, int *track, int *tlen)
{
    int along = s->vert ? s->h : s->w;
    *track = (s->vert ? s->y : s->x) + SCROLL_W;
    *tlen = along - 2 * SCROLL_W;
    int range = s->max - s->min;
    if (range <= 0 || *tlen <= 0) { *pos = *track; *len = 0; return; }
    int l = *tlen * s->page / (range + s->page);
    if (l < 12) l = 12;
    if (l > *tlen) l = *tlen;
    *len = l;
    *pos = *track + (*tlen - l) * (clampi(s->value, s->min, s->max) - s->min) / range;
}

static void arrow_box(HDC dc, int x, int y, const uint16_t *glyph, bool pressed, bool enabled)
{
    RECT r = { x, y, x + SCROLL_W, y + SCROLL_W };
    FrameRect(dc, &r, C_GRAY2);
    RECT in = { x + 1, y + 1, x + SCROLL_W - 1, y + SCROLL_W - 1 };
    FillRect(dc, &in, pressed ? C_GRAY4 : C_FACE);
    if (!pressed)
        DrawBevel(dc, &in, true);
    DrawBitmap(dc, x + 4, y + 5, glyph, 7, enabled ? C_BLACK : C_GRAY4);
}

void sbar_draw(HDC dc, const struct sbar *s, int pressed)
{
    int pos, len, track, tlen;
    sbar_thumb(s, &pos, &len, &track, &tlen);
    bool enabled = s->max > s->min;
    if (s->vert) {
        RECT t = { s->x, track, s->x + SCROLL_W, track + tlen };
        FillRect(dc, &t, C_GRAY5);
        DrawLine(dc, s->x, track, tlen, true, C_GRAY2);
        DrawLine(dc, s->x + SCROLL_W - 1, track, tlen, true, C_GRAY2);
        DrawLine(dc, s->x + 1, track, tlen, true, C_GRAY4);
        arrow_box(dc, s->x, s->y, g_up, pressed == SB_PART_DEC, enabled);
        arrow_box(dc, s->x, s->y + s->h - SCROLL_W, g_down, pressed == SB_PART_INC, enabled);
        if (len) {
            RECT th = { s->x, pos, s->x + SCROLL_W, pos + len };
            FrameRect(dc, &th, C_GRAY2);
            RECT in = { s->x + 1, pos + 1, s->x + SCROLL_W - 1, pos + len - 1 };
            FillRect(dc, &in, C_ACCENT_LT);
            DrawBevel(dc, &in, true);
            for (int i = -2; i <= 2; i += 2) {
                DrawLine(dc, s->x + 4, pos + len / 2 + i, 8, false, C_ACCENT);
                DrawLine(dc, s->x + 5, pos + len / 2 + i + 1, 8, false, C_WHITE);
            }
        }
    } else {
        RECT t = { track, s->y, track + tlen, s->y + SCROLL_W };
        FillRect(dc, &t, C_GRAY5);
        DrawLine(dc, track, s->y, tlen, false, C_GRAY2);
        DrawLine(dc, track, s->y + SCROLL_W - 1, tlen, false, C_GRAY2);
        DrawLine(dc, track, s->y + 1, tlen, false, C_GRAY4);
        arrow_box(dc, s->x, s->y, g_left, pressed == SB_PART_DEC, enabled);
        arrow_box(dc, s->x + s->w - SCROLL_W, s->y, g_right, pressed == SB_PART_INC, enabled);
        if (len) {
            RECT th = { pos, s->y, pos + len, s->y + SCROLL_W };
            FrameRect(dc, &th, C_GRAY2);
            RECT in = { pos + 1, s->y + 1, pos + len - 1, s->y + SCROLL_W - 1 };
            FillRect(dc, &in, C_ACCENT_LT);
            DrawBevel(dc, &in, true);
            for (int i = -2; i <= 2; i += 2) {
                DrawLine(dc, pos + len / 2 + i, s->y + 4, 8, true, C_ACCENT);
                DrawLine(dc, pos + len / 2 + i + 1, s->y + 5, 8, true, C_WHITE);
            }
        }
    }
}

int sbar_hit(const struct sbar *s, int x, int y)
{
    int pos, len, track, tlen;
    sbar_thumb(s, &pos, &len, &track, &tlen);
    int a = s->vert ? y : x, start = s->vert ? s->y : s->x, along = s->vert ? s->h : s->w;
    if (a < start + SCROLL_W) return SB_PART_DEC;
    if (a >= start + along - SCROLL_W) return SB_PART_INC;
    if (!len) return SB_PART_NONE;
    if (a < pos) return SB_PART_PGDEC;
    if (a >= pos + len) return SB_PART_PGINC;
    return SB_PART_THUMB;
}

/* The value the thumb would have if its near edge were at `along` - grab. */
int sbar_thumb_value(const struct sbar *s, int along, int grab)
{
    int pos, len, track, tlen;
    sbar_thumb(s, &pos, &len, &track, &tlen);
    int span = tlen - len, range = s->max - s->min;
    if (span <= 0)
        return s->value;
    return clampi(s->min + ((along - grab - track) * range + span / 2) / span, s->min, s->max);
}

/* ---- the SCROLLBAR class ------------------------------------------------------ */

static struct sbar bar_of(struct wnd *w)
{
    bool vert = (w->style & SBS_VERT) != 0;
    int value = w->tracking && w->part == SB_PART_THUMB ? w->caret : w->value;
    return (struct sbar){ vert, 0, 0, w->w, w->h, value, w->min, w->max, w->page ? w->page : 1 };
}

static void notify(struct wnd *w, int code, int pos)
{
    if (w->parent)
        SendMessage(w->parent, (w->style & SBS_VERT) ? WM_VSCROLL : WM_HSCROLL,
                    MAKEWPARAM(code, pos), (LPARAM)w);
}

LRESULT scrollbar_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    switch (msg) {
    case WM_CREATE:
        w->max = 100;
        w->page = 10;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        struct sbar s = bar_of(w);
        sbar_draw(dc, &s, w->tracking ? w->part : SB_PART_NONE);
        EndPaint(h, &ps);
        return 0;
    }
    case SBM_SETRANGE: w->min = (int)wp; w->max = (int)lp; InvalidateRect(h, NULL); return 0;
    case SBM_SETPAGE:  w->page = (int)wp; InvalidateRect(h, NULL); return 0;
    case SBM_SETPOS: {
        int old = w->value;
        w->value = clampi((int)wp, w->min, w->max);
        InvalidateRect(h, NULL);
        return old;
    }
    case SBM_GETPOS:   return w->value;
    case WM_LBUTTONDOWN: {
        struct sbar s = bar_of(w);
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        w->part = sbar_hit(&s, x, y);
        w->tracking = true;
        SetCapture(h);
        SetFocus(h);
        if (w->part == SB_PART_THUMB) {
            int pos, len, track, tlen;
            sbar_thumb(&s, &pos, &len, &track, &tlen);
            w->grab = (s.vert ? y : x) - pos;
            w->caret = w->value;                    /* tracked thumb value */
        } else {
            static const int code[] = { 0, SB_LINEUP, SB_LINEDOWN, SB_PAGEUP, SB_PAGEDOWN };
            if (w->part >= SB_PART_DEC && w->part <= SB_PART_PGINC)
                notify(w, code[w->part], w->value);
        }
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (w->tracking && w->part == SB_PART_THUMB) {
            struct sbar s = bar_of(w);
            s.value = w->value;
            int v = sbar_thumb_value(&s, (w->style & SBS_VERT) ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp), w->grab);
            if (v != w->caret) {
                w->caret = v;
                InvalidateRect(h, NULL);
                notify(w, SB_THUMBTRACK, v);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->tracking) {
            ReleaseCapture();
            w->tracking = false;
            if (w->part == SB_PART_THUMB)
                notify(w, SB_THUMBPOSITION, w->caret);
            notify(w, SB_ENDSCROLL, w->value);
            w->part = SB_PART_NONE;
            InvalidateRect(h, NULL);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_LEFT || wp == VK_UP)    notify(w, SB_LINEUP, w->value);
        if (wp == VK_RIGHT || wp == VK_DOWN) notify(w, SB_LINEDOWN, w->value);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}
