/*
 * input.c -- turning raw keyboard and mouse events into messages, the
 * cursor, and the modal loops that move and size windows.
 *
 * Raw input is shared by every process; whichever one pumps first turns it
 * into messages for the right windows (any process's) and wakes their
 * owners. Keys go to the focus window as WM_KEYDOWN/UP (WM_SYSKEYDOWN/UP
 * while Alt is held). Mouse events go to the capture window if there is
 * one, else to the deepest child under the pointer, in client coordinates.
 * A click activates its window first. A click on a frame becomes
 * WM_NCLBUTTONDOWN for the window's owner, and raw input is reserved for
 * that process: its DefWindowProc runs the loop that drags the outline,
 * tracks a box or opens a menu (so a program can intercept them).
 */
#include "uipriv.h"

struct gfx ui_screen;

#define IN (S->in)

/* ---- cursor and outline ------------------------------------------------------ */

/* Cursor shapes: 'k' black, 'w' white, anything else see-through. In
 * linear (VESA) modes a shape is drawn solid over a saved copy of what it
 * covers; in 16-colour mode its black pixels are XORed onto the screen
 * (XOR in 256 colours would only swap palette entries). The resize arrows
 * are made at first use from two drawn ones. */
struct cursor { const char *rows[CUR_H]; int w, h, hx, hy; };

static const char *const arrow_art[] = {
    "k           ", "kk          ", "kwk         ", "kwwk        ", "kwwwk       ",
    "kwwwwk      ", "kwwwwwk     ", "kwwwwwwk    ", "kwwwwwwwk   ", "kwwwwwwwwk  ",
    "kwwwwwwwwwk ", "kwwwwwwkkkkk", "kwwwkwwk    ", "kwwk kwwk   ", "kwk  kwwk   ",
    "kk    kwwk  ", "k     kwwk  ", "       kwwk ", "        kk  ",
};
static const char *const ibeam_art[] = {
    "kkk kkk", "   k   ", "   k   ", "   k   ", "   k   ", "   k   ", "   k   ", "   k   ",
    "   k   ", "   k   ", "   k   ", "   k   ", "   k   ", "   k   ", "   k   ", "kkk kkk",
};
static const char *const wait_art[] = {
    "kkkkkkkkkkk", "kwwwwwwwwwk", " kkkkkkkkk ", " kwwwwwwwk ", " kwkwkwkwk ",
    "  kwkwkwk  ", "   kwkwk   ", "    kwk    ", "    kwk    ", "   kwwwk   ",
    "  kwwkwwk  ", " kwwkwkwwk ", " kwkwkwkwk ", " kkkkkkkkk ", "kwwwwwwwwwk",
    "kkkkkkkkkkk",
};
static const char *const we_art[] = {
    "   kk     kk   ", "  kwk     kwk  ", " kwwkkkkkkkwwk ", "kwwwwwwwwwwwwwk",
    " kwwkkkkkkkwwk ", "  kwk     kwk  ", "   kk     kk   ",
};
static const char *const cross_art[] = {
    "       k       ", "       k       ", "       k       ", "       k       ", "       k       ",
    "       k       ", "               ", "kkkkkk   kkkkkk", "               ", "       k       ",
    "       k       ", "       k       ", "       k       ", "       k       ", "       k       ",
};
static const char *const no_art[] = {
    "    kkkkk    ", "  kkwwwwwkk  ", " kwwkkkkkwwk ", " kwkk   kkwk ", "kwkkwk   kwk ",
    "kwk kwk   kwk", "kwk  kwk  kwk", "kwk   kwk kwk", " kwk   kwkkwk", " kwkk   kkwk ",
    " kwwkkkkkwwk ", "  kkwwwwwkk  ", "    kkkkk    ",
};

static struct cursor cursors[CURSOR_COUNT];
static char gen[4][CUR_H][CUR_W + 1];          /* SIZENS, NWSE, NESW, SIZEALL */

