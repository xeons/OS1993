/*
 * listbox.c -- the LISTBOX class: a white list with a lavender selection
 * and its own vertical scroll bar. The outer 3 pixels hold the focus ring.
 *
 * Items live in the owning process's heap (w->ctl): a string and a value
 * each (LB_SETITEMDATA). LBS_SORT keeps them in order; LBS_MULTIPLESEL
 * toggles items with a click or Space, LBS_EXTENDEDSEL selects ranges with
 * Shift and adds with Ctrl; LBS_OWNERDRAWFIXED rows are drawn by the
 * parent (WM_DRAWITEM; their height asked once with WM_MEASUREITEM).
 * Typing a letter goes to the next item starting with it. The parent may
 * colour it (WM_CTLCOLORLISTBOX).
 *
 * w->value = the caret item (the selection, single-select; -1 none),
 * w->top = first visible, w->max = count.
 */
#include "uipriv.h"

#define RING 3

struct lbitem { char *text; LPARAM data; bool sel; };

struct lbox {
    struct lbitem *it;
    int cap, item_h, anchor;
    char find[16];                      /* type-ahead */
    int nfind;
    uint32_t find_at;
    bool dragging;
};

static struct lbox *lb(struct wnd *w) { return w->ctl; }
static bool multi(struct wnd *w) { return (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0; }
static bool owner(struct wnd *w) { return (w->style & LBS_OWNERDRAWFIXED) != 0; }
static bool has_strings(struct wnd *w) { return !owner(w) || (w->style & LBS_HASSTRINGS); }
static int item_h(struct wnd *w) { return lb(w)->item_h; }
static int visible(struct wnd *w) { int v = (w->h - 2 * RING - 4) / item_h(w); return v > 0 ? v : 1; }

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

static void notify(struct wnd *w, int code)
{
    if ((w->style & LBS_NOTIFY) && w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, code), (LPARAM)w);
}

static void show(struct wnd *w, int item)
{
    if (item < 0) return;
    if (item < w->top) scroll_to(w, item);
    if (item >= w->top + visible(w)) scroll_to(w, item - visible(w) + 1);
}

static void set_all(struct wnd *w, bool sel)
{
    for (int i = 0; i < w->max; i++)
        lb(w)->it[i].sel = sel;
}

/* Move the caret to item; mode 0 plain (single, or extended without keys:
 * just this one), 1 toggle it (multiple; Ctrl), 2 range from the anchor (Shift). */
static void select(struct wnd *w, int item, bool notify_parent, int mode)
{
    struct lbox *b = lb(w);
    if (!w->max)
        return;
    item = clampi(item, 0, w->max - 1);
    if (!multi(w)) {
        if (item == w->value)
            return;
        w->value = item;
    } else if (mode == 2) {
        int a = clampi(b->anchor, 0, w->max - 1), lo = a < item ? a : item, hi = a < item ? item : a;
        set_all(w, false);
        for (int i = lo; i <= hi; i++) b->it[i].sel = true;
        w->value = item;
    } else if (mode == 1) {
        b->it[item].sel = !b->it[item].sel;
        w->value = b->anchor = item;
    } else {
        if (w->style & LBS_EXTENDEDSEL) {
            set_all(w, false);
            b->it[item].sel = true;
        }
        w->value = b->anchor = item;
    }
    show(w, item);
    InvalidateRect(w, NULL);
    if (notify_parent)
        notify(w, LBN_SELCHANGE);
}

static bool is_sel(struct wnd *w, int i) { return multi(w) ? lb(w)->it[i].sel : i == w->value; }

static int cmp_text(const char *a, const char *b)       /* without case */
{
    for (;; a++, b++) {
        int ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (ca != cb || !ca) return ca - cb;
    }
}

static int insert(struct wnd *w, int at, const char *text, LPARAM data)
{
    struct lbox *b = lb(w);
    if (w->max == b->cap) {
        int cap = b->cap ? b->cap * 2 : 16;
        struct lbitem *p = LocalReAlloc(b->it, (uint32_t)cap * sizeof *p, 0);
        if (!p) return LB_ERRSPACE;
        b->it = p;
        b->cap = cap;
    }
    char *t = NULL;
    if (has_strings(w)) {
        t = LocalAlloc(0, (uint32_t)strlen(text ? text : "") + 1);
        if (!t) return LB_ERRSPACE;
        strcpy(t, text ? text : "");
    }
    if (at < 0 || at > w->max) {
        at = w->max;
        if ((w->style & LBS_SORT) && t)
            for (at = 0; at < w->max && cmp_text(b->it[at].text, t) <= 0; at++)
                ;
    }
    memmove(b->it + at + 1, b->it + at, (size_t)(w->max - at) * sizeof *b->it);
    b->it[at] = (struct lbitem){ t, t ? data : (LPARAM)text, false };
    w->max++;
    if (!multi(w) && w->value >= at) w->value++;
    InvalidateRect(w, NULL);
    return at;
}

