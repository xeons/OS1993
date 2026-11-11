/*
 * listview.c -- the list view class (WC_LISTVIEW): items with an image, a
 * label and sub-items, shown one of four ways (the style's LVS_TYPEMASK):
 *
 *   LVS_ICON       large icons in a grid, the labels wrapped below them
 *                  (two lines; all of it for the focused, selected item)
 *   LVS_SMALLICON  small icons in rows, left to right
 *   LVS_LIST       small icons in columns, top to bottom, scrolled sideways
 *   LVS_REPORT     details: a row each under column headers; a click on a
 *                  header is LVN_COLUMNCLICK, dragging its right edge
 *                  resizes the column; a click anywhere in a row takes it
 *
 * Scroll bars come and go as needed, in pixels. Items live in the owning
 * process's heap (w->ctl), an array of pointers so sorting is cheap.
 * Selection: a click takes one item, Ctrl adds or removes, Shift takes the
 * range from the anchor (unless LVS_SINGLESEL). Labels are edited in an
 * EDIT control over them (F2, LVS_EDITLABELS, or LVM_EDITLABEL): Enter or
 * a click elsewhere keeps the text if the parent agrees, Esc cancels.
 * Notifications are WM_NOTIFY to the parent.
 *
 * Extended styles (LVM_SETEXTENDEDLISTVIEWSTYLE): LVS_EX_GRIDLINES draws
 * lines between the details rows and columns, LVS_EX_CHECKBOXES gives each
 * item a check box (its state image: 1 unchecked, 2 checked),
 * LVS_EX_FULLROWSELECT highlights whole rows. A column's format can carry
 * a sort arrow (LVCFMT_SORTUP / SORTDOWN). Dragging over empty space draws
 * a marquee that selects what it touches. Dragging items sends
 * LVN_BEGINDRAG; if the parent doesn't take it over, in the icon views the
 * items move where they are dropped (LVM_SETITEMPOSITION places one,
 * LVM_ARRANGE puts them back on the grid). LVIS_CUT items are drawn faded,
 * LVIS_DROPHILITED ones highlighted. LVS_OWNERDATA makes a virtual list:
 * the program says how many items there are (LVM_SETITEMCOUNT) and gives
 * their texts and images when they are drawn (LVN_GETDISPINFO); the list
 * keeps only their states.
 */
#include "uipriv.h"

#define HDR_H    20
#define ROW_H    18
#define CELL_W   84             /* large icons */
#define CELL_H   70
#define COL_MAX  16
#define LINE_MAX 4              /* label lines under a large icon, at most */
#define EDIT_ID  0x7E01

struct litem {
    char    *text;
    char   **sub;               /* sub[k]: column k's text (k >= 1), or NULL */
    int      nsub;
    int      image;
    LPARAM   param;
    unsigned state;
    int16_t  px, py;            /* icon views: where it was put (content coordinates) */
    bool     placed;
};

struct lcol { char text[32]; int cx, fmt; };

struct ldata {
    struct litem **items;
    int      n, cap;
    struct lcol cols[COL_MAX];
    int      ncol;
    HIMAGELIST il_normal, il_small;
    int      focus, anchor;     /* items, -1 none */
    int      sx, sy;            /* pixels scrolled */
    int      bar;               /* tracking: 0 vertical, 1 horizontal */
    int      drag_col, drag_x0, drag_cx0;     /* resizing a column */
    int      press_col;         /* a header being clicked, -1 */
    HWND     edit;              /* a label being edited */
    int      edit_item;
    WNDPROC  edit_old;
    unsigned ex;                /* LVS_EX_* */
    bool     any_placed;        /* some item has a position of its own */
    bool     marquee;           /* a marquee being dragged (content coordinates) */
    int      mx0, my0, mx1, my1;
    int      press_item, press_x, press_y;   /* a button down on an item: a drag? */
    bool     moving;            /* moving icons ourselves */
    struct litem *vblock;       /* LVS_OWNERDATA: the items (states only) */
    char     vbuf[4][128];      /* ... and their texts, as last asked for */
    int      vslot;
};

struct lay {
    int  cx, cy, cw, ch;        /* the items' area (client coordinates, below the header) */
    int  hdr;
    bool vb, hb;
    int  cw_all, ch_all;        /* the content's size */
    int  per;                   /* items a row (icons) or a column (list) */
    int  cellw, cellh;
};

static struct ldata *data_of(struct wnd *w) { return (struct ldata *)w->ctl; }
static int view_of(struct wnd *w) { return (int)(w->style & LVS_TYPEMASK); }
static bool owner_data(struct wnd *w) { return (w->style & LVS_OWNERDATA) != 0; }
static bool checks(struct wnd *w) { return (data_of(w)->ex & LVS_EX_CHECKBOXES) != 0; }
static int check_w(struct wnd *w) { return checks(w) ? 16 : 0; }

/* LVS_OWNERDATA: item i's text and image, from the parent (good until a
 * few more are asked for). */
static LRESULT lv_notify(struct wnd *w, int code, int item, int sub, POINT pt);
static void vfetch(struct wnd *w, int i)
{
    struct ldata *d = data_of(w);
    if (!owner_data(w) || i < 0 || i >= d->n)
        return;
    char *buf = d->vbuf[d->vslot = (d->vslot + 1) & 3];
    buf[0] = '\0';
    NMLVDISPINFO nm;
    memset(&nm, 0, sizeof nm);
    nm.item.mask = LVIF_TEXT | LVIF_IMAGE;
    nm.item.iItem = i;
    nm.item.pszText = buf;
    nm.item.cchTextMax = (int)sizeof d->vbuf[0];
    ui_notify(w, &nm.hdr, LVN_GETDISPINFO);
    if (!wnd_from(w) || i >= d->n)
        return;
    d->items[i]->text = nm.item.pszText ? nm.item.pszText : buf;
    d->items[i]->image = nm.item.iImage;
}

static char *dup_text(const char *s)
{
    if (!s)
        s = "";
    char *t = LocalAlloc(LMEM_FIXED, (uint32_t)strlen(s) + 1);
    if (t)
        memcpy(t, s, strlen(s) + 1);
    return t;
}

static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

/* ---- geometry ------------------------------------------------------------------------- */

static int small_cell_w(struct ldata *d)
{
    if (d->vblock)
        return 160;                                     /* (virtual: not every text is at hand) */
    int m = 0;
    for (int i = 0; i < d->n; i++) {
        int tw = GUI_W(d->items[i]->text);
        if (tw > m)
            m = tw;
    }
    m += 16 + 6 + 10;
    return clampi(m, 80, 220);
}

static void content_size(struct wnd *w, struct lay *L)
{
    struct ldata *d = data_of(w);
    switch (view_of(w)) {
    case LVS_ICON:
        L->cellw = CELL_W;
        L->cellh = CELL_H;
        L->per = L->cw / CELL_W > 0 ? L->cw / CELL_W : 1;
        L->cw_all = L->per * CELL_W;
        L->ch_all = (d->n + L->per - 1) / L->per * CELL_H;
        break;
    case LVS_SMALLICON:
        L->cellw = small_cell_w(d);
        L->cellh = ROW_H;
        L->per = L->cw / L->cellw > 0 ? L->cw / L->cellw : 1;
        L->cw_all = L->per * L->cellw;
        L->ch_all = (d->n + L->per - 1) / L->per * ROW_H;
        break;
    case LVS_LIST:
        L->cellw = small_cell_w(d);
        L->cellh = ROW_H;
        L->per = L->ch / ROW_H > 0 ? L->ch / ROW_H : 1;
        L->cw_all = (d->n + L->per - 1) / L->per * L->cellw;
        L->ch_all = L->per * ROW_H;
        break;
    default: {                                          /* LVS_REPORT */
        int total = 0;
        for (int c = 0; c < d->ncol; c++)
            total += d->cols[c].cx;
        L->cellw = total;
        L->cellh = ROW_H;
        L->per = 1;
        L->cw_all = total;
        L->ch_all = d->n * ROW_H;
        break;
    }
    }
}