static void set_shape(int id, const char *const *rows, int n, int hx, int hy)
{
    struct cursor *c = &cursors[id];
    c->h = n;
    c->w = (int)strlen(rows[0]);
    for (int i = 0; i < n; i++) c->rows[i] = rows[i];
    c->hx = hx;
    c->hy = hy;
}

static void make_cursors(void)
{
    if (cursors[CURSOR_ARROW].h)
        return;
    set_shape(CURSOR_ARROW, arrow_art, 19, 0, 0);
    set_shape(CURSOR_IBEAM, ibeam_art, 16, 3, 8);
    set_shape(CURSOR_WAIT, wait_art, 16, 5, 8);
    set_shape(CURSOR_SIZEWE, we_art, 7, 7, 3);
    set_shape(CURSOR_CROSS, cross_art, 15, 7, 7);
    set_shape(CURSOR_NO, no_art, 13, 6, 6);
    const char *r[CUR_H];
    for (int y = 0; y < 15; y++) {               /* NS: WE turned on its side */
        for (int x = 0; x < 7; x++) gen[0][y][x] = we_art[x][y];
        gen[0][y][7] = '\0';
        r[y] = gen[0][y];
    }
    set_shape(CURSOR_SIZENS, r, 15, 3, 7);
    for (int d = 0; d < 2; d++) {               /* the diagonals, 13 square */
        char (*g)[CUR_W + 1] = gen[1 + d];
        for (int y = 0; y < 13; y++) { memset(g[y], ' ', 13); g[y][13] = '\0'; }
        for (int y = 0; y < 13; y++)
            for (int x = 0; x < 13; x++) {
                int a = x + y, b = (12 - x) + (12 - y);
                char c = ' ';
                if (a <= 5) c = (x == 0 || y == 0 || a == 5) ? 'k' : 'w';
                else if (b <= 5) c = (x == 12 || y == 12 || b == 5) ? 'k' : 'w';
                else if (x == y) c = 'w';
                else if (x == y + 1 || y == x + 1) c = 'k';
                g[y][d ? 12 - x : x] = c;
            }
        for (int y = 0; y < 13; y++) r[y] = g[y];
        set_shape(d ? CURSOR_SIZENESW : CURSOR_SIZENWSE, r, 13, 6, 6);
    }
    char (*g)[CUR_W + 1] = gen[3];                /* SIZEALL: both arrows crossed */
    for (int y = 0; y < 15; y++) {
        for (int x = 0; x < 15; x++) {
            char c = ' ';
            if (y >= 4 && y < 11 && we_art[y - 4][x] != ' ') c = we_art[y - 4][x];
            if (x >= 4 && x < 11 && we_art[x - 4][y] != ' ' && c != 'w') c = we_art[x - 4][y];
            g[y][x] = c;
        }
        g[y][15] = '\0';
        r[y] = g[y];
    }
    g[7][7] = 'w';
    set_shape(CURSOR_SIZEALL, r, 15, 7, 7);
}

static bool sprite(void) { return ui_screen.bpp != 4; }

static const struct cursor *drawn(void)
{
    make_cursors();
    int id = IN.cur_shown >= 0 && IN.cur_shown < CURSOR_COUNT ? IN.cur_shown : CURSOR_ARROW;
    return &cursors[id];
}

static void xor_shape(const struct cursor *c, int x, int y)
{
    uint16_t rows[CUR_H];
    for (int i = 0; i < c->h; i++) {
        rows[i] = 0;
        for (int k = 0; k < c->w && k < 16; k++)
            if (c->rows[i][k] == 'k') rows[i] |= (uint16_t)(0x8000u >> k);
    }
    gfx_xor_bitmap(&ui_screen, x, y, rows, c->h);
}

void ui_cursor(bool on)
{
    if (S->background || on == IN.cur_on)
        return;
    if (on)
        IN.cur_shown = IN.cur_shape;
    const struct cursor *c = drawn();
    int x = IN.cur_x - c->hx, y = IN.cur_y - c->hy;
    if (!sprite()) {
        xor_shape(c, x, y);
    } else if (on) {
        gfx_sprite_save(&ui_screen, x, y, c->w, c->h, IN.cur_save);
        gfx_sprite_draw(&ui_screen, x, y, c->rows, c->w, c->h);
    } else {
        gfx_sprite_restore(&ui_screen, x, y, c->w, c->h, IN.cur_save);
    }
    IN.cur_on = on;
}

