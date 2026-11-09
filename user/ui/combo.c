/*
 * combo.c -- the COMBOBOX class: a closed box with a drop arrow, and the
 * list that drops down from it.
 *
 * CBS_DROPDOWNLIST draws the box as a Platinum pop-up button showing the
 * selection; CBS_DROPDOWN puts a child EDIT there instead (subclassed for
 * the arrow keys), so the text can also be typed. The outer 3 pixels hold
 * the focus ring, as for edits. The window is as tall as the closed box;
 * the list sizes itself to at most ROWS rows, with a scroll bar when there
 * are more items.
 *
 * The list is a top-level window of the internal "#COMBOLBOX" class, which
 * never activates, below the box (above it when the work area has no room
 * below). As with menus, showing it is modal: drop() runs an input loop
 * until an item is chosen (click, Enter) or the list is dismissed (Esc, a
 * click outside). Hovering highlights; the keys move the highlight.
 *
 * Item strings live in the owning process's heap: w->ctl is a struct
 * combo. w->value = selection (-1 none), w->pressed = the list is down.
 */
#include "uipriv.h"

#define RING     3
#define ARROW_W  18             /* the drop arrow's part of the box */
#define ROWS     8              /* the most rows the list shows */
#define EDIT_ID  1001           /* CBS_DROPDOWN's field (as in Windows) */

static const uint16_t g_drop[4] = { 0xFE00, 0x7C00, 0x3800, 0x1000 };

struct combo {
    char      **items;
    int         n, cap;
    struct wnd *edit;           /* CBS_DROPDOWN */
    struct wnd *list;           /* while dropped down */
    int         hot, top;       /* the list's highlight and first row */
    bool        close;          /* CB_SHOWDROPDOWN 0 while dropped */
};

static struct combo *data_of(struct wnd *w) { return (struct combo *)w->ctl; }

static bool editable(struct wnd *w) { return (w->style & 3) == CBS_DROPDOWN; }

static void notify(struct wnd *w, int code)
{
    if (w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(w->id, code), (LPARAM)w);
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Does s start with (exact: equal) p, ignoring case? */
static bool matches(const char *s, const char *p, bool exact)
{
    for (; *p; s++, p++)
        if (lower(*s) != lower(*p))
            return false;
    return !exact || !*s;
}

static int find(struct combo *c, int after, const char *s, bool exact)
{
    if (!s)
        return CB_ERR;
    for (int k = 1; k <= c->n; k++) {               /* after the start, wrapping */
        int i = (after < 0 || after >= c->n) ? k - 1 : (after + k) % c->n;
        if (matches(c->items[i], s, exact))
            return i;
    }
    return CB_ERR;
}

/* ---- items ------------------------------------------------------------- */

static int insert(struct wnd *w, int at, const char *s)
{
    struct combo *c = data_of(w);
    if (!c || !s)
        return CB_ERR;
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        char **p = c->items ? LocalReAlloc(c->items, (uint32_t)cap * sizeof *p, 0)
                            : LocalAlloc(LMEM_FIXED, (uint32_t)cap * sizeof *p);
        if (!p)
            return CB_ERR;
        c->items = p;
        c->cap = cap;
    }
    char *copy = LocalAlloc(LMEM_FIXED, (uint32_t)strlen(s) + 1);
    if (!copy)
        return CB_ERR;
    memcpy(copy, s, strlen(s) + 1);
    if (at < 0 || at > c->n)
        at = c->n;
    memmove(&c->items[at + 1], &c->items[at], (size_t)(c->n - at) * sizeof *c->items);
    c->items[at] = copy;
    c->n++;
    if (w->value >= at)
        w->value++;
    return at;
}

static void show_text(struct wnd *w)               /* the selection, in the box */
{
    struct combo *c = data_of(w);
    const char *s = w->value >= 0 ? c->items[w->value] : "";
    strncpy0(w->text, s, TEXT_MAX);
    if (c->edit) {
        SendMessage(c->edit, WM_SETTEXT, 0, (LPARAM)w->text);
        if (GetFocus() == c->edit)
            SendMessage(c->edit, EM_SETSEL, 0, -1);
    }
    InvalidateRect(w, NULL);
}

