/*
 * listbox.c -- the LISTBOX class: a white list with a lavender selection
 * and its own vertical scroll bar. The outer 3 pixels hold the focus ring.
 * Strings are copied into a pool shared by all list boxes.
 * w->value = selection (-1 none), w->top = first visible, w->max = count.
 */
#include "uipriv.h"

#define RING     3
#define ITEM_MAX 256
#define ITEM_LEN 32

static struct { struct wnd *owner; char text[ITEM_LEN]; } items[ITEM_MAX];
static int nitems;

static int find(struct wnd *w, int n)              /* pool index of item n */
{
    for (int i = 0; i < nitems; i++)
        if (items[i].owner == w && n-- == 0)
            return i;
    return -1;
}

static void remove_all(struct wnd *w)
{
    int j = 0;
    for (int i = 0; i < nitems; i++)
        if (items[i].owner != w)
            items[j++] = items[i];
    nitems = j;
    w->max = 0;
}

static int visible(struct wnd *w) { return (w->h - 2 * RING - 4) / ITEM_H; }

static struct sbar bar_of(struct wnd *w)
{
    int max = w->max - visible(w);
    return (struct sbar){ true, w->w - RING - SCROLL_W - 1, RING + 1, SCROLL_W, w->h - 2 * RING - 2,
                          w->top, 0, max < 0 ? 0 : max, visible(w) };
}

static void scroll_to(struct wnd *w, int top)
{
    struct sbar s = bar_of(w);
    top = clampi(top, 0, s.max);
    if (top != w->top) {
        w->top = top;
        InvalidateRect(w, NULL);
    }
}

static void select(struct wnd *w, int item, bool notify)
{
    item = clampi(item, 0, w->max - 1);
    if (item == w->value)
        return;
    w->value = item;
    if (item < w->top) scroll_to(w, item);
    if (item >= w->top + visible(w)) scroll_to(w, item - visible(w) + 1);
    InvalidateRect(w, NULL);
    if (notify && (w->style & LBS_NOTIFY) && w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, LBN_SELCHANGE), (LPARAM)w);
}

LRESULT listbox_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    switch (msg) {
    case WM_CREATE:
        w->value = -1;
        return 0;
    case WM_DESTROY:
        remove_all(w);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (ui_has_focus(w)) {
            RECT a = { 0, 0, w->w, w->h }, b = { 1, 1, w->w - 1, w->h - 1 };
            FrameRect(dc, &a, C_ACCENT);
            FrameRect(dc, &b, C_ACCENT);
        }
        int x = RING, y = RING, bw = w->w - 2 * RING, bh = w->h - 2 * RING;
        RECT o = { x, y, x + bw, y + bh };
        FrameRect(dc, &o, C_GRAY1);
        int iw = bw - SCROLL_W - 2;
        RECT in = { x + 1, y + 1, x + 1 + iw, y + bh - 1 };
        FillRect(dc, &in, C_WHITE);
        for (int i = 0; i < visible(w) && w->top + i < w->max; i++) {
            int item = w->top + i, iy = y + 2 + i * ITEM_H;
            bool sel = item == w->value;
            if (sel) {
                RECT r = { x + 1, iy, x + 1 + iw, iy + ITEM_H };
                FillRect(dc, &r, C_ACCENT);
            }
            TextOut(dc, x + 6, iy, items[find(w, item)].text, sel ? C_WHITE : C_BLACK, -1);
        }
        struct sbar s = bar_of(w);
        sbar_draw(dc, &s, w->tracking ? w->part : SB_PART_NONE);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        struct sbar s = bar_of(w);
        SetFocus(h);
        if (x >= s.x) {
            w->part = sbar_hit(&s, x, y);
            w->tracking = true;
            SetCapture(h);
            switch (w->part) {
            case SB_PART_DEC:   scroll_to(w, w->top - 1); break;
            case SB_PART_INC:   scroll_to(w, w->top + 1); break;
            case SB_PART_PGDEC: scroll_to(w, w->top - s.page); break;
            case SB_PART_PGINC: scroll_to(w, w->top + s.page); break;
            case SB_PART_THUMB: {
                int pos, len, track, tlen;
                sbar_thumb(&s, &pos, &len, &track, &tlen);
                w->grab = y - pos;
                break;
            }
            }
            InvalidateRect(h, NULL);
        } else {
            int item = w->top + (y - RING - 2) / ITEM_H;
            if (y >= RING + 2 && item < w->max)
                select(w, item, true);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        struct sbar s = bar_of(w);
        int item = w->top + (y - RING - 2) / ITEM_H;
        if (x < s.x && y >= RING + 2 && item < w->max) {
            select(w, item, true);
            if ((w->style & LBS_NOTIFY) && w->parent)
                SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, LBN_DBLCLK), (LPARAM)w);
        } else {
            SendMessage(h, WM_LBUTTONDOWN, wp, lp);     /* the scroll bar: a click */
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (w->tracking && w->part == SB_PART_THUMB) {
            struct sbar s = bar_of(w);
            scroll_to(w, sbar_thumb_value(&s, GET_Y_LPARAM(lp), w->grab));
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->tracking) {
            ReleaseCapture();
            w->tracking = false;
            w->part = SB_PART_NONE;
            InvalidateRect(h, NULL);
        }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_UP:    select(w, w->value - 1, true); break;
        case VK_DOWN:  select(w, w->value + 1, true); break;
        case VK_PRIOR: select(w, w->value - visible(w), true); break;
        case VK_NEXT:  select(w, w->value + visible(w), true); break;
        case VK_HOME:  select(w, 0, true); break;
        case VK_END:   select(w, w->max - 1, true); break;
        }
        return 0;
    case LB_ADDSTRING:
        if (nitems == ITEM_MAX || !lp)
            return LB_ERR;
        items[nitems].owner = w;
        strncpy0(items[nitems].text, (const char *)lp, ITEM_LEN);
        nitems++;
        InvalidateRect(h, NULL);
        return w->max++;
    case LB_RESETCONTENT:
        remove_all(w);
        w->value = -1;
        w->top = 0;
        InvalidateRect(h, NULL);
        return 0;
    case LB_SETCURSEL:
        if ((int)wp < 0 || (int)wp >= w->max) {
            w->value = -1;
            InvalidateRect(h, NULL);
            return LB_ERR;
        }
        select(w, (int)wp, false);
        return (LRESULT)wp;
    case LB_GETCURSEL:
        return w->value;
    case LB_GETCOUNT:
        return w->max;
    case LB_GETTEXT: {
        int i = find(w, (int)wp);
        if (i < 0 || !lp)
            return LB_ERR;
        strncpy0((char *)lp, items[i].text, ITEM_LEN);
        return (LRESULT)strlen((char *)lp);
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}
