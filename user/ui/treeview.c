/*
 * treeview.c -- the tree view class (WC_TREEVIEW): items under items, a row
 * each when their parents are open, with +/- boxes, dotted lines, an image
 * from the image list and a label. Scroll bars appear as needed (rows up
 * and down, pixels sideways).
 *
 * The items live in the owning process's heap (w->ctl); an HTREEITEM points
 * to one. w->top = the first row shown. Notifications go to the parent as
 * WM_NOTIFY (NMTREEVIEW): selection changing and changed, expanding and
 * expanded (by the user: TVM_EXPAND tells nobody, as in Windows), an item
 * deleted (so its lParam can be freed), keys, clicks.
 *
 * TVS_CHECKBOXES puts a check box before each item (its state image: 1
 * unchecked, 2 checked; a click or Space toggles it). TVS_EDITLABELS lets
 * F2 or TVM_EDITLABEL edit a label in place (TVN_BEGINLABELEDIT,
 * TVN_ENDLABELEDIT). Dragging an item sends TVN_BEGINDRAG; the dragger
 * marks the item under the pointer with TVM_SELECTITEM TVGN_DROPHILITE.
 */
#include "uipriv.h"

#define ROW_H    18
#define INDENT   19
#define BTN      9              /* the +/- box */
#define EDIT_ID  0x7E02

struct tree_item {
    struct tree_item *parent, *first, *last, *next, *prev;
    char    *text;
    int      image, simage, children;
    unsigned state;
    LPARAM   param;
};

struct tdata {
    struct tree_item root;      /* its children are the top-level items */
    struct tree_item *caret;    /* the selection */
    HIMAGELIST il;
    int      sx;                /* pixels scrolled sideways */
    int      count;
    int      bar;               /* tracking: 0 vertical, 1 horizontal */
    struct tree_item *drop;     /* TVGN_DROPHILITE */
    struct tree_item *press;    /* a button down on it: a drag? */
    int      press_x, press_y;
    HWND     edit;              /* a label being edited */
    struct tree_item *edit_item;
    WNDPROC  edit_old;
};

struct lay { int cx, cy, cw, ch, rows, vis, maxw; bool vb, hb; };

static struct tdata *data_of(struct wnd *w) { return (struct tdata *)w->ctl; }

static bool is_item(struct tdata *d, HTREEITEM it)  /* (not a TVI_* value) */
{
    return it && it != &d->root && (uintptr_t)it < 0xFFFF0000u;
}

/* ---- walking the rows ---------------------------------------------------------- */

static bool open_(const struct tree_item *it) { return (it->state & TVIS_EXPANDED) && it->first; }

static struct tree_item *next_vis(struct tdata *d, struct tree_item *it)
{
    if (open_(it))
        return it->first;
    for (; it && it != &d->root; it = it->parent)
        if (it->next)
            return it->next;
    return NULL;
}

static struct tree_item *prev_vis(struct tdata *d, struct tree_item *it)
{
    if (it->prev) {
        it = it->prev;
        while (open_(it))
            it = it->last;
        return it;
    }
    return it->parent == &d->root ? NULL : it->parent;
}

static int level_of(struct tdata *d, const struct tree_item *it)
{
    int n = 0;
    for (; it->parent != &d->root; it = it->parent)
        n++;
    return n;
}

static bool shown(struct tdata *d, const struct tree_item *it)     /* every parent open */
{
    for (it = it->parent; it != &d->root; it = it->parent)
        if (!(it->state & TVIS_EXPANDED))
            return false;
    return true;
}

static int row_of(struct tdata *d, struct tree_item *want)
{
    int n = 0;
    for (struct tree_item *it = d->root.first; it; it = next_vis(d, it), n++)
        if (it == want)
            return n;
    return -1;
}

static struct tree_item *at_row(struct tdata *d, int row)
{
    struct tree_item *it = d->root.first;
    while (it && row-- > 0)
        it = next_vis(d, it);
    return it;
}

/* ---- geometry -------------------------------------------------------------------- */

static int col_of(struct wnd *w, struct tdata *d, const struct tree_item *it)
{
    return level_of(d, it) + ((w->style & TVS_LINESATROOT) ? 1 : 0);
}

static int box_x(struct wnd *w, struct tdata *d, const struct tree_item *it)    /* TVS_CHECKBOXES */
{
    return 3 + col_of(w, d, it) * INDENT - d->sx;
}

static int icon_x(struct wnd *w, struct tdata *d, const struct tree_item *it)
{
    return box_x(w, d, it) + ((w->style & TVS_CHECKBOXES) ? 16 : 0);
}

static unsigned check_of(const struct tree_item *it) { return (it->state & TVIS_STATEIMAGEMASK) >> 12; }

static int text_x(struct wnd *w, struct tdata *d, const struct tree_item *it)
{
    return icon_x(w, d, it) + (d->il ? 20 : 0);
}

