/*
 * input.c -- turning raw keyboard and mouse events into messages.
 *
 * Keys go to the focus window as WM_KEYDOWN/UP (WM_SYSKEYDOWN/UP while Alt
 * is held); TranslateMessage makes WM_CHAR from them. Mouse events go to
 * the capture window if there is one, else to the deepest child under the
 * pointer, in client coordinates. Clicks on a frame are handled here: the
 * title bar and borders drag an XOR outline, the boxes track a press and
 * send WM_SYSCOMMAND -- so a program can intercept close, minimize and
 * maximize. A click activates its window first.
 */
#include "uipriv.h"

struct gfx ui_screen;
bool ui_background;

/* ---- cursor --------------------------------------------------------------- */

static const uint16_t arrow[16] = {
    0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100,
    0x8080, 0x8040, 0x8780, 0x9400, 0xB400, 0xCA00, 0x0A00, 0x0400,
};
static int cur_x = 320, cur_y = 240;
static bool cur_on;

void ui_cursor(bool on)
{
    if (ui_background || on == cur_on)
        return;
    gfx_xor_bitmap(&ui_screen, cur_x, cur_y, arrow, 16);
    cur_on = on;
}

bool ui_cursor_overlaps(int x0, int y0, int x1, int y1)
{
    return cur_on && cur_x < x1 && cur_x + 16 > x0 && cur_y < y1 && cur_y + 16 > y0;
}

/* ---- keyboard --------------------------------------------------------------- */

static bool shift, ctrl, alt;
static bool alt_alone;                      /* Alt pressed, nothing else yet */

int GetKeyState(int vk)
{
    bool down = (vk == VK_SHIFT && shift) || (vk == VK_CONTROL && ctrl) || (vk == VK_MENU && alt);
    return down ? -128 : 0;
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
        alt_alone = vk == VK_MENU ? (alt_alone || !alt) : false;
    if (vk == VK_SHIFT)   shift = !up;
    if (vk == VK_CONTROL) ctrl = !up;
    if (vk == VK_MENU)    alt = !up;
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
    bool alone = up && vk == VK_MENU && alt_alone;
    if (up && vk == VK_MENU)
        alt_alone = false;

    struct wnd *target = wnd_from(GetFocus());
    if (!target)
        target = wnd_active();
    if (!target)
        return;
    bool sys = alt || vk == VK_MENU || vk == VK_F10;
    unsigned msg = up ? (sys ? WM_SYSKEYUP : WM_KEYUP) : (sys ? WM_SYSKEYDOWN : WM_KEYDOWN);
    uint32_t lp = 1u | ((uint32_t)code << 16) | (alt ? 1u << 29 : 0) |
                  (shift ? 1u << 25 : 0) | (alone ? 1u << 26 : 0) | (ctrl ? 1u << 27 : 0) |
                  (up ? (1u << 30) | (1u << 31) : 0);
    PostMessage(target, msg, (WPARAM)vk, (LPARAM)lp);
}

/* ---- mouse ------------------------------------------------------------------ */

static enum { M_IDLE, M_MOVE, M_SIZE, M_BOX } mode;
static struct wnd *mw;
static RECT outline;
static int grab_x, grab_y, edges_held;
static uint8_t last_buttons;
static uint32_t last_click;
static struct wnd *last_click_wnd;
static int last_click_part;

static void xor_outline(void)
{
    if (!ui_background)
        gfx_xor_frame(&ui_screen, outline.left, outline.top,
                      outline.right - outline.left, outline.bottom - outline.top, 2);
}