/* After the screen under a shown cursor was copied from the back buffer:
 * what it covers now comes from there, and it is drawn again at once (no
 * moment without it, so no flicker). */
void ui_cursor_refresh(void)
{
    if (S->background || !IN.cur_on || !sprite())
        return;
    const struct cursor *c = drawn();
    int x = IN.cur_x - c->hx, y = IN.cur_y - c->hy;
    gfx_sprite_save(&ui_gfx, x, y, c->w, c->h, IN.cur_save);
    gfx_sprite_draw(&ui_screen, x, y, c->rows, c->w, c->h);
}

bool ui_cursor_overlaps(int x0, int y0, int x1, int y1)
{
    if (!IN.cur_on)
        return false;
    const struct cursor *c = drawn();
    int x = IN.cur_x - c->hx, y = IN.cur_y - c->hy;
    return x < x1 && x + 16 > x0 && y < y1 && y + c->h > y0;
}

/* A new shape (CURSOR_*), at once. */
void ui_set_cursor(int id)
{
    if (id < 0 || id >= CURSOR_COUNT || id == IN.cur_shape)
        return;
    bool on = IN.cur_on;
    ui_cursor(false);
    IN.cur_shape = id;
    if (on)
        ui_cursor(true);
}

int ui_edge_cursor(int edges)
{
    if ((edges & (E_L | E_T)) == (E_L | E_T) || (edges & (E_R | E_B)) == (E_R | E_B))
        return CURSOR_SIZENWSE;
    if ((edges & (E_R | E_T)) == (E_R | E_T) || (edges & (E_L | E_B)) == (E_L | E_B))
        return CURSOR_SIZENESW;
    if (edges & (E_L | E_R)) return CURSOR_SIZEWE;
    if (edges & (E_T | E_B)) return CURSOR_SIZENS;
    return CURSOR_ARROW;
}

static const int idc_ids[CURSOR_COUNT] = {
    [CURSOR_ARROW] = IDC_ARROW, [CURSOR_IBEAM] = IDC_IBEAM, [CURSOR_WAIT] = IDC_WAIT,
    [CURSOR_CROSS] = IDC_CROSS, [CURSOR_SIZENWSE] = IDC_SIZENWSE, [CURSOR_SIZENESW] = IDC_SIZENESW,
    [CURSOR_SIZEWE] = IDC_SIZEWE, [CURSOR_SIZENS] = IDC_SIZENS, [CURSOR_SIZEALL] = IDC_SIZEALL,
    [CURSOR_NO] = IDC_NO,
};

int ui_cursor_index(HCURSOR h)
{
    for (int i = 0; i < CURSOR_COUNT; i++)
        if (idc_ids[i] == (int)h)
            return i;
    return -1;
}

HCURSOR LoadCursor(void *instance, int idc)
{
    (void)instance;
    return ui_cursor_index(idc) >= 0 ? idc : 0;
}

/* Until the pointer moves into another window (or part of one); a program
 * that wants a shape of its own over part of a window sets it on each
 * WM_MOUSEMOVE there. */
HCURSOR SetCursor(HCURSOR h)
{
    HCURSOR old = idc_ids[IN.cur_shape];
    int i = ui_cursor_index(h);
    if (i >= 0)
        ui_set_cursor(i);
    return old;
}

HCURSOR GetCursor(void) { return idc_ids[IN.cur_shape]; }

bool GetCursorPos(POINT *p)
{
    if (!p) return false;
    p->x = IN.cur_x;
    p->y = IN.cur_y;
    return true;
}

static void cursor_to(int x, int y)
{
    if (x == IN.cur_x && y == IN.cur_y)
        return;
    ui_cursor(false);
    IN.cur_x = x;
    IN.cur_y = y;
    ui_cursor(true);
}