static void layout(struct wnd *w, struct lay *L)
{
    struct tdata *d = data_of(w);
    L->cx = 1;
    L->cy = 1;
    L->rows = 0;
    L->maxw = 0;
    for (struct tree_item *it = d->root.first; it; it = next_vis(d, it)) {
        int right = text_x(w, d, it) + d->sx + GUI_W(it->text) + 6;
        if (right > L->maxw)
            L->maxw = right;
        L->rows++;
    }
    int W = w->w - 2, H = w->h - 2;
    L->vb = L->rows * ROW_H > H;
    L->hb = L->maxw > W - (L->vb ? SCROLL_W : 0);
    if (L->hb && !L->vb)
        L->vb = L->rows * ROW_H > H - SCROLL_W;
    L->cw = W - (L->vb ? SCROLL_W : 0);
    L->ch = H - (L->hb ? SCROLL_W : 0);
    L->vis = L->ch / ROW_H;
    if (L->vis < 1)
        L->vis = 1;
}

static struct sbar vbar(struct wnd *w, const struct lay *L)
{
    int max = L->rows - L->vis;
    return (struct sbar){ true, L->cx + L->cw, L->cy, SCROLL_W, L->ch, w->top, 0, max < 0 ? 0 : max, L->vis };
}

static struct sbar hbar(struct wnd *w, const struct lay *L)
{
    struct tdata *d = data_of(w);
    int max = L->maxw - L->cw;
    return (struct sbar){ false, L->cx, L->cy + L->ch, L->cw, SCROLL_W, d->sx, 0, max < 0 ? 0 : max, L->cw };
}

static void clamp_scroll(struct wnd *w)
{
    struct tdata *d = data_of(w);
    struct lay L;
    layout(w, &L);
    struct sbar v = vbar(w, &L), hz = hbar(w, &L);
    w->top = clampi(w->top, 0, L.vb ? v.max : 0);
    d->sx = clampi(d->sx, 0, L.hb ? hz.max : 0);
}

static void scroll_rows(struct wnd *w, int top)
{
    int old = w->top;
    w->top = top;
    clamp_scroll(w);
    if (w->top != old)
        InvalidateRect(w, NULL);
}

static void scroll_px(struct wnd *w, int sx)
{
    struct tdata *d = data_of(w);
    int old = d->sx;
    d->sx = sx;
    clamp_scroll(w);
    if (d->sx != old)
        InvalidateRect(w, NULL);
}

static void ensure_visible(struct wnd *w, struct tree_item *it)
{
    struct tdata *d = data_of(w);
    for (struct tree_item *p = it->parent; p != &d->root; p = p->parent)
        p->state |= TVIS_EXPANDED;
    struct lay L;
    layout(w, &L);
    int row = row_of(d, it);
    if (row < w->top)
        w->top = row;
    else if (row >= w->top + L.vis)
        w->top = row - L.vis + 1;
    int x = icon_x(w, d, it) + d->sx;
    if (x - 3 < d->sx || x > d->sx + L.cw - 40)
        d->sx = x > 40 ? x - 40 : 0;
    clamp_scroll(w);
    InvalidateRect(w, NULL);
}

/* ---- notifications ----------------------------------------------------------------- */

static void fill_nm_item(TVITEM *t, struct tree_item *it)
{
    memset(t, 0, sizeof *t);
    if (!it)
        return;
    t->mask = TVIF_HANDLE | TVIF_STATE | TVIF_PARAM;
    t->hItem = it;
    t->state = it->state;
    t->lParam = it->param;
}

static LRESULT tv_notify(struct wnd *w, int code, unsigned action, struct tree_item *old, struct tree_item *now)
{
    NMTREEVIEW nm;
    memset(&nm, 0, sizeof nm);
    nm.action = action;
    fill_nm_item(&nm.itemOld, old);
    fill_nm_item(&nm.itemNew, now);
    return ui_notify(w, &nm.hdr, code);
}

static void tv_simple(struct wnd *w, int code)
{
    NMHDR nm;
    ui_notify(w, &nm, code);
}

/* ---- changes ----------------------------------------------------------------------- */

static bool select_item(struct wnd *w, struct tree_item *it, unsigned action)
{
    struct tdata *d = data_of(w);
    if (it == d->caret) {
        if (it)
            ensure_visible(w, it);
        return true;
    }
    struct tree_item *old = d->caret;
    if (tv_notify(w, TVN_SELCHANGING, action, old, it))
        return false;
    if (!wnd_from(w))
        return false;
    if (old)
        old->state &= ~(unsigned)TVIS_SELECTED;
    d->caret = it;
    if (it) {
        it->state |= TVIS_SELECTED;
        ensure_visible(w, it);
    }
    InvalidateRect(w, NULL);
    tv_notify(w, TVN_SELCHANGED, action, old, it);
    return true;
}

static bool inside(const struct tree_item *it, const struct tree_item *top)
{
    for (; it; it = it->parent)
        if (it == top)
            return true;
    return false;
}

static void delete_item(struct wnd *w, struct tree_item *it, bool tell);