static void layout(struct wnd *w, struct lay *L)
{
    memset(L, 0, sizeof *L);
    L->cx = 1;
    L->hdr = view_of(w) == LVS_REPORT && !(w->style & LVS_NOCOLUMNHEADER) ? HDR_H : 0;
    L->cy = 1 + L->hdr;
    int W = w->w - 2, H = w->h - 2 - L->hdr;
    for (int pass = 0; pass < 3; pass++) {
        L->cw = W - (L->vb ? SCROLL_W : 0);
        L->ch = H - (L->hb ? SCROLL_W : 0);
        if (L->cw < 1) L->cw = 1;
        if (L->ch < 1) L->ch = 1;
        content_size(w, L);
        L->vb = L->ch_all > L->ch;
        L->hb = L->cw_all > L->cw;
    }
    L->cw = W - (L->vb ? SCROLL_W : 0);
    L->ch = H - (L->hb ? SCROLL_W : 0);
    if (L->cw < 1) L->cw = 1;
    if (L->ch < 1) L->ch = 1;
    content_size(w, L);
    struct ldata *d = data_of(w);
    if (d->any_placed && (view_of(w) == LVS_ICON || view_of(w) == LVS_SMALLICON))
        for (int i = 0; i < d->n; i++) {                /* room for icons put further out */
            struct litem *it = d->items[i];
            if (!it->placed) continue;
            if (it->px + L->cellw > L->cw_all) L->cw_all = it->px + L->cellw;
            if (it->py + L->cellh > L->ch_all) L->ch_all = it->py + L->cellh;
        }
    if (view_of(w) == LVS_REPORT) L->cw_all += check_w(w);
}

/* Item i's cell, in content coordinates. */
static RECT cell_of(struct wnd *w, const struct lay *L, int i)
{
    int x, y;
    struct ldata *d = data_of(w);
    if ((view_of(w) == LVS_ICON || view_of(w) == LVS_SMALLICON) && d->items[i]->placed)
        return (RECT){ d->items[i]->px, d->items[i]->py, d->items[i]->px + L->cellw, d->items[i]->py + L->cellh };
    switch (view_of(w)) {
    case LVS_ICON:
    case LVS_SMALLICON:
        x = i % L->per * L->cellw;
        y = i / L->per * L->cellh;
        break;
    case LVS_LIST:
        x = i / L->per * L->cellw;
        y = i % L->per * L->cellh;
        break;
    default:
        x = 0;
        y = i * ROW_H;
        break;
    }
    return (RECT){ x, y, x + L->cellw, y + L->cellh };
}

static RECT to_client(struct wnd *w, const struct lay *L, RECT r)
{
    struct ldata *d = data_of(w);
    int dx = L->cx - d->sx, dy = L->cy - d->sy;
    return (RECT){ r.left + dx, r.top + dy, r.right + dx, r.bottom + dy };
}

/* Wrap a label into lines of at most max characters (at spaces when it
 * can); more than nmax lines: the last ends in "...". -> lines. */
/* s in at most nmax lines of px pixels (breaking at a space or after a
 * dot); "..." ends the last if it doesn't all fit. */
static int wrap(const char *s, int px, char lines[][64], int nmax)
{
    int n = 0, len = (int)strlen(s), at = 0;
    while (at < len && n < nmax) {
        int take = gui_fit(s + at, len - at, px);
        if (take > 63) take = 63;
        if (take < 1) take = 1;
        if (take < len - at) {                          /* break at a space, or after a dot */
            for (int k = take; k > take / 3; k--)
                if (s[at + k] == ' ' || s[at + k - 1] == '.') {
                    take = k;
                    break;
                }
        }
        memcpy(lines[n], s + at, (size_t)take);
        lines[n][take] = '\0';
        at += take;
        while (s[at] == ' ')
            at++;
        n++;
    }
    if (at < len && n > 0) {                            /* cut */
        char *l = lines[n - 1];
        int k = (int)strlen(l), dots = TextWidthFont(FONT_GUI, "...", 3);
        while (k > 0 && TextWidthFont(FONT_GUI, l, k) + dots > px)
            k--;
        if (k > 59) k = 59;
        memcpy(l + k, "...", 4);
    }
    if (!n) {
        lines[0][0] = '\0';
        n = 1;
    }
    return n;
}

static bool show_all_lines(struct wnd *w, int i)
{
    struct ldata *d = data_of(w);
    return i == d->focus && (d->items[i]->state & LVIS_SELECTED) && ui_has_focus(w);
}

/* The icon and label rectangles of item i (client coordinates). */
static void parts_of(struct wnd *w, const struct lay *L, int i, RECT *icon, RECT *label)
{
    struct ldata *d = data_of(w);
    vfetch(w, i);
    RECT c = to_client(w, L, cell_of(w, L, i));
    const char *t = d->items[i]->text;
    if (view_of(w) == LVS_ICON) {
        int ix = c.left + (CELL_W - 32) / 2;
        *icon = (RECT){ ix, c.top + 3, ix + 32, c.top + 35 };
        char lines[LINE_MAX][64];
        int n = wrap(t, CELL_W - 6, lines, show_all_lines(w, i) ? LINE_MAX : 2), lw = 0;
        for (int k = 0; k < n; k++)
            if (GUI_W(lines[k]) > lw)
                lw = GUI_W(lines[k]);
        int lx = c.left + (CELL_W - lw) / 2 - 2;
        *label = (RECT){ lx, c.top + 37, lx + lw + 4, c.top + 37 + n * 16 };
        return;
    }
    int ix = c.left + 2 + check_w(w);
    *icon = (RECT){ ix, c.top + 1, ix + 16, c.top + 17 };
    int tx = ix + 20, tw = GUI_W(t) + 4;
    int room = view_of(w) == LVS_REPORT ? (d->ncol ? d->cols[0].cx : 100) - 22 : L->cellw - 24 - check_w(w);
    if (tw > room)
        tw = room;
    *label = (RECT){ tx - 2, c.top + 1, tx - 2 + tw, c.top + ROW_H - 1 };
}

/* LVS_EX_CHECKBOXES: item i's box (client coordinates). */
static RECT box_of(struct wnd *w, const struct lay *L, int i)
{
    RECT icon, label;
    parts_of(w, L, i, &icon, &label);
    if (view_of(w) == LVS_ICON)
        return (RECT){ icon.left - 14, icon.top + 2, icon.left - 1, icon.top + 15 };
    return (RECT){ icon.left - 15, icon.top + 1, icon.left - 2, icon.top + 14 };
}

static unsigned check_of(struct litem *it) { return (it->state & LVIS_STATEIMAGEMASK) >> 12; }

static struct sbar vbar(struct wnd *w, const struct lay *L)
{
    struct ldata *d = data_of(w);
    int max = L->ch_all - L->ch;
    return (struct sbar){ true, L->cx + L->cw, L->cy, SCROLL_W, L->ch, d->sy, 0, max < 0 ? 0 : max, L->ch };
}

static struct sbar hbar(struct wnd *w, const struct lay *L)
{
    struct ldata *d = data_of(w);
    int max = L->cw_all - L->cw;
    return (struct sbar){ false, L->cx, L->cy + L->ch, L->cw, SCROLL_W, d->sx, 0, max < 0 ? 0 : max, L->cw };
}

static void clamp_scroll(struct wnd *w)
{
    struct ldata *d = data_of(w);
    struct lay L;
    layout(w, &L);
    d->sy = clampi(d->sy, 0, L.ch_all - L.ch > 0 ? L.ch_all - L.ch : 0);
    d->sx = clampi(d->sx, 0, L.cw_all - L.cw > 0 ? L.cw_all - L.cw : 0);
}

static void scroll_to(struct wnd *w, int sx, int sy)
{
    struct ldata *d = data_of(w);
    int ox = d->sx, oy = d->sy;
    d->sx = sx;
    d->sy = sy;
    clamp_scroll(w);
    if (d->sx != ox || d->sy != oy) {
        if (d->edit)
            DestroyWindow(d->edit), d->edit = NULL;
        InvalidateRect(w, NULL);
    }
}

static void ensure_visible(struct wnd *w, int i)
{
    struct ldata *d = data_of(w);
    if (i < 0 || i >= d->n)
        return;
    struct lay L;
    layout(w, &L);
    RECT c = cell_of(w, &L, i);
    int sx = d->sx, sy = d->sy;
    if (view_of(w) != LVS_REPORT) {
        if (c.left < sx) sx = c.left;
        else if (c.right > sx + L.cw) sx = c.right - L.cw;
    }
    if (c.top < sy) sy = c.top;
    else if (c.bottom > sy + L.ch) sy = c.bottom - L.ch;
    scroll_to(w, sx, sy);
}

/* ---- notifications and selection ------------------------------------------------------- */

static LRESULT lv_notify(struct wnd *w, int code, int item, int sub, POINT pt)
{
    struct ldata *d = data_of(w);
    NMLISTVIEW nm;
    memset(&nm, 0, sizeof nm);
    nm.iItem = item;
    nm.iSubItem = sub;
    nm.ptAction = pt;
    if (item >= 0 && item < d->n)
        nm.lParam = d->items[item]->param;
    return ui_notify(w, &nm.hdr, code);
}