static void select(struct wnd *w, int i, bool notify_parent)
{
    struct combo *c = data_of(w);
    i = c->n ? clampi(i, 0, c->n - 1) : -1;
    if (i == w->value)
        return;
    w->value = i;
    show_text(w);
    if (notify_parent)
        notify(w, CBN_SELCHANGE);
}

/* ---- the dropped list ---------------------------------------------------- */

static int rows_of(struct combo *c) { return c->n < ROWS ? (c->n ? c->n : 1) : ROWS; }

static struct sbar list_bar(struct wnd *l, struct combo *c)
{
    return (struct sbar){ true, l->w - 1 - SCROLL_W, 1, SCROLL_W, l->h - 2,
                          c->top, 0, c->n - ROWS, ROWS };
}

LRESULT combolist_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *l = wnd_from(h), *w = wnd_from((HWND)(uintptr_t)l->userdata);
    if (msg != WM_PAINT || !w || !data_of(w))
        return DefWindowProc(h, msg, wp, lp);
    struct combo *c = data_of(w);
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    bool bar = c->n > ROWS;
    int iw = l->w - 2 - (bar ? SCROLL_W : 0);
    RECT o = { 0, 0, l->w, l->h }, in = { 1, 1, 1 + iw, l->h - 1 };
    FrameRect(dc, &o, C_BLACK);
    FillRect(dc, &in, C_WHITE);
    for (int i = 0; i < ROWS && c->top + i < c->n; i++) {
        int item = c->top + i, y = 2 + i * ITEM_H;
        bool hot = item == c->hot;
        if (hot) {
            RECT r = { 1, y, 1 + iw, y + ITEM_H };
            FillRect(dc, &r, C_ACCENT);
        }
        TextOut(dc, 6, y, c->items[item], hot ? C_WHITE : C_BLACK, -1);
    }
    if (bar) {
        struct sbar s = list_bar(l, c);
        sbar_draw(dc, &s, l->part);
    }
    EndPaint(h, &ps);
    return 0;
}

static void list_scroll(struct combo *c, int top)
{
    top = clampi(top, 0, c->n > ROWS ? c->n - ROWS : 0);
    if (top != c->top) {
        c->top = top;
        InvalidateRect(c->list, NULL);
    }
}

static void list_hot(struct combo *c, int i)
{
    if (!c->n)
        return;
    i = clampi(i, 0, c->n - 1);
    if (i < c->top) list_scroll(c, i);
    if (i >= c->top + ROWS) list_scroll(c, i - ROWS + 1);
    if (i != c->hot) {
        c->hot = i;
        InvalidateRect(c->list, NULL);
    }
}

