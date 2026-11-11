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
 *
 * Items are text (in the window-text font), MF_BITMAP pictures, or
 * MF_OWNERDRAW (the owner measures and draws them: WM_MEASUREITEM,
 * WM_DRAWITEM); any item can carry an icon in the check column
 * (SetMenuItemImage), and a menu a banner up its left side. The owner
 * hears WM_MENUSELECT as the highlight moves. A menu bar too wide for its
 * window wraps onto more rows.
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
    char buf[ITEM_TEXT];
    int n = 0;
    for (; *s && *s != '\t' && n < ITEM_TEXT - 1; s++) {
        if (*s == '&' && s[1]) s++;
        buf[n++] = *s;
    }
    return gfx_text_width(&ui_gfx, GFX_FONT_PROP, buf, n);
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
    gfx_text_font(&ui_gfx, GFX_FONT_PROP, x, y, buf, n, fg, GFX_TRANSPARENT);
    if (ul >= 0) {
        int ux = gfx_text_width(&ui_gfx, GFX_FONT_PROP, buf, ul);
        int uw = gfx_text_width(&ui_gfx, GFX_FONT_PROP, buf + ul, 1) - 1;
        gfx_hline(&ui_gfx, x + ux, y + 14, uw > 0 ? uw : 1, fg);
    }
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

/* Fill in an item: text is a string, or MF_BITMAP's BITMAPIMAGE *, or
 * MF_OWNERDRAW's value for WM_DRAWITEM. */
static void set_item(struct mitem *it, unsigned flags, uintptr_t id, const char *text)
{
    const uint8_t *image = it->image;
    int iw = it->img_w, ih = it->img_h;
    bool keep_icon = !(flags & MF_BITMAP) && !(it->flags & MF_BITMAP);
    memset(it, 0, sizeof *it);
    it->flags = flags & ~MF_BYPOSITION;
    if (flags & MF_POPUP)
        it->sub = (struct menu *)id;
    else
        it->id = (unsigned)id;
    if (flags & MF_OWNERDRAW) {
        it->data = (LPARAM)text;
    } else if (flags & MF_BITMAP) {
        const BITMAPIMAGE *b = (const BITMAPIMAGE *)text;
        if (b) { it->image = b->pixels; it->img_w = (int16_t)b->width; it->img_h = (int16_t)b->height; }
    } else if (text && !(flags & MF_SEPARATOR)) {
        strncpy0(it->text, text, ITEM_TEXT);
    }
    if (keep_icon && image) {                   /* ModifyMenu keeps the icon */
        it->image = image;
        it->img_w = (int16_t)iw;
        it->img_h = (int16_t)ih;
    }
}

static bool insert_at(struct menu *m, int pos, unsigned flags, uintptr_t id, const char *text)
{
    if (m->n == ITEM_MAX || ((flags & MF_POPUP) && !menu_from((HMENU)id)))
        return false;
    if (pos < 0 || pos > m->n)
        pos = m->n;
    memmove(&m->items[pos + 1], &m->items[pos], (size_t)(m->n - pos) * sizeof m->items[0]);
    m->n++;
    memset(&m->items[pos], 0, sizeof m->items[pos]);
    set_item(&m->items[pos], flags, id, text);
    return true;
}