static void set_state(struct wnd *w, int i, unsigned mask, unsigned value)
{
    struct ldata *d = data_of(w);
    if (i < 0 || i >= d->n)
        return;
    struct litem *it = d->items[i];
    unsigned old = it->state, now = (old & ~mask) | (value & mask);
    if (now == old)
        return;
    if ((now & LVIS_FOCUSED) && !(old & LVIS_FOCUSED)) {   /* one focused item */
        if (d->focus >= 0 && d->focus != i && d->focus < d->n)
            set_state(w, d->focus, LVIS_FOCUSED, 0);
        d->focus = i;
    } else if (!(now & LVIS_FOCUSED) && d->focus == i) {
        d->focus = -1;
    }
    it->state = now;
    InvalidateRect(w, NULL);
    NMLISTVIEW nm;
    memset(&nm, 0, sizeof nm);
    nm.iItem = i;
    nm.uOldState = old;
    nm.uNewState = now;
    nm.uChanged = LVIF_STATE;
    nm.lParam = it->param;
    ui_notify(w, &nm.hdr, LVN_ITEMCHANGED);
}

static void select_range(struct wnd *w, int a, int b, bool keep_others)
{
    struct ldata *d = data_of(w);
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = 0; i < d->n && wnd_from(w); i++) {
        bool in = i >= a && i <= b;
        if (in)
            set_state(w, i, LVIS_SELECTED, LVIS_SELECTED);
        else if (!keep_others)
            set_state(w, i, LVIS_SELECTED, 0);
    }
}

/* The user moves to item i (by mouse or key), with Ctrl or Shift held or not. */
static void choose(struct wnd *w, int i, bool ctrl, bool shift, bool by_key)
{
    struct ldata *d = data_of(w);
    if (i < 0 || i >= d->n)
        return;
    bool single = w->style & LVS_SINGLESEL;
    if (shift && !single && d->anchor >= 0) {
        select_range(w, d->anchor, i, ctrl);
    } else if (ctrl && !single) {
        if (!by_key)
            set_state(w, i, LVIS_SELECTED, (d->items[i]->state & LVIS_SELECTED) ? 0 : LVIS_SELECTED);
        d->anchor = i;
    } else {
        select_range(w, i, i, false);
        d->anchor = i;
    }
    if (wnd_from(w)) {
        set_state(w, i, LVIS_FOCUSED, LVIS_FOCUSED);
        ensure_visible(w, i);
    }
}

/* ---- label editing --------------------------------------------------------------------- */

static void end_edit(struct wnd *w, bool keep)
{
    struct ldata *d = data_of(w);
    if (!d || !d->edit)
        return;
    HWND e = d->edit;
    d->edit = NULL;
    char text[TEXT_MAX];
    GetWindowText(e, text, sizeof text);
    bool had_focus = GetFocus() == e;
    int i = d->edit_item;
    if (i >= 0 && i < d->n) {
        NMLVDISPINFO nm;
        memset(&nm, 0, sizeof nm);
        nm.item.mask = LVIF_TEXT | LVIF_PARAM;
        nm.item.iItem = i;
        nm.item.pszText = keep ? text : NULL;
        nm.item.cchTextMax = (int)sizeof text;
        nm.item.lParam = d->items[i]->param;
        if (ui_notify(w, &nm.hdr, LVN_ENDLABELEDIT) && keep && wnd_from(w) && i < d->n && !owner_data(w)) {
            char *s = dup_text(text);
            if (s) {
                LocalFree(d->items[i]->text);
                d->items[i]->text = s;
            }
        }
    }
    if (IsWindow(e))
        DestroyWindow(e);
    if (wnd_from(w)) {
        if (had_focus)
            SetFocus(w);
        InvalidateRect(w, NULL);
    }
}

static LRESULT edit_sub(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *e = wnd_from(h);
    struct wnd *w = e ? e->parent : NULL;
    struct ldata *d = w ? data_of(w) : NULL;
    WNDPROC old = d ? d->edit_old : edit_proc;
    if (d && d->edit == h) {
        if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
            end_edit(w, wp == VK_RETURN);
            return 0;
        }
        if (msg == WM_CHAR && (wp == '\r' || wp == 27))
            return 0;
        if (msg == WM_KILLFOCUS) {
            LRESULT r = CallWindowProc(old, h, msg, wp, lp);
            end_edit(w, true);
            return r;
        }
    }
    return CallWindowProc(old, h, msg, wp, lp);
}

static HWND begin_edit(struct wnd *w, int i)
{
    struct ldata *d = data_of(w);
    if (i < 0 || i >= d->n)
        return NULL;
    end_edit(w, true);
    if (!wnd_from(w) || i >= d->n)
        return NULL;
    ensure_visible(w, i);
    vfetch(w, i);
    NMLVDISPINFO nm;
    memset(&nm, 0, sizeof nm);
    nm.item.mask = LVIF_TEXT | LVIF_PARAM;
    nm.item.iItem = i;
    nm.item.pszText = d->items[i]->text;
    nm.item.lParam = d->items[i]->param;
    if (ui_notify(w, &nm.hdr, LVN_BEGINLABELEDIT) || !wnd_from(w) || i >= d->n)
        return NULL;
    struct lay L;
    layout(w, &L);
    RECT icon, label;
    parts_of(w, &L, i, &icon, &label);
    int ew = GUI_W(d->items[i]->text) + 40;
    if (ew < 120) ew = 120;
    int ex = label.left - 3;
    if (view_of(w) == LVS_ICON)
        ex = (label.left + label.right) / 2 - ew / 2;
    if (ex + ew > w->w - 1) ex = w->w - 1 - ew;
    if (ex < 1) ex = 1;
    HWND e = CreateWindow(WC_EDIT, d->items[i]->text, WS_CHILD | WS_VISIBLE, ex, label.top - 4, ew, 24, w,
                          EDIT_ID, NULL);
    if (!e)
        return NULL;
    d->edit = e;
    d->edit_item = i;
    d->edit_old = (WNDPROC)(uintptr_t)SetWindowLong(e, GWL_WNDPROC, (int32_t)(uintptr_t)edit_sub);
    SendMessage(e, EM_SETSEL, 0, -1);
    SetFocus(e);
    return e;
}

/* ---- items ----------------------------------------------------------------------------- */

static void free_item(struct litem *it)
{
    if (!it->sub && it->nsub < 0)                      /* (an LVS_OWNERDATA item: not ours) */
        return;
    for (int k = 0; k < it->nsub; k++)
        LocalFree(it->sub[k]);
    LocalFree(it->sub);
    LocalFree(it->text);
    LocalFree(it);
}

static bool set_sub(struct litem *it, int k, const char *s)
{
    if (k <= 0)
        return false;
    if (k >= it->nsub) {
        char **p = it->sub ? LocalReAlloc(it->sub, (uint32_t)(k + 1) * sizeof *p, 0)
                           : LocalAlloc(LMEM_FIXED, (uint32_t)(k + 1) * sizeof *p);
        if (!p)
            return false;
        for (int j = it->nsub; j <= k; j++)
            p[j] = NULL;
        it->sub = p;
        it->nsub = k + 1;
    }
    char *t = dup_text(s);
    if (!t)
        return false;
    LocalFree(it->sub[k]);
    it->sub[k] = t;
    return true;
}

static const char *text_of(const struct litem *it, int k)
{
    if (k == 0)
        return it->text;
    return k < it->nsub && it->sub[k] ? it->sub[k] : "";
}

static int insert_item(struct wnd *w, const LVITEM *li)
{
    struct ldata *d = data_of(w);
    if (!li || li->iSubItem)
        return -1;
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 32;
        struct litem **p = d->items ? LocalReAlloc(d->items, (uint32_t)cap * sizeof *p, 0)
                                    : LocalAlloc(LMEM_FIXED, (uint32_t)cap * sizeof *p);
        if (!p)
            return -1;
        d->items = p;
        d->cap = cap;
    }
    struct litem *it = LocalAlloc(LMEM_ZEROINIT, sizeof *it);
    if (!it)
        return -1;
    it->text = dup_text((li->mask & LVIF_TEXT) ? li->pszText : "");
    if (li->mask & LVIF_IMAGE)
        it->image = li->iImage;
    if (li->mask & LVIF_PARAM)
        it->param = li->lParam;
    int at = clampi(li->iItem, 0, d->n);
    memmove(&d->items[at + 1], &d->items[at], (size_t)(d->n - at) * sizeof *d->items);
    d->items[at] = it;
    d->n++;
    if (d->focus >= at) d->focus++;
    if (d->anchor >= at) d->anchor++;
    if (d->edit && d->edit_item >= at) d->edit_item++;
    if (li->mask & LVIF_STATE)
        set_state(w, at, li->stateMask & (LVIS_SELECTED | LVIS_FOCUSED | LVIS_CUT | LVIS_DROPHILITED |
                                          LVIS_STATEIMAGEMASK), li->state);
    InvalidateRect(w, NULL);
    return at;
}

