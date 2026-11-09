/*
 * tab.c -- the TABCONTROL class: Platinum tabs along the top of a raised
 * page. Tabs have rounded top corners; the others are darker and lower,
 * the selected one is the page's colour, a little wider and taller, and
 * joins the page. The page itself is only a panel: the program puts its
 * controls over it and shows the right set when the selection changes.
 *
 * A click or Left/Right (when focused) changes the selection, after
 * asking the parent: WM_COMMAND TCN_SELCHANGING (nonzero keeps the tab),
 * then TCN_SELCHANGE -- WM_COMMAND because OS1993 has no WM_NOTIFY.
 * The tabs live in the owning process's heap (w->ctl). w->value =
 * selection (-1 none).
 */
#include "uipriv.h"

#define TAB_H    22             /* the strip of tabs */
#define TAB_TEXT 32
#define MARGIN   4              /* page edge to the display area */

struct tab { char text[TAB_TEXT]; LPARAM param; };
struct tabs { struct tab *t; int n, cap; };

static struct tabs *data_of(struct wnd *w) { return (struct tabs *)w->ctl; }

static int tab_w(const struct tab *t)
{
    int w = TextWidth(t->text) + 20;
    return w < 40 ? 40 : w;
}

/* Tab i's rectangle (client coordinates), the selected one enlarged. */
static RECT tab_rect(struct wnd *w, int i)
{
    struct tabs *d = data_of(w);
    int x = 4;
    for (int k = 0; k < i; k++)
        x += tab_w(&d->t[k]);
    RECT r = { x, 3, x + tab_w(&d->t[i]), TAB_H };
    if (i == w->value) {
        r.left -= 2; r.right += 2;
        r.top = 0; r.bottom = TAB_H + 1;
    }
    return r;
}

static int hit(struct wnd *w, int x, int y)
{
    struct tabs *d = data_of(w);
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
    bool sel = i == w->value, dis = !w->enabled;
    int x = r.left, y = r.top, tw = r.right - r.left, th = TAB_H - y;
    RECT in = { x + 1, y + 1, r.right - 1, TAB_H + (sel ? 2 : 0) };
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
    int tx = x + (tw - TextWidth(d->t[i].text)) / 2, ty = y + (th - 16) / 2 + 1;
    TextOut(dc, tx, ty, d->t[i].text, dis ? C_GRAY3 : C_BLACK, -1);
    if (sel && ui_has_focus(w)) {
        RECT f = { tx - 3, ty, tx + TextWidth(d->t[i].text) + 3, ty + 16 };
        FrameRect(dc, &f, C_ACCENT);
    }
}

static void paint(HDC dc, struct wnd *w)
{
    struct tabs *d = data_of(w);
    RECT page = { 0, TAB_H, w->w, w->h }, in = { 1, TAB_H + 1, w->w - 1, w->h - 1 };
    FrameRect(dc, &page, w->enabled ? C_GRAY1 : C_GRAY4);
    FillRect(dc, &in, C_FACE);
    DrawBevel(dc, &in, true);
    for (int i = 0; i < d->n; i++)
        if (i != w->value)
            paint_tab(dc, w, i);
    if (w->value >= 0)
        paint_tab(dc, w, w->value);                 /* last: it overlaps the others */
}

/* A change by the user: asks the parent first, tells it after. */
static void choose(struct wnd *w, int i)
{
    struct tabs *d = data_of(w);
    if (i < 0 || i >= d->n || i == w->value)
        return;
    if (w->parent && SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, TCN_SELCHANGING), (LPARAM)w))
        return;
    if (!wnd_from(w))
        return;
    w->value = i;
    InvalidateRect(w, NULL);
    if (w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, TCN_SELCHANGE), (LPARAM)w);
}

static void set_item(struct tab *t, const TCITEM *it)
{
    if (it->mask & TCIF_TEXT)
        strncpy0(t->text, it->pszText ? it->pszText : "", TAB_TEXT);
    if (it->mask & TCIF_PARAM)
        t->param = it->lParam;
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
    set_item(&d->t[at], it);
    d->n++;
    if (w->value < 0)
        w->value = 0;                               /* the first tab starts selected */
    else if (at <= w->value)
        w->value++;
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
        return (w->ctl = LocalAlloc(LMEM_ZEROINIT, sizeof *d)) ? 0 : -1;
    case WM_DESTROY:
        LocalFree(d->t);
        LocalFree(d);
        w->ctl = NULL;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc, w);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN: {
        int t = hit(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (t >= 0) {
            SetFocus(h);
            choose(w, t);
        }
        return 0;
    }
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
        InvalidateRect(h, NULL);
        return 1;
    case TCM_DELETEALLITEMS:
        d->n = 0;
        w->value = -1;
        InvalidateRect(h, NULL);
        return 1;
    case TCM_GETITEMCOUNT:
        return d->n;
    case TCM_GETITEM: {
        TCITEM *it = (TCITEM *)lp;
        if (i < 0 || i >= d->n || !it)
            return 0;
        if ((it->mask & TCIF_TEXT) && it->pszText && it->cchTextMax > 0)
            strncpy0(it->pszText, d->t[i].text, (size_t)it->cchTextMax);
        if (it->mask & TCIF_PARAM)
            it->lParam = d->t[i].param;
        return 1;
    }
    case TCM_SETITEM:
        if (i < 0 || i >= d->n || !lp)
            return 0;
        set_item(&d->t[i], (const TCITEM *)lp);
        InvalidateRect(h, NULL);
        return 1;
    case TCM_GETITEMRECT:
        if (i < 0 || i >= d->n || !lp)
            return 0;
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
        r->left += s * MARGIN;
        r->right -= s * MARGIN;
        r->top += s * (TAB_H + MARGIN);
        r->bottom -= s * MARGIN;
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}