bool AppendMenu(HMENU h, unsigned flags, uintptr_t id, const char *text)
{
    struct menu *m = menu_from(h);
    return m && insert_at(m, m->n, flags, id, text);
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

/* Where an item is: its menu and position (by command: searched in submenus too). */
static struct menu *find_pos(struct menu *m, unsigned item, unsigned flags, int *pos)
{
    if (flags & MF_BYPOSITION) {
        *pos = (int)item;
        return item < (unsigned)m->n ? m : NULL;
    }
    for (int i = 0; i < m->n; i++) {
        struct mitem *it = &m->items[i];
        if (it->flags & MF_POPUP) {
            struct menu *r = find_pos(it->sub, item, flags, pos);
            if (r) return r;
        } else if (!(it->flags & MF_SEPARATOR) && it->id == item) {
            *pos = i;
            return m;
        }
    }
    return NULL;
}

/* item: the position (MF_BYPOSITION) or the command it goes before. */
bool InsertMenu(HMENU h, unsigned item, unsigned flags, uintptr_t id, const char *text)
{
    struct menu *m = menu_from(h);
    if (!m) return false;
    if (item == (unsigned)-1 && (flags & MF_BYPOSITION))
        return insert_at(m, m->n, flags, id, text);
    int pos;
    struct menu *in = find_pos(m, item, flags, &pos);
    if (!in) {
        if (flags & MF_BYPOSITION) return insert_at(m, m->n, flags, id, text);
        return false;
    }
    return insert_at(in, pos, flags, id, text);
}

static bool take_out(HMENU h, unsigned item, unsigned flags, bool destroy)
{
    struct menu *m = menu_from(h);
    int pos;
    struct menu *in = m ? find_pos(m, item, flags, &pos) : NULL;
    if (!in) return false;
    if (destroy && (in->items[pos].flags & MF_POPUP))
        DestroyMenu(in->items[pos].sub);
    memmove(&in->items[pos], &in->items[pos + 1], (size_t)(in->n - pos - 1) * sizeof in->items[0]);
    in->n--;
    return true;
}

bool DeleteMenu(HMENU h, unsigned item, unsigned flags) { return take_out(h, item, flags, true); }
bool RemoveMenu(HMENU h, unsigned item, unsigned flags) { return take_out(h, item, flags, false); }

bool ModifyMenu(HMENU h, unsigned item, unsigned flags, uintptr_t id, const char *text)
{
    struct menu *m = menu_from(h);
    int pos;
    struct menu *in = m ? find_pos(m, item, flags, &pos) : NULL;
    if (!in || ((flags & MF_POPUP) && !menu_from((HMENU)id)))
        return false;
    set_item(&in->items[pos], flags, id, text);
    return true;
}

unsigned GetMenuItemID(HMENU h, int pos)
{
    struct menu *m = menu_from(h);
    if (!m || pos < 0 || pos >= m->n || (m->items[pos].flags & MF_POPUP))
        return (unsigned)-1;
    return m->items[pos].id;
}

int GetMenuString(HMENU h, unsigned item, char *buf, int max, unsigned flags)
{
    struct menu *m = menu_from(h);
    int pos;
    struct menu *in = m ? find_pos(m, item, flags, &pos) : NULL;
    if (!in || !buf || max <= 0) return 0;
    strncpy0(buf, in->items[pos].text, (size_t)max);
    return (int)strlen(buf);
}

int GetMenuState(HMENU h, unsigned item, unsigned flags)
{
    struct menu *m = menu_from(h);
    int pos;
    struct menu *in = m ? find_pos(m, item, flags, &pos) : NULL;
    if (!in) return -1;
    unsigned f = in->items[pos].flags & ~(unsigned)MF_BYPOSITION;
    if (f & MF_POPUP)                           /* Windows: the item count in the high byte */
        f |= (unsigned)in->items[pos].sub->n << 8;
    return (int)f;
}

/* A 16x16 (or 32x32) picture, one byte a pixel in the theme's colours, FFh
 * see-through, in the check column; NULL takes it away. The pixels stay
 * the program's: they must outlive the menu. */
bool SetMenuItemImage(HMENU h, unsigned item, unsigned flags, const uint8_t *pixels, int size)
{
    struct menu *m = menu_from(h);
    int pos;
    struct menu *in = m ? find_pos(m, item, flags, &pos) : NULL;
    if (!in || (in->items[pos].flags & MF_BITMAP)) return false;
    in->items[pos].image = pixels;
    in->items[pos].img_w = in->items[pos].img_h = (int16_t)(pixels ? (size == 32 ? 32 : 16) : 0);
    return true;
}

bool SetMenuBanner(HMENU h, const char *text, uint8_t bg)
{
    struct menu *m = menu_from(h);
    if (!m) return false;
    strncpy0(m->banner, text ? text : "", sizeof m->banner);
    m->banner_bg = bg;
    return true;
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
    ui_reflow(w);
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

static int bar_item_w(struct mitem *it)
{
    if (it->flags & MF_BITMAP) return it->img_w + 8;
    return label_width(it->text) + 16;
}

/* Item i of a bar from left to right, a new row when one doesn't fit
 * (left: the bar's left edge, right: its right; -> its row). */
static int bar_layout(struct menu *m, int i, int left, int right, int *x0, int *x1)
{
    int x = left + 6, row = 0;
    for (int k = 0; ; k++) {
        int iw = bar_item_w(&m->items[k]);
        if (x + iw > right - 2 && x > left + 6) {
            x = left + 6;
            row++;
        }
        if (k == i) {
            *x0 = x;
            *x1 = x + iw;
            return row;
        }
        x += iw;
    }
}

/* Rows the bar takes at a window's outer width. */
int menu_bar_rows(struct menu *m, int outer_w)
{
    if (!m || !m->n)
        return 1;
    int x0, x1;
    return bar_layout(m, m->n - 1, FRAME, outer_w - FRAME, &x0, &x1) + 1;
}

static bool bar_item(struct wnd *w, int i, RECT *r)
{
    RECT bar;
    if (!frame_menubar(w, &bar) || i < 0 || i >= w->menu->n)
        return false;
    int x0, x1, row = bar_layout(w->menu, i, bar.left, bar.right, &x0, &x1);
    *r = (RECT){ x0, bar.top + row * MENUBAR_H + 1, x1, bar.top + (row + 1) * MENUBAR_H - 1 };
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
        if (sel)
            gfx_fill(&ui_gfx, r.left, r.top, r.right - r.left, r.bottom - r.top, C_ACCENT);
        uint8_t fg = sel ? C_WHITE : (it->flags & (MF_GRAYED | MF_DISABLED)) || !active ? C_GRAY3 : C_BLACK;
        if (it->flags & MF_BITMAP) {
            if (it->image)
                gfx_blit8(&ui_gfx, r.left + 4, r.top + (r.bottom - r.top - it->img_h) / 2,
                          it->img_w, it->img_h, it->image, it->img_w);
        } else {
            draw_label(r.left + 8, r.top + 1, it->text, fg);
        }
    }
}

/* ---- pop-up windows ------------------------------------------------------------------ */

void menu_process_gone(int pid)
{
    for (int i = 0; i < MENU_MAX; i++)
        if (menus[i].magic == MENU_MAGIC && menus[i].pid == pid)
            menus[i].magic = 0;
}

static int item_h(const struct mitem *it)
{
    if (it->flags & MF_SEPARATOR) return SEP_H;
    if (it->flags & MF_OWNERDRAW) return it->oh > 0 ? it->oh : MITEM_H;
    if (it->flags & MF_BITMAP) return it->img_h + 4;
    return it->img_h + 4 > MITEM_H ? it->img_h + 4 : MITEM_H;
}

static int banner_w(const struct menu *m) { return m->banner[0] ? 22 : 0; }
static int text_x(const struct menu *m)               /* where labels start */
{
    int icon = 0;
    for (int i = 0; i < m->n; i++)
        if (!(m->items[i].flags & MF_BITMAP) && m->items[i].img_w > icon) icon = m->items[i].img_w;
    return banner_w(m) + (icon > 16 ? icon + 10 : TEXT_X);
}

/* MF_OWNERDRAW items: the owner says how big (once a menu opens). */
static void measure(struct menu *m)
{
    for (int i = 0; i < m->n; i++) {
        struct mitem *it = &m->items[i];
        if (!(it->flags & MF_OWNERDRAW) || !T.owner || !wnd_mine(T.owner))
            continue;
        MEASUREITEMSTRUCT mi = { ODT_MENU, 0, it->id, 80, MITEM_H, it->data };
        SendMessage(T.owner, WM_MEASUREITEM, 0, (LPARAM)&mi);
        it->ow = (int16_t)clampi((int)mi.itemWidth, 8, 400);
        it->oh = (int16_t)clampi((int)mi.itemHeight, 4, 200);
    }
}

static bool selectable(const struct mitem *it) { return !(it->flags & MF_SEPARATOR); }

static bool enabled(const struct mitem *it)
{
    return !(it->flags & (MF_GRAYED | MF_DISABLED | MF_SEPARATOR));
}

static void popup_size(struct menu *m, int *w, int *h)
{
    int label = 0, accel = 0, height = 2 * PAD, other = 0;
    for (int i = 0; i < m->n; i++) {
        struct mitem *it = &m->items[i];
        height += item_h(it);
        if (it->flags & MF_OWNERDRAW) { if (it->ow + 4 > other) other = it->ow + 4; continue; }
        if (it->flags & MF_BITMAP) { if (banner_w(m) + it->img_w + 8 > other) other = banner_w(m) + it->img_w + 8; continue; }
        const char *a = accel_of(it->text);
        int lw = label_width(it->text), aw = a ? gfx_text_width(&ui_gfx, GFX_FONT_PROP, a, -1) : 0;
        if (lw > label) label = lw;
        if (aw > accel) accel = aw;
    }
    *w = text_x(m) + label + (accel ? 24 + accel : 0) + 22;
    if (other > *w) *w = other;
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
    struct menu *m = lv->m;
    int bw = banner_w(m), tx = text_x(m);
    if (bw) {                                   /* the banner, written bottom to top */
        gfx_fill(g, x + 2, y + 2, bw, ch - 4, m->banner_bg ? m->banner_bg : C_ACCENT_DK);
        int len = (int)strlen(m->banner);
        int by = y + ch - 6;
        for (int c = 0; c < len; c++) {
            uint8_t ch8 = (uint8_t)m->banner[c];
            const uint8_t *glyph = g->font + ch8 * 16;
            int adv = gfx_text_width(g, GFX_FONT_PROP, (const char *)&m->banner[c], 1);
            for (int r = 0; r < 16; r++)            /* row r of the glyph: column x+3+r */
                for (int b = 0; b < 8; b++)
                    if (glyph[r] & (0x80 >> b))
                        gfx_pixel(g, x + 5 + r, by - b, C_WHITE);
            by -= adv;
            if (by < y + 4) break;
        }
    }
    for (int i = 0; i < m->n; i++) {
        struct mitem *it = &m->items[i];
        int iy = y + item_top(m, i), ih = item_h(it), ix = x + 2 + bw;
        if (it->flags & MF_SEPARATOR) {
            gfx_hline(g, ix, iy + 3, cw - 4 - bw, C_GRAY4);
            gfx_hline(g, ix, iy + 4, cw - 4 - bw, C_WHITE);
            continue;
        }
        bool sel = i == lv->sel, on = enabled(it);
        if (it->flags & MF_OWNERDRAW) {
            if (T.owner && wnd_mine(T.owner)) {
                struct dc d = { .w = w, .ox = x, .oy = y, .cx0 = ui_gfx.cx0, .cy0 = ui_gfx.cy0,
                                .cx1 = ui_gfx.cx1, .cy1 = ui_gfx.cy1 };
                RECT r = { 2 + bw, iy - y, cw - 2, iy - y + ih };
                DRAWITEMSTRUCT di = { ODT_MENU, 0, it->id, ODA_DRAWENTIRE,
                                      (sel ? ODS_SELECTED : 0) | (on ? 0 : ODS_GRAYED) |
                                      ((it->flags & MF_CHECKED) ? ODS_CHECKED : 0),
                                      (HWND)(void *)m, &d, r, it->data };
                SendMessage(T.owner, WM_DRAWITEM, 0, (LPARAM)&di);
                dc_apply(&d);
            }
            continue;
        }
        if (sel)
            gfx_fill(g, ix, iy, cw - 4 - bw, ih, C_ACCENT);
        uint8_t fg = sel ? (on ? C_WHITE : C_GRAY5) : (on ? C_BLACK : C_GRAY4);
        if (it->flags & MF_BITMAP) {
            if (it->image)
                gfx_blit8(g, ix + 4, iy + 2, it->img_w, it->img_h, it->image, it->img_w);
        } else {
            int ty = iy + (ih - 16) / 2;
            if (it->image)
                gfx_blit8(g, ix + (tx - bw - 2 - it->img_w) / 2, iy + (ih - it->img_h) / 2,
                          it->img_w, it->img_h, it->image, it->img_w);
            if (it->flags & MF_CHECKED) {
                if (it->image)                      /* the icon pushed in */
                    gfx_rect(g, ix + (tx - bw - 2 - it->img_w) / 2 - 2, iy + (ih - it->img_h) / 2 - 2,
                             it->img_w + 4, it->img_h + 4, fg);
                else if (it->flags & MFT_RADIOCHECK)
                    gfx_bitmap(g, ix + 5, ty + 4, g_dot, 7, fg);
                else
                    gfx_bitmap(g, ix + 4, ty + 3, g_check, 9, fg);
            }
            draw_label(x + tx, ty, it->text, fg);
            const char *a = accel_of(it->text);
            if (a)
                gfx_text_font(g, GFX_FONT_PROP, x + cw - 22 - gfx_text_width(g, GFX_FONT_PROP, a, -1), ty,
                              a, -1, fg, GFX_TRANSPARENT);
        }
        if (it->flags & MF_POPUP)
            gfx_bitmap(g, x + cw - 12, iy + (ih - 7) / 2, g_arrow, 7, fg);
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

/* WM_MENUSELECT: the highlighted item (a command's id, a submenu's
 * position) and its flags, lParam the menu; 0xFFFF flags when it closes. */
static void menu_select(struct menu *m, int i)
{
    if (!T.owner || !wnd_mine(T.owner))
        return;
    if (!m || i < 0) {
        SendMessage(T.owner, WM_MENUSELECT, MAKEWPARAM(0, m ? 0 : 0xFFFF), (LPARAM)m);
        return;
    }
    struct mitem *it = &m->items[i];
    unsigned f = (it->flags & 0xFFFF) | (m->system ? MF_SYSMENU : 0);
    SendMessage(T.owner, WM_MENUSELECT, MAKEWPARAM((it->flags & MF_POPUP) ? (unsigned)i : it->id, f),
                (LPARAM)m);
}

static void set_sel(int k, int i)
{
    struct menu_level *lv = &T.lv[k];
    if (lv->sel == i)
        return;
    invalidate_item(lv, lv->sel);
    lv->sel = i;
    invalidate_item(lv, i);
    menu_select(lv->m, i);
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
    measure(m);
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
    if (lv->sel >= 0)
        menu_select(m, lv->sel);
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
        menu_select(w->menu, i);
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
        menu_select(T.owner->menu, i);
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
    if (owner)
        menu_select(NULL, -1);                      /* closed */
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
