/*
 * tab.c -- the TABCONTROL class: Platinum tabs along the top of a raised
 * page. Tabs have rounded top corners; the others are darker and lower,
 * the selected one is the page's colour, a little wider and taller, and
 * joins the page. The page itself is only a panel: the program puts its
 * controls over it and shows the right set when the selection changes.
 *
 * Tabs that don't fit wrap onto more rows with TCS_MULTILINE (the selected
 * tab's row moving next to the page, as in Windows); otherwise two arrow
 * buttons at the right scroll them. Tabs can carry an icon from an image
 * list (TCM_SETIMAGELIST, TCIF_IMAGE).
 *
 * A click, Left/Right (when focused) or Ctrl+Tab in a dialog changes the
 * selection, after asking the parent: TCN_SELCHANGING (nonzero keeps the
 * tab), then TCN_SELCHANGE -- each sent both as WM_NOTIFY and, as before
 * WM_NOTIFY existed, as WM_COMMAND with the code in HIWORD(wParam).
 * The tabs live in the owning process's heap (w->ctl). w->value =
 * selection (-1 none).
 */
#include "uipriv.h"

#define TAB_H    22             /* a row of tabs */
#define TAB_TEXT 32
#define MARGIN   4              /* page edge to the display area */
#define ARROW    16             /* the scroll buttons */

struct tab { char text[TAB_TEXT]; LPARAM param; int image; int x, row; };
struct tabs {
    struct tab *t;
    int n, cap;
    int rows, first;            /* rows of tabs; single-line: the first shown */
    bool arrows;                /* single-line: scroll buttons shown */
    int pressed;                /* an arrow held: 1 left, 2 right */
    HIMAGELIST il;
};

static struct tabs *data_of(struct wnd *w) { return (struct tabs *)w->ctl; }
static bool multiline(struct wnd *w) { return (w->style & TCS_MULTILINE) != 0; }

static int tab_w(struct tabs *d, const struct tab *t)
{
    int w = GUI_W(t->text) + 20;
    if (d->il && t->image >= 0) {
        int cx, cy;
        ImageList_GetIconSize(d->il, &cx, &cy);
        w += cx + 4;
    }
    return w < 40 ? 40 : w;
}

/* Where each tab goes: x and row. */
static void layout(struct wnd *w)
{
    struct tabs *d = data_of(w);
    int x = 4, row = 0, right = w->w - 4, total = 4;
    for (int i = 0; i < d->n; i++)
        total += tab_w(d, &d->t[i]);
    d->arrows = !multiline(w) && total > right && d->n > 1;
    if (!d->arrows) d->first = 0;
    if (d->arrows) right = w->w - 2 * ARROW - 6;
    d->first = clampi(d->first, 0, d->n ? d->n - 1 : 0);
    for (int i = 0; i < d->n; i++) {
        int tw = tab_w(d, &d->t[i]);
        if (multiline(w) && x + tw > right && x > 4) {
            x = 4;
            row++;
        }
        if (!multiline(w) && i < d->first) {    /* scrolled off to the left */
            d->t[i].x = -10000;
            d->t[i].row = 0;
            continue;
        }
        d->t[i].x = x;
        d->t[i].row = row;
        x += tw;
    }
    d->rows = row + 1;
}

static int strip_h(struct wnd *w) { return data_of(w)->rows * TAB_H; }

/* Tab i's rectangle (client coordinates), the selected one enlarged. */
static RECT tab_rect(struct wnd *w, int i)
{
    struct tabs *d = data_of(w);
    int sel_row = w->value >= 0 && w->value < d->n ? d->t[w->value].row : d->rows - 1;
    int dr = (d->t[i].row + (d->rows - 1 - sel_row)) % d->rows;   /* the selected row at the bottom */
    int y = dr * TAB_H;
    RECT r = { d->t[i].x, y + 3, d->t[i].x + tab_w(d, &d->t[i]), y + TAB_H };
    if (i == w->value) {
        r.left -= 2; r.right += 2;
        r.top = y; r.bottom = y + TAB_H + 1;
    }
    return r;
}