/* Open or close (TVE_*); notify: the user did it (TVN_ITEMEXPANDING/ED). */
static bool expand(struct wnd *w, struct tree_item *it, unsigned code, bool notify)
{
    struct tdata *d = data_of(w);
    if (!is_item(d, it))
        return false;
    unsigned how = code & 3;
    bool is_open = it->state & TVIS_EXPANDED;
    if (how == TVE_TOGGLE)
        how = is_open ? TVE_COLLAPSE : TVE_EXPAND;
    if (how == TVE_EXPAND) {
        if (is_open && it->first)
            return true;
        if (!it->first && !it->children)
            return false;
        if (notify && tv_notify(w, TVN_ITEMEXPANDING, TVE_EXPAND, NULL, it))
            return false;
        if (!wnd_from(w))
            return false;
        if (!it->first) {                               /* nothing after all: no box */
            it->children = 0;
            it->state &= ~(unsigned)TVIS_EXPANDED;
            InvalidateRect(w, NULL);
            return false;
        }
        it->state |= TVIS_EXPANDED;
    } else if (how == TVE_COLLAPSE) {
        if (!is_open && !(code & TVE_COLLAPSERESET))
            return true;
        if (notify && tv_notify(w, TVN_ITEMEXPANDING, TVE_COLLAPSE, NULL, it))
            return false;
        if (!wnd_from(w))
            return false;
        it->state &= ~(unsigned)TVIS_EXPANDED;
        if (d->caret && d->caret != it && inside(d->caret, it))
            select_item(w, it, notify ? TVC_BYMOUSE : TVC_UNKNOWN);
        if (code & TVE_COLLAPSERESET)
            while (it->first)
                delete_item(w, it->first, true);
    } else {
        return false;
    }
    clamp_scroll(w);
    InvalidateRect(w, NULL);
    if (notify)
        tv_notify(w, TVN_ITEMEXPANDED, how, NULL, it);
    return true;
}

static void unlink_item(struct tree_item *it)
{
    struct tree_item *p = it->parent;
    if (it->prev) it->prev->next = it->next; else p->first = it->next;
    if (it->next) it->next->prev = it->prev; else p->last = it->prev;
    it->next = it->prev = NULL;
}

static void free_subtree(struct wnd *w, struct tree_item *it, bool tell)
{
    struct tdata *d = data_of(w);
    while (it->first) {
        struct tree_item *c = it->first;
        unlink_item(c);
        free_subtree(w, c, tell);
    }
    if (tell)
        tv_notify(w, TVN_DELETEITEM, 0, it, NULL);
    LocalFree(it->text);
    LocalFree(it);
    d->count--;
}

static void delete_item(struct wnd *w, struct tree_item *it, bool tell)
{
    struct tdata *d = data_of(w);
    if (d->caret && inside(d->caret, it)) {             /* the selection moves off it */
        struct tree_item *to = it->next ? it->next : it->prev ? it->prev
                             : it->parent != &d->root ? it->parent : NULL;
        d->caret->state &= ~(unsigned)TVIS_SELECTED;
        struct tree_item *old = d->caret;
        d->caret = to;
        if (to)
            to->state |= TVIS_SELECTED;
        if (tell)
            tv_notify(w, TVN_SELCHANGED, TVC_UNKNOWN, old == it ? NULL : NULL, to);
    }
    if (d->drop && inside(d->drop, it)) d->drop = NULL;
    if (d->press && inside(d->press, it)) d->press = NULL;
    if (d->edit && inside(d->edit_item, it)) {
        HWND e = d->edit;
        d->edit = NULL;
        DestroyWindow(e);
    }
    struct tree_item *p = it->parent;
    unlink_item(it);
    free_subtree(w, it, tell);
    if (p != &d->root && !p->first)
        p->state &= ~(unsigned)TVIS_EXPANDED;
    clamp_scroll(w);
    InvalidateRect(w, NULL);
}

static int cmp_text(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y || !x)
            return x - y;
    }
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

static void set_fields(struct tree_item *it, const TVITEM *t)
{
    if (t->mask & TVIF_TEXT) {
        char *s = dup_text(t->pszText);
        if (s) {
            LocalFree(it->text);
            it->text = s;
        }
    }
    if (t->mask & TVIF_IMAGE)
        it->image = t->iImage;
    if (t->mask & TVIF_SELECTEDIMAGE)
        it->simage = t->iSelectedImage;
    if (t->mask & TVIF_CHILDREN)
        it->children = t->cChildren;
    if (t->mask & TVIF_PARAM)
        it->param = t->lParam;
    if (t->mask & TVIF_STATE)
        it->state = (it->state & ~(t->stateMask & ~(unsigned)TVIS_SELECTED)) |
                    (t->state & t->stateMask & ~(unsigned)TVIS_SELECTED);
}

