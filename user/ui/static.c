/* static.c -- the STATIC class (labels) and the PROGRESS class. Both are
 * mouse-transparent: clicks go to whatever is beneath them. */
#include "uipriv.h"

LRESULT static_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    if (msg == WM_SETTEXT) {
        LRESULT r = DefWindowProc(h, msg, wp, lp);
        InvalidateRect(h, NULL);
        return r;
    }
    if (msg != WM_PAINT)
        return DefWindowProc(h, msg, wp, lp);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r = { 0, 0, w->w, w->h };
    unsigned type = w->style & SS_TYPEMASK;
    uint8_t fg = w->enabled ? C_BLACK : C_GRAY3, bg = C_FACE;
    switch (type) {
    case SS_BLACKRECT: FillRect(dc, &r, C_BLACK); break;
    case SS_GRAYRECT:  FillRect(dc, &r, C_GRAY3); break;
    case SS_WHITERECT: FillRect(dc, &r, C_WHITE); break;
    case SS_BLACKFRAME: FrameRect(dc, &r, C_BLACK); break;
    case SS_GRAYFRAME:  FrameRect(dc, &r, C_GRAY3); break;
    case SS_WHITEFRAME: FrameRect(dc, &r, C_WHITE); break;
    case SS_ETCHEDHORZ:
        DrawLine(dc, 0, 0, w->w, false, C_GRAY4);
        DrawLine(dc, 0, 1, w->w, false, C_WHITE);
        break;
    case SS_ETCHEDVERT:
        DrawLine(dc, 0, 0, w->h, true, C_GRAY4);
        DrawLine(dc, 1, 0, w->h, true, C_WHITE);
        break;
    case SS_ETCHEDFRAME: {
        RECT a = { 0, 0, w->w - 1, w->h - 1 }, b = { 1, 1, w->w, w->h };
        FrameRect(dc, &b, C_WHITE);
        FrameRect(dc, &a, C_GRAY4);
        break;
    }
    case SS_OWNERDRAW:
        owner_draw(w, dc, ODT_STATIC, 0, ODA_DRAWENTIRE, w->enabled ? 0 : ODS_DISABLED, r, 0);
        break;
    default: {                                  /* text */
        bool filled = false;
        if (w->parent && wnd_mine(w->parent)) {
            uint8_t b0 = bg;
            ctl_color(w, dc, WM_CTLCOLORSTATIC, &fg, &bg);
            if (bg != b0) { FillRect(dc, &r, bg); filled = true; }
        }
        (void)filled;
        if (w->style & SS_SUNKEN) {
            DrawBevel(dc, &r, false);
            r = (RECT){ 2, 1, w->w - 2, w->h - 1 };
        }
        unsigned f = type == SS_CENTER ? DT_CENTER : type == SS_RIGHT ? DT_RIGHT : DT_LEFT;
        if (type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP) f = DT_SINGLELINE;
        else f |= DT_WORDBREAK;
        if (w->style & SS_NOPREFIX) f |= DT_NOPREFIX;
        if (w->style & SS_ENDELLIPSIS) f = (f & ~(unsigned)DT_WORDBREAK) | DT_SINGLELINE | DT_END_ELLIPSIS;
        if (w->style & SS_CENTERIMAGE) f = (f & ~(unsigned)DT_WORDBREAK) | DT_SINGLELINE | DT_VCENTER;
        SelectFont(dc, FONT_GUI);
        SetTextColor(dc, fg);
        DrawText(dc, w->text, -1, &r, f);
        break;
    }
    }
    EndPaint(h, &ps);
    return 0;
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
