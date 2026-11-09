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

static const uint16_t arrow[16] = {
    0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100,
    0x8080, 0x8040, 0x8780, 0x9400, 0xB400, 0xCA00, 0x0A00, 0x0400,
};

void ui_cursor(bool on)
{
    if (S->background || on == IN.cur_on)
        return;
    gfx_xor_bitmap(&ui_screen, IN.cur_x, IN.cur_y, arrow, 16);
    IN.cur_on = on;
}

bool ui_cursor_overlaps(int x0, int y0, int x1, int y1)
{
    return IN.cur_on && IN.cur_x < x1 && IN.cur_x + 16 > x0 && IN.cur_y < y1 && IN.cur_y + 16 > y0;
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
    if (!S->background)
        gfx_xor_frame(&ui_screen, IN.outline.left, IN.outline.top,
                      IN.outline.right - IN.outline.left, IN.outline.bottom - IN.outline.top, 2);
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

/* -> true if the rest of the input now belongs to another process */
static bool mouse_event(int x, int y, uint8_t buttons)
{
    bool ldown = (buttons & MB_LEFT) && !(IN.last_buttons & MB_LEFT);
    bool lup = !(buttons & MB_LEFT) && (IN.last_buttons & MB_LEFT);
    bool rdown = (buttons & MB_RIGHT) && !(IN.last_buttons & MB_RIGHT);
    bool rup = !(buttons & MB_RIGHT) && (IN.last_buttons & MB_RIGHT);
    bool moved = x != IN.cur_x || y != IN.cur_y;
    IN.last_buttons = buttons;
    unsigned keys = ((buttons & MB_LEFT) ? MK_LBUTTON : 0) | ((buttons & MB_RIGHT) ? MK_RBUTTON : 0);
    if (moved)
        cursor_to(x, y);

    struct wnd *cap = wnd_from(S->capture);
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
    r.top = clampi(o.top + dy, 0, 480 - TILE_H);
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
    switch (part) {
    case HT_CAPTION:
        if (w->state == WS_STATE_NORMAL)
            drag(w, false, 0, x, y);
        break;
    case HT_BORDER: {
        int edges;
        if (frame_hit(w, x, y, &edges) == HT_BORDER)
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