void ui_outline(bool on)
{
    if (on == IN.outline_on)
        return;
    IN.outline_on = on;
    if (S->background)
        return;
    bool cur = IN.cur_on && sprite();                   /* the sprite stays on top */
    if (cur)
        ui_cursor(false);
    gfx_xor_frame(&ui_screen, IN.outline.left, IN.outline.top,
                  IN.outline.right - IN.outline.left, IN.outline.bottom - IN.outline.top, 2);
    if (cur)
        ui_cursor(true);
}

/* ---- keyboard --------------------------------------------------------------- */

int GetKeyState(int vk)
{
    bool down = (vk == VK_SHIFT && IN.shift) || (vk == VK_CONTROL && IN.ctrl) ||
                (vk == VK_MENU && IN.alt);
    return down ? -128 : 0;
}

void ui_input_reset(void)
{
    IN.shift = IN.ctrl = IN.alt = IN.alt_alone = false;
}

/* Track the modifiers; -> the virtual key, 0 for none. */
static int key_update(int sc, bool *up_out, int *code_out)
{
    if (sc == 0xE0)                             /* extended-key prefix */
        return 0;
    bool up = sc & 0x80;
    int code = sc & 0x7F;
    int vk = kbd_vk(code);
    if (!vk)
        return 0;
    if (!up)
        IN.alt_alone = vk == VK_MENU ? (IN.alt_alone || !IN.alt) : false;
    if (vk == VK_SHIFT)   IN.shift = !up;
    if (vk == VK_CONTROL) IN.ctrl = !up;
    if (vk == VK_MENU)    IN.alt = !up;
    *up_out = up;
    *code_out = code;
    return vk;
}

static void key_event(int sc)
{
    bool up;
    int code, vk = key_update(sc, &up, &code);
    if (!vk)
        return;
    bool alone = up && vk == VK_MENU && IN.alt_alone;
    if (up && vk == VK_MENU)
        IN.alt_alone = false;

    if (!up && vk == VK_ESCAPE && IN.ctrl && wnd_from(S->shell_hook)) {
        PostMessage(S->shell_hook, WM_SYSCOMMAND, SC_TASKLIST, 0);    /* the Start menu */
        return;
    }
    if (!up && vk == VK_ESCAPE && IN.alt && !IN.ctrl) {           /* Alt+Esc: next window */
        IN.alt_alone = false;
        if (S->active && !wnd_owned_popup(S->active))
            wnd_lower(S->active);
        return;
    }
    struct wnd *target = wnd_from(S->focus);
    if (!target)
        target = S->active;
    if (!target)
        return;
    bool sys = IN.alt || vk == VK_MENU || vk == VK_F10;
    unsigned msg = up ? (sys ? WM_SYSKEYUP : WM_KEYUP) : (sys ? WM_SYSKEYDOWN : WM_KEYDOWN);
    uint32_t lp = 1u | ((uint32_t)code << 16) | (IN.alt ? 1u << 29 : 0) |
                  (IN.shift ? 1u << 25 : 0) | (alone ? 1u << 26 : 0) | (IN.ctrl ? 1u << 27 : 0) |
                  (up ? (1u << 30) | (1u << 31) : 0);
    PostMessage(target, msg, (WPARAM)vk, (LPARAM)lp);
}

/* ---- mouse ------------------------------------------------------------------ */

/* The top-level window and frame part under x,y (windows over tiles). */
static struct wnd *window_at(int x, int y, int *part, int *edges)
{
    struct wnd *list[WND_MAX];
    int n = wnd_toplevels(list);
    for (int pass = 0; pass < 2; pass++)
        for (int i = n - 1; i >= 0; i--) {
            struct wnd *t = list[i];
            if (!t->visible || !frame_shown(t) || (t->state == WS_STATE_MIN) != (pass == 1))
                continue;
            *part = frame_hit(t, x, y, edges);
            if (*part != HT_NOWHERE)
                return t;
        }
    return NULL;
}