/* The modal loop; -> the item chosen, or -1 if dismissed. */
static int track(struct wnd *w)
{
    struct combo *c = data_of(w);
    struct wnd *l = c->list;
    int chosen = -1;
    bool done = false;
    ui_modal_begin();
    while (!done && !c->close) {
        if (ui_paint_pending())
            ui_paint();
        struct modal_ev e;
        ui_modal_event(&e);
        if (!wnd_from(w) || !wnd_from(l) || e.type == EV_FOCUS)
            break;
        if (e.type == EV_KEY) {
            if (e.up)
                continue;
            bool alt = GetKeyState(VK_MENU) < 0;
            switch (e.vk) {
            case VK_ESCAPE: done = true; break;
            case VK_F4:
            case VK_RETURN:
            case VK_TAB:    chosen = c->hot; done = true; break;
            case VK_UP:     if (alt) { chosen = c->hot; done = true; } else list_hot(c, c->hot - 1); break;
            case VK_DOWN:   if (alt) { chosen = c->hot; done = true; } else list_hot(c, c->hot + 1); break;
            case VK_PRIOR:  list_hot(c, c->hot - ROWS + 1); break;
            case VK_NEXT:   list_hot(c, c->hot + ROWS - 1); break;
            case VK_HOME:   list_hot(c, 0); break;
            case VK_END:    list_hot(c, c->n - 1); break;
            default: {
                char ch[2] = { kbd_char(e.vk, false), 0 };
                int i = ch[0] ? find(c, c->hot, ch, false) : CB_ERR;
                if (i >= 0)
                    list_hot(c, i);
            }
            }
            continue;
        }
        if (e.type != EV_MOUSE)
            continue;
        RECT lr = { l->x, l->y, l->x + l->w, l->y + l->h };
        int x = e.x - l->x, y = e.y - l->y;
        struct sbar s = list_bar(l, c);
        bool bar = c->n > ROWS;
        if (l->part == SB_PART_THUMB) {             /* dragging the thumb */
            if (e.moved)
                list_scroll(c, sbar_thumb_value(&s, y, l->grab));
            if (e.lup) {
                l->part = SB_PART_NONE;
                InvalidateRect(l, NULL);
            }
            continue;
        }
        if (e.lup && l->part != SB_PART_NONE) {     /* an arrow or the track let go */
            l->part = SB_PART_NONE;
            InvalidateRect(l, NULL);
            continue;
        }
        if (!rect_contains(&lr, e.x, e.y)) {
            if (e.ldown || e.rdown)
                done = true;                        /* outside: dismiss */
            continue;
        }
        if (bar && x >= s.x) {
            if (!e.ldown)
                continue;
            l->part = sbar_hit(&s, x, y);
            switch (l->part) {
            case SB_PART_DEC:   list_scroll(c, c->top - 1); break;
            case SB_PART_INC:   list_scroll(c, c->top + 1); break;
            case SB_PART_PGDEC: list_scroll(c, c->top - ROWS); break;
            case SB_PART_PGINC: list_scroll(c, c->top + ROWS); break;
            case SB_PART_THUMB: {
                int pos, len, tr, tl;
                sbar_thumb(&s, &pos, &len, &tr, &tl);
                l->grab = y - pos;
                break;
            }
            }
            InvalidateRect(l, NULL);
            continue;
        }
        int row = (y - 2) / ITEM_H, item = c->top + row;
        if (y < 2 || row >= ROWS || item >= c->n)
            continue;
        if (e.moved || e.ldown)
            list_hot(c, item);
        if (e.lup) {                                /* released on an item: chosen */
            chosen = item;
            done = true;
        }
    }
    ui_modal_end();
    return chosen;
}

static void drop(struct wnd *w)
{
    struct combo *c = data_of(w);
    if (!c || c->list || !w->enabled)
        return;
    notify(w, CBN_DROPDOWN);
    if (!wnd_from(w))
        return;
    RECT r, work = S->work;
    wnd_screen_rect(w, &r);
    int lw = r.right - r.left - 2 * RING, lh = rows_of(c) * ITEM_H + 4;
    int x = r.left + RING, y = r.bottom - RING;
    if (y + lh > work.bottom && r.top + RING - lh >= work.top)
        y = r.top + RING - lh;                      /* no room below: above */
    if (x + lw > work.right) x = work.right - lw;
    if (x < work.left) x = work.left;
    c->hot = w->value;
    if (c->edit) {                                  /* the item the text names */
        int i = find(c, -1, c->edit->text, true);
        if (i >= 0)
            c->hot = i;
    }
    c->top = 0;
    c->close = false;
    c->list = wnd_from(CreateWindow("#COMBOLBOX", "", WS_POPUP | WS_VISIBLE, x, y, lw, lh, NULL, 0, NULL));
    if (!c->list)
        return;
    c->list->userdata = (int32_t)(uintptr_t)w;
    if (c->hot >= 0)
        list_hot(c, c->hot);
    w->pressed = true;
    InvalidateRect(w, NULL);
    int chosen = track(w);
    DestroyWindow(c->list);
    c->list = NULL;
    if (!wnd_from(w))
        return;
    w->pressed = false;
    InvalidateRect(w, NULL);
    notify(w, CBN_CLOSEUP);
    if (chosen >= 0 && wnd_from(w)) {
        if (chosen == w->value && c->edit && strcmp(c->edit->text, c->items[chosen]))
            show_text(w);                           /* typed over: back to the item */
        select(w, chosen, true);
    }
}

/* ---- the box -------------------------------------------------------------- */

static void paint_arrow(HDC dc, int x, int y, int h, bool dis)
{
    DrawBitmap(dc, x + (ARROW_W - 7) / 2, y + (h - 4) / 2, g_drop, 4, dis ? C_GRAY4 : C_BLACK);
}