static bool delete_item(struct wnd *w, int i)
{
    struct ldata *d = data_of(w);
    if (i < 0 || i >= d->n)
        return false;
    if (d->edit && d->edit_item == i)
        end_edit(w, false);
    lv_notify(w, LVN_DELETEITEM, i, 0, (POINT){ 0, 0 });
    if (!wnd_from(w) || i >= d->n)
        return false;
    free_item(d->items[i]);
    memmove(&d->items[i], &d->items[i + 1], (size_t)(d->n - i - 1) * sizeof *d->items);
    d->n--;
    if (d->focus == i) d->focus = -1; else if (d->focus > i) d->focus--;
    if (d->anchor == i) d->anchor = -1; else if (d->anchor > i) d->anchor--;
    if (d->edit && d->edit_item > i) d->edit_item--;
    clamp_scroll(w);
    InvalidateRect(w, NULL);
    return true;
}

static void sort_items(struct wnd *w, PFNLVCOMPARE cmp, LPARAM sort)
{
    struct ldata *d = data_of(w);
    if (!cmp || d->n < 2)
        return;
    end_edit(w, false);
    if (!wnd_from(w))
        return;
    struct litem *focus = d->focus >= 0 ? d->items[d->focus] : NULL;
    struct litem *anchor = d->anchor >= 0 ? d->items[d->anchor] : NULL;
    /* a merge sort (stable), through a scratch array */
    struct litem **tmp = LocalAlloc(LMEM_FIXED, (uint32_t)d->n * sizeof *tmp);
    if (!tmp)
        return;
    for (int width = 1; width < d->n; width *= 2) {
        for (int lo = 0; lo < d->n; lo += 2 * width) {
            int mid = lo + width < d->n ? lo + width : d->n, hi = lo + 2 * width < d->n ? lo + 2 * width : d->n;
            int a = lo, b = mid, o = lo;
            while (a < mid && b < hi)
                tmp[o++] = cmp(d->items[a]->param, d->items[b]->param, sort) <= 0 ? d->items[a++] : d->items[b++];
            while (a < mid) tmp[o++] = d->items[a++];
            while (b < hi) tmp[o++] = d->items[b++];
        }
        memcpy(d->items, tmp, (size_t)d->n * sizeof *tmp);
    }
    LocalFree(tmp);
    for (int i = 0; i < d->n; i++) {
        if (d->items[i] == focus) d->focus = i;
        if (d->items[i] == anchor) d->anchor = i;
    }
    InvalidateRect(w, NULL);
}

/* ---- painting -------------------------------------------------------------------------- */

static void paint_header(HDC dc, struct wnd *w, const struct lay *L)
{
    struct ldata *d = data_of(w);
    RECT bar = { 1, 1, w->w - 1, 1 + HDR_H };
    RECT saved;
    dc_clip(dc, bar, &saved);
    FillRect(dc, &bar, C_FACE);
    int x = L->cx - d->sx;
    for (int c = 0; c < d->ncol; c++) {
        RECT r = { x, 1, x + d->cols[c].cx, 1 + HDR_H };
        bool pressed = c == d->press_col;
        DrawBevel(dc, &r, !pressed);
        DrawLine(dc, r.right - 1, r.top, HDR_H, true, C_GRAY2);
        char t[64];
        fit_text(d->cols[c].text, d->cols[c].cx - 12, t, sizeof t);
        int tw = GUI_W(t), tx = x + 6, just = d->cols[c].fmt & LVCFMT_JUSTIFYMASK;
        bool arrow = (d->cols[c].fmt & (LVCFMT_SORTUP | LVCFMT_SORTDOWN)) != 0;
        if (just == LVCFMT_RIGHT) tx = r.right - 6 - tw - (arrow ? 12 : 0);
        else if (just == LVCFMT_CENTER) tx = x + (d->cols[c].cx - tw) / 2;
        TextOut(dc, tx + (pressed ? 1 : 0), 3 + (pressed ? 1 : 0), t, C_BLACK, -1);
        if (arrow && d->cols[c].cx > 24) {             /* the sort arrow */
            static const uint16_t up_[4] = { 0x1000, 0x3800, 0x7C00, 0xFE00 };
            static const uint16_t dn_[4] = { 0xFE00, 0x7C00, 0x3800, 0x1000 };
            int ax = just == LVCFMT_RIGHT ? r.right - 14 : tx + tw + 6;
            if (ax + 7 > r.right - 3) ax = r.right - 10;
            DrawBitmap(dc, ax, 8, (d->cols[c].fmt & LVCFMT_SORTUP) ? up_ : dn_, 4, C_GRAY2);
        }
        x += d->cols[c].cx;
    }
    if (x < w->w - 1) {
        RECT rest = { x, 1, w->w - 1, 1 + HDR_H };
        DrawBevel(dc, &rest, true);
    }
    dc_unclip(dc, &saved);
}

static void paint_item(HDC dc, struct wnd *w, const struct lay *L, int i, bool focus)
{
    struct ldata *d = data_of(w);
    struct litem *it = d->items[i];
    bool sel = it->state & LVIS_SELECTED, shown_sel = sel && (focus || (w->style & LVS_SHOWSELALWAYS));
    if (it->state & LVIS_DROPHILITED) { shown_sel = true; focus = true; }
    RECT icon, label;
    parts_of(w, L, i, &icon, &label);
    unsigned ild = it->state & LVIS_CUT ? ILD_BLEND25 : 0;
    if (checks(w)) {                                    /* the check box */
        RECT b = box_of(w, L, i), in = { b.left + 1, b.top + 1, b.right - 1, b.bottom - 1 };
        static const uint16_t tick[9] = { 0x0080, 0x0180, 0x0300, 0x8600, 0xCC00, 0x7800, 0x3000, 0, 0 };
        FrameRect(dc, &b, C_BLACK);
        FillRect(dc, &in, C_WHITE);
        if (check_of(it) == 2)
            DrawBitmap(dc, b.left + 2, b.top + 2, tick, 9, C_BLACK);
    }
    int view = view_of(w);
    HIMAGELIST il = view == LVS_ICON ? d->il_normal : d->il_small;
    uint8_t hl = focus ? C_ACCENT : C_GRAY5, fg = shown_sel && focus ? C_WHITE : C_BLACK;
    if (!w->enabled)
        fg = C_GRAY3;
    if (view == LVS_REPORT) {
        RECT row = to_client(w, L, cell_of(w, L, i));
        row.right += check_w(w);
        int sel_left = d->ex & LVS_EX_FULLROWSELECT ? row.left : label.left;
        if (shown_sel) {
            RECT r = { sel_left, row.top + 1, row.right, row.bottom - 1 };
            FillRect(dc, &r, hl);
        }
        if (il)
            ImageList_Draw(il, it->image, dc, icon.left, icon.top, (shown_sel && focus ? ILD_SELECTED : ILD_NORMAL) | ild);
        int x = row.left;
        for (int c = 0; c < d->ncol; c++) {
            int cw = d->cols[c].cx;
            int tx0 = c == 0 ? label.left + 2 : x + 6, room = c == 0 ? label.right - label.left - 4 : cw - 12;
            char t[128];
            fit_text(text_of(it, c), room, t, sizeof t);
            int tw = GUI_W(t), tx = tx0, just = d->cols[c].fmt & LVCFMT_JUSTIFYMASK;
            if (c > 0 && just == LVCFMT_RIGHT) tx = x + cw - 6 - tw;
            else if (c > 0 && just == LVCFMT_CENTER) tx = x + (cw - tw) / 2;
            TextOut(dc, tx + (c > 0 ? check_w(w) : 0), row.top + 1, t, fg, -1);
            x += cw;
        }
        if (i == d->focus && focus) {
            RECT f = { sel_left, row.top + 1, row.right, row.bottom - 1 };
            FrameRect(dc, &f, C_ACCENT_DK);
        }
        return;
    }
    if (il)
        ImageList_Draw(il, it->image, dc, icon.left, icon.top, (shown_sel && focus ? ILD_SELECTED : ILD_NORMAL) | ild);
    if (shown_sel)
        FillRect(dc, &label, hl);
    if (view == LVS_ICON) {
        char lines[LINE_MAX][64];
        int n = wrap(it->text, CELL_W - 6, lines, show_all_lines(w, i) ? LINE_MAX : 2);
        RECT c = to_client(w, L, cell_of(w, L, i));
        for (int k = 0; k < n; k++)
            TextOut(dc, c.left + (CELL_W - GUI_W(lines[k])) / 2, label.top + k * 16, lines[k], fg, -1);
    } else {
        char t[128];
        fit_text(it->text, label.right - label.left - 4, t, sizeof t);
        TextOut(dc, label.left + 2, label.top, t, fg, -1);
    }
    if (i == d->focus && focus)
        FrameRect(dc, &label, C_ACCENT_DK);
}