static int remove_item(struct wnd *w, int i)
{
    struct lbox *b = lb(w);
    if (i < 0 || i >= w->max) return LB_ERR;
    LocalFree(b->it[i].text);
    memmove(b->it + i, b->it + i + 1, (size_t)(w->max - i - 1) * sizeof *b->it);
    w->max--;
    if (w->value == i) w->value = -1;
    else if (w->value > i) w->value--;
    scroll_to(w, w->top);
    InvalidateRect(w, NULL);
    return w->max;
}

static void remove_all(struct wnd *w)
{
    struct lbox *b = lb(w);
    for (int i = 0; i < w->max; i++) LocalFree(b->it[i].text);
    w->max = 0;
    w->value = -1;
    w->top = 0;
}

static bool starts_with(const char *t, const char *s)  /* without case */
{
    for (; *s; s++, t++) {
        int a = *t >= 'a' && *t <= 'z' ? *t - 32 : *t, b = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
        if (a != b) return false;
    }
    return true;
}

/* The first item after `after` (-1: from the top) matching s, wrapping round. */
static int find_text(struct wnd *w, int after, const char *s, bool exact)
{
    if (after < -1 || after >= w->max) after = -1;
    for (int k = 1; k <= w->max; k++) {
        int i = (after + k + w->max) % w->max;
        const char *t = lb(w)->it[i].text;
        if (t && (exact ? !cmp_text(t, s) : starts_with(t, s)))
            return i;
    }
    return LB_ERR;
}

static int item_at(struct wnd *w, int y)
{
    if (y < RING + 2) return w->top - 1;
    return w->top + (y - RING - 2) / item_h(w);
}