static void paint(HDC dc, struct wnd *w)
{
    struct combo *c = data_of(w);
    bool dis = !w->enabled, down = w->pressed;
    int x = RING, y = RING, bw = w->w - 2 * RING, bh = w->h - 2 * RING;
    if (c->edit) {                                  /* just the arrow button */
        int ax = w->w - ARROW_W;
        RECT o = { ax, y, ax + ARROW_W - RING, y + bh }, in = { ax + 1, y + 1, ax + ARROW_W - RING - 1, y + bh - 1 };
        FrameRect(dc, &o, dis ? C_GRAY4 : C_GRAY1);
        FillRect(dc, &in, down ? C_GRAY4 : C_FACE);
        if (!down)
            DrawBevel(dc, &in, true);
        DrawBitmap(dc, ax + (ARROW_W - RING - 7) / 2, y + (bh - 4) / 2, g_drop, 4, dis ? C_GRAY4 : C_BLACK);
        return;
    }
    if (ui_has_focus(w)) {
        RECT a = { 0, 0, w->w, w->h }, b = { 1, 1, w->w - 1, w->h - 1 };
        FrameRect(dc, &a, C_ACCENT);
        FrameRect(dc, &b, C_ACCENT);
    }
    uint8_t face = down ? C_GRAY4 : C_FACE;         /* a pop-up button */
    RECT a = { x + 2, y + 1, x + bw - 2, y + bh - 1 }, b = { x + 1, y + 2, x + bw - 1, y + bh - 2 };
    FillRect(dc, &a, face);
    FillRect(dc, &b, face);
    RECT o = { x, y, x + bw, y + bh };
    DrawRoundRect(dc, &o, dis ? C_GRAY4 : C_BLACK);
    if (!down) {
        DrawLine(dc, x + 2, y + 1, bw - 4, false, C_WHITE);
        DrawLine(dc, x + 1, y + 2, bh - 4, true, C_WHITE);
        DrawLine(dc, x + 2, y + bh - 2, bw - 4, false, C_GRAY4);
        DrawLine(dc, x + bw - 2, y + 2, bh - 4, true, C_GRAY4);
    }
    int ax = x + bw - ARROW_W;
    DrawLine(dc, ax, y + 3, bh - 6, true, C_GRAY4);
    DrawLine(dc, ax + 1, y + 3, bh - 6, true, C_WHITE);
    paint_arrow(dc, ax, y, bh, dis);
    char buf[TEXT_MAX];
    int room = clampi((ax - x - 8) / 8, 0, TEXT_MAX - 1);     /* what fits before the arrow */
    strncpy0(buf, w->text, (size_t)room + 1);
    TextOut(dc, x + 6, y + (bh - 16) / 2, buf, dis ? C_GRAY3 : down ? C_WHITE : C_BLACK, -1);
}

/* CBS_DROPDOWN's field: the arrow keys and F4 belong to the combo box. */
static LRESULT edit_sub(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *e = wnd_from(h), *w = e->parent;
    if (((msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN || wp == VK_F4)) ||
         (msg == WM_SYSKEYDOWN && (wp == VK_UP || wp == VK_DOWN))) && w)
        return SendMessage(w, msg, wp, lp);
    return edit_proc(h, msg, wp, lp);
}