static void paint(HDC dc, struct wnd *w)
{
    struct ldata *d = data_of(w);
    struct lay L;
    layout(w, &L);
    bool focus = ui_has_focus(w) || (d->edit && GetFocus() == d->edit);
    RECT o = { 0, 0, w->w, w->h };
    FrameRect(dc, &o, focus ? C_ACCENT : C_GRAY1);
    if (L.hdr)
        paint_header(dc, w, &L);
    RECT c = { L.cx, L.cy, L.cx + L.cw, L.cy + L.ch };
    FillRect(dc, &c, C_WHITE);
    RECT saved;
    dc_clip(dc, c, &saved);
    for (int i = 0; i < d->n; i++) {
        RECT r = to_client(w, &L, cell_of(w, &L, i));
        if (r.bottom + 48 < c.top || r.top > c.bottom || r.right < c.left || r.left > c.right)
            continue;
        if (i == d->edit_item && d->edit)
            continue;
        paint_item(dc, w, &L, i, focus);
    }
    if ((d->ex & LVS_EX_GRIDLINES) && view_of(w) == LVS_REPORT) {   /* the grid */
        int x = L.cx - d->sx;
        for (int k = 0; k < d->ncol; k++) {
            x += d->cols[k].cx + (k == 0 ? check_w(w) : 0);
            DrawLine(dc, x - 1, L.cy, L.ch, true, C_GRAY5);
        }
        for (int y = L.cy - d->sy % ROW_H + ROW_H - 1; y < L.cy + L.ch; y += ROW_H)
            DrawLine(dc, L.cx, y, L.cw, false, C_GRAY5);
    }
    if (d->marquee) {                                   /* the marquee, dotted */
        RECT m = { d->mx0 < d->mx1 ? d->mx0 : d->mx1, d->my0 < d->my1 ? d->my0 : d->my1,
                   d->mx0 < d->mx1 ? d->mx1 : d->mx0, d->my0 < d->my1 ? d->my1 : d->my0 };
        m = to_client(w, &L, m);
        for (int x = m.left; x < m.right; x += 2) {
            SetPixel(dc, x, m.top, C_BLACK);
            SetPixel(dc, x, m.bottom - 1, C_BLACK);
        }
        for (int y = m.top; y < m.bottom; y += 2) {
            SetPixel(dc, m.left, y, C_BLACK);
            SetPixel(dc, m.right - 1, y, C_BLACK);
        }
    }
    dc_unclip(dc, &saved);
    if (L.vb) {
        struct sbar s = vbar(w, &L);
        sbar_draw(dc, &s, w->tracking && d->bar == 0 ? w->part : SB_PART_NONE);
    }
    if (L.hb) {
        struct sbar s = hbar(w, &L);
        sbar_draw(dc, &s, w->tracking && d->bar == 1 ? w->part : SB_PART_NONE);
    }
    if (L.vb && L.hb) {
        RECT corner = { L.cx + L.cw, L.cy + L.ch, L.cx + L.cw + SCROLL_W, L.cy + L.ch + SCROLL_W };
        FillRect(dc, &corner, C_FACE);
    }
}

/* ---- input ------------------------------------------------------------------------------ */

static int hit(struct wnd *w, int x, int y, unsigned *flags)
{
    struct ldata *d = data_of(w);
    struct lay L;
    layout(w, &L);
    *flags = LVHT_NOWHERE;
    if (x < L.cx || y < L.cy || x >= L.cx + L.cw || y >= L.cy + L.ch)
        return -1;
    int px = x - L.cx + d->sx, py = y - L.cy + d->sy, i;
    if (checks(w)) {                                    /* a check box? (the visible ones) */
        for (int k = 0; k < d->n; k++) {
            RECT b = box_of(w, &L, k);
            if (rect_contains(&b, x, y)) { *flags = LVHT_ONITEMSTATEICON; return k; }
        }
    }
    switch (view_of(w)) {
    case LVS_ICON:
    case LVS_SMALLICON:
        if (d->any_placed) {                            /* icons anywhere: the topmost there */
            for (int k = d->n - 1; k >= 0; k--) {
                RECT icon, label;
                parts_of(w, &L, k, &icon, &label);
                if (rect_contains(&icon, x, y)) { *flags = LVHT_ONITEMICON; return k; }
                if (rect_contains(&label, x, y)) { *flags = LVHT_ONITEMLABEL; return k; }
            }
            return -1;
        }
        if (px >= L.per * L.cellw)
            return -1;
        i = py / L.cellh * L.per + px / L.cellw;
        break;
    case LVS_LIST:
        if (py >= L.per * ROW_H)
            return -1;
        i = px / L.cellw * L.per + py / ROW_H;
        break;
    default:
        if (px >= L.cw_all)
            return -1;
        i = py / ROW_H;
        if (i < d->n)
            *flags = px < 22 + check_w(w) ? LVHT_ONITEMICON : LVHT_ONITEMLABEL;
        return i < d->n ? i : -1;
    }
    if (i < 0 || i >= d->n)
        return -1;
    RECT icon, label;
    parts_of(w, &L, i, &icon, &label);
    if (rect_contains(&icon, x, y)) { *flags = LVHT_ONITEMICON; return i; }
    if (rect_contains(&label, x, y)) { *flags = LVHT_ONITEMLABEL; return i; }
    return -1;
}

/* The header column at x, and whether x is on its right edge (for resizing). */
static int header_at(struct wnd *w, const struct lay *L, int x, bool *edge)
{
    struct ldata *d = data_of(w);
    int cx = L->cx - d->sx;
    *edge = false;
    for (int c = 0; c < d->ncol; c++) {
        int right = cx + d->cols[c].cx;
        if (x >= right - 4 && x <= right + 3) {
            *edge = true;
            return c;
        }
        if (x >= cx && x < right)
            return c;
        cx = right;
    }
    return -1;
}

static void bar_click(struct wnd *w, const struct sbar *s, int bar, int x, int y, const struct lay *L)
{
    struct ldata *d = data_of(w);
    d->bar = bar;
    w->part = sbar_hit(s, x, y);
    w->tracking = true;
    SetCapture(w);
    int line = bar == 0 ? (view_of(w) == LVS_ICON ? CELL_H / 2 : ROW_H) : (view_of(w) == LVS_LIST ? L->cellw : 24);
    int cur = bar == 0 ? d->sy : d->sx, to = cur;
    switch (w->part) {
    case SB_PART_DEC:   to = cur - line; break;
    case SB_PART_INC:   to = cur + line; break;
    case SB_PART_PGDEC: to = cur - s->page; break;
    case SB_PART_PGINC: to = cur + s->page; break;
    case SB_PART_THUMB: {
        int pos, len, track, tlen;
        sbar_thumb(s, &pos, &len, &track, &tlen);
        w->grab = (bar == 0 ? y : x) - pos;
        break;
    }
    }
    if (bar == 0) scroll_to(w, d->sx, to); else scroll_to(w, to, d->sy);
    InvalidateRect(w, NULL);
}