static RECT arrow_rect(struct wnd *w, int which)    /* 1 left, 2 right */
{
    int x = w->w - 2 * ARROW - 2 + (which == 2 ? ARROW : 0);
    return (RECT){ x, 3, x + ARROW, 3 + ARROW };
}

static int hit(struct wnd *w, int x, int y)
{
    struct tabs *d = data_of(w);
    if (d->arrows) {
        RECT a = arrow_rect(w, 1), b = arrow_rect(w, 2);
        if (rect_contains(&a, x, y) || rect_contains(&b, x, y))
            return -1;
    }
    if (w->value >= 0) {
        RECT r = tab_rect(w, w->value);
        if (rect_contains(&r, x, y))
            return w->value;
    }
    for (int i = 0; i < d->n; i++) {
        RECT r = tab_rect(w, i);
        if (rect_contains(&r, x, y))
            return i;
    }
    return -1;
}

static void paint_tab(HDC dc, struct wnd *w, int i)
{
    struct tabs *d = data_of(w);
    RECT r = tab_rect(w, i);
    if (r.right < 0)
        return;
    bool sel = i == w->value, dis = !w->enabled;
    int x = r.left, y = r.top, tw = r.right - r.left, base = r.bottom - (sel ? 1 : 0), th = base - y;
    RECT in = { x + 1, y + 1, r.right - 1, base + (sel ? 2 : 0) };
    FillRect(dc, &in, sel ? C_FACE : C_GRAY5);
    uint8_t edge = dis ? C_GRAY4 : C_GRAY1;
    DrawLine(dc, x + 2, y, tw - 4, false, edge);
    DrawLine(dc, x, y + 2, th - 2 + (sel ? 1 : 0), true, edge);
    DrawLine(dc, r.right - 1, y + 2, th - 2 + (sel ? 1 : 0), true, edge);
    SetPixel(dc, x + 1, y + 1, edge);
    SetPixel(dc, r.right - 2, y + 1, edge);
    DrawLine(dc, x + 2, y + 1, tw - 4, false, C_WHITE);                     /* bevel */
    DrawLine(dc, x + 1, y + 2, th - 2 + (sel ? 2 : 0), true, C_WHITE);
    DrawLine(dc, r.right - 2, y + 2, th - 2, true, sel ? C_GRAY4 : C_GRAY3);
    int cx = 0, cy = 0;
    bool icon = d->il && d->t[i].image >= 0;
    if (icon) ImageList_GetIconSize(d->il, &cx, &cy);
    int content = GUI_W(d->t[i].text) + (icon ? cx + 4 : 0);
    int tx = x + (tw - content) / 2, ty = y + (th - 16) / 2 + 1;
    if (icon) {
        ImageList_Draw(d->il, d->t[i].image, dc, tx, y + (th - cy) / 2 + 1, dis ? ILD_DISABLED : ILD_NORMAL);
        tx += cx + 4;
    }
    TextOut(dc, tx, ty, d->t[i].text, dis ? C_GRAY3 : C_BLACK, -1);
    if (sel && ui_has_focus(w)) {
        RECT f = { tx - 3, ty, tx + GUI_W(d->t[i].text) + 3, ty + 16 };
        FrameRect(dc, &f, C_ACCENT);
    }
}

static const uint16_t g_left[7]  = { 0x1000, 0x3000, 0x7000, 0xF000, 0x7000, 0x3000, 0x1000 };
static const uint16_t g_right[7] = { 0x8000, 0xC000, 0xE000, 0xF000, 0xE000, 0xC000, 0x8000 };