LRESULT listbox_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct lbox *b = lb(w);
    if (!b && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        b = LocalAlloc(LMEM_ZEROINIT, sizeof *b);
        if (!b) return -1;
        w->ctl = b;
        w->value = -1;
        b->item_h = ITEM_H;
        if (owner(w) && w->parent && wnd_mine(w->parent)) {
            MEASUREITEMSTRUCT mi = { ODT_LISTBOX, (unsigned)w->id, 0, (unsigned)w->w, ITEM_H, 0 };
            SendMessage(w->parent, WM_MEASUREITEM, (WPARAM)w->id, (LPARAM)&mi);
            if (mi.itemHeight > 0 && mi.itemHeight < 200) b->item_h = (int)mi.itemHeight;
        }
        return 0;
    case WM_DESTROY:
        remove_all(w);
        LocalFree(b->it);
        LocalFree(b);
        w->ctl = NULL;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        bool focus = ui_has_focus(w);
        if (focus) {
            RECT a = { 0, 0, w->w, w->h }, c = { 1, 1, w->w - 1, w->h - 1 };
            FrameRect(dc, &a, C_ACCENT);
            FrameRect(dc, &c, C_ACCENT);
        }
        int x = RING, y = RING, bw = w->w - 2 * RING, bh = w->h - 2 * RING;
        RECT o = { x, y, x + bw, y + bh };
        FrameRect(dc, &o, C_GRAY1);
        int iw = bw - SCROLL_W - 2;
        RECT in = { x + 1, y + 1, x + 1 + iw, y + bh - 1 }, saved;
        uint8_t fg = w->enabled ? C_BLACK : C_GRAY3, bg = C_WHITE;
        ctl_color(w, dc, WM_CTLCOLORLISTBOX, &fg, &bg);
        FillRect(dc, &in, bg);
        dc_clip(dc, in, &saved);
        SelectFont(dc, FONT_GUI);
        for (int i = 0; i <= visible(w) && w->top + i < w->max; i++) {
            int item = w->top + i, iy = y + 2 + i * item_h(w);
            bool sel = is_sel(w, item);
            RECT r = { x + 1, iy, x + 1 + iw, iy + item_h(w) };
            if (owner(w)) {
                unsigned st = (sel ? ODS_SELECTED : 0) | (focus && item == w->value ? ODS_FOCUS : 0) |
                              (w->enabled ? 0 : ODS_DISABLED);
                owner_draw(w, dc, ODT_LISTBOX, (unsigned)item, ODA_DRAWENTIRE, st, r, b->it[item].data);
                SelectFont(dc, FONT_GUI);
                continue;
            }
            if (sel)
                FillRect(dc, &r, focus || !multi(w) ? C_ACCENT : C_GRAY4);
            TextOut(dc, x + 6, iy + (item_h(w) - 16) / 2, b->it[item].text, sel ? C_WHITE : fg, -1);
            if (multi(w) && focus && item == w->value)  /* the caret, in a multi-select list */
                FrameRect(dc, &r, sel ? C_WHITE : C_ACCENT);
        }
        dc_unclip(dc, &saved);
        struct sbar s = bar_of(w);
        sbar_draw(dc, &s, w->tracking ? w->part : SB_PART_NONE);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        if (msg != WM_ENABLE) notify(w, msg == WM_SETFOCUS ? LBN_SETFOCUS : LBN_KILLFOCUS);
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
            int item = item_at(w, y);
            if (item >= w->top && item < w->max) {
                int mode = 0;
                if (w->style & LBS_MULTIPLESEL) mode = 1;
                else if (w->style & LBS_EXTENDEDSEL)
                    mode = (wp & MK_SHIFT) ? 2 : (wp & MK_CONTROL) ? 1 : 0;
                select(w, item, true, mode);
                b->dragging = true;
                SetCapture(h);
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        struct sbar s = bar_of(w);
        int item = item_at(w, y);
        if (x < s.x && item >= w->top && item < w->max) {
            if (!multi(w)) select(w, item, true, 0);
            notify(w, LBN_DBLCLK);
        } else {
            SendMessage(h, WM_LBUTTONDOWN, wp, lp);     /* the scroll bar: a click */
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (w->tracking && w->part == SB_PART_THUMB) {
            struct sbar s = bar_of(w);
            scroll_to(w, sbar_thumb_value(&s, GET_Y_LPARAM(lp), w->grab));
        } else if (b->dragging && !(w->style & LBS_MULTIPLESEL)) {   /* dragging along: follow */
            int item = clampi(item_at(w, GET_Y_LPARAM(lp)), 0, w->max - 1);
            if (item != w->value)
                select(w, item, true, (w->style & LBS_EXTENDEDSEL) ? 2 : 0);
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->tracking || b->dragging) {
            ReleaseCapture();
            w->tracking = b->dragging = false;
            w->part = SB_PART_NONE;
            InvalidateRect(h, NULL);
        }
        return 0;
    case WM_KEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        int cur = w->value < 0 ? 0 : w->value, to = -1;
        switch (wp) {
        case VK_UP:    to = w->value < 0 ? 0 : cur - 1; break;
        case VK_DOWN:  to = w->value < 0 ? 0 : cur + 1; break;
        case VK_PRIOR: to = cur - visible(w); break;
        case VK_NEXT:  to = cur + visible(w); break;
        case VK_HOME:  to = 0; break;
        case VK_END:   to = w->max - 1; break;
        case VK_SPACE:
            if (multi(w) && w->value >= 0)
                select(w, w->value, true, (w->style & LBS_EXTENDEDSEL) && shift ? 2 : 1);
            return 0;
        }
        if (to < 0 && wp != VK_UP && wp != VK_PRIOR && wp != VK_HOME)
            return 0;
        if (!w->max)
            return 0;
        to = clampi(to, 0, w->max - 1);
        if (w->style & LBS_MULTIPLESEL || ((w->style & LBS_EXTENDEDSEL) && ctrl)) {
            w->value = to;                          /* the caret moves alone */
            show(w, to);
            InvalidateRect(h, NULL);
        } else {
            select(w, to, true, (w->style & LBS_EXTENDEDSEL) && shift ? 2 : 0);
        }
        return 0;
    }
    case WM_CHAR: {                                 /* type-ahead */
        char c = (char)wp;
        if ((uint8_t)c < 32 || !has_strings(w))
            return 0;
        uint32_t now = GetTickCount();
        if (now - b->find_at > 1000) b->nfind = 0;
        b->find_at = now;
        if (b->nfind < (int)sizeof b->find - 1) b->find[b->nfind++] = c;
        b->find[b->nfind] = '\0';
        bool same = b->nfind > 1;                   /* the same letter again: the next one */
        for (int i = 1; i < b->nfind; i++) if (b->find[i] != b->find[0]) same = false;
        int at = same ? find_text(w, w->value, b->find + b->nfind - 1, false)
                      : find_text(w, b->nfind > 1 ? w->value - 1 : w->value, b->find, false);
        if (at >= 0) select(w, at, true, 0);
        return 0;
    }
    case LB_ADDSTRING:
        return insert(w, -1, (const char *)lp, 0);
    case LB_INSERTSTRING:
        return insert(w, (int)wp < 0 ? w->max : (int)wp, (const char *)lp, 0);
    case LB_DELETESTRING:
        return remove_item(w, (int)wp);
    case LB_RESETCONTENT:
        remove_all(w);
        InvalidateRect(h, NULL);
        return 0;
    case LB_SETCURSEL:
        if ((int)wp < 0 || (int)wp >= w->max) {
            w->value = -1;
            InvalidateRect(h, NULL);
            return (int)wp == -1 ? 0 : LB_ERR;
        }
        if (multi(w)) return LB_ERR;
        select(w, (int)wp, false, 0);
        return (LRESULT)wp;
    case LB_GETCURSEL:
        return w->value;
    case LB_GETCARETINDEX:
        return w->value;
    case LB_SETCARETINDEX:
        if ((int)wp < 0 || (int)wp >= w->max) return LB_ERR;
        w->value = (int)wp;
        show(w, w->value);
        InvalidateRect(h, NULL);
        return 0;
    case LB_GETCOUNT:
        return w->max;
    case LB_GETTEXT:
        if ((int)wp < 0 || (int)wp >= w->max || !lp)
            return LB_ERR;
        if (!has_strings(w)) {
            *(LPARAM *)lp = b->it[wp].data;
            return 4;
        }
        strcpy((char *)lp, b->it[wp].text);
        return (LRESULT)strlen((char *)lp);
    case LB_GETTEXTLEN:
        if ((int)wp < 0 || (int)wp >= w->max) return LB_ERR;
        return has_strings(w) ? (LRESULT)strlen(b->it[wp].text) : 4;
    case LB_SETITEMDATA:
        if ((int)wp < 0 || (int)wp >= w->max) return LB_ERR;
        b->it[wp].data = lp;
        return 0;
    case LB_GETITEMDATA:
        if ((int)wp < 0 || (int)wp >= w->max) return LB_ERR;
        return b->it[wp].data;
    case LB_FINDSTRING:
    case LB_FINDSTRINGEXACT:
        if (!lp || !w->max) return LB_ERR;
        return find_text(w, (int)wp, (const char *)lp, msg == LB_FINDSTRINGEXACT);
    case LB_SELECTSTRING: {
        if (!lp || !w->max) return LB_ERR;
        int i = find_text(w, (int)wp, (const char *)lp, false);
        if (i >= 0 && !multi(w)) select(w, i, false, 0);
        return i;
    }
    case LB_GETTOPINDEX:
        return w->top;
    case LB_SETTOPINDEX:
        scroll_to(w, (int)wp);
        return 0;
    case LB_GETITEMHEIGHT:
        return item_h(w);
    case LB_SETITEMHEIGHT:
        if ((int)lp < 1 || (int)lp > 200) return LB_ERR;
        b->item_h = (int)lp;
        scroll_to(w, w->top);
        InvalidateRect(h, NULL);
        return 0;
    case LB_GETITEMRECT: {
        if ((int)wp < 0 || (int)wp >= w->max || !lp) return LB_ERR;
        int iy = RING + 2 + ((int)wp - w->top) * item_h(w);
        *(RECT *)lp = (RECT){ RING + 1, iy, w->w - RING - SCROLL_W - 1, iy + item_h(w) };
        return 1;
    }
    case LB_GETSEL:
        if ((int)wp < 0 || (int)wp >= w->max) return LB_ERR;
        return is_sel(w, (int)wp);
    case LB_SETSEL:                                 /* wp = select?, lp = item (-1: all) */
        if (!multi(w)) return LB_ERR;
        if ((int)lp == -1) set_all(w, wp != 0);
        else if ((int)lp >= 0 && (int)lp < w->max) b->it[lp].sel = wp != 0;
        else return LB_ERR;
        InvalidateRect(h, NULL);
        return 0;
    case LB_GETSELCOUNT: {
        if (!multi(w)) return LB_ERR;
        int n = 0;
        for (int i = 0; i < w->max; i++) n += b->it[i].sel;
        return n;
    }
    case LB_GETSELITEMS: {                          /* wp = room, lp = int[] */
        if (!multi(w)) return LB_ERR;
        int n = 0;
        for (int i = 0; i < w->max && n < (int)wp; i++)
            if (b->it[i].sel) ((int *)lp)[n++] = i;
        return n;
    }
    case LB_SELITEMRANGE:                           /* wp = select?, lp = MAKELPARAM(first, last) */
        if (!multi(w)) return LB_ERR;
        for (int i = LOWORD(lp); i <= HIWORD(lp) && i < w->max; i++) b->it[i].sel = wp != 0;
        InvalidateRect(h, NULL);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}
