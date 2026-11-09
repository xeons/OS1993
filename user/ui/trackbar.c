/*
 * trackbar.c -- the TRACKBAR class: a sunken groove with tick marks and a
 * pointed lavender thumb. The outer 2 pixels hold the focus ring. It moves
 * itself and tells the parent with WM_HSCROLL (TB_THUMBTRACK while it
 * changes, TB_ENDTRACK when the mouse lets go, SB_LINE* from the keys).
 */
#include "uipriv.h"

#define RING 2

static const uint16_t g_slider[14] = { 0xFFE0, 0x8020, 0x8020, 0x8020, 0x8020, 0x8020, 0x8020,
                                       0x8020, 0x8020, 0x4040, 0x2080, 0x1100, 0x0A00, 0x0400 };
static const uint16_t g_slider_fill[14] = { 0, 0x7FC0, 0x7FC0, 0x7FC0, 0x7FC0, 0x7FC0, 0x7FC0,
                                            0x7FC0, 0x7FC0, 0x3F80, 0x1F00, 0x0E00, 0x0400, 0 };

static int body_w(struct wnd *w) { return w->w - 2 * RING; }

static void notify(struct wnd *w, int code)
{
    if (w->parent)
        SendMessage(w->parent, WM_HSCROLL, MAKEWPARAM(code, w->value), (LPARAM)w);
}

static bool set(struct wnd *w, int v)
{
    v = clampi(v, w->min, w->max);
    if (v == w->value)
        return false;
    w->value = v;
    InvalidateRect(w, NULL);
    return true;
}

static void follow(struct wnd *w, int x)
{
    int range = w->max - w->min, span = body_w(w) - 12;
    int v = w->min + (span ? ((x - RING - 6) * range + span / 2) / span : 0);
    if (set(w, v))
        notify(w, TB_THUMBTRACK);
}

LRESULT trackbar_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    switch (msg) {
    case WM_CREATE:
        w->max = 100;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int x = RING, y = RING, bw = body_w(w), bh = w->h - 2 * RING;
        RECT groove = { x, y + 6, x + bw, y + 11 }, in = { x + 1, y + 7, x + bw - 1, y + 10 };
        DrawBevel(dc, &groove, false);
        FillRect(dc, &in, C_WHITE);
        for (int i = 0; i <= 10; i++)
            DrawLine(dc, x + 6 + (bw - 12) * i / 10, y + bh - 4, 3, true, C_GRAY3);
        int range = w->max - w->min;
        int tx = x + 6 + (range ? (bw - 12) * (clampi(w->value, w->min, w->max) - w->min) / range : 0) - 5;
        DrawBitmap(dc, tx, y, g_slider_fill, 14, w->tracking ? C_ACCENT : C_ACCENT_LT);
        DrawBitmap(dc, tx, y, g_slider, 14, C_BLACK);
        if (ui_has_focus(w)) {
            RECT f = { 0, 0, w->w, w->h };
            FrameRect(dc, &f, C_ACCENT);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        w->tracking = true;
        follow(w, GET_X_LPARAM(lp));
        InvalidateRect(h, NULL);
        return 0;
    case WM_MOUSEMOVE:
        if (w->tracking)
            follow(w, GET_X_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (w->tracking) {
            ReleaseCapture();
            w->tracking = false;
            InvalidateRect(h, NULL);
            notify(w, TB_ENDTRACK);
        }
        return 0;
    case WM_KEYDOWN:
        if ((wp == VK_LEFT || wp == VK_UP) && set(w, w->value - 1))     notify(w, SB_LINEUP);
        if ((wp == VK_RIGHT || wp == VK_DOWN) && set(w, w->value + 1))  notify(w, SB_LINEDOWN);
        return 0;
    case TBM_GETPOS:
        return w->value;
    case TBM_SETPOS:
        set(w, (int)lp);
        return 0;
    case TBM_SETRANGE:
        w->min = LOWORD(lp);
        w->max = HIWORD(lp);
        set(w, w->value);
        InvalidateRect(h, NULL);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}