static void key(struct wnd *w, unsigned vk)
{
    struct ldata *d = data_of(w);
    struct lay L;
    layout(w, &L);
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    int view = view_of(w), f = d->focus >= 0 ? d->focus : -1, to = -2;
    int rows_vis = L.ch / L.cellh > 0 ? L.ch / L.cellh : 1;
    switch (vk) {
    case VK_HOME:  to = 0; break;
    case VK_END:   to = d->n - 1; break;
    case VK_UP:    to = f < 0 ? 0 : view == LVS_ICON || view == LVS_SMALLICON ? f - L.per : f - 1; break;
    case VK_DOWN:  to = f < 0 ? 0 : view == LVS_ICON || view == LVS_SMALLICON ? f + L.per : f + 1; break;
    case VK_LEFT:
        if (view == LVS_REPORT) { scroll_to(w, d->sx - 24, d->sy); return; }
        to = f < 0 ? 0 : view == LVS_LIST ? f - L.per : f - 1;
        break;
    case VK_RIGHT:
        if (view == LVS_REPORT) { scroll_to(w, d->sx + 24, d->sy); return; }
        to = f < 0 ? 0 : view == LVS_LIST ? f + L.per : f + 1;
        break;
    case VK_PRIOR:
        to = f < 0 ? 0 : view == LVS_LIST ? f - L.per * (L.cw / L.cellw > 1 ? L.cw / L.cellw : 1)
                                          : f - (rows_vis - 1 > 0 ? rows_vis - 1 : 1) * L.per;
        break;
    case VK_NEXT:
        to = f < 0 ? 0 : view == LVS_LIST ? f + L.per * (L.cw / L.cellw > 1 ? L.cw / L.cellw : 1)
                                          : f + (rows_vis - 1 > 0 ? rows_vis - 1 : 1) * L.per;
        break;
    case VK_SPACE:
        if (f >= 0 && checks(w) && !ctrl) {             /* the focused item's box */
            set_state(w, f, LVIS_STATEIMAGEMASK, INDEXTOSTATEIMAGEMASK(check_of(d->items[f]) == 2 ? 1 : 2));
            return;
        }
        if (f >= 0) {
            if (ctrl && !(w->style & LVS_SINGLESEL))
                set_state(w, f, LVIS_SELECTED, (d->items[f]->state & LVIS_SELECTED) ? 0 : LVIS_SELECTED);
            else
                choose(w, f, false, false, true);
        }
        return;
    case VK_RETURN: {
        NMHDR nm;
        ui_notify(w, &nm, NM_RETURN);
        if (wnd_from(w) && f >= 0)
            lv_notify(w, LVN_ITEMACTIVATE, f, 0, (POINT){ 0, 0 });
        return;
    }
    case VK_F1 + 1:                                     /* F2 */
        if ((w->style & LVS_EDITLABELS) && f >= 0)
            begin_edit(w, f);
        return;
    }
    if (to == -2 || !d->n)
        return;
    to = clampi(to, 0, d->n - 1);
    choose(w, to, ctrl, shift, true);
}

static void typed(struct wnd *w, char ch)
{
    struct ldata *d = data_of(w);
    if ((unsigned char)ch <= ' ' || !d->n)
        return;
    int start = d->focus >= 0 ? d->focus : -1;
    for (int k = 1; k <= d->n; k++) {
        int i = (start + k) % d->n;
        vfetch(w, i);
        if (up(d->items[i]->text[0]) == up(ch)) {
            choose(w, i, false, false, true);
            return;
        }
    }
}

static void set_view(struct wnd *w, int view)
{
    struct ldata *d = data_of(w);
    end_edit(w, true);
    if (!wnd_from(w))
        return;
    w->style = (w->style & ~(unsigned)LVS_TYPEMASK) | (unsigned)(view & LVS_TYPEMASK);
    d->sx = d->sy = 0;
    if (d->focus >= 0)
        ensure_visible(w, d->focus);
    InvalidateRect(w, NULL);
}

