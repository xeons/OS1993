/*
 * colordlg.c -- ChooseColor, the colour dialog (SHELL.DLL).
 *
 * The theme's 16 colours as swatches to click, 16 custom colours below
 * them (the program keeps them between calls in cc->custom), and with
 * "Define Custom Colours >>" (or CC_FULLOPEN) red, green and blue sliders
 * with a preview, which "Add to Custom Colours" saves. The answer is an
 * RGB value and, for programs that draw in the theme's colours, the
 * nearest of those. Modal to the owner, like the file dialogs.
 */
#include "os1993.h"
#include <stdio.h>
#include <stdlib.h>

#define ID_DEFINE  200
#define ID_ADD     201
#define ID_R       210              /* + 0..2: the sliders */
#define ID_RE      220              /* + 0..2: their numbers */
#define SW         22               /* a swatch */
#define GAP        6
#define BASIC_Y    28
#define CUSTOM_Y   (BASIC_Y + 2 * (SW + GAP) + 24)
#define SMALL_W    (8 * (SW + GAP) + 16)
#define FULL_W     (SMALL_W + 200)
#define DLG_H      (CUSTOM_Y + 2 * (SW + GAP) + 50)

struct cdlg {
    CHOOSECOLOR *cc;
    COLORREF rgb;
    int sel;                        /* 0-15 basic, 16-31 custom, -1 none */
    bool full, done, ok, syncing;
};

static struct cdlg *cd(HWND h) { return (struct cdlg *)(uintptr_t)GetWindowLong(h, GWL_USERDATA); }

static RECT swatch(int i)           /* 0-15 basic, 16-31 custom */
{
    int k = i & 15, y0 = i < 16 ? BASIC_Y : CUSTOM_Y;
    int x = 10 + (k % 8) * (SW + GAP), y = y0 + (k / 8) * (SW + GAP);
    return (RECT){ x, y, x + SW, y + SW };
}

static COLORREF color_of(struct cdlg *d, int i)
{
    if (i < 16) return GetThemeColor(i);
    return d->cc->custom ? d->cc->custom[i - 16] : RGB(255, 255, 255);
}

static int nearest(COLORREF c)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        COLORREF t = GetThemeColor(i);
        int dr = (int)GetRValue(c) - (int)GetRValue(t), dg = (int)GetGValue(c) - (int)GetGValue(t),
            db = (int)GetBValue(c) - (int)GetBValue(t);
        int dist = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (dist < bd) { bd = dist; best = i; }
    }
    return best;
}

static void fill(HDC dc, RECT r, COLORREF c, int index)
{
    DEVMODE m;
    EnumDisplaySettings(ENUM_CURRENT_SETTINGS, &m);
    if (m.dmBitsPerPel >= 15)
        FillRectRGB(dc, &r, GetRValue(c), GetGValue(c), GetBValue(c));
    else
        FillRect(dc, &r, (uint8_t)(index >= 0 ? index : nearest(c)));
}

static void sync_sliders(HWND h)
{
    struct cdlg *d = cd(h);
    uint8_t v[3] = { GetRValue(d->rgb), GetGValue(d->rgb), GetBValue(d->rgb) };
    d->syncing = true;
    for (int k = 0; k < 3; k++) {
        SendMessage(GetDlgItem(h, ID_R + k), TBM_SETPOS, 1, v[k]);
        char t[8];
        snprintf(t, sizeof t, "%u", v[k]);
        SetDlgItemText(h, ID_RE + k, t);
    }
    d->syncing = false;
}

static void expand(HWND h)
{
    struct cdlg *d = cd(h);
    if (d->full) return;
    d->full = true;
    RECT r;
    GetWindowRect(h, &r);
    MoveWindow(h, r.left, r.top, FULL_W + 10, r.bottom - r.top, true);
    for (int id = ID_ADD; id <= ID_RE + 2; id++) {
        HWND c = GetDlgItem(h, id);
        if (c) ShowWindow(c, SW_SHOW);
    }
    HWND st;
    for (int k = 0; k < 4; k++)
        if ((st = GetDlgItem(h, 230 + k))) ShowWindow(st, SW_SHOW);
    EnableWindow(GetDlgItem(h, ID_DEFINE), false);
    sync_sliders(h);
}