LRESULT combo_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct combo *c = data_of(w);
    if (!c && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        w->value = -1;
        if (!(w->ctl = c = LocalAlloc(LMEM_ZEROINIT, sizeof *c)))
            return -1;
        if (editable(w)) {
            c->edit = wnd_from(CreateWindow(WC_EDIT, w->text, WS_CHILD | WS_VISIBLE | (w->style & WS_DISABLED),
                                            0, 0, w->w - ARROW_W, w->h, h, EDIT_ID, NULL));
            if (c->edit)
                SetWindowLong(c->edit, GWL_WNDPROC, (int32_t)(uintptr_t)edit_sub);
        }
        return 0;
    case WM_DESTROY:
        if (c) {
            for (int i = 0; i < c->n; i++)
                LocalFree(c->items[i]);
            LocalFree(c->items);
            LocalFree(c);
            w->ctl = NULL;
        }
        return 0;
    case WM_SIZE:
        if (c && c->edit)
            MoveWindow(c->edit, 0, 0, w->w - ARROW_W, w->h, true);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc, w);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
        if (c->edit) {
            SetFocus(c->edit);
            SendMessage(c->edit, EM_SETSEL, 0, -1);
        }
        InvalidateRect(h, NULL);
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL);
        return 0;
    case WM_ENABLE:
        if (c->edit)
            EnableWindow(c->edit, w->enabled);
        InvalidateRect(h, NULL);
        return 0;
    case WM_SETTEXT:
        strncpy0(w->text, lp ? (const char *)lp : "", TEXT_MAX);
        if (c->edit)
            SendMessage(c->edit, WM_SETTEXT, 0, (LPARAM)w->text);
        InvalidateRect(h, NULL);
        return 1;
    case WM_COMMAND:                                /* from the field */
        if ((HWND)lp == c->edit && HIWORD(wp) == EN_CHANGE) {
            strncpy0(w->text, c->edit->text, TEXT_MAX);
            notify(w, CBN_EDITCHANGE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        drop(w);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_UP || wp == VK_DOWN) {         /* Alt+Down */
            drop(w);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_F4:    drop(w); break;
        case VK_UP:
        case VK_LEFT:  select(w, w->value < 0 ? 0 : w->value - 1, true); break;
        case VK_DOWN:
        case VK_RIGHT: select(w, w->value + 1, true); break;
        case VK_PRIOR: select(w, w->value - ROWS + 1, true); break;
        case VK_NEXT:  select(w, w->value + ROWS - 1, true); break;
        case VK_HOME:  select(w, 0, true); break;
        case VK_END:   select(w, c->n - 1, true); break;
        }
        return 0;
    case WM_CHAR: {                                 /* a letter: the next item it starts */
        char s[2] = { (char)wp, 0 };
        int i = (uint8_t)wp > ' ' ? find(c, w->value, s, false) : CB_ERR;
        if (i >= 0)
            select(w, i, true);
        return 0;
    }
    case CB_ADDSTRING:
    case CB_INSERTSTRING: {
        int i = insert(w, msg == CB_ADDSTRING ? -1 : (int)wp, (const char *)lp);
        if (c->list)
            InvalidateRect(c->list, NULL);
        return i;
    }
    case CB_DELETESTRING: {
        int i = (int)wp;
        if (i < 0 || i >= c->n)
            return CB_ERR;
        LocalFree(c->items[i]);
        memmove(&c->items[i], &c->items[i + 1], (size_t)(c->n - i - 1) * sizeof *c->items);
        c->n--;
        if (w->value == i) {
            w->value = -1;
            if (!c->edit)
                show_text(w);
        } else if (w->value > i) {
            w->value--;
        }
        return c->n;
    }
    case CB_RESETCONTENT:
        for (int i = 0; i < c->n; i++)
            LocalFree(c->items[i]);
        c->n = 0;
        w->value = -1;
        if (!c->edit)
            show_text(w);
        return 0;
    case CB_GETCOUNT:
        return c->n;
    case CB_GETCURSEL:
        return w->value;
    case CB_SETCURSEL:
        if ((int)wp < 0 || (int)wp >= c->n) {
            w->value = -1;
            show_text(w);
            return CB_ERR;
        }
        select(w, (int)wp, false);
        return (LRESULT)wp;
    case CB_GETLBTEXT:
        if ((int)wp < 0 || (int)wp >= c->n || !lp)
            return CB_ERR;
        memcpy((char *)lp, c->items[wp], strlen(c->items[wp]) + 1);
        return (LRESULT)strlen(c->items[wp]);
    case CB_GETLBTEXTLEN:
        return (int)wp < 0 || (int)wp >= c->n ? CB_ERR : (LRESULT)strlen(c->items[wp]);
    case CB_FINDSTRING:
    case CB_FINDSTRINGEXACT:
        return find(c, (int)wp, (const char *)lp, msg == CB_FINDSTRINGEXACT);
    case CB_SHOWDROPDOWN:
        if (wp)
            drop(w);
        else if (c->list)
            c->close = true;
        return 1;
    case CB_GETDROPPEDSTATE:
        return c->list != NULL;
    }
    return DefWindowProc(h, msg, wp, lp);
}
