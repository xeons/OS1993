/*
 * button.c -- the BUTTON class: push buttons, check boxes, radio buttons
 * and group boxes, drawn Platinum-style.
 *
 * Push buttons are rounded and bevelled; the default one (BS_DEFPUSHBUTTON)
 * wears a heavy ring in the outer 4 pixels of its rectangle. A click
 * (mouse, Space, or BM_CLICK) sends WM_COMMAND BN_CLICKED to the parent;
 * the AUTO styles check themselves first, and an auto radio button clears
 * the others in its group (siblings from a WS_GROUP to the next).
 */
#include "uipriv.h"

#define DEF_RING 4

static const uint16_t g_check[9] = { 0x0080, 0x0180, 0x0300, 0x8600, 0xCC00, 0x7800, 0x3000, 0, 0 };
static const uint16_t g_ring[13] = { 0x0F80, 0x3060, 0x4010, 0x4010, 0x8008, 0x8008, 0x8008,
                                     0x8008, 0x8008, 0x4010, 0x4010, 0x3060, 0x0F80 };
static const uint16_t g_disc[13] = { 0, 0x0F80, 0x3FE0, 0x3FE0, 0x7FF0, 0x7FF0, 0x7FF0,
                                     0x7FF0, 0x7FF0, 0x3FE0, 0x3FE0, 0x0F80, 0 };
static const uint16_t g_dot[13]  = { 0, 0, 0, 0, 0x0700, 0x0F80, 0x0F80, 0x0F80, 0x0700, 0, 0, 0, 0 };

static int type_of(struct wnd *w) { return (int)(w->style & BS_TYPEMASK); }

static bool is_radio(struct wnd *w)
{
    return type_of(w) == BS_RADIOBUTTON || type_of(w) == BS_AUTORADIOBUTTON;
}

static bool is_check(struct wnd *w)
{
    int t = type_of(w);
    return t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_3STATE || t == BS_AUTO3STATE;
}

/* The label, in the window-text font with its mnemonic underlined; -> its width. */
static int label(HDC dc, struct wnd *w, int x, int y, uint8_t fg)
{
    SelectFont(dc, FONT_GUI);
    SetTextColor(dc, fg);
    RECT r = { x, y, w->w, y + 16 }, m = r;
    DrawText(dc, w->text, -1, &m, DT_SINGLELINE | DT_CALCRECT);
    DrawText(dc, w->text, -1, &r, DT_SINGLELINE);
    return m.right - m.left;
}

static int label_width(struct wnd *w)
{
    char t[TEXT_MAX];
    int k = 0;
    for (const char *s = w->text; *s && k < TEXT_MAX - 1; s++) {
        if (*s == '&' && s[1] != '&') continue;
        if (*s == '&') s++;
        t[k++] = *s;
    }
    t[k] = '\0';
    return TextWidthFont(FONT_GUI, t, k);
}

bool ui_has_focus(struct wnd *w)
{
    return GetFocus() == w && wnd_top(w) == wnd_active();
}

/* ---- painting ---------------------------------------------------------- */

static void paint_push(HDC dc, struct wnd *w)
{
    bool dis = !w->enabled, down = w->pressed;
    int x = 0, y = 0, cw = w->w, ch = w->h;
    if (type_of(w) == BS_DEFPUSHBUTTON) {
        for (int i = 0; i < 3; i++) {
            RECT r = { i, i, cw - i, ch - i };
            DrawRoundRect(dc, &r, C_GRAY1);
        }
        x = y = DEF_RING;
        cw -= 2 * DEF_RING;
        ch -= 2 * DEF_RING;
    }
    uint8_t face = down ? C_GRAY3 : C_FACE;
    RECT a = { x + 2, y + 1, x + cw - 2, y + ch - 1 }, b = { x + 1, y + 2, x + cw - 1, y + ch - 2 };
    FillRect(dc, &a, face);
    FillRect(dc, &b, face);
    RECT o = { x, y, x + cw, y + ch };
    DrawRoundRect(dc, &o, dis ? C_GRAY4 : C_BLACK);
    if (!down) {
        DrawLine(dc, x + 2, y + 1, cw - 4, false, C_WHITE);
        DrawLine(dc, x + 1, y + 2, ch - 4, true, C_WHITE);
        DrawLine(dc, x + 2, y + ch - 2, cw - 4, false, C_GRAY4);
        DrawLine(dc, x + cw - 2, y + 2, ch - 4, true, C_GRAY4);
    }
    int tw = label_width(w);
    label(dc, w, x + (cw - tw) / 2 + (down ? 1 : 0), y + (ch - 16) / 2 + (down ? 1 : 0),
          dis ? C_GRAY3 : down ? C_WHITE : C_BLACK);
    if (ui_has_focus(w)) {
        RECT f = { x + 3, y + 3, x + cw - 3, y + ch - 3 };
        FrameRect(dc, &f, C_ACCENT);
    }
}