static void pick(HWND h, int i)
{
    struct cdlg *d = cd(h);
    d->sel = i;
    d->rgb = color_of(d, i);
    if (d->full) sync_sliders(h);
    InvalidateRect(h, NULL);
}

static LRESULT dlg_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct cdlg *d = cd(h);
    switch (msg) {
    case WM_CREATE: {
        d = (struct cdlg *)lp;
        SetWindowLong(h, GWL_USERDATA, (int32_t)lp);
        CreateWindow(WC_STATIC, "&Basic colours:", WS_CHILD | WS_VISIBLE, 10, 8, 160, 16, h, 0, NULL);
        CreateWindow(WC_STATIC, "&Custom colours:", WS_CHILD | WS_VISIBLE, 10, CUSTOM_Y - 20, 160, 16, h, 0, NULL);
        int by = DLG_H - 40;
        CreateWindow(WC_BUTTON, "&Define Custom Colours >>", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                     ((d->cc->flags & CC_PREVENTFULLOPEN) ? WS_DISABLED : 0),
                     10, by - 30, SMALL_W - 20, 22, h, ID_DEFINE, NULL);
        CreateWindow(WC_BUTTON, "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                     10, by - 4, 84, 30, h, IDOK, NULL);
        CreateWindow(WC_BUTTON, "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 104, by, 76, 22, h, IDCANCEL, NULL);
        static const char *const names[3] = { "&Red:", "&Green:", "Bl&ue:" };
        for (int k = 0; k < 3; k++) {             /* the custom part, hidden until asked for */
            int y = 104 + k * 34;
            CreateWindow(WC_STATIC, names[k], WS_CHILD, SMALL_W + 4, y + 4, 52, 16, h, 230 + k, NULL);
            HWND tb = CreateWindow(WC_TRACKBAR, "", WS_CHILD | WS_TABSTOP, SMALL_W + 54, y, 92, 26, h, ID_R + k, NULL);
            SendMessage(tb, TBM_SETRANGE, 0, MAKELPARAM(0, 255));
            CreateWindow(WC_EDIT, "0", WS_CHILD | WS_TABSTOP | ES_NUMBER, SMALL_W + 148, y - 1, 46, 28, h, ID_RE + k, NULL);
        }
        CreateWindow(WC_STATIC, "Colour", WS_CHILD, SMALL_W + 4, 8, 80, 16, h, 233, NULL);
        CreateWindow(WC_BUTTON, "&Add to Custom Colours", WS_CHILD | WS_TABSTOP,
                     SMALL_W + 4, 104 + 3 * 34 + 4, 190, 22, h, ID_ADD, NULL);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        for (int i = 0; i < 32; i++) {
            RECT r = swatch(i), in = { r.left + 2, r.top + 2, r.right - 2, r.bottom - 2 };
            DrawBevel(dc, &r, false);
            fill(dc, in, color_of(d, i), i < 16 ? i : -1);
            if (i == d->sel) {
                RECT o = { r.left - 3, r.top - 3, r.right + 3, r.bottom + 3 },
                     o2 = { r.left - 2, r.top - 2, r.right + 2, r.bottom + 2 };
                FrameRect(dc, &o, C_BLACK);
                FrameRect(dc, &o2, C_BLACK);
            }
        }
        if (d->full) {                           /* the preview */
            RECT p = { SMALL_W + 4, 28, FULL_W - 10, 92 }, in = { p.left + 2, p.top + 2, p.right - 2, p.bottom - 2 };
            DrawBevel(dc, &p, false);
            fill(dc, in, d->rgb, -1);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        for (int i = 0; i < 32; i++) {
            RECT r = swatch(i);
            if (PtInRect(&r, x, y)) {
                pick(h, i);
                if (msg == WM_LBUTTONDBLCLK) { d->ok = d->done = true; }
                return 0;
            }
        }
        return 0;
    }
    case WM_HSCROLL:
        if (!d->syncing) {
            int k = GetDlgCtrlID((HWND)lp) - ID_R;
            if (k >= 0 && k < 3) {
                uint8_t v[3] = { GetRValue(d->rgb), GetGValue(d->rgb), GetBValue(d->rgb) };
                v[k] = (uint8_t)SendMessage((HWND)lp, TBM_GETPOS, 0, 0);
                d->rgb = RGB(v[0], v[1], v[2]);
                d->sel = -1;
                sync_sliders(h);
                InvalidateRect(h, NULL);
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:     d->ok = d->done = true; break;
        case IDCANCEL: d->done = true; break;
        case ID_DEFINE: expand(h); break;
        case ID_ADD:
            if (d->cc->custom) {                 /* the selected custom slot, or the next */
                int slot = d->sel >= 16 ? d->sel - 16 : 0;
                if (d->sel < 16)
                    for (slot = 0; slot < 15 && d->cc->custom[slot] != RGB(255, 255, 255); slot++)
                        ;
                d->cc->custom[slot] = d->rgb;
                d->sel = 16 + slot;
                InvalidateRect(h, NULL);
            }
            break;
        default:
            if (HIWORD(wp) == EN_CHANGE && !d->syncing && LOWORD(wp) >= ID_RE && LOWORD(wp) < ID_RE + 3) {
                char t[8];
                GetDlgItemText(h, LOWORD(wp), t, sizeof t);
                int v = atoi(t);
                if (v > 255) v = 255;
                uint8_t c[3] = { GetRValue(d->rgb), GetGValue(d->rgb), GetBValue(d->rgb) };
                c[LOWORD(wp) - ID_RE] = (uint8_t)v;
                d->rgb = RGB(c[0], c[1], c[2]);
                d->sel = -1;
                d->syncing = true;
                SendMessage(GetDlgItem(h, ID_R + LOWORD(wp) - ID_RE), TBM_SETPOS, 1, v);
                d->syncing = false;
                InvalidateRect(h, NULL);
            }
        }
        return 0;
    case WM_CLOSE:
        d->done = true;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

bool ChooseColor(CHOOSECOLOR *cc)
{
    static bool registered;
    if (!registered) {
        WNDCLASS wc = { .name = "#COLORDLG", .proc = dlg_proc, .background = C_FACE, .style = CS_DBLCLKS };
        RegisterClass(&wc);
        registered = true;
    }
    if (!cc)
        return false;
    struct cdlg d = { .cc = cc, .sel = -1 };
    d.rgb = (cc->flags & CC_RGBINIT) ? cc->rgb : GetThemeColor(cc->index & 15);
    if (!(cc->flags & CC_RGBINIT)) d.sel = cc->index & 15;
    else
        for (int i = 0; i < 32; i++)
            if (color_of(&d, i) == d.rgb) { d.sel = i; break; }

    HWND owner = cc->owner;
    while (owner && GetParent(owner))
        owner = GetParent(owner);
    RECT o, work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    o = work;
    if (owner) GetWindowRect(owner, &o);
    int w = SMALL_W + 10, hh = DLG_H + 25 + 5;
    int x = o.left + (o.right - o.left - FULL_W) / 2, y = o.top + (o.bottom - o.top - hh) / 3;
    if (x + FULL_W + 10 > work.right) x = work.right - FULL_W - 10;
    if (y + hh > work.bottom) y = work.bottom - hh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    bool reenable = owner && IsWindowEnabled(owner);
    if (owner) EnableWindow(owner, false);
    HWND h = CreateWindow("#COLORDLG", "Colour", WS_DIALOG | WS_VISIBLE, x, y, w, hh, owner, 0, &d);
    if (h) {
        if (cc->flags & CC_FULLOPEN) expand(h);
        SetFocus(GetDlgItem(h, IDOK));
        MSG m;
        while (!d.done && IsWindow(h)) {
            if (!GetMessage(&m)) {
                PostQuitMessage((int)m.wParam);
                break;
            }
            if (!IsDialogMessage(h, &m)) {
                TranslateMessage(&m);
                DispatchMessage(&m);
            }
        }
        if (IsWindow(h)) {
            if (reenable) EnableWindow(owner, true);
            DestroyWindow(h);
        }
    }
    if (reenable && IsWindow(owner)) {
        EnableWindow(owner, true);
        SetActiveWindow(owner);
    }
    if (d.ok) {
        cc->rgb = d.rgb;
        cc->index = d.sel >= 0 && d.sel < 16 ? d.sel : nearest(d.rgb);
        DebugLog("colour %06X (%d)", (unsigned)d.rgb, cc->index);
    }
    return d.ok;
}