/* The window that gets a client-area mouse message at x,y */
static struct wnd *client_target(struct wnd *t, int x, int y)
{
    int cx, cy;
    wnd_client_screen(t, &cx, &cy);
    return wnd_child_at(t, x - cx, y - cy);
}

static bool is_double(struct wnd *w, int part, uint32_t now)
{
    bool dbl = IN.last_click_wnd == w && IN.last_click_part == part && now - IN.last_click <= 500;
    IN.last_click_wnd = dbl ? NULL : w;
    IN.last_click_part = part;
    IN.last_click = now;
    return dbl;
}

/* -> true if the rest of the input now belongs to another process */
static bool press(int x, int y)
{
    int part, edges;
    struct wnd *t = window_at(x, y, &part, &edges);
    if (!t)
        return false;
    if (!t->enabled) {                          /* modal: go to its dialog */
        struct wnd *p = wnd_owned_popup(t);
        if (p)
            wnd_activate(p);
        return false;
    }
    uint32_t now = GetTickCount();
    if (part == HT_TILE) {
        PostMessage(t, WM_SYSCOMMAND, SC_RESTORE, 0);
        return false;
    }
    if (t != S->active && !(t->exstyle & WS_EX_NOACTIVATE)) {
        wnd_activate(t);
        DebugLog("activate %s", t->text);
    }
    if (part == HT_CLIENT) {
        struct wnd *c = client_target(t, x, y);
        bool dbl = (c->cls->style & CS_DBLCLKS) && is_double(c, HT_CLIENT, now);
        ui_post_mouse(c, dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, x, y, MK_LBUTTON);
        return false;
    }
    if (part == HT_CAPTION && is_double(t, part, now)) {
        PostMessage(t, WM_NCLBUTTONDBLCLK, HT_CAPTION, MAKELPARAM(x, y));
        return false;
    }
    PostMessage(t, WM_NCLBUTTONDOWN, (WPARAM)part, MAKELPARAM(x, y));
    ui_reserve_input(t->pid);                   /* its DefWindowProc tracks the mouse */
    return t->pid != ui_pid;
}

/* The shape for where the pointer is, when it has moved into another
 * window or part of one: the resize arrows on borders, the window's or its
 * class's cursor in a client area. */
static void default_cursor(int x, int y)
{
    int part, edges = 0, shape = CURSOR_ARROW;
    struct wnd *t = window_at(x, y, &part, &edges), *c = NULL;
    if (t && part == HT_CLIENT && t->enabled)
        c = client_target(t, x, y);
    uint32_t key = (uint32_t)(uintptr_t)(c ? c : t) ^ ((uint32_t)part << 24) ^ ((uint32_t)edges << 28);
    if (key == IN.cur_key)
        return;
    IN.cur_key = key;
    if (t && part == HT_BORDER && t->enabled && t->state == WS_STATE_NORMAL)
        shape = ui_edge_cursor(edges);
    else if (c) {
        int i = ui_cursor_index(c->cursor ? c->cursor : c->cls->cursor);
        if (i >= 0) shape = i;
    }
    ui_set_cursor(shape);
}