static struct tree_item *insert_(struct wnd *w, const TVINSERTSTRUCT *ins);
static struct tree_item *insert(struct wnd *w, const TVINSERTSTRUCT *ins)
{
    struct tree_item *it = insert_(w, ins);
    if (it && (w->style & TVS_CHECKBOXES) && !check_of(it))
        it->state |= 1u << 12;                          /* boxes start unchecked */
    return it;
}

static struct tree_item *insert_(struct wnd *w, const TVINSERTSTRUCT *ins)
{
    struct tdata *d = data_of(w);
    if (!ins)
        return NULL;
    struct tree_item *p = !ins->hParent || ins->hParent == TVI_ROOT ? &d->root : ins->hParent;
    struct tree_item *it = LocalAlloc(LMEM_ZEROINIT, sizeof *it);
    if (!it)
        return NULL;
    it->text = dup_text("");
    it->parent = p;
    set_fields(it, &ins->item);
    struct tree_item *after = ins->hInsertAfter;        /* NULL: before the first */
    if (after == TVI_LAST || (after && after != TVI_FIRST && after != TVI_SORT && after->parent != p))
        after = p->last;
    else if (after == TVI_FIRST)
        after = NULL;
    else if (after == TVI_SORT) {
        after = NULL;
        for (struct tree_item *c = p->first; c && cmp_text(c->text, it->text) <= 0; c = c->next)
            after = c;
    }
    it->prev = after;
    it->next = after ? after->next : p->first;
    if (it->next) it->next->prev = it; else p->last = it;
    if (after) after->next = it; else p->first = it;
    d->count++;
    clamp_scroll(w);
    InvalidateRect(w, NULL);
    return it;
}

static void sort_children(struct tree_item *p)
{
    struct tree_item *list = p->first;
    p->first = p->last = NULL;
    while (list) {                                      /* insertion sort: few items */
        struct tree_item *it = list;
        list = list->next;
        struct tree_item *after = NULL;
        for (struct tree_item *c = p->first; c && cmp_text(c->text, it->text) <= 0; c = c->next)
            after = c;
        it->prev = after;
        it->next = after ? after->next : p->first;
        if (it->next) it->next->prev = it; else p->last = it;
        if (after) after->next = it; else p->first = it;
    }
}

/* ---- painting ------------------------------------------------------------------------- */

static void dots(HDC dc, int x, int y, int len, bool vert, int phase)
{
    for (int i = 0; i < len; i++)
        if (((vert ? y + i : x + i) + phase) % 2 == 0)
            SetPixel(dc, vert ? x : x + i, vert ? y + i : y, C_GRAY3);
}

static void paint_row(HDC dc, struct wnd *w, struct tree_item *it, int y, const struct lay *L, bool focus)
{
    struct tdata *d = data_of(w);
    int col = col_of(w, d, it), ix = icon_x(w, d, it), mid = y + ROW_H / 2;
    int phase = d->sx & 1;
    if (w->style & TVS_HASLINES) {
        for (struct tree_item *a = it->parent; a != &d->root; a = a->parent) {     /* the parents' lines */
            int c = col_of(w, d, a);
            if (a->next && c >= 1)
                dots(dc, 3 + (c - 1) * INDENT + INDENT / 2 - d->sx, y, ROW_H, true, phase);
        }
        if (col >= 1) {
            int lx = 3 + (col - 1) * INDENT + INDENT / 2 - d->sx;
            bool first_root = it->parent == &d->root && !it->prev;
            dots(dc, lx, first_root ? mid : y, first_root ? ROW_H / 2 + 1 : ROW_H / 2 + 1, true, phase);
            if (it->next)
                dots(dc, lx, mid, ROW_H - ROW_H / 2, true, phase);
            dots(dc, lx, mid, ix - lx - 1, false, phase);
        }
    }
    if ((w->style & TVS_HASBUTTONS) && col >= 1 && (it->first || it->children)) {
        int bx = 3 + (col - 1) * INDENT + INDENT / 2 - BTN / 2 - d->sx, by = mid - BTN / 2;
        RECT b = { bx, by, bx + BTN, by + BTN }, in = { bx + 1, by + 1, bx + BTN - 1, by + BTN - 1 };
        FillRect(dc, &in, C_WHITE);
        FrameRect(dc, &b, C_GRAY2);
        DrawLine(dc, bx + 2, mid, BTN - 4, false, C_BLACK);
        if (!(it->state & TVIS_EXPANDED) || !it->first)
            DrawLine(dc, bx + BTN / 2, by + 2, BTN - 4, true, C_BLACK);
    }
    if (w->style & TVS_CHECKBOXES) {                    /* the check box */
        static const uint16_t tick[9] = { 0x0080, 0x0180, 0x0300, 0x8600, 0xCC00, 0x7800, 0x3000, 0, 0 };
        int bx = box_x(w, d, it);
        RECT b = { bx, mid - 6, bx + 13, mid + 7 }, in = { bx + 1, mid - 5, bx + 12, mid + 6 };
        FrameRect(dc, &b, C_BLACK);
        FillRect(dc, &in, C_WHITE);
        if (check_of(it) == 2)
            DrawBitmap(dc, bx + 2, mid - 4, tick, 9, C_BLACK);
    }
    bool sel = d->drop ? it == d->drop : it == d->caret;
    if (d->drop && it == d->drop) focus = true;
    if (d->edit && it == d->edit_item)
        return;
    if (d->il)
        ImageList_Draw(d->il, sel ? it->simage : it->image, dc, ix, y + 1, ILD_NORMAL);
    int tx = text_x(w, d, it), tw = GUI_W(it->text);
    RECT hl = { tx - 2, y + 1, tx + tw + 2, y + ROW_H - 1 };
    uint8_t fg = C_BLACK;
    if (sel && focus) {
        FillRect(dc, &hl, C_ACCENT);
        fg = C_WHITE;
    } else if (sel && (w->style & TVS_SHOWSELALWAYS)) {
        FillRect(dc, &hl, C_GRAY5);
    }
    TextOut(dc, tx, y + 1, it->text, w->enabled ? fg : C_GRAY3, -1);
    if (sel && focus)
        FrameRect(dc, &hl, C_ACCENT_DK);
    (void)L;
}