static void paint_check(HDC dc, struct wnd *w)
{
    bool dis = !w->enabled, down = w->pressed;
    int by = (w->h - CHECK) / 2;
    if (is_check(w)) {
        RECT r = { 0, by, CHECK, by + CHECK };
        FrameRect(dc, &r, dis ? C_GRAY4 : C_BLACK);
        RECT in = { 1, by + 1, CHECK - 1, by + CHECK - 1 };
        FillRect(dc, &in, down ? C_GRAY5 : C_WHITE);
        DrawLine(dc, 1, by + 1, CHECK - 2, false, C_GRAY5);
        DrawLine(dc, 1, by + 1, CHECK - 2, true, C_GRAY5);
        if (w->value == BST_INDETERMINATE) {        /* a grey tick on a grey square */
            FillRect(dc, &in, C_GRAY5);
            DrawBitmap(dc, 2, by + 2, g_check, 9, C_GRAY3);
        } else if (w->value)
            DrawBitmap(dc, 2, by + 2, g_check, 9, dis ? C_GRAY4 : C_BLACK);
    } else {
        DrawBitmap(dc, 0, by, g_disc, 13, down ? C_GRAY5 : C_WHITE);
        DrawBitmap(dc, 0, by, g_ring, 13, dis ? C_GRAY4 : C_BLACK);
        if (w->value)
            DrawBitmap(dc, 0, by, g_dot, 13, dis ? C_GRAY4 : C_BLACK);
    }
    int tx = CHECK + 6, ty = (w->h - 16) / 2;
    uint8_t fg = dis ? C_GRAY3 : C_BLACK, bg = C_FACE;
    ctl_color(w, dc, WM_CTLCOLORBTN, &fg, &bg);
    if (bg != C_FACE) {
        RECT t = { CHECK + 1, 0, w->w, w->h };
        FillRect(dc, &t, bg);
    }
    int tw = label(dc, w, tx, ty, fg);
    if (ui_has_focus(w)) {
        RECT f = { tx - 2, ty, tx + tw + 2, ty + 16 };
        FrameRect(dc, &f, C_ACCENT);
    }
}

static void paint_group(HDC dc, struct wnd *w)
{
    RECT a = { 0, 8, w->w - 1, w->h - 1 }, b = { 1, 9, w->w, w->h };
    FrameRect(dc, &a, C_GRAY4);
    FrameRect(dc, &b, C_WHITE);
    uint8_t bg = w->parent ? w->parent->cls->background : C_FACE, fg = w->enabled ? C_BLACK : C_GRAY3;
    ctl_color(w, dc, WM_CTLCOLORBTN, &fg, &bg);
    if (!w->text[0])
        return;
    RECT t = { 8, 0, 16 + label_width(w), 16 };
    FillRect(dc, &t, bg);
    label(dc, w, 12, 0, fg);
}

/* ---- behaviour ----------------------------------------------------------- */

static void check_radio(struct wnd *w)
{
    struct wnd *p = w->parent, *start = NULL;
    if (!p)
        return;
    for (struct wnd *c = p->first; c; c = c->next) {     /* the group's start */
        if (c->style & WS_GROUP) start = c;
        if (c == w) break;
    }
    if (!start)
        start = p->first;
    for (struct wnd *c = start; c; c = c->next) {
        if (c != start && (c->style & WS_GROUP))
            break;
        if (c != w && c->cls->proc == button_proc && type_of(c) == BS_AUTORADIOBUTTON && c->value) {
            c->value = 0;
            InvalidateRect(c, NULL);
        }
    }
    w->value = 1;
    InvalidateRect(w, NULL);
}

static void click(struct wnd *w)
{
    if (type_of(w) == BS_AUTOCHECKBOX) {
        w->value = !w->value;
        InvalidateRect(w, NULL);
    } else if (type_of(w) == BS_AUTO3STATE) {       /* unchecked, checked, indeterminate */
        w->value = (w->value + 1) % 3;
        InvalidateRect(w, NULL);
    } else if (type_of(w) == BS_AUTORADIOBUTTON) {
        check_radio(w);
    }
    if (w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, BN_CLICKED), (LPARAM)w);
}

LRESULT button_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    bool group = type_of(w) == BS_GROUPBOX;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (type_of(w) == BS_OWNERDRAW) {
            RECT r = { 0, 0, w->w, w->h };
            unsigned st = (w->pressed ? ODS_SELECTED : 0) | (w->enabled ? 0 : ODS_DISABLED) |
                          (ui_has_focus(w) ? ODS_FOCUS : 0);
            if (!owner_draw(w, dc, ODT_BUTTON, 0, ODA_DRAWENTIRE, st, r, 0))
                paint_push(dc, w);
        } else if (group)          paint_group(dc, w);
        else if ((is_check(w) || is_radio(w)) && !(w->style & BS_PUSHLIKE)) paint_check(dc, w);
        else {
            bool p = w->pressed;
            if (w->style & BS_PUSHLIKE) w->pressed = p || w->value;   /* stays in while checked */
            paint_push(dc, w);
            w->pressed = p;
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN:
        if (group)
            return 0;
        SetFocus(h);
        SetCapture(h);
        w->tracking = w->pressed = true;
        InvalidateRect(h, NULL);
        return 0;
    case WM_MOUSEMOVE:
        if (w->tracking) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            bool in = x >= 0 && y >= 0 && x < w->w && y < w->h;
            if (in != w->pressed) {
                w->pressed = in;
                InvalidateRect(h, NULL);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->tracking) {
            bool fire = w->pressed;
            ReleaseCapture();
            w->tracking = w->pressed = false;
            InvalidateRect(h, NULL);
            if (fire)
                click(w);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && !group) {
            click(w);
            return 0;
        }
        break;
    case BM_CLICK:
        if (!group && w->enabled)
            click(w);
        return 0;
    case BM_GETCHECK:
        return w->value;
    case BM_GETSTATE:
        return w->value | (w->pressed ? BST_PUSHED : 0) | (ui_has_focus(w) ? BST_FOCUS : 0);
    case BM_SETSTATE:
        w->pressed = wp != 0;
        InvalidateRect(h, NULL);
        return 0;
    case WM_SETTEXT: {
        LRESULT r = DefWindowProc(h, msg, wp, lp);
        InvalidateRect(h, NULL);
        return r;
    }
    case BM_SETCHECK:
        if (is_radio(w) && wp == BST_CHECKED && type_of(w) == BS_AUTORADIOBUTTON)
            check_radio(w);
        else
            w->value = (int)wp;
        InvalidateRect(h, NULL);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}