LRESULT listview_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct ldata *d = data_of(w);
    if (!d && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    int i = (int)wp;
    switch (msg) {
    case WM_CREATE:
        if (!(w->ctl = LocalAlloc(LMEM_ZEROINIT, sizeof *d)))
            return -1;
        d = data_of(w);
        d->focus = d->anchor = d->press_col = d->drag_col = d->press_item = -1;
        return 0;
    case WM_DESTROY:
        if (d->edit && IsWindow(d->edit))
            DestroyWindow(d->edit);
        d->edit = NULL;
        for (int k = 0; k < d->n; k++) {
            lv_notify(w, LVN_DELETEITEM, k, 0, (POINT){ 0, 0 });
            free_item(d->items[k]);
        }
        LocalFree(d->items);
        LocalFree(d->vblock);
        LocalFree(d);
        w->ctl = NULL;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SelectFont(dc, FONT_GUI);
        paint(dc, w);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return 0;
    case WM_STYLECHANGED:
        set_view(w, view_of(w));
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        InvalidateRect(h, NULL);
        NMHDR nm;
        ui_notify(w, &nm, msg == WM_SETFOCUS ? NM_SETFOCUS : NM_KILLFOCUS);
        return 0;
    }
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (d->edit)
            end_edit(w, true);
        if (!wnd_from(h))
            return 0;
        SetFocus(h);
        if (!wnd_from(h))
            return 0;
        struct lay L;
        layout(w, &L);
        if (msg != WM_RBUTTONDOWN && L.vb && x >= L.cx + L.cw && y >= L.cy && y < L.cy + L.ch) {
            struct sbar s = vbar(w, &L);
            bar_click(w, &s, 0, x, y, &L);
            return 0;
        }
        if (msg != WM_RBUTTONDOWN && L.hb && y >= L.cy + L.ch && x < L.cx + L.cw) {
            struct sbar s = hbar(w, &L);
            bar_click(w, &s, 1, x, y, &L);
            return 0;
        }
        if (L.hdr && y < L.cy) {                        /* the header */
            if (msg == WM_RBUTTONDOWN)
                return 0;
            bool edge;
            int c = header_at(w, &L, x, &edge);
            if (c < 0)
                return 0;
            if (edge) {
                d->drag_col = c;
                d->drag_x0 = x;
                d->drag_cx0 = d->cols[c].cx;
            } else {
                d->press_col = c;
            }
            SetCapture(h);
            InvalidateRect(h, NULL);
            return 0;
        }
        unsigned flags;
        int it = hit(w, x, y, &flags);
        bool ctrl = (wp & 0x0008) || GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (it >= 0 && flags == LVHT_ONITEMSTATEICON && msg != WM_RBUTTONDOWN) {   /* a check box */
            set_state(w, it, LVIS_STATEIMAGEMASK, INDEXTOSTATEIMAGEMASK(check_of(d->items[it]) == 2 ? 1 : 2));
            return 0;
        }
        if (it < 0) {
            if (!ctrl && msg != WM_LBUTTONDBLCLK)
                select_range(w, -1, -1, false);
            if (wnd_from(h) && msg == WM_LBUTTONDOWN && !(w->style & LVS_SINGLESEL)) {   /* a marquee */
                d->marquee = true;
                d->mx0 = d->mx1 = x - L.cx + d->sx;
                d->my0 = d->my1 = y - L.cy + d->sy;
                SetCapture(h);
            }
            if (wnd_from(h) && msg == WM_RBUTTONDOWN) {
                NMITEMACTIVATE nm;
                memset(&nm, 0, sizeof nm);
                nm.iItem = -1;
                nm.ptAction = (POINT){ x, y };
                ui_notify(w, &nm.hdr, NM_RCLICK);
            }
            return 0;
        }
        if (msg == WM_RBUTTONDOWN) {
            if (!(d->items[it]->state & LVIS_SELECTED))
                choose(w, it, false, false, false);
            else
                set_state(w, it, LVIS_FOCUSED, LVIS_FOCUSED);
        } else if (msg == WM_LBUTTONDBLCLK) {
            choose(w, it, false, false, false);
        } else {
            choose(w, it, ctrl, shift, false);
        }
        if (!wnd_from(h))
            return 0;
        NMITEMACTIVATE nm;
        memset(&nm, 0, sizeof nm);
        nm.iItem = it;
        nm.ptAction = (POINT){ x, y };
        nm.lParam = it < d->n ? d->items[it]->param : 0;
        int code = msg == WM_RBUTTONDOWN ? NM_RCLICK : msg == WM_LBUTTONDBLCLK ? NM_DBLCLK : NM_CLICK;
        ui_notify(w, &nm.hdr, code);
        if (code == NM_DBLCLK && wnd_from(h))
            lv_notify(w, LVN_ITEMACTIVATE, it, 0, (POINT){ x, y });
        if (code == NM_CLICK && wnd_from(h) && it < d->n && (d->items[it]->state & LVIS_SELECTED)) {
            d->press_item = it;                         /* held: perhaps a drag */
            d->press_x = x;
            d->press_y = y;
            SetCapture(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (d->marquee) {                               /* select what it touches */
            struct lay L;
            layout(w, &L);
            d->mx1 = x - L.cx + d->sx;
            d->my1 = y - L.cy + d->sy;
            RECT m = { d->mx0 < d->mx1 ? d->mx0 : d->mx1, d->my0 < d->my1 ? d->my0 : d->my1,
                       d->mx0 < d->mx1 ? d->mx1 : d->mx0, d->my0 < d->my1 ? d->my1 : d->my0 };
            RECT mc = to_client(w, &L, m), in;
            bool keep = GetKeyState(VK_CONTROL) < 0;
            for (int k = 0; k < d->n && wnd_from(h); k++) {
                RECT icon, label, cell = to_client(w, &L, cell_of(w, &L, k));
                if (cell.bottom < L.cy - 64 || cell.top > L.cy + L.ch + 64) {
                    if (!keep && !rect_intersect(cell, mc, &in) && (d->items[k]->state & LVIS_SELECTED))
                        set_state(w, k, LVIS_SELECTED, 0);
                    continue;
                }
                parts_of(w, &L, k, &icon, &label);
                bool touch = rect_intersect(icon, mc, &in) || rect_intersect(label, mc, &in);
                if (touch) set_state(w, k, LVIS_SELECTED, LVIS_SELECTED);
                else if (!keep) set_state(w, k, LVIS_SELECTED, 0);
            }
            if (y < L.cy) scroll_to(w, d->sx, d->sy - ROW_H);                 /* past the edge: scroll */
            else if (y >= L.cy + L.ch) scroll_to(w, d->sx, d->sy + ROW_H);
            InvalidateRect(h, NULL);
            return 0;
        }
        if (d->press_item >= 0 && (GET_X_LPARAM(lp) - d->press_x) * (GET_X_LPARAM(lp) - d->press_x) +
            (GET_Y_LPARAM(lp) - d->press_y) * (GET_Y_LPARAM(lp) - d->press_y) > 16) {
            int it = d->press_item;                     /* a drag: the parent's, or icons moving */
            d->press_item = -1;
            ReleaseCapture();
            if (lv_notify(w, LVN_BEGINDRAG, it, 0, (POINT){ d->press_x, d->press_y }) || !wnd_from(h))
                return 0;
            if (view_of(w) == LVS_ICON || view_of(w) == LVS_SMALLICON) {
                d->moving = true;
                SetCapture(h);
                SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            }
            return 0;
        }
        if (d->moving) {
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            return 0;
        }
        if (!w->tracking) {                             /* a column's edge: the sizing arrows */
            struct lay L;
            layout(w, &L);
            bool edge = false;
            if (d->drag_col < 0 && L.hdr && y < L.cy)
                header_at(w, &L, x, &edge);
            SetCursor(LoadCursor(NULL, edge || d->drag_col >= 0 ? IDC_SIZEWE : IDC_ARROW));
        }
        if (d->drag_col >= 0) {
            d->cols[d->drag_col].cx = clampi(d->drag_cx0 + x - d->drag_x0, 8, 2000);
            clamp_scroll(w);
            InvalidateRect(h, NULL);
        } else if (w->tracking && w->part == SB_PART_THUMB) {
            struct lay L;
            layout(w, &L);
            if (d->bar == 0) {
                struct sbar s = vbar(w, &L);
                scroll_to(w, d->sx, sbar_thumb_value(&s, y, w->grab));
            } else {
                struct sbar s = hbar(w, &L);
                scroll_to(w, sbar_thumb_value(&s, x, w->grab), d->sy);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (d->marquee) {
            d->marquee = false;
            ReleaseCapture();
            InvalidateRect(h, NULL);
            return 0;
        }
        if (d->press_item >= 0) {
            d->press_item = -1;
            ReleaseCapture();
            return 0;
        }
        if (d->moving) {                                /* the selected icons go where dropped */
            d->moving = false;
            ReleaseCapture();
            struct lay L;
            layout(w, &L);
            int dx = x - d->press_x, dy = y - d->press_y;
            for (int k = 0; k < d->n; k++) {            /* everything gets a place, first */
                RECT c = cell_of(w, &L, k);
                d->items[k]->px = (int16_t)c.left;
                d->items[k]->py = (int16_t)c.top;
                d->items[k]->placed = true;
            }
            for (int k = 0; k < d->n; k++)
                if (d->items[k]->state & LVIS_SELECTED) {
                    d->items[k]->px = (int16_t)clampi(d->items[k]->px + dx, 0, 30000);
                    d->items[k]->py = (int16_t)clampi(d->items[k]->py + dy, 0, 30000);
                }
            d->any_placed = true;
            clamp_scroll(w);
            InvalidateRect(h, NULL);
            return 0;
        }
        if (d->drag_col >= 0) {
            d->drag_col = -1;
            ReleaseCapture();
            return 0;
        }
        if (d->press_col >= 0) {
            int c = d->press_col;
            d->press_col = -1;
            ReleaseCapture();
            InvalidateRect(h, NULL);
            struct lay L;
            layout(w, &L);
            bool edge;
            if (y >= 0 && y < L.cy && header_at(w, &L, x, &edge) == c)
                lv_notify(w, LVN_COLUMNCLICK, -1, c, (POINT){ x, y });
            return 0;
        }
        if (w->tracking) {
            ReleaseCapture();
            w->tracking = false;
            w->part = SB_PART_NONE;
            InvalidateRect(h, NULL);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        NMLVKEYDOWN nm;
        memset(&nm, 0, sizeof nm);
        nm.wVKey = (uint16_t)wp;
        ui_notify(w, &nm.hdr, LVN_KEYDOWN);
        if (wnd_from(h))
            key(w, wp);
        return 0;
    }
    case WM_CHAR:
        typed(w, (char)wp);
        return 0;

    case LVM_GETIMAGELIST:
        return (LRESULT)(uintptr_t)(wp == LVSIL_SMALL ? d->il_small : d->il_normal);
    case LVM_SETIMAGELIST: {
        HIMAGELIST *slot = wp == LVSIL_SMALL ? &d->il_small : &d->il_normal, old = *slot;
        *slot = (HIMAGELIST)lp;
        InvalidateRect(h, NULL);
        return (LRESULT)(uintptr_t)old;
    }
    case LVM_GETITEMCOUNT:
        return d->n;
    case LVM_GETITEM: {
        LVITEM *li = (LVITEM *)lp;
        if (!li || li->iItem < 0 || li->iItem >= d->n)
            return 0;
        struct litem *it = d->items[li->iItem];
        if ((li->mask & LVIF_TEXT) && li->pszText && li->cchTextMax > 0)
            strncpy0(li->pszText, text_of(it, li->iSubItem), (size_t)li->cchTextMax);
        if (li->mask & LVIF_IMAGE)
            li->iImage = it->image;
        if (li->mask & LVIF_PARAM)
            li->lParam = it->param;
        if (li->mask & LVIF_STATE)
            li->state = it->state & (li->stateMask ? li->stateMask : ~0u);
        return 1;
    }
    case LVM_SETITEM: {
        LVITEM *li = (LVITEM *)lp;
        if (!li || li->iItem < 0 || li->iItem >= d->n)
            return 0;
        struct litem *it = d->items[li->iItem];
        if (li->mask & LVIF_TEXT) {
            if (li->iSubItem) {
                set_sub(it, li->iSubItem, li->pszText);
            } else {
                char *s = dup_text(li->pszText);
                if (s) {
                    LocalFree(it->text);
                    it->text = s;
                }
            }
        }
        if (li->iSubItem == 0) {
            if (li->mask & LVIF_IMAGE)
                it->image = li->iImage;
            if (li->mask & LVIF_PARAM)
                it->param = li->lParam;
            if (li->mask & LVIF_STATE)
                set_state(w, li->iItem, li->stateMask & (LVIS_SELECTED | LVIS_FOCUSED), li->state);
        }
        InvalidateRect(h, NULL);
        return 1;
    }
    case LVM_INSERTITEM:
        return owner_data(w) ? -1 : insert_item(w, (const LVITEM *)lp);
    case LVM_DELETEITEM:
        return owner_data(w) ? 0 : delete_item(w, i);
    case LVM_DELETEALLITEMS:
        if (owner_data(w))
            return listview_proc(h, LVM_SETITEMCOUNT, 0, 0);
        end_edit(w, false);
        while (wnd_from(h) && d->n)
            delete_item(w, d->n - 1);
        d->focus = d->anchor = -1;
        d->sx = d->sy = 0;
        InvalidateRect(h, NULL);
        return 1;
    case LVM_GETNEXTITEM:
        for (int k = i < 0 ? 0 : i + 1; k < d->n; k++) {
            unsigned st = d->items[k]->state;
            if ((!(lp & LVNI_SELECTED) || (st & LVIS_SELECTED)) && (!(lp & LVNI_FOCUSED) || (st & LVIS_FOCUSED)))
                return k;
        }
        return -1;
    case LVM_GETITEMRECT: {
        RECT *r = (RECT *)lp;
        if (!r || i < 0 || i >= d->n)
            return 0;
        struct lay L;
        layout(w, &L);
        RECT icon, label;
        parts_of(w, &L, i, &icon, &label);
        int part = r->left;
        if (part == LVIR_ICON) *r = icon;
        else if (part == LVIR_LABEL) *r = label;
        else *r = to_client(w, &L, cell_of(w, &L, i));
        return 1;
    }
    case LVM_HITTEST: {
        LVHITTESTINFO *ht = (LVHITTESTINFO *)lp;
        if (!ht)
            return -1;
        ht->iItem = hit(w, ht->pt.x, ht->pt.y, &ht->flags);
        ht->iSubItem = 0;
        return ht->iItem;
    }
    case LVM_ENSUREVISIBLE:
        ensure_visible(w, i);
        return 1;
    case LVM_EDITLABEL:
        if (i < 0) {
            end_edit(w, false);
            return 0;
        }
        return (LRESULT)(uintptr_t)begin_edit(w, i);
    case LVM_GETCOLUMN: {
        LVCOLUMN *c = (LVCOLUMN *)lp;
        if (!c || i < 0 || i >= d->ncol)
            return 0;
        if (c->mask & LVCF_FMT) c->fmt = d->cols[i].fmt;
        if (c->mask & LVCF_WIDTH) c->cx = d->cols[i].cx;
        if (c->mask & LVCF_SUBITEM) c->iSubItem = i;
        if ((c->mask & LVCF_TEXT) && c->pszText && c->cchTextMax > 0)
            strncpy0(c->pszText, d->cols[i].text, (size_t)c->cchTextMax);
        return 1;
    }
    case LVM_SETCOLUMN:
    case LVM_INSERTCOLUMN: {
        const LVCOLUMN *c = (const LVCOLUMN *)lp;
        if (!c)
            return -1;
        int at = i;
        if (msg == LVM_INSERTCOLUMN) {
            if (d->ncol == COL_MAX)
                return -1;
            at = clampi(i, 0, d->ncol);
            memmove(&d->cols[at + 1], &d->cols[at], (size_t)(d->ncol - at) * sizeof *d->cols);
            d->cols[at] = (struct lcol){ "", 100, LVCFMT_LEFT };
            d->ncol++;
        } else if (at < 0 || at >= d->ncol) {
            return 0;
        }
        if (c->mask & LVCF_FMT) d->cols[at].fmt = c->fmt;
        if (c->mask & LVCF_WIDTH) d->cols[at].cx = clampi(c->cx, 8, 2000);
        if (c->mask & LVCF_TEXT) strncpy0(d->cols[at].text, c->pszText ? c->pszText : "", sizeof d->cols[at].text);
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return msg == LVM_INSERTCOLUMN ? at : 1;
    }
    case LVM_DELETECOLUMN:
        if (i < 0 || i >= d->ncol)
            return 0;
        memmove(&d->cols[i], &d->cols[i + 1], (size_t)(d->ncol - i - 1) * sizeof *d->cols);
        d->ncol--;
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return 1;
    case LVM_GETCOLUMNWIDTH:
        return i >= 0 && i < d->ncol ? d->cols[i].cx : 0;
    case LVM_SETCOLUMNWIDTH:
        if (i < 0 || i >= d->ncol)
            return 0;
        d->cols[i].cx = clampi((int)lp, 8, 2000);
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return 1;
    case LVM_SETITEMSTATE: {
        const LVITEM *li = (const LVITEM *)lp;
        if (!li)
            return 0;
        unsigned mask = li->stateMask & (LVIS_SELECTED | LVIS_FOCUSED | LVIS_CUT | LVIS_DROPHILITED |
                                         LVIS_STATEIMAGEMASK);
        if (i == -1) {
            for (int k = 0; k < d->n && wnd_from(h); k++)
                set_state(w, k, mask, li->state);
        } else if (i >= 0 && i < d->n) {
            set_state(w, i, mask, li->state);
            if ((li->state & mask & LVIS_SELECTED) && d->anchor < 0)
                d->anchor = i;
        } else {
            return 0;
        }
        return 1;
    }
    case LVM_GETITEMSTATE:
        return i >= 0 && i < d->n ? (LRESULT)(d->items[i]->state & (unsigned)lp) : 0;
    case LVM_GETITEMTEXT: {
        LVITEM *li = (LVITEM *)lp;
        if (!li || i < 0 || i >= d->n || !li->pszText || li->cchTextMax <= 0)
            return 0;
        vfetch(w, i);
        strncpy0(li->pszText, li->iSubItem && owner_data(w) ? "" : text_of(d->items[i], li->iSubItem),
                 (size_t)li->cchTextMax);
        return (LRESULT)strlen(li->pszText);
    }
    case LVM_SETITEMTEXT: {
        LVITEM *li = (LVITEM *)lp;
        if (!li || i < 0 || i >= d->n)
            return 0;
        LVITEM t = *li;
        t.iItem = i;
        t.mask = LVIF_TEXT;
        return listview_proc(h, LVM_SETITEM, 0, (LPARAM)&t);
    }
    case LVM_SORTITEMS:
        sort_items(w, (PFNLVCOMPARE)lp, (LPARAM)wp);
        return 1;
    case LVM_GETSELECTEDCOUNT: {
        int n = 0;
        for (int k = 0; k < d->n; k++)
            n += (d->items[k]->state & LVIS_SELECTED) != 0;
        return n;
    }
    case LVM_SETVIEW:
        if (wp > LV_VIEW_LIST)
            return -1;
        set_view(w, (int)wp);
        return 1;
    case LVM_GETVIEW:
        return view_of(w);
    case LVM_SETEXTENDEDLISTVIEWSTYLE: {
        unsigned old = d->ex, mask = wp ? (unsigned)wp : 0xFFFFFFFFu;
        d->ex = (d->ex & ~mask) | ((unsigned)lp & mask);
        if ((d->ex & LVS_EX_CHECKBOXES) && !(old & LVS_EX_CHECKBOXES))
            for (int k = 0; k < d->n; k++)              /* boxes start unchecked */
                if (!check_of(d->items[k]))
                    d->items[k]->state |= INDEXTOSTATEIMAGEMASK(1);
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return old;
    }
    case LVM_GETEXTENDEDLISTVIEWSTYLE:
        return d->ex;
    case LVM_SETITEMPOSITION:
        if (i < 0 || i >= d->n) return 0;
        if (!d->any_placed) {                           /* the others keep their grid places */
            struct lay L;
            layout(w, &L);
            for (int k = 0; k < d->n; k++) {
                RECT c = cell_of(w, &L, k);
                d->items[k]->px = (int16_t)c.left;
                d->items[k]->py = (int16_t)c.top;
                d->items[k]->placed = true;
            }
            d->any_placed = true;
        }
        d->items[i]->px = (int16_t)GET_X_LPARAM(lp);
        d->items[i]->py = (int16_t)GET_Y_LPARAM(lp);
        d->items[i]->placed = true;
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return 1;
    case LVM_GETITEMPOSITION: {
        if (i < 0 || i >= d->n || !lp) return 0;
        struct lay L;
        layout(w, &L);
        RECT c = cell_of(w, &L, i);
        ((POINT *)lp)->x = c.left;
        ((POINT *)lp)->y = c.top;
        return 1;
    }
    case LVM_ARRANGE:
        for (int k = 0; k < d->n; k++)
            d->items[k]->placed = false;
        d->any_placed = false;
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return 1;
    case LVM_SETITEMCOUNT: {                            /* LVS_OWNERDATA */
        if (!owner_data(w)) return 0;
        int n = clampi((int)wp, 0, 32767);
        end_edit(w, false);
        struct litem *blk = n ? LocalAlloc(LMEM_ZEROINIT, (uint32_t)n * sizeof *blk) : NULL;
        struct litem **ptr = n ? LocalAlloc(LMEM_FIXED, (uint32_t)n * sizeof *ptr) : NULL;
        if (n && (!blk || !ptr)) { LocalFree(blk); LocalFree(ptr); return 0; }
        for (int k = 0; k < n; k++) {
            if (k < d->n && d->vblock) blk[k].state = d->vblock[k].state;   /* states kept */
            else if (checks(w)) blk[k].state = INDEXTOSTATEIMAGEMASK(1);
            blk[k].nsub = -1;                           /* (texts aren't ours to free) */
            blk[k].text = (char *)"";
            ptr[k] = &blk[k];
        }
        LocalFree(d->items);
        LocalFree(d->vblock);
        d->items = ptr;
        d->vblock = blk;
        d->n = d->cap = n;
        if (d->focus >= n) d->focus = -1;
        if (d->anchor >= n) d->anchor = -1;
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return 1;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}