/* -> true if the rest of the input now belongs to another process */
static bool mouse_event(int x, int y, uint8_t buttons)
{
    bool ldown = (buttons & MB_LEFT) && !(IN.last_buttons & MB_LEFT);
    bool lup = !(buttons & MB_LEFT) && (IN.last_buttons & MB_LEFT);
    bool rdown = (buttons & MB_RIGHT) && !(IN.last_buttons & MB_RIGHT);
    bool rup = !(buttons & MB_RIGHT) && (IN.last_buttons & MB_RIGHT);
    bool moved = x != IN.cur_x || y != IN.cur_y;
    IN.last_buttons = buttons;
    unsigned keys = ((buttons & MB_LEFT) ? MK_LBUTTON : 0) | ((buttons & MB_RIGHT) ? MK_RBUTTON : 0) |
                    (S->in.shift ? MK_SHIFT : 0) | (S->in.ctrl ? MK_CONTROL : 0);
    if (moved)
        cursor_to(x, y);

    struct wnd *cap = wnd_from(S->capture);
    if (!cap)
        default_cursor(x, y);
    if (cap) {                                  /* everything to the capture */
        if (moved) ui_post_mouse(cap, WM_MOUSEMOVE, x, y, keys);
        if (ldown) ui_post_mouse(cap, WM_LBUTTONDOWN, x, y, keys);
        if (lup)   ui_post_mouse(cap, WM_LBUTTONUP, x, y, keys);
        if (rdown) ui_post_mouse(cap, WM_RBUTTONDOWN, x, y, keys);
        if (rup)   ui_post_mouse(cap, WM_RBUTTONUP, x, y, keys);
        return false;
    }
    if (ldown)
        return press(x, y);
    int part, edges;
    struct wnd *t = window_at(x, y, &part, &edges);
    if (!t || part != HT_CLIENT || !t->enabled)
        return false;
    struct wnd *c = client_target(t, x, y);
    if (moved) ui_post_mouse(c, WM_MOUSEMOVE, x, y, keys);
    if (lup)   ui_post_mouse(c, WM_LBUTTONUP, x, y, keys);
    if (rdown) ui_post_mouse(c, WM_RBUTTONDOWN, x, y, keys);
    if (rup)   ui_post_mouse(c, WM_RBUTTONUP, x, y, keys);
    return false;
}

static void focus_event(int ours)
{
    /* Keys pressed for the switch (Alt+Tab) went elsewhere: forget them,
     * or a lone Alt release would open the menu bar. */
    ui_input_reset();
    if (ours) IN.cur_on = false;                /* repaint draws a fresh cursor */
    if (!ours) IN.outline_on = false;
    ui_set_background(!ours);
}

/* Ctrl+Alt+Del: the shell shows its task list (and tells the VMM so). */
static void task_list(void)
{
    ui_input_reset();                           /* the VMM ate the Del */
    if (wnd_from(S->shell_hook))
        PostMessage(S->shell_hook, WM_SHELLHOOK, HSHELL_TASKMAN, 0);
}

void ui_pump_input(void)
{
    struct user_event e;
    for (int n = 0; n < 64 && GetInputEvent(&e); n++) {
        bool handed_off = false;
        switch (e.type) {
        case EV_FOCUS: focus_event(e.key); break;
        case EV_KEY:   key_event(e.key); break;
        case EV_MOUSE: handed_off = mouse_event(e.x, e.y, e.buttons); break;
        case EV_CAD:   task_list(); break;
        }
        if (handed_off)
            return;
    }
}

/* ---- modal loops ------------------------------------------------------------ */

void ui_modal_begin(void)
{
    IN.modal_pid = ui_pid;
    IN.modal_running = true;
}

void ui_modal_end(void)
{
    IN.modal_running = false;
    IN.modal_pid = 0;
    IN.cur_key = 0;                             /* the shape for wherever it ended */
}

/* The next raw input event, translated, for a loop that owns the input
 * (menus, moving and sizing). Painting goes on while it waits. */
void ui_modal_event(struct modal_ev *m)
{
    struct user_event e;
    for (;;) {
        while (GetInputEvent(&e)) {
            memset(m, 0, sizeof *m);
            m->type = e.type;
            m->x = IN.cur_x;
            m->y = IN.cur_y;
            switch (e.type) {
            case EV_FOCUS:
                focus_event(e.key);
                return;
            case EV_CAD:                        /* cancels the loop, then the task list */
                task_list();
                m->type = EV_FOCUS;
                return;
            case EV_KEY: {
                int code;
                m->vk = key_update(e.key, &m->up, &code);
                IN.alt_alone = false;           /* Alt in a menu doesn't reopen it */
                if (m->vk)
                    return;
                break;
            }
            case EV_MOUSE:
                m->ldown = (e.buttons & MB_LEFT) && !(IN.last_buttons & MB_LEFT);
                m->lup = !(e.buttons & MB_LEFT) && (IN.last_buttons & MB_LEFT);
                m->rdown = (e.buttons & MB_RIGHT) && !(IN.last_buttons & MB_RIGHT);
                m->rup = !(e.buttons & MB_RIGHT) && (IN.last_buttons & MB_RIGHT);
                m->moved = e.x != IN.cur_x || e.y != IN.cur_y;
                IN.last_buttons = e.buttons;
                cursor_to(e.x, e.y);
                m->x = e.x;
                m->y = e.y;
                return;
            }
        }
        ui_cleanup_dead();
        if (ui_paint_pending())
            ui_paint();
        ui_wait(110, true);
    }
}