static void paint(HDC dc, struct wnd *w)
{
    struct tdata *d = data_of(w);
    struct lay L;
    layout(w, &L);
    RECT o = { 0, 0, w->w, w->h };
    FrameRect(dc, &o, ui_has_focus(w) ? C_ACCENT : C_GRAY1);
    RECT c = { L.cx, L.cy, L.cx + L.cw, L.cy + L.ch };
    FillRect(dc, &c, C_WHITE);
    RECT saved;
    dc_clip(dc, c, &saved);
    bool focus = ui_has_focus(w);
    struct tree_item *it = at_row(d, w->top);
    for (int i = 0; it && i <= L.vis; i++, it = next_vis(d, it))
        paint_row(dc, w, it, L.cy + i * ROW_H, &L, focus);
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

/* ---- input ------------------------------------------------------------------------------- */

static void bar_click(struct wnd *w, const struct sbar *s, int bar, int x, int y)
{
    struct tdata *d = data_of(w);
    d->bar = bar;
    w->part = sbar_hit(s, x, y);
    w->tracking = true;
    SetCapture(w);
    int line = bar == 0 ? 1 : 16;
    int cur = bar == 0 ? w->top : d->sx, to = cur;
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
    if (bar == 0) scroll_rows(w, to); else scroll_px(w, to);
    InvalidateRect(w, NULL);
}

/* What is at x, y: the item and the part (TVHT_*). */
static struct tree_item *hit(struct wnd *w, int x, int y, unsigned *part)
{
    struct tdata *d = data_of(w);
    struct lay L;
    layout(w, &L);
    *part = TVHT_NOWHERE;
    if (x < L.cx || y < L.cy || x >= L.cx + L.cw || y >= L.cy + L.ch)
        return NULL;
    struct tree_item *it = at_row(d, w->top + (y - L.cy) / ROW_H);
    if (!it)
        return NULL;
    int col = col_of(w, d, it), ix = icon_x(w, d, it), tx = text_x(w, d, it);
    int bc = 3 + (col - 1) * INDENT + INDENT / 2 - d->sx;
    int bx = box_x(w, d, it);
    if (col >= 1 && (it->first || it->children) && x >= bc - BTN / 2 - 2 && x <= bc + BTN / 2 + 2)
        *part = TVHT_ONITEMBUTTON;
    else if ((w->style & TVS_CHECKBOXES) && x >= bx && x < bx + 14)
        *part = TVHT_ONITEMSTATEICON;
    else if (x < ix)
        *part = TVHT_ONITEMINDENT;
    else if (x < tx)
        *part = TVHT_ONITEMICON;
    else if (x < tx + GUI_W(it->text) + 4)
        *part = TVHT_ONITEMLABEL;
    else
        *part = TVHT_NOWHERE;
    return it;
}

/* ---- label editing ---------------------------------------------------------------- */

static void end_edit(struct wnd *w, bool keep)
{
    struct tdata *d = data_of(w);
    if (!d || !d->edit)
        return;
    HWND e = d->edit;
    struct tree_item *it = d->edit_item;
    d->edit = NULL;
    char text[TEXT_MAX];
    GetWindowText(e, text, sizeof text);
    bool had_focus = GetFocus() == e;
    NMTVDISPINFO nm;
    memset(&nm, 0, sizeof nm);
    nm.item.mask = TVIF_HANDLE | TVIF_TEXT | TVIF_PARAM;
    nm.item.hItem = it;
    nm.item.pszText = keep ? text : NULL;
    nm.item.cchTextMax = (int)sizeof text;
    nm.item.lParam = it->param;
    if (ui_notify(w, &nm.hdr, TVN_ENDLABELEDIT) && keep && wnd_from(w)) {
        char *s = dup_text(text);
        if (s) {
            LocalFree(it->text);
            it->text = s;
        }
    }
    if (IsWindow(e))
        DestroyWindow(e);
    if (wnd_from(w)) {
        if (had_focus) SetFocus(w);
        clamp_scroll(w);
        InvalidateRect(w, NULL);
    }
}

static LRESULT edit_sub(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *e = wnd_from(h), *w = e ? e->parent : NULL;
    struct tdata *d = w ? data_of(w) : NULL;
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

static HWND begin_edit(struct wnd *w, struct tree_item *it)
{
    struct tdata *d = data_of(w);
    end_edit(w, true);
    if (!wnd_from(w) || !is_item(d, it))
        return NULL;
    ensure_visible(w, it);
    NMTVDISPINFO nm;
    memset(&nm, 0, sizeof nm);
    nm.item.mask = TVIF_HANDLE | TVIF_TEXT | TVIF_PARAM;
    nm.item.hItem = it;
    nm.item.pszText = it->text;
    nm.item.lParam = it->param;
    if (ui_notify(w, &nm.hdr, TVN_BEGINLABELEDIT) || !wnd_from(w))
        return NULL;
    int row = row_of(d, it) - w->top, tx = text_x(w, d, it);
    int ew = GUI_W(it->text) + 40;
    if (ew < 100) ew = 100;
    if (tx - 4 + ew > w->w - 2) ew = w->w - 2 - (tx - 4);
    HWND e = CreateWindow(WC_EDIT, it->text, WS_CHILD | WS_VISIBLE, tx - 6, 1 + row * ROW_H - 4, ew, 26, w,
                          EDIT_ID, NULL);
    if (!e)
        return NULL;
    d->edit = e;
    d->edit_item = it;
    d->edit_old = (WNDPROC)(uintptr_t)SetWindowLong(e, GWL_WNDPROC, (int32_t)(uintptr_t)edit_sub);
    SendMessage(e, EM_SETSEL, 0, -1);
    SetFocus(e);
    InvalidateRect(w, NULL);
    return e;
}

static void toggle_check(struct wnd *w, struct tree_item *it)
{
    it->state = (it->state & ~(unsigned)TVIS_STATEIMAGEMASK) | ((check_of(it) == 2 ? 1u : 2u) << 12);
    InvalidateRect(w, NULL);
}

static void key(struct wnd *w, unsigned vk)
{
    struct tdata *d = data_of(w);
    struct tree_item *c = d->caret;
    struct lay L;
    layout(w, &L);
    if (!d->root.first)
        return;
    if (!c) {
        select_item(w, d->root.first, TVC_BYKEYBOARD);
        return;
    }
    struct tree_item *to = NULL;
    switch (vk) {
    case VK_UP:    to = prev_vis(d, c); break;
    case VK_DOWN:  to = next_vis(d, c); break;
    case VK_HOME:  to = d->root.first; break;
    case VK_END:
        for (to = d->root.first; next_vis(d, to);)
            to = next_vis(d, to);
        break;
    case VK_PRIOR:
        to = c;
        for (int i = 1; i < L.vis && prev_vis(d, to); i++)
            to = prev_vis(d, to);
        break;
    case VK_NEXT:
        to = c;
        for (int i = 1; i < L.vis && next_vis(d, to); i++)
            to = next_vis(d, to);
        break;
    case VK_LEFT:
        if ((c->state & TVIS_EXPANDED) && c->first)
            expand(w, c, TVE_COLLAPSE, true);
        else if (c->parent != &d->root)
            to = c->parent;
        break;
    case VK_RIGHT:
        if (!(c->state & TVIS_EXPANDED) || !c->first)
            expand(w, c, TVE_EXPAND, true);
        else
            to = c->first;
        break;
    case VK_RETURN:
        tv_simple(w, NM_RETURN);
        break;
    case VK_SPACE:
        if (w->style & TVS_CHECKBOXES)
            toggle_check(w, c);
        return;
    case VK_F1 + 1:                                     /* F2 */
        if (w->style & TVS_EDITLABELS)
            begin_edit(w, c);
        return;
    }
    if (to && wnd_from(w))
        select_item(w, to, TVC_BYKEYBOARD);
}

static void expand_all(struct wnd *w, struct tree_item *it)
{
    if (!expand(w, it, TVE_EXPAND, true) || !wnd_from(w))
        return;
    for (struct tree_item *c = it->first; c; c = c->next)
        expand_all(w, c);
}

static void typed(struct wnd *w, char ch)
{
    struct tdata *d = data_of(w);
    if (!d->caret)
        return;
    if (ch == '+') { expand(w, d->caret, TVE_EXPAND, true); return; }
    if (ch == '-') { expand(w, d->caret, TVE_COLLAPSE, true); return; }
    if (ch == '*') { expand_all(w, d->caret); return; }
    if ((unsigned char)ch <= ' ')
        return;
    char up = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
    struct tree_item *it = d->caret;
    for (int guard = 0; guard <= d->count; guard++) {
        it = next_vis(d, it);
        if (!it)
            it = d->root.first;
        char f = it->text[0] >= 'a' && it->text[0] <= 'z' ? (char)(it->text[0] - 32) : it->text[0];
        if (f == up) {
            select_item(w, it, TVC_BYKEYBOARD);
            return;
        }
        if (it == d->caret)
            return;
    }
}

static void get_item(struct tree_item *it, TVITEM *t)
{
    if (t->mask & TVIF_TEXT && t->pszText && t->cchTextMax > 0)
        strncpy0(t->pszText, it->text, (size_t)t->cchTextMax);
    if (t->mask & TVIF_IMAGE)
        t->iImage = it->image;
    if (t->mask & TVIF_SELECTEDIMAGE)
        t->iSelectedImage = it->simage;
    if (t->mask & TVIF_CHILDREN)
        t->cChildren = it->first ? 1 : it->children;
    if (t->mask & TVIF_PARAM)
        t->lParam = it->param;
    if (t->mask & TVIF_STATE)
        t->state = it->state & (t->stateMask ? t->stateMask : ~0u);
}

LRESULT treeview_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct tdata *d = data_of(w);
    if (!d && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    struct tree_item *it = (struct tree_item *)lp;
    switch (msg) {
    case WM_CREATE:
        w->top = 0;
        return (w->ctl = LocalAlloc(LMEM_ZEROINIT, sizeof *d)) ? 0 : -1;
    case WM_DESTROY:
        while (d->root.first) {
            struct tree_item *c = d->root.first;
            unlink_item(c);
            free_subtree(w, c, true);
        }
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
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL);
        tv_simple(w, msg == WM_SETFOCUS ? NM_SETFOCUS : NM_KILLFOCUS);
        return 0;
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        struct lay L;
        layout(w, &L);
        SetFocus(h);
        if (!wnd_from(h))
            return 0;
        if (msg != WM_RBUTTONDOWN && L.vb && x >= L.cx + L.cw && y < L.cy + L.ch) {
            struct sbar s = vbar(w, &L);
            bar_click(w, &s, 0, x, y);
            return 0;
        }
        if (msg != WM_RBUTTONDOWN && L.hb && y >= L.cy + L.ch && x < L.cx + L.cw) {
            struct sbar s = hbar(w, &L);
            bar_click(w, &s, 1, x, y);
            return 0;
        }
        unsigned part;
        struct tree_item *t = hit(w, x, y, &part);
        if (d->edit)
            end_edit(w, true);
        if (!t || !wnd_from(h))
            return 0;
        if (part == TVHT_ONITEMBUTTON && msg != WM_RBUTTONDOWN) {
            expand(w, t, TVE_TOGGLE, true);
            return 0;
        }
        if (part == TVHT_ONITEMSTATEICON && msg != WM_RBUTTONDOWN) {
            toggle_check(w, t);
            tv_simple(w, NM_CLICK);
            return 0;
        }
        if (!(part & TVHT_ONITEM))
            return 0;
        if (!select_item(w, t, TVC_BYMOUSE) || !wnd_from(h))
            return 0;
        if (msg == WM_RBUTTONDOWN) {
            tv_simple(w, NM_RCLICK);
        } else if (msg == WM_LBUTTONDBLCLK) {
            NMHDR nm;
            if (!ui_notify(w, &nm, NM_DBLCLK) && wnd_from(h))
                expand(w, t, TVE_TOGGLE, true);
        } else {
            tv_simple(w, NM_CLICK);
            if (wnd_from(h) && d->caret == t) {         /* held: perhaps a drag */
                d->press = t;
                d->press_x = x;
                d->press_y = y;
                SetCapture(h);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (d->press) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if ((x - d->press_x) * (x - d->press_x) + (y - d->press_y) * (y - d->press_y) > 16) {
                struct tree_item *t = d->press;
                d->press = NULL;
                ReleaseCapture();
                NMTREEVIEW nm;
                memset(&nm, 0, sizeof nm);
                fill_nm_item(&nm.itemNew, t);
                nm.ptDrag = (POINT){ d->press_x, d->press_y };
                ui_notify(w, &nm.hdr, TVN_BEGINDRAG);
            }
            return 0;
        }
        if (w->tracking && w->part == SB_PART_THUMB) {
            struct lay L;
            layout(w, &L);
            if (d->bar == 0) {
                struct sbar s = vbar(w, &L);
                scroll_rows(w, sbar_thumb_value(&s, GET_Y_LPARAM(lp), w->grab));
            } else {
                struct sbar s = hbar(w, &L);
                scroll_px(w, sbar_thumb_value(&s, GET_X_LPARAM(lp), w->grab));
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (d->press) {
            d->press = NULL;
            ReleaseCapture();
        }
        if (w->tracking) {
            ReleaseCapture();
            w->tracking = false;
            w->part = SB_PART_NONE;
            InvalidateRect(h, NULL);
        }
        return 0;
    case WM_KEYDOWN: {
        struct { NMHDR hdr; uint16_t wVKey; unsigned flags; } nm = { .wVKey = (uint16_t)wp };
        ui_notify(w, &nm.hdr, TVN_KEYDOWN);
        if (wnd_from(h))
            key(w, wp);
        return 0;
    }
    case WM_CHAR:
        typed(w, (char)wp);
        return 0;
    case TVM_INSERTITEM:
        return (LRESULT)(uintptr_t)insert(w, (const TVINSERTSTRUCT *)lp);
    case TVM_DELETEITEM:
        if (it == TVI_ROOT || !it) {
            while (d->root.first)
                delete_item(w, d->root.first, true);
            w->top = 0;
            d->sx = 0;
            return 1;
        }
        if (!is_item(d, it))
            return 0;
        delete_item(w, it, true);
        return 1;
    case TVM_EXPAND:
        return expand(w, it, (unsigned)wp, false);
    case TVM_GETITEMRECT: {
        RECT *r = (RECT *)lp;
        if (!r)
            return 0;
        struct tree_item *t = *(struct tree_item **)r;
        if (!is_item(d, t) || !shown(d, t))
            return 0;
        int row = row_of(d, t) - w->top;
        int tx = text_x(w, d, t);
        *r = (RECT){ wp ? tx - 2 : 1, 1 + row * ROW_H, wp ? tx + GUI_W(t->text) + 2 : w->w - 1,
                     1 + (row + 1) * ROW_H };
        return 1;
    }
    case TVM_GETCOUNT:
        return d->count;
    case TVM_GETIMAGELIST:
        return (LRESULT)(uintptr_t)d->il;
    case TVM_SETIMAGELIST: {
        HIMAGELIST old = d->il;
        d->il = (HIMAGELIST)lp;
        InvalidateRect(h, NULL);
        return (LRESULT)(uintptr_t)old;
    }
    case TVM_GETNEXTITEM: {
        struct tree_item *r = NULL;
        bool ok = is_item(d, it);
        switch (wp) {
        case TVGN_ROOT:            r = d->root.first; break;
        case TVGN_CARET:           r = d->caret; break;
        case TVGN_FIRSTVISIBLE:    r = at_row(d, w->top); break;
        case TVGN_NEXT:            r = ok ? it->next : NULL; break;
        case TVGN_PREVIOUS:        r = ok ? it->prev : NULL; break;
        case TVGN_PARENT:          r = ok && it->parent != &d->root ? it->parent : NULL; break;
        case TVGN_CHILD:           r = ok ? it->first : (!it || it == TVI_ROOT) ? d->root.first : NULL; break;
        case TVGN_NEXTVISIBLE:     r = ok && shown(d, it) ? next_vis(d, it) : NULL; break;
        case TVGN_PREVIOUSVISIBLE: r = ok && shown(d, it) ? prev_vis(d, it) : NULL; break;
        }
        return (LRESULT)(uintptr_t)r;
    }
    case TVM_SELECTITEM:
        if (wp == TVGN_DROPHILITE) {
            if (it && !is_item(d, it))
                return 0;
            if (d->drop) d->drop->state &= ~(unsigned)TVIS_DROPHILITED;
            d->drop = it;
            if (it) it->state |= TVIS_DROPHILITED;
            InvalidateRect(h, NULL);
            return 1;
        }
        if (wp != TVGN_CARET)
            return 0;
        if (it && !is_item(d, it))
            return 0;
        return select_item(w, it, TVC_UNKNOWN);
    case TVM_GETITEM: {
        TVITEM *t = (TVITEM *)lp;
        if (!t || !is_item(d, t->hItem))
            return 0;
        get_item(t->hItem, t);
        return 1;
    }
    case TVM_SETITEM: {
        TVITEM *t = (TVITEM *)lp;
        if (!t || !is_item(d, t->hItem))
            return 0;
        set_fields(t->hItem, t);
        clamp_scroll(w);
        InvalidateRect(h, NULL);
        return 1;
    }
    case TVM_HITTEST: {
        TVHITTESTINFO *ht = (TVHITTESTINFO *)lp;
        if (!ht)
            return 0;
        ht->hItem = hit(w, ht->pt.x, ht->pt.y, &ht->flags);
        return (LRESULT)(uintptr_t)ht->hItem;
    }
    case TVM_SORTCHILDREN:
        sort_children(!it || it == TVI_ROOT ? &d->root : it);
        InvalidateRect(h, NULL);
        return 1;
    case TVM_ENSUREVISIBLE:
        if (!is_item(d, it))
            return 0;
        ensure_visible(w, it);
        return 1;
    case TVM_EDITLABEL:
        return (LRESULT)(uintptr_t)begin_edit(w, it);
    case TVM_ENDEDITLABELNOW:
        end_edit(w, !wp);
        return 1;
    case TVM_GETITEMSTATE: {
        struct tree_item *t = (struct tree_item *)wp;
        return is_item(d, t) ? (LRESULT)(t->state & (unsigned)lp) : 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}
