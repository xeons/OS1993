/*
 * menu.c -- menu bars, pop-up menus and the system menu.
 *
 * Menus are objects (HMENU) holding items; a top-level window can have a
 * menu bar (SetMenu) and has a system menu (GetSystemMenu). Showing them
 * is modal, as in Windows: menu_track_*() and TrackPopupMenu() run their
 * own input loop until an item is chosen or the menu is dismissed, each
 * open pop-up being a top-level window of the internal "#MENU" class that
 * never activates. The chosen item's id goes to the owner window as
 * WM_COMMAND, or WM_SYSCOMMAND when it came from the system menu.
 *
 * Mouse: click a bar title and click an item, or press, drag and release
 * (both work); hovering opens submenus; clicking outside dismisses.
 * Keys: arrows, Enter, Esc, mnemonics (the letter after '&'); Alt or F10
 * leaves the menu. Look: Platinum -- light grey, a black border, lavender-
 * blue highlight with white text.
 */
#include "uipriv.h"

#define ITEM_MAX   MITEM_MAX
#define ITEM_TEXT  MITEM_TEXT
#define MENU_MAGIC 0x554E454Du  /* "MENU" */
#define DEPTH      MENU_DEPTH
#define menus      (S->menus)
#define T          (S->mt)

#define MITEM_H     18
#define SEP_H      8
#define PAD        3            /* border + bevel + 1 */
#define TEXT_X     20           /* check-mark column before the text */

static struct menu *menu_from(HMENU h)
{
    uintptr_t p = (uintptr_t)h, base = (uintptr_t)menus;
    if (p < base || p >= base + sizeof menus || (p - base) % sizeof(struct menu))
        return NULL;
    struct menu *m = (struct menu *)h;
    return m->magic == MENU_MAGIC ? m : NULL;
}

/* ---- labels: '&' mnemonics and tab-separated accelerators ----------------- */

static int label_width(const char *s)               /* the part before a tab */
{
    int n = 0;
    for (; *s && *s != '\t'; s++) {
        if (*s == '&' && s[1]) s++;
        n++;
    }
    return n * 8;
}