static void finish_move(struct wnd *w, RECT o)
{
    RECT old;
    frame_outer(w, &old);
    if (o.left == old.left && o.top == old.top)
        return;
    ui_invalidate_wnd(w);
    w->x = o.left;
    w->y = o.top;
    ui_invalidate_wnd(w);
    SendMessage(w, WM_MOVE, 0, MAKELPARAM(w->x, w->y));
    DebugLog("moved %s to %d,%d", w->text, w->x, w->y);
}

static void finish_size(struct wnd *w, RECT o)
{
    ui_invalidate_wnd(w);
    w->x = o.left;
    w->y = o.top;
    w->w = o.right - o.left;
    w->h = o.bottom - o.top;
    ui_invalidate_wnd(w);
    ui_reflow(w);
    RECT c;
    GetClientRect(w, &c);
    SendMessage(w, WM_SIZE, 0, MAKELPARAM(c.right, c.bottom));
    DebugLog("resized %s to %dx%d", w->text, w->w, w->h);
}

static void outline_set(RECT r)
{
    bool on = IN.outline_on;
    ui_outline(false);
    IN.outline = r;
    if (on) ui_outline(true);
}

static RECT sized(RECT o, int edges, int dx, int dy)
{
    RECT r = o;
    if (edges & E_L) r.left = o.left + dx;
    if (edges & E_R) r.right = o.right + dx;
    if (edges & E_T) r.top = o.top + dy;
    if (edges & E_B) r.bottom = o.bottom + dy;
    if (r.right - r.left < MIN_W) {
        if (edges & E_L) r.left = r.right - MIN_W;
        else r.right = r.left + MIN_W;
    }
    if (r.bottom - r.top < MIN_H) {
        if (edges & E_T) r.top = r.bottom - MIN_H;
        else r.bottom = r.top + MIN_H;
    }
    return r;
}

