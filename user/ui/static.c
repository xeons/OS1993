/* static.c -- the STATIC class (labels) and the PROGRESS class. Both are
 * mouse-transparent: clicks go to whatever is beneath them. */
#include "uipriv.h"

LRESULT static_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        TextOut(dc, 0, 0, w->text, w->enabled ? C_BLACK : C_GRAY3, -1);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* A sunken trough filled with banded accent colour. */
LRESULT progress_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    switch (msg) {
    case WM_CREATE:
        w->max = 100;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT o = { 0, 0, w->w, w->h }, s = { 1, 1, w->w - 1, w->h - 1 }, in = { 2, 2, w->w - 2, w->h - 2 };
        FrameRect(dc, &o, C_GRAY1);
        DrawBevel(dc, &s, false);
        FillRect(dc, &in, C_GRAY5);
        int range = w->max - w->min;
        int fw = range > 0 ? (w->w - 4) * (clampi(w->value, w->min, w->max) - w->min) / range : 0;
        if (fw > 0) {
            RECT f = { 2, 2, 2 + fw, w->h - 2 };
            FillRect(dc, &f, C_ACCENT);
            DrawLine(dc, 2, 3, fw, false, C_ACCENT_LT);
            for (int x = 2 + 6; x < 2 + fw; x += 8)          /* bands */
                DrawLine(dc, x, 4, w->h - 6, true, C_ACCENT_DK);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case PBM_SETRANGE:
        w->min = (int)wp;
        w->max = (int)lp;
        InvalidateRect(h, NULL);
        return 0;
    case PBM_SETPOS: {
        int old = w->value;
        if (wp != (WPARAM)old) {
            w->value = (int)wp;
            InvalidateRect(h, NULL);
        }
        return old;
    }
    case PBM_GETPOS:
        return w->value;
    }
    return DefWindowProc(h, msg, wp, lp);
}