static const char *accel_of(const char *s)
{
    while (*s && *s != '\t') s++;
    return *s ? s + 1 : NULL;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static char mnemonic(const char *s)
{
    for (; *s && *s != '\t'; s++)
        if (*s == '&' && s[1] && s[1] != '&')
            return lower(s[1]);
        else if (*s == '&' && s[1] == '&')
            s++;
    return 0;
}

/* In screen coordinates, on the back buffer with its current clip. */
static void draw_label(int x, int y, const char *s, uint8_t fg)
{
    char buf[ITEM_TEXT];
    int n = 0, ul = -1;
    for (; *s && *s != '\t' && n < ITEM_TEXT - 1; s++) {
        if (*s == '&' && s[1]) {
            s++;
            if (*s != '&')
                ul = n;
        }
        buf[n++] = *s;
    }
    buf[n] = '\0';
    gfx_text(&ui_gfx, x, y, buf, fg, GFX_TRANSPARENT);
    if (ul >= 0)
        gfx_hline(&ui_gfx, x + ul * 8, y + 14, 8, fg);
}

/* ---- menu objects -------------------------------------------------------------- */

static struct menu *new_menu(bool bar)
{
    for (int i = 0; i < MENU_MAX; i++)
        if (menus[i].magic != MENU_MAGIC) {
            memset(&menus[i], 0, sizeof menus[i]);
            menus[i].magic = MENU_MAGIC;
            menus[i].pid = ui_pid;
            menus[i].bar = bar;
            return &menus[i];
        }
    return NULL;
}

HMENU CreateMenu(void)      { return new_menu(true); }
HMENU CreatePopupMenu(void) { return new_menu(false); }

bool DestroyMenu(HMENU h)
{
    struct menu *m = menu_from(h);
    if (!m)
        return false;
    m->magic = 0;
    for (int i = 0; i < m->n; i++)
        if (m->items[i].flags & MF_POPUP)
            DestroyMenu(m->items[i].sub);
    return true;
}

bool AppendMenu(HMENU h, unsigned flags, uintptr_t id, const char *text)
{
    struct menu *m = menu_from(h);
    if (!m || m->n == ITEM_MAX || ((flags & MF_POPUP) && !menu_from((HMENU)id)))
        return false;
    struct mitem *it = &m->items[m->n++];
    memset(it, 0, sizeof *it);
    it->flags = flags & ~MF_BYPOSITION;
    if (flags & MF_POPUP)
        it->sub = (struct menu *)id;
    else
        it->id = (unsigned)id;
    if (text && !(flags & MF_SEPARATOR))
        strncpy0(it->text, text, ITEM_TEXT);
    return true;
}

static struct mitem *find_item(struct menu *m, unsigned item, unsigned flags)
{
    if (flags & MF_BYPOSITION)
        return item < (unsigned)m->n ? &m->items[item] : NULL;
    for (int i = 0; i < m->n; i++) {
        struct mitem *it = &m->items[i];
        if (it->flags & MF_POPUP) {
            struct mitem *r = find_item(it->sub, item, flags);
            if (r) return r;
        } else if (!(it->flags & MF_SEPARATOR) && it->id == item) {
            return it;
        }
    }
    return NULL;
}

int CheckMenuItem(HMENU h, unsigned item, unsigned flags)
{
    struct menu *m = menu_from(h);
    struct mitem *it = m ? find_item(m, item, flags) : NULL;
    if (!it)
        return -1;
    int old = (int)(it->flags & MF_CHECKED);
    it->flags = (it->flags & ~MF_CHECKED) | (flags & MF_CHECKED);
    return old;
}

int EnableMenuItem(HMENU h, unsigned item, unsigned flags)
{
    struct menu *m = menu_from(h);
    struct mitem *it = m ? find_item(m, item, flags) : NULL;
    if (!it)
        return -1;
    int old = (int)(it->flags & (MF_GRAYED | MF_DISABLED));
    it->flags = (it->flags & ~(MF_GRAYED | MF_DISABLED)) | (flags & (MF_GRAYED | MF_DISABLED));
    return old;
}

bool CheckMenuRadioItem(HMENU h, unsigned first, unsigned last, unsigned check, unsigned flags)
{
    struct menu *m = menu_from(h);
    if (!m)
        return false;
    bool found = false;
    for (unsigned i = first; i <= last; i++) {
        struct mitem *it = find_item(m, i, flags);
        if (!it)
            continue;
        it->flags &= ~MF_CHECKED;
        if (i == check) {
            it->flags |= MF_CHECKED | MFT_RADIOCHECK;
            found = true;
        }
    }
    return found;
}

HMENU GetSubMenu(HMENU h, int pos)
{
    struct menu *m = menu_from(h);
    if (!m || pos < 0 || pos >= m->n || !(m->items[pos].flags & MF_POPUP))
        return NULL;
    return m->items[pos].sub;
}

int GetMenuItemCount(HMENU h)
{
    struct menu *m = menu_from(h);
    return m ? m->n : -1;
}

/* ---- per-window menus -------------------------------------------------------------- */

bool SetMenu(HWND h, HMENU menu)
{
    struct wnd *w = wnd_from(h);
    if (!w || w->parent || (menu && !menu_from(menu)))
        return false;
    ui_invalidate_wnd(w);
    w->menu = menu;
    w->bar_sel = -1;
    RECT c;
    GetClientRect(w, &c);
    SendMessage(w, WM_SIZE, 0, MAKELPARAM(c.right, c.bottom));
    return true;
}

HMENU GetMenu(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w ? w->menu : NULL;
}

bool DrawMenuBar(HWND h)
{
    struct wnd *w = wnd_from(h);
    RECT r;
    if (!w || !frame_menubar(w, &r))
        return false;
    ui_invalidate_screen(r.left, r.top, r.right - r.left, r.bottom - r.top);
    return true;
}

HMENU GetSystemMenu(HWND h, bool revert)
{
    struct wnd *w = wnd_from(h);
    if (!w || w->parent)
        return NULL;
    if (revert) {
        DestroyMenu(w->sysmenu);
        w->sysmenu = NULL;
        return NULL;
    }
    if (!w->sysmenu && (w->sysmenu = new_menu(false))) {
        w->sysmenu->system = true;
        AppendMenu(w->sysmenu, MF_STRING, SC_RESTORE, "&Restore");
        AppendMenu(w->sysmenu, MF_STRING, SC_MOVE, "&Move");
        AppendMenu(w->sysmenu, MF_STRING, SC_SIZE, "&Size");
        AppendMenu(w->sysmenu, MF_STRING, SC_MINIMIZE, "Mi&nimize");
        AppendMenu(w->sysmenu, MF_STRING, SC_MAXIMIZE, "Ma&ximize");
        AppendMenu(w->sysmenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(w->sysmenu, MF_STRING, SC_CLOSE, "&Close\tAlt+F4");
    }
    return w->sysmenu;
}

/* Grey out what doesn't apply to the window as it is now. */
static void sysmenu_update(struct wnd *w)
{
    struct menu *m = w->sysmenu;
    bool normal = w->state == WS_STATE_NORMAL;
    EnableMenuItem(m, SC_RESTORE, normal ? MF_GRAYED : 0);
    EnableMenuItem(m, SC_MOVE, normal ? 0 : MF_GRAYED);
    EnableMenuItem(m, SC_SIZE, normal && (w->style & WS_THICKFRAME) ? 0 : MF_GRAYED);
    EnableMenuItem(m, SC_MINIMIZE, w->state != WS_STATE_MIN && (w->style & WS_MINIMIZEBOX) ? 0 : MF_GRAYED);
    EnableMenuItem(m, SC_MAXIMIZE, w->state != WS_STATE_MAX && (w->style & WS_MAXIMIZEBOX) ? 0 : MF_GRAYED);
}

void menu_window_gone(struct wnd *w)
{
    if (!w->parent) {
        DestroyMenu(w->menu);
        DestroyMenu(w->sysmenu);
        w->menu = w->sysmenu = NULL;
    }
}

/* ---- the menu bar ------------------------------------------------------------------ */

static bool bar_item(struct wnd *w, int i, RECT *r)
{
    RECT bar;
    if (!frame_menubar(w, &bar) || i < 0 || i >= w->menu->n)
        return false;
    int x = bar.left + 6;
    for (int k = 0; k < i; k++)
        x += label_width(w->menu->items[k].text) + 16;
    *r = (RECT){ x, bar.top + 1, x + label_width(w->menu->items[i].text) + 16, bar.bottom - 1 };
    return true;
}

static int bar_hit(struct wnd *w, int x, int y)
{
    RECT r;
    for (int i = 0; bar_item(w, i, &r); i++)
        if (rect_contains(&r, x, y))
            return i;
    return -1;
}

static void bar_invalidate(struct wnd *w)
{
    DrawMenuBar(w);
}

void menubar_paint(struct wnd *w)
{
    RECT bar, r;
    if (!frame_menubar(w, &bar))
        return;
    gfx_fill(&ui_gfx, bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top, C_FACE);
    bool active = w == wnd_active();
    for (int i = 0; bar_item(w, i, &r); i++) {
        struct mitem *it = &w->menu->items[i];
        bool sel = i == w->bar_sel;
        if (r.left >= bar.right)
            break;
        if (sel)
            gfx_fill(&ui_gfx, r.left, r.top, r.right - r.left, r.bottom - r.top, C_ACCENT);
        uint8_t fg = sel ? C_WHITE : (it->flags & (MF_GRAYED | MF_DISABLED)) || !active ? C_GRAY3 : C_BLACK;
        draw_label(r.left + 8, r.top + 1, it->text, fg);
    }
}

/* ---- pop-up windows ------------------------------------------------------------------ */

void menu_process_gone(int pid)
{
    for (int i = 0; i < MENU_MAX; i++)
        if (menus[i].magic == MENU_MAGIC && menus[i].pid == pid)
            menus[i].magic = 0;
}

static int item_h(const struct mitem *it) { return (it->flags & MF_SEPARATOR) ? SEP_H : MITEM_H; }

static bool selectable(const struct mitem *it) { return !(it->flags & MF_SEPARATOR); }

static bool enabled(const struct mitem *it)
{
    return !(it->flags & (MF_GRAYED | MF_DISABLED | MF_SEPARATOR));
}

static void popup_size(struct menu *m, int *w, int *h)
{
    int label = 0, accel = 0, height = 2 * PAD;
    for (int i = 0; i < m->n; i++) {
        const char *a = accel_of(m->items[i].text);
        int lw = label_width(m->items[i].text), aw = a ? (int)strlen(a) * 8 : 0;
        if (lw > label) label = lw;
        if (aw > accel) accel = aw;
        height += item_h(&m->items[i]);
    }
    *w = TEXT_X + label + (accel ? 24 + accel : 0) + 22;
    *h = height;
}

static int item_top(struct menu *m, int i)          /* relative to the pop-up */
{
    int y = PAD;
    for (int k = 0; k < i; k++)
        y += item_h(&m->items[k]);
    return y;
}

static const uint16_t g_check[9] = { 0x0080, 0x0180, 0x0300, 0x8600, 0xCC00, 0x7800, 0x3000, 0, 0 };
static const uint16_t g_dot[7]   = { 0x3800, 0x7C00, 0xFE00, 0xFE00, 0xFE00, 0x7C00, 0x3800 };
static const uint16_t g_arrow[7] = { 0x8000, 0xC000, 0xE000, 0xF000, 0xE000, 0xC000, 0x8000 };

static void popup_paint(struct wnd *w, struct menu_level *lv)
{
    struct gfx *g = &ui_gfx;
    int x = w->x, y = w->y, cw = w->w, ch = w->h;
    gfx_fill(g, x, y, cw, ch, C_FACE);
    gfx_rect(g, x, y, cw, ch, C_BLACK);
    gfx_hline(g, x + 1, y + 1, cw - 2, C_WHITE);
    gfx_vline(g, x + 1, y + 1, ch - 2, C_WHITE);
    gfx_hline(g, x + 1, y + ch - 2, cw - 2, C_GRAY4);
    gfx_vline(g, x + cw - 2, y + 1, ch - 2, C_GRAY4);
    for (int i = 0; i < lv->m->n; i++) {
        struct mitem *it = &lv->m->items[i];
        int iy = y + item_top(lv->m, i);
        if (it->flags & MF_SEPARATOR) {
            gfx_hline(g, x + 2, iy + 3, cw - 4, C_GRAY4);
            gfx_hline(g, x + 2, iy + 4, cw - 4, C_WHITE);
            continue;
        }
        bool sel = i == lv->sel, on = enabled(it);
        if (sel)
            gfx_fill(g, x + 2, iy, cw - 4, MITEM_H, C_ACCENT);
        uint8_t fg = sel ? (on ? C_WHITE : C_GRAY5) : (on ? C_BLACK : C_GRAY4);
        if (it->flags & MF_CHECKED) {
            if (it->flags & MFT_RADIOCHECK)
                gfx_bitmap(g, x + 7, iy + 5, g_dot, 7, fg);
            else
                gfx_bitmap(g, x + 6, iy + 4, g_check, 9, fg);
        }
        draw_label(x + TEXT_X, iy + 1, it->text, fg);
        const char *a = accel_of(it->text);
        if (a)
            gfx_text(g, x + cw - 22 - (int)strlen(a) * 8, iy + 1, a, fg, GFX_TRANSPARENT);
        if (it->flags & MF_POPUP)
            gfx_bitmap(g, x + cw - 12, iy + 5, g_arrow, 7, fg);
    }
}

LRESULT menu_popup_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        dc_apply(dc);
        struct menu_level *lv = (struct menu_level *)(uintptr_t)w->userdata;
        if (lv && lv->m)
            popup_paint(w, lv);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void invalidate_item(struct menu_level *lv, int i)
{
    if (i < 0 || !lv->w)
        return;
    ui_invalidate_screen(lv->w->x, lv->w->y + item_top(lv->m, i), lv->w->w, item_h(&lv->m->items[i]));
}

static void set_sel(int k, int i)
{
    struct menu_level *lv = &T.lv[k];
    if (lv->sel == i)
        return;
    invalidate_item(lv, lv->sel);
    lv->sel = i;
    invalidate_item(lv, i);
}

static int first_selectable(struct menu *m)
{
    for (int i = 0; i < m->n; i++)
        if (selectable(&m->items[i]))
            return i;
    return -1;
}

static void close_to(int k)                         /* keep levels below k */
{
    while (T.n > k) {
        T.n--;
        DestroyWindow(T.lv[T.n].w);
        T.lv[T.n].w = NULL;
    }
}

/* flip_x: where to go instead if it doesn't fit to the right (submenus) */
static void open_popup_at(struct menu *m, int x, int y, bool keyboard, int pos, int flip_x)
{
    if (T.n == DEPTH || !m)
        return;
    if (m->system)
        sysmenu_update(T.owner);
    SendMessage(T.owner, WM_INITMENUPOPUP, (WPARAM)m, MAKELPARAM(pos, m->system));
    int w, h;
    popup_size(m, &w, &h);
    if (x + w > S->scr_w && flip_x >= 0) x = flip_x - w;
    if (x + w > S->scr_w) x = S->scr_w - w;
    if (y + h > S->scr_h) y = S->scr_h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    struct menu_level *lv = &T.lv[T.n];
    lv->m = m;
    lv->sel = keyboard ? first_selectable(m) : -1;
    lv->w = wnd_from(CreateWindow("#MENU", "", WS_POPUP | WS_VISIBLE, x, y, w, h, NULL, 0, NULL));
    if (!lv->w)
        return;
    lv->w->userdata = (int32_t)(uintptr_t)lv;
    T.n++;
}

static void open_popup(struct menu *m, int x, int y, bool keyboard, int pos)
{
    open_popup_at(m, x, y, keyboard, pos, -1);
}

static void open_bar_item(int i, bool keyboard)
{
    struct wnd *w = T.owner;
    close_to(0);
    if (w->bar_sel != i) {
        w->bar_sel = i;
        bar_invalidate(w);
    }
    RECT r;
    if (i >= 0 && bar_item(w, i, &r) && (w->menu->items[i].flags & MF_POPUP) && enabled(&w->menu->items[i]))
        open_popup(w->menu->items[i].sub, r.left, r.bottom + 1, keyboard, i);
}

static void open_sub(int k, int i, bool keyboard)   /* item i of level k */
{
    struct menu_level *lv = &T.lv[k];
    close_to(k + 1);
    struct mitem *it = &lv->m->items[i];
    if ((it->flags & MF_POPUP) && enabled(it))
        open_popup_at(it->sub, lv->w->x + lv->w->w - 3, lv->w->y + item_top(lv->m, i) - PAD,
                      keyboard, i, lv->w->x + 3);
}

static void choose(unsigned id)
{
    T.chosen = true;
    T.result = id;
    T.done = true;
}

static void activate_item(int k, int i, bool keyboard)
{
    struct mitem *it = &T.lv[k].m->items[i];
    if (!enabled(it))
        return;
    if (it->flags & MF_POPUP) {
        set_sel(k, i);
        open_sub(k, i, keyboard);
    } else {
        choose(it->id);
    }
}

static void activate_bar_item(int i, bool keyboard)
{
    struct mitem *it = &T.owner->menu->items[i];
    if (!enabled(it))
        return;
    if (it->flags & MF_POPUP)
        open_bar_item(i, keyboard);
    else
        choose(it->id);
}

/* ---- the modal loop ---------------------------------------------------------------- */

static int level_at(int x, int y)
{
    for (int k = T.n - 1; k >= 0; k--) {
        RECT r = { T.lv[k].w->x, T.lv[k].w->y, T.lv[k].w->x + T.lv[k].w->w, T.lv[k].w->y + T.lv[k].w->h };
        if (rect_contains(&r, x, y))
            return k;
    }
    return -1;
}

static int item_at(int k, int y)
{
    struct menu_level *lv = &T.lv[k];
    int ry = y - lv->w->y;
    for (int i = 0; i < lv->m->n; i++) {
        int top = item_top(lv->m, i);
        if (ry >= top && ry < top + item_h(&lv->m->items[i]))
            return selectable(&lv->m->items[i]) ? i : -1;
    }
    return -1;
}

static int bar_count(void) { return T.bar ? T.owner->menu->n : 0; }

static void step_bar(int dir)
{
    int n = bar_count(), i = T.owner->bar_sel;
    if (!n)
        return;
    bool was_open = T.n > 0;
    i = (i + dir + n) % n;
    if (was_open)
        open_bar_item(i, true);
    else {
        T.owner->bar_sel = i;
        bar_invalidate(T.owner);
    }
}

static void step_sel(int k, int dir)
{
    struct menu *m = T.lv[k].m;
    int i = T.lv[k].sel;
    for (int tries = 0; tries < m->n; tries++) {
        i = i < 0 ? (dir > 0 ? 0 : m->n - 1) : (i + dir + m->n) % m->n;
        if (selectable(&m->items[i])) {
            set_sel(k, i);
            close_to(k + 1);
            return;
        }
    }
}

static void mouse(const struct modal_ev *e)
{
    int k = level_at(e->x, e->y);
    int b = T.bar ? bar_hit(T.owner, e->x, e->y) : -1;

    if (e->moved || e->ldown) {
        if (k >= 0) {
            int i = item_at(k, e->y);
            if (i != T.lv[k].sel) {
                set_sel(k, i);
                close_to(k + 1);
                if (i >= 0 && (T.lv[k].m->items[i].flags & MF_POPUP))
                    open_sub(k, i, false);
            } else if (k < T.n - 1 && T.lv[k + 1].sel >= 0) {
                set_sel(k + 1, -1);                 /* back on the parent item */
            }
        } else if (b >= 0 && b != T.owner->bar_sel) {
            open_bar_item(b, false);
        } else if (b < 0 && T.n && T.lv[T.n - 1].sel >= 0) {
            set_sel(T.n - 1, -1);                   /* pointer left the menu */
        }
    }
    if (e->ldown && k < 0) {
        if (T.sysclick && GetTickCount() - T.opened <= 500) {
            RECT box = frame_box(T.owner, HT_SYSMENU);
            if (rect_contains(&box, e->x, e->y)) {   /* double-click: close */
                choose(SC_CLOSE);
                return;
            }
        }
        if (b < 0 || (b == T.owner->bar_sel && T.n))
            T.done = true;                          /* outside, or the open title again */
        else if (b >= 0)
            activate_bar_item(b, false);
    }
    if (e->lup && k >= 0) {
        int i = item_at(k, e->y);
        if (i >= 0 && !(T.lv[k].m->items[i].flags & MF_POPUP))
            activate_item(k, i, false);
    }
    if (e->rdown && k < 0 && b < 0)
        T.done = true;
}

static void key(int vk)
{
    int k = T.n - 1;
    switch (vk) {
    case VK_MENU:
    case VK_F10:
        T.done = true;
        return;
    case VK_ESCAPE:
        if (T.n > 1 || (T.n == 1 && T.bar))
            close_to(T.n - 1);
        else
            T.done = true;
        return;
    case VK_UP:
    case VK_DOWN:
        if (k >= 0)
            step_sel(k, vk == VK_UP ? -1 : 1);
        else if (T.bar)
            activate_bar_item(T.owner->bar_sel, true);
        return;
    case VK_RIGHT:
        if (k >= 0 && T.lv[k].sel >= 0 && (T.lv[k].m->items[T.lv[k].sel].flags & MF_POPUP))
            activate_item(k, T.lv[k].sel, true);
        else if (T.bar)
            step_bar(1);
        return;
    case VK_LEFT:
        if (T.n > 1)
            close_to(T.n - 1);
        else if (T.bar)
            step_bar(-1);
        return;
    case VK_RETURN:
        if (k >= 0 && T.lv[k].sel >= 0)
            activate_item(k, T.lv[k].sel, true);
        else if (k < 0 && T.bar)
            activate_bar_item(T.owner->bar_sel, true);
        return;
    }
    char ch = kbd_char(vk, false);
    if (!ch)
        return;
    if (k >= 0) {
        for (int i = 0; i < T.lv[k].m->n; i++)
            if (mnemonic(T.lv[k].m->items[i].text) == ch) {
                set_sel(k, i);
                activate_item(k, i, true);
                return;
            }
    } else if (T.bar) {
        for (int i = 0; i < bar_count(); i++)
            if (mnemonic(T.owner->menu->items[i].text) == ch) {
                activate_bar_item(i, true);
                return;
            }
    }
}

/* Run the loop; -> the chosen id (0 for none), already sent unless asked. */
static unsigned run(bool return_cmd)
{
    ui_modal_begin();
    if (T.bar || T.system)
        SendMessage(T.owner, WM_INITMENU, (WPARAM)(T.bar ? T.owner->menu : T.owner->sysmenu), 0);
    while (!T.done) {
        struct modal_ev e;
        ui_modal_event(&e);
        if (!wnd_from(T.owner) || e.type == EV_FOCUS)
            break;
        if (e.type == EV_MOUSE)
            mouse(&e);
        else if (e.type == EV_KEY && !e.up)
            key(e.vk);
        if (!T.done && T.bar && T.n == 0 && T.owner->bar_sel < 0)
            T.done = true;
    }
    close_to(0);
    struct wnd *owner = wnd_from(T.owner);
    if (owner && T.bar) {
        owner->bar_sel = -1;
        bar_invalidate(owner);
    }
    unsigned id = T.chosen ? T.result : 0;
    if (owner && T.chosen && !return_cmd)
        PostMessage(owner, T.system ? WM_SYSCOMMAND : WM_COMMAND, T.system ? id : MAKEWPARAM(id, 0), 0);
    memset(&T, 0, sizeof T);
    ui_modal_end();
    return id;
}

static bool start(struct wnd *owner)
{
    if (T.owner || !owner)                          /* no nesting */
        return false;
    memset(&T, 0, sizeof T);
    T.owner = owner;
    T.opened = GetTickCount();
    ui_paint();
    return true;
}

void menu_track_bar(struct wnd *w, int x, int y)
{
    int b = bar_hit(w, x, y);
    if (b < 0 || !start(w))
        return;
    T.bar = true;
    activate_bar_item(b, false);
    run(false);
}

static void track_sys(struct wnd *w, bool click)
{
    if (!(w->style & WS_SYSMENU) || !GetSystemMenu(w, false) || !start(w))
        return;
    T.system = true;
    T.sysclick = click;
    RECT box = frame_box(w, HT_SYSMENU), o;
    frame_outer(w, &o);
    if (w->state == WS_STATE_MIN)                   /* a tile: above it */
        open_popup(w->sysmenu, o.left, o.top - 2 * PAD - 6 * MITEM_H - SEP_H, !click, 0);
    else
        open_popup(w->sysmenu, box.left, o.top + FRAME + TITLE_H, !click, 0);
    run(false);
}

void menu_track_sys(struct wnd *w) { track_sys(w, true); }

void menu_keymenu(struct wnd *w, int ch)
{
    w = wnd_top(w);
    if (!w || T.owner)
        return;
    if (ch == ' ') {
        track_sys(w, false);
        return;
    }
    RECT bar;
    if (!frame_menubar(w, &bar) || !w->menu->n)
        return;
    if (ch) {                                       /* Alt+letter: that menu */
        for (int i = 0; i < w->menu->n; i++)
            if (mnemonic(w->menu->items[i].text) == lower((char)ch)) {
                start(w);
                T.bar = true;
                activate_bar_item(i, true);
                run(false);
                return;
            }
        return;
    }
    start(w);                                       /* Alt or F10: the bar */
    T.bar = true;
    w->bar_sel = 0;
    bar_invalidate(w);
    run(false);
}

int TrackPopupMenu(HMENU h, unsigned flags, int x, int y, HWND owner)
{
    struct menu *m = menu_from(h);
    struct wnd *o = wnd_from(owner);
    if (!m || !o || !start(o))
        return 0;
    T.system = m->system;
    if (flags & TPM_BOTTOMALIGN) {
        int w, h;
        popup_size(m, &w, &h);
        y -= h;
    }
    open_popup(m, x, y, false, 0);
    unsigned id = run(flags & TPM_RETURNCMD);
    return (flags & TPM_RETURNCMD) ? (int)id : id != 0;
}