static RECT moved_to(RECT o, int dx, int dy)
{
    int w = o.right - o.left, h = o.bottom - o.top;
    RECT r;
    r.left = o.left + dx;
    r.top = clampi(o.top + dy, 0, S->scr_h - TILE_H);
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

/* Drag the outline from x,y until the button comes up. */
static void drag(struct wnd *w, bool size, int edges, int x, int y)
{
    RECT start;
    frame_outer(w, &start);
    ui_paint();
    ui_set_cursor(size ? ui_edge_cursor(edges) : CURSOR_ARROW);
    IN.outline = start;
    ui_outline(true);
    for (;;) {
        if (!(IN.last_buttons & MB_LEFT))       /* released before we started */
            break;
        struct modal_ev e;
        ui_modal_event(&e);
        if (e.type == EV_FOCUS || !wnd_from(w))
            break;
        if (e.type == EV_KEY && !e.up && e.vk == VK_ESCAPE) {
            ui_outline(false);
            return;
        }
        if (e.type != EV_MOUSE)
            continue;
        outline_set(size ? sized(start, edges, e.x - x, e.y - y) : moved_to(start, e.x - x, e.y - y));
        if (e.lup)
            break;
    }
    RECT r = IN.outline;
    ui_outline(false);
    if (!wnd_from(w))
        return;
    if (size) finish_size(w, r);
    else finish_move(w, r);
}

/* Track a press on a title-bar box: it pushes in while the pointer is on it. */
static void press_box(struct wnd *w, int part)
{
    S->frame_pressed_wnd = w;
    S->frame_pressed_box = part;
    ui_invalidate_wnd(w);
    RECT b = frame_box(w, part);
    bool inside = true;
    while (IN.last_buttons & MB_LEFT) {
        ui_paint();
        struct modal_ev e;
        ui_modal_event(&e);
        if (e.type == EV_FOCUS || !wnd_from(w)) {
            inside = false;
            break;
        }
        if (e.type != EV_MOUSE)
            continue;
        inside = rect_contains(&b, e.x, e.y);
        if ((S->frame_pressed_wnd == w) != inside) {
            S->frame_pressed_wnd = inside ? w : NULL;
            ui_invalidate_screen(b.left, b.top, BOX, BOX);
        }
        if (e.lup)
            break;
    }
    S->frame_pressed_wnd = NULL;
    S->frame_pressed_box = 0;
    if (!wnd_from(w))
        return;
    ui_invalidate_wnd(w);
    if (inside) {
        WPARAM sc = part == HT_CLOSE ? SC_CLOSE : part == HT_MINBUTTON ? SC_MINIMIZE :
                    w->state == WS_STATE_MAX ? SC_RESTORE : SC_MAXIMIZE;
        SendMessage(w, WM_SYSCOMMAND, sc, 0);
    }
}

/* WM_NCLBUTTONDOWN, in DefWindowProc: the owner of w runs this. */
void ui_track_frame(struct wnd *w, int part, int x, int y)
{
    if (w->parent)
        return;
    ui_modal_begin();
    int given = part >> 8;                      /* edges from WM_NCHITTEST */
    switch (part & 0xFF) {
    case HT_CAPTION:
        if (w->state == WS_STATE_NORMAL)
            drag(w, false, 0, x, y);
        break;
    case HT_BORDER: {
        int edges;
        if (given && w->state == WS_STATE_NORMAL)
            drag(w, true, given, x, y);
        else if (frame_hit(w, x, y, &edges) == HT_BORDER)
            drag(w, true, edges, x, y);
        break;
    }
    case HT_MINBUTTON: case HT_MAXBUTTON: case HT_CLOSE:
        press_box(w, part);
        break;
    case HT_SYSMENU:
        menu_track_sys(w);
        break;
    case HT_MENU:
        menu_track_bar(w, x, y);
        break;
    }
    ui_modal_end();
}

/* Move or size a window from the keyboard (system menu Move / Size): the
 * arrow keys move the outline 8 pixels, so does the mouse; Enter or a
 * click accepts, Esc cancels. Sizing moves the bottom-right corner. */
void ui_keyboard_track(struct wnd *w, bool size)
{
    if (w->parent || w->state != WS_STATE_NORMAL || (size && !(w->style & WS_THICKFRAME)))
        return;
    ui_modal_begin();
    RECT start;
    frame_outer(w, &start);
    int dx = 0, dy = 0, lx = IN.cur_x, ly = IN.cur_y, result = 0;
    ui_paint();
    IN.outline = start;
    ui_outline(true);
    while (!result) {
        struct modal_ev e;
        ui_modal_event(&e);
        if (e.type == EV_FOCUS || !wnd_from(w)) {
            result = -1;
        } else if (e.type == EV_KEY && !e.up) {
            switch (e.vk) {
            case VK_LEFT:   dx -= 8; break;
            case VK_RIGHT:  dx += 8; break;
            case VK_UP:     dy -= 8; break;
            case VK_DOWN:   dy += 8; break;
            case VK_RETURN: result = 1; break;
            case VK_ESCAPE: result = -1; break;
            }
        } else if (e.type == EV_MOUSE) {
            dx += e.x - lx;
            dy += e.y - ly;
            lx = e.x;
            ly = e.y;
            if (e.ldown) result = 1;
        }
        outline_set(size ? sized(start, E_R | E_B, dx, dy) : moved_to(start, dx, dy));
    }
    RECT r = IN.outline;
    ui_outline(false);
    ui_modal_end();
    if (result > 0 && wnd_from(w)) {
        if (size) finish_size(w, r);
        else finish_move(w, r);
    }
}