static void paint(HDC dc, struct wnd *w)
{
    struct tabs *d = data_of(w);
    layout(w);
    int sh = strip_h(w);
    RECT page = { 0, sh, w->w, w->h }, in = { 1, sh + 1, w->w - 1, w->h - 1 };
    FrameRect(dc, &page, w->enabled ? C_GRAY1 : C_GRAY4);
    FillRect(dc, &in, C_FACE);
    DrawBevel(dc, &in, true);
    RECT strip = { 0, 0, d->arrows ? w->w - 2 * ARROW - 4 : w->w, sh + 2 }, saved;
    dc_clip(dc, strip, &saved);
    for (int i = 0; i < d->n; i++)
        if (i != w->value)
            paint_tab(dc, w, i);
    if (w->value >= 0)
        paint_tab(dc, w, w->value);                 /* last: it overlaps the others */
    dc_unclip(dc, &saved);
    if (d->arrows)
        for (int k = 1; k <= 2; k++) {
            RECT a = arrow_rect(w, k), b = { a.left + 1, a.top + 1, a.right - 1, a.bottom - 1 };
            bool down = d->pressed == k;
            bool can = k == 1 ? d->first > 0 : tab_rect(w, d->n - 1).right > w->w - 2 * ARROW - 6;
            FrameRect(dc, &a, C_GRAY1);
            FillRect(dc, &b, down ? C_GRAY4 : C_FACE);
            if (!down) DrawBevel(dc, &b, true);
            DrawBitmap(dc, a.left + 6, a.top + 4, k == 1 ? g_left : g_right, 7, can ? C_BLACK : C_GRAY4);
        }
}

static LRESULT tell(struct wnd *w, int code)        /* WM_NOTIFY, then WM_COMMAND */
{
    if (!w->parent)
        return 0;
    NMHDR nm;
    LRESULT r = wnd_mine(w->parent) ? ui_notify(w, &nm, code) : 0;
    if (r || !wnd_from(w))
        return r;
    return SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, code), (LPARAM)w);
}

static void ensure_visible(struct wnd *w, int i)
{
    struct tabs *d = data_of(w);
    if (multiline(w) || i < 0)
        return;
    layout(w);
    if (!d->arrows) return;
    if (i < d->first)
        d->first = i;
    else
        while (d->first < i && tab_rect(w, i).right > w->w - 2 * ARROW - 6) {
            d->first++;
            layout(w);
        }
}

/* A change by the user: asks the parent first, tells it after. */
static void choose(struct wnd *w, int i)
{
    struct tabs *d = data_of(w);
    if (i < 0 || i >= d->n || i == w->value)
        return;
    if (tell(w, TCN_SELCHANGING))
        return;
    if (!wnd_from(w))
        return;
    w->value = i;
    ensure_visible(w, i);
    InvalidateRect(w, NULL);
    tell(w, TCN_SELCHANGE);
}

static void set_item(struct tab *t, const TCITEM *it)
{
    if (it->mask & TCIF_TEXT)
        strncpy0(t->text, it->pszText ? it->pszText : "", TAB_TEXT);
    if (it->mask & TCIF_PARAM)
        t->param = it->lParam;
    if (it->mask & TCIF_IMAGE)
        t->image = it->iImage;
}

static int insert(struct wnd *w, int at, const TCITEM *it)
{
    struct tabs *d = data_of(w);
    if (!it)
        return -1;
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 8;
        struct tab *t = d->t ? LocalReAlloc(d->t, (uint32_t)cap * sizeof *t, 0)
                             : LocalAlloc(LMEM_FIXED, (uint32_t)cap * sizeof *t);
        if (!t)
            return -1;
        d->t = t;
        d->cap = cap;
    }
    if (at < 0 || at > d->n)
        at = d->n;
    memmove(&d->t[at + 1], &d->t[at], (size_t)(d->n - at) * sizeof *d->t);
    memset(&d->t[at], 0, sizeof *d->t);
    d->t[at].image = -1;
    set_item(&d->t[at], it);
    d->n++;
    if (w->value < 0)
        w->value = 0;                               /* the first tab starts selected */
    else if (at <= w->value)
        w->value++;
    layout(w);
    InvalidateRect(w, NULL);
    return at;
}