/* The top-level window and frame part under x,y (windows over tiles). */
static struct wnd *window_at(int x, int y, int *part, int *edges)
{
    struct wnd *list[WND_MAX];
    int n = wnd_toplevels(list);
    for (int pass = 0; pass < 2; pass++)
        for (int i = n - 1; i >= 0; i--) {
            struct wnd *t = list[i];
            if (!t->visible || (t->state == WS_STATE_MIN) != (pass == 1))
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
    bool dbl = last_click_wnd == w && last_click_part == part && now - last_click <= 500;
    last_click_wnd = dbl ? NULL : w;
    last_click_part = part;
    last_click = now;
    return dbl;
}

static void press(int x, int y)
{
    int part, edges;
    struct wnd *t = window_at(x, y, &part, &edges);
    if (!t)
        return;
    if (!t->enabled) {                          /* modal: go to its dialog */
        struct wnd *p = wnd_owned_popup(t);
        if (p)
            wnd_activate(p);
        return;
    }
    uint32_t now = GetTickCount();
    if (part == HT_TILE) {
        SendMessage(t, WM_SYSCOMMAND, SC_RESTORE, 0);
        return;
    }
    if (t != wnd_active()) {
        wnd_activate(t);
        DebugLog("activate %s", t->text);
    }

    switch (part) {
    case HT_MINBUTTON: case HT_MAXBUTTON: case HT_CLOSE:
        mode = M_BOX;
        mw = frame_pressed_wnd = t;
        frame_pressed_box = part;
        ui_invalidate_wnd(t);
        break;
    case HT_SYSMENU:                            /* double-click closes (in the menu) */
        menu_track_sys(t);
        break;
    case HT_MENU:
        menu_track_bar(t, x, y);
        break;
    case HT_CAPTION:
        if (is_double(t, part, now)) {
            SendMessage(t, WM_SYSCOMMAND, t->state == WS_STATE_MAX ? SC_RESTORE : SC_MAXIMIZE, 0);
            break;
        }
        if (t->state == WS_STATE_NORMAL) {
            mode = M_MOVE;
            mw = t;
            frame_outer(t, &outline);
            grab_x = x - outline.left;
            grab_y = y - outline.top;
            ui_paint();
            xor_outline();
        }
        break;
    case HT_BORDER:
        mode = M_SIZE;
        mw = t;
        frame_outer(t, &outline);
        edges_held = edges;
        grab_x = x;
        grab_y = y;
        ui_paint();
        xor_outline();
        break;
    case HT_CLIENT: {
        struct wnd *c = client_target(t, x, y);
        bool dbl = (c->cls->style & CS_DBLCLKS) && is_double(c, HT_CLIENT, now);
        ui_post_mouse(c, dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, x, y, MK_LBUTTON);
        break;
    }
    }
}

static void finish_move(void)
{
    RECT o;
    frame_outer(mw, &o);
    if (outline.left != o.left || outline.top != o.top) {
        ui_invalidate_wnd(mw);
        mw->x = outline.left;
        mw->y = outline.top;
        ui_invalidate_wnd(mw);
        SendMessage(mw, WM_MOVE, 0, MAKELPARAM(mw->x, mw->y));
        DebugLog("moved %s to %d,%d", mw->text, mw->x, mw->y);
    }
}

static void finish_size(void)
{
    ui_invalidate_wnd(mw);
    mw->x = outline.left;
    mw->y = outline.top;
    mw->w = outline.right - outline.left;
    mw->h = outline.bottom - outline.top;
    ui_invalidate_wnd(mw);
    RECT c;
    GetClientRect(mw, &c);
    SendMessage(mw, WM_SIZE, 0, MAKELPARAM(c.right, c.bottom));
    DebugLog("resized %s to %dx%d", mw->text, mw->w, mw->h);
}

static void cursor_to(int x, int y)
{
    if (x == cur_x && y == cur_y)
        return;
    ui_cursor(false);
    cur_x = x;
    cur_y = y;
    ui_cursor(true);
}

static void mouse_event(int x, int y, uint8_t buttons)
{
    bool ldown = (buttons & MB_LEFT) && !(last_buttons & MB_LEFT);
    bool lup = !(buttons & MB_LEFT) && (last_buttons & MB_LEFT);
    bool rdown = (buttons & MB_RIGHT) && !(last_buttons & MB_RIGHT);
    bool rup = !(buttons & MB_RIGHT) && (last_buttons & MB_RIGHT);
    bool moved = x != cur_x || y != cur_y;
    last_buttons = buttons;
    unsigned keys = ((buttons & MB_LEFT) ? MK_LBUTTON : 0) | ((buttons & MB_RIGHT) ? MK_RBUTTON : 0);
    if (moved)
        cursor_to(x, y);

    if (!wnd_from(mw))
        mw = NULL;
    switch (mode) {
    case M_MOVE:
        if (!mw) { mode = M_IDLE; return; }
        xor_outline();
        {
            int w = outline.right - outline.left, h = outline.bottom - outline.top;
            outline.left = x - grab_x;
            outline.top = clampi(y - grab_y, 0, 480 - TILE_H);
            outline.right = outline.left + w;
            outline.bottom = outline.top + h;
        }
        if (lup) { finish_move(); mode = M_IDLE; }
        else xor_outline();
        return;

    case M_SIZE: {
        if (!mw) { mode = M_IDLE; return; }
        xor_outline();
        RECT o;
        frame_outer(mw, &o);
        int ddx = x - grab_x, ddy = y - grab_y;
        outline = o;
        if (edges_held & E_L) outline.left = o.left + ddx;
        if (edges_held & E_R) outline.right = o.right + ddx;
        if (edges_held & E_T) outline.top = o.top + ddy;
        if (edges_held & E_B) outline.bottom = o.bottom + ddy;
        if (outline.right - outline.left < MIN_W) {
            if (edges_held & E_L) outline.left = outline.right - MIN_W;
            else outline.right = outline.left + MIN_W;
        }
        if (outline.bottom - outline.top < MIN_H) {
            if (edges_held & E_T) outline.top = outline.bottom - MIN_H;
            else outline.bottom = outline.top + MIN_H;
        }
        if (lup) { finish_size(); mode = M_IDLE; }
        else xor_outline();
        return;
    }

    case M_BOX: {
        if (!mw) { mode = M_IDLE; frame_pressed_wnd = NULL; return; }
        RECT b = frame_box(mw, frame_pressed_box);
        bool inside = rect_contains(&b, x, y);
        if (!lup) {
            if ((frame_pressed_wnd == mw) != inside) {
                frame_pressed_wnd = inside ? mw : NULL;
                ui_invalidate_screen(b.left, b.top, BOX, BOX);
            }
            return;
        }
        int which = frame_pressed_box;
        frame_pressed_wnd = NULL;
        frame_pressed_box = 0;
        mode = M_IDLE;
        ui_invalidate_wnd(mw);
        if (inside) {
            WPARAM sc = which == HT_CLOSE ? SC_CLOSE : which == HT_MINBUTTON ? SC_MINIMIZE :
                        mw->state == WS_STATE_MAX ? SC_RESTORE : SC_MAXIMIZE;
            SendMessage(mw, WM_SYSCOMMAND, sc, 0);
        }
        return;
    }

    case M_IDLE:
        break;
    }

    struct wnd *cap = wnd_from(GetCapture());
    if (cap) {                                  /* everything to the capture */
        if (moved) ui_post_mouse(cap, WM_MOUSEMOVE, x, y, keys);
        if (ldown) ui_post_mouse(cap, WM_LBUTTONDOWN, x, y, keys);
        if (lup)   ui_post_mouse(cap, WM_LBUTTONUP, x, y, keys);
        if (rdown) ui_post_mouse(cap, WM_RBUTTONDOWN, x, y, keys);
        if (rup)   ui_post_mouse(cap, WM_RBUTTONUP, x, y, keys);
        return;
    }
    if (ldown) {
        press(x, y);
        return;
    }
    int part, edges;
    struct wnd *t = window_at(x, y, &part, &edges);
    if (!t || part != HT_CLIENT || !t->enabled)
        return;
    struct wnd *c = client_target(t, x, y);
    if (moved) ui_post_mouse(c, WM_MOUSEMOVE, x, y, keys);
    if (lup)   ui_post_mouse(c, WM_LBUTTONUP, x, y, keys);
    if (rdown) ui_post_mouse(c, WM_RBUTTONDOWN, x, y, keys);
    if (rup)   ui_post_mouse(c, WM_RBUTTONUP, x, y, keys);
}

void ui_input_event(const struct user_event *e)
{
    switch (e->type) {
    case EV_FOCUS:
        /* Keys pressed for the switch (Alt+Tab) went elsewhere: forget them,
         * or a lone Alt release would open the menu bar. */
        shift = ctrl = alt = alt_alone = false;
        if (e->key) cur_on = false;             /* repaint draws a fresh cursor */
        ui_set_background(e->key == 0);
        break;
    case EV_KEY:
        key_event(e->key);
        break;
    case EV_MOUSE:
        mouse_event(e->x, e->y, e->buttons);
        break;
    }
}

/* ---- modal loops ------------------------------------------------------------ */

/* The next raw input event, translated, for a loop that owns the input
 * (menus, keyboard move and size). Painting goes on while it waits. */
void ui_modal_event(struct modal_ev *m)
{
    struct user_event e;
    for (;;) {
        while (GetInputEvent(&e)) {
            memset(m, 0, sizeof *m);
            m->type = e.type;
            m->x = cur_x;
            m->y = cur_y;
            switch (e.type) {
            case EV_FOCUS:
                ui_input_event(&e);
                return;
            case EV_KEY: {
                int code;
                m->vk = key_update(e.key, &m->up, &code);
                alt_alone = false;              /* Alt in a menu doesn't reopen it */
                if (m->vk)
                    return;
                break;
            }
            case EV_MOUSE:
                m->ldown = (e.buttons & MB_LEFT) && !(last_buttons & MB_LEFT);
                m->lup = !(e.buttons & MB_LEFT) && (last_buttons & MB_LEFT);
                m->rdown = (e.buttons & MB_RIGHT) && !(last_buttons & MB_RIGHT);
                m->rup = !(e.buttons & MB_RIGHT) && (last_buttons & MB_RIGHT);
                m->moved = e.x != cur_x || e.y != cur_y;
                last_buttons = e.buttons;
                cursor_to(e.x, e.y);
                m->x = e.x;
                m->y = e.y;
                return;
            }
        }
        if (ui_paint_pending())
            ui_paint();
        WaitForInput();
    }
}

static void size_outline(RECT o, int dx, int dy)
{
    outline.right = clampi(o.right + dx, o.left + MIN_W, 640);
    outline.bottom = clampi(o.bottom + dy, o.top + MIN_H, 480);
}

/* Move or size a window from the keyboard (system menu Move / Size): the
 * arrow keys move the outline 8 pixels, so does the mouse; Enter or a
 * click accepts, Esc cancels. Sizing moves the bottom-right corner. */
void ui_keyboard_track(struct wnd *w, bool size)
{
    if (w->parent || w->state != WS_STATE_NORMAL || (size && !(w->style & WS_THICKFRAME)))
        return;
    mw = w;
    frame_outer(w, &outline);
    RECT start = outline;
    int dx = 0, dy = 0, lx = cur_x, ly = cur_y;
    ui_paint();
    xor_outline();
    for (;;) {
        struct modal_ev e;
        ui_modal_event(&e);
        int result = 0;                         /* 1 accept, -1 cancel */
        if (e.type == EV_FOCUS) {
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
        xor_outline();
        if (!wnd_from(w)) {
            mw = NULL;
            return;
        }
        if (size) {
            size_outline(start, dx, dy);
        } else {
            int ww = start.right - start.left, hh = start.bottom - start.top;
            outline.left = start.left + dx;
            outline.top = clampi(start.top + dy, 0, 480 - TILE_H);
            outline.right = outline.left + ww;
            outline.bottom = outline.top + hh;
        }
        if (result > 0) {
            if (size) finish_size();
            else finish_move();
        }
        if (result) {
            mw = NULL;
            return;
        }
        xor_outline();
    }
}