LRESULT tab_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct tabs *d = data_of(w);
    if (!d && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    int i = (int)wp;
    switch (msg) {
    case WM_CREATE:
        w->value = -1;
        if (!(w->ctl = d = LocalAlloc(LMEM_ZEROINIT, sizeof *d)))
            return -1;
        d->rows = 1;
        return 0;
    case WM_DESTROY:
        LocalFree(d->t);
        LocalFree(d);
        w->ctl = NULL;
        return 0;
    case WM_SIZE:
        layout(w);
        InvalidateRect(h, NULL);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SelectFont(dc, FONT_GUI);
        paint(dc, w);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (d->arrows)
            for (int k = 1; k <= 2; k++) {
                RECT a = arrow_rect(w, k);
                if (rect_contains(&a, x, y)) {
                    d->pressed = k;
                    if (k == 1 && d->first > 0) d->first--;
                    else if (k == 2 && tab_rect(w, d->n - 1).right > w->w - 2 * ARROW - 6) d->first++;
                    SetCapture(h);
                    InvalidateRect(h, NULL);
                    return 0;
                }
            }
        int t = hit(w, x, y);
        if (t >= 0) {
            SetFocus(h);
            choose(w, t);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (d->pressed) {
            d->pressed = 0;
            ReleaseCapture();
            InvalidateRect(h, NULL);
        }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:  choose(w, w->value - 1); break;
        case VK_RIGHT: choose(w, w->value + 1); break;
        case VK_HOME:  choose(w, 0); break;
        case VK_END:   choose(w, d->n - 1); break;
        }
        return 0;
    case TCM_INSERTITEM:
        return insert(w, i, (const TCITEM *)lp);
    case TCM_DELETEITEM:
        if (i < 0 || i >= d->n)
            return 0;
        memmove(&d->t[i], &d->t[i + 1], (size_t)(d->n - i - 1) * sizeof *d->t);
        d->n--;
        if (w->value == i)
            w->value = -1;                          /* as in Windows: no selection */
        else if (w->value > i)
            w->value--;
        layout(w);
        InvalidateRect(h, NULL);
        return 1;
    case TCM_DELETEALLITEMS:
        d->n = 0;
        d->first = 0;
        w->value = -1;
        layout(w);
        InvalidateRect(h, NULL);
        return 1;
    case TCM_GETITEMCOUNT:
        return d->n;
    case TCM_GETROWCOUNT:
        layout(w);
        return d->rows;
    case TCM_SETIMAGELIST: {
        HIMAGELIST old = d->il;
        d->il = (HIMAGELIST)lp;
        layout(w);
        InvalidateRect(h, NULL);
        return (LRESULT)old;
    }
    case TCM_GETIMAGELIST:
        return (LRESULT)d->il;
    case TCM_GETITEM: {
        TCITEM *it = (TCITEM *)lp;
        if (i < 0 || i >= d->n || !it)
            return 0;
        if ((it->mask & TCIF_TEXT) && it->pszText && it->cchTextMax > 0)
            strncpy0(it->pszText, d->t[i].text, (size_t)it->cchTextMax);
        if (it->mask & TCIF_PARAM)
            it->lParam = d->t[i].param;
        if (it->mask & TCIF_IMAGE)
            it->iImage = d->t[i].image;
        return 1;
    }
    case TCM_SETITEM:
        if (i < 0 || i >= d->n || !lp)
            return 0;
        set_item(&d->t[i], (const TCITEM *)lp);
        layout(w);
        InvalidateRect(h, NULL);
        return 1;
    case TCM_GETITEMRECT:
        if (i < 0 || i >= d->n || !lp)
            return 0;
        layout(w);
        *(RECT *)lp = tab_rect(w, i);
        return 1;
    case TCM_GETCURSEL:
    case TCM_GETCURFOCUS:
        return w->value;
    case TCM_SETCURSEL: {
        int old = w->value;
        if (i < 0 || i >= d->n)
            return -1;
        w->value = i;
        ensure_visible(w, i);
        InvalidateRect(h, NULL);
        return old;
    }
    case TCM_SETCURFOCUS:
        choose(w, i);
        return 0;
    case TCM_ADJUSTRECT: {
        RECT *r = (RECT *)lp;
        int s = wp ? -1 : 1;                        /* 1: window -> display area */
        if (!r)
            return 0;
        layout(w);
        r->left += s * MARGIN;
        r->right -= s * MARGIN;
        r->top += s * (strip_h(w) + MARGIN);
        r->bottom -= s * MARGIN;
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}
