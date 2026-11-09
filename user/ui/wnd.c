/*
 * wnd.c -- window classes, windows, messages, focus and activation.
 *
 * The model follows Windows: every window belongs to a class whose window
 * procedure handles its messages; DefWindowProc supplies the default
 * behaviour; controls are child windows; anything can be subclassed by
 * swapping its window procedure (SetWindowLong GWL_WNDPROC) and passing
 * the rest on with CallWindowProc. There is one message queue (one
 * program at a time); painting happens when it runs dry.
 */
#include "uipriv.h"

static struct wclass classes[CLASS_MAX];
static int nclasses;
static struct wnd wnds[WND_MAX];
static struct wnd *tops[WND_MAX];            /* top-level z-order, bottom first */
static int ntops;
static struct wnd *active, *focus, *capture;

/* ---- lookups ------------------------------------------------------------ */

struct wnd *wnd_from(HWND h)
{
    uintptr_t p = (uintptr_t)h, base = (uintptr_t)wnds;
    if (p < base || p >= base + sizeof wnds || (p - base) % sizeof(struct wnd))
        return NULL;
    struct wnd *w = (struct wnd *)h;
    return w->magic == WND_MAGIC ? w : NULL;
}

bool IsWindow(HWND h) { return wnd_from(h) != NULL; }

static bool same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

static struct wclass *find_class(const char *name)
{
    for (int i = 0; i < nclasses; i++)
        if (same_name(classes[i].name, name))
            return &classes[i];
    return NULL;
}

struct wnd *wnd_top(struct wnd *w)
{
    while (w && w->parent)
        w = w->parent;
    return w;
}

int wnd_toplevels(struct wnd **out)
{
    for (int i = 0; i < ntops; i++)
        out[i] = tops[i];
    return ntops;
}

struct wnd *wnd_active(void) { return active; }

/* ---- classes ------------------------------------------------------------ */

bool RegisterClass(const WNDCLASS *wc)
{
    if (!wc || !wc->name || !wc->proc || find_class(wc->name) || nclasses == CLASS_MAX ||
        wc->extra < 0 || wc->extra > EXTRA_MAX)
        return false;
    struct wclass *c = &classes[nclasses++];
    memset(c, 0, sizeof *c);
    strncpy0(c->name, wc->name, sizeof c->name);
    c->proc = wc->proc;
    c->style = wc->style;
    c->extra = wc->extra;
    c->background = wc->background;
    return true;
}

static struct wclass *builtin(const char *name, WNDPROC proc, bool transparent)
{
    WNDCLASS wc = { .name = name, .proc = proc, .background = C_FACE };
    RegisterClass(&wc);
    struct wclass *c = find_class(name);
    c->transparent = transparent;
    return c;
}

void ui_init_classes(void)
{
    builtin(WC_BUTTON, button_proc, false);
    builtin(WC_STATIC, static_proc, true);
    builtin(WC_EDIT, edit_proc, false);
    builtin(WC_LISTBOX, listbox_proc, false);
    builtin(WC_SCROLLBAR, scrollbar_proc, false);
    builtin(WC_TRACKBAR, trackbar_proc, false);
    builtin(WC_PROGRESS, progress_proc, true);
    builtin("#MENU", menu_popup_proc, false)->noactivate = true;   /* menus never activate */
}

/* ---- geometry ----------------------------------------------------------- */

void wnd_client_screen(struct wnd *w, int *x, int *y)
{
    if (!w->parent) {
        RECT r;
        frame_client(w, &r);
        *x = r.left;
        *y = r.top;
        return;
    }
    int px, py;
    wnd_client_screen(w->parent, &px, &py);
    *x = px + w->x;
    *y = py + w->y;
}

void wnd_screen_rect(struct wnd *w, RECT *r)
{
    if (!w->parent) {
        frame_outer(w, r);
        return;
    }
    int x, y;
    wnd_client_screen(w, &x, &y);
    *r = (RECT){ x, y, x + w->w, y + w->h };
}

bool GetWindowRect(HWND h, RECT *r)
{
    struct wnd *w = wnd_from(h);
    if (!w || !r) return false;
    wnd_screen_rect(w, r);
    return true;
}

bool GetClientRect(HWND h, RECT *r)
{
    struct wnd *w = wnd_from(h);
    if (!w || !r) return false;
    if (!w->parent) {
        RECT c;
        frame_client(w, &c);
        *r = (RECT){ 0, 0, c.right - c.left, c.bottom - c.top };
    } else {
        *r = (RECT){ 0, 0, w->w, w->h };
    }
    return true;
}

/* The deepest visible child under x,y (in w's client coordinates). Mouse
 * input skips transparent classes and goes to disabled windows' parents. */
struct wnd *wnd_child_at(struct wnd *w, int x, int y)
{
    struct wnd *hit = NULL;
    for (struct wnd *c = w->first; c; c = c->next)
        if (c->visible && !c->cls->transparent &&
            !(c->cls->proc == button_proc && (c->style & BS_TYPEMASK) == BS_GROUPBOX) &&
            x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h)
            hit = c;                                /* later = higher */
    if (!hit || !hit->enabled)
        return w;
    return wnd_child_at(hit, x - hit->x, y - hit->y);
}

/* ---- windows ------------------------------------------------------------ */

static void unlink(struct wnd *w)
{
    if (w->parent) {
        struct wnd **pp = &w->parent->first;
        while (*pp && *pp != w)
            pp = &(*pp)->next;
        if (*pp)
            *pp = w->next;
        return;
    }
    int i = 0;
    while (i < ntops && tops[i] != w)
        i++;
    for (; i < ntops - 1; i++)
        tops[i] = tops[i + 1];
    if (ntops)
        ntops--;
}

HWND CreateWindow(const char *cls, const char *text, unsigned style,
                  int x, int y, int w, int h, HWND parent, int id, void *param)
{
    struct wclass *c = find_class(cls ? cls : "");
    struct wnd *p = parent ? wnd_from(parent) : NULL;
    if (!c || (parent && !p) || ((style & WS_CHILD) && !p))
        return NULL;
    struct wnd *n = NULL;
    for (int i = 0; i < WND_MAX; i++)
        if (wnds[i].magic != WND_MAGIC) { n = &wnds[i]; break; }
    if (!n || (!p && ntops == WND_MAX))
        return NULL;

    memset(n, 0, sizeof *n);
    n->magic = WND_MAGIC;
    n->cls = c;
    n->proc = c->proc;
    n->style = style;
    n->id = id;
    n->x = x; n->y = y; n->w = w; n->h = h;
    n->visible = (style & WS_VISIBLE) != 0;
    n->enabled = !(style & WS_DISABLED);
    n->bar_sel = -1;
    if (text)
        strncpy0(n->text, text, TEXT_MAX);
    n->parent = (style & WS_CHILD) ? p : NULL;
    if (n->parent) {                                /* top of the siblings */
        struct wnd **pp = &p->first;
        while (*pp)
            pp = &(*pp)->next;
        *pp = n;
    } else {
        tops[ntops++] = n;
    }

    if (SendMessage(n, WM_CREATE, 0, (LPARAM)param) == -1) {
        DestroyWindow(n);
        return NULL;
    }
    if (n->visible) {
        ui_invalidate_wnd(n);
        if (!n->parent && !c->noactivate)
            wnd_activate(n);
    }
    return n;
}

static void kill_timers(struct wnd *w);
static void drop_messages(struct wnd *w);

bool DestroyWindow(HWND h)
{
    struct wnd *w = wnd_from(h);
    if (!w)
        return false;
    if (w->visible)
        ui_invalidate_wnd(w);
    if (frame_has(w))
        DebugLog("closed %s", w->text);
    SendMessage(w, WM_DESTROY, 0, 0);
    while (w->first)
        DestroyWindow(w->first);
    kill_timers(w);
    drop_messages(w);
    menu_window_gone(w);
    if (focus == w) focus = NULL;
    if (capture == w) capture = NULL;
    struct wnd *t = wnd_top(w);
    if (t && t != w && t->focus_child == w)
        t->focus_child = NULL;
    unlink(w);
    bool was_active = active == w;
    w->magic = 0;
    if (was_active) {
        active = NULL;
        for (int i = ntops - 1; i >= 0; i--)
            if (tops[i]->visible && tops[i]->state != WS_STATE_MIN) {
                wnd_activate(tops[i]);
                break;
            }
    }
    return true;
}

HWND GetParent(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w ? w->parent : NULL;
}

HWND GetDlgItem(HWND parent, int id)
{
    struct wnd *p = wnd_from(parent);
    for (struct wnd *c = p ? p->first : NULL; c; c = c->next)
        if (c->id == id)
            return c;
    return NULL;
}

int GetDlgCtrlID(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w ? w->id : 0;
}

bool MoveWindow(HWND h, int x, int y, int w, int hh, bool repaint)
{
    struct wnd *n = wnd_from(h);
    if (!n) return false;
    if (n->visible) ui_invalidate_wnd(n);
    n->x = x; n->y = y; n->w = w; n->h = hh;
    if (n->visible && repaint) ui_invalidate_wnd(n);
    RECT c;
    GetClientRect(n, &c);
    SendMessage(n, WM_SIZE, 0, MAKELPARAM(c.right, c.bottom));
    SendMessage(n, WM_MOVE, 0, MAKELPARAM(x, y));
    return true;
}

bool ShowWindow(HWND h, int cmd)
{
    struct wnd *w = wnd_from(h);
    if (!w) return false;
    bool was = w->visible;
    switch (cmd) {
    case SW_HIDE:
        if (w->visible) ui_invalidate_wnd(w);
        w->visible = false;
        if (active == w) {
            active = NULL;
            for (int i = ntops - 1; i >= 0; i--)
                if (tops[i] != w && tops[i]->visible && tops[i]->state != WS_STATE_MIN) {
                    wnd_activate(tops[i]);
                    break;
                }
        }
        break;
    case SW_SHOWMINIMIZED: w->visible = true; frame_minimize(w); break;
    case SW_SHOWMAXIMIZED: w->visible = true; frame_maximize(w); break;
    case SW_RESTORE:       w->visible = true; frame_restore(w); break;
    default:
        w->visible = true;
        ui_invalidate_wnd(w);
        if (!w->parent && !w->cls->noactivate)
            wnd_activate(w);
    }
    return was;
}

bool EnableWindow(HWND h, bool enable)
{
    struct wnd *w = wnd_from(h);
    if (!w) return false;
    bool was = !w->enabled;
    if (w->enabled != enable) {
        w->enabled = enable;
        if (!enable && focus == w)
            SetFocus(w->parent);
        SendMessage(w, WM_ENABLE, enable, 0);
        ui_invalidate_wnd(w);
    }
    return was;
}

bool IsWindowEnabled(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w && w->enabled;
}

int SetWindowText(HWND h, const char *text) { return (int)SendMessage(h, WM_SETTEXT, 0, (LPARAM)text); }
int GetWindowText(HWND h, char *buf, int max) { return (int)SendMessage(h, WM_GETTEXT, (WPARAM)max, (LPARAM)buf); }

int32_t GetWindowLong(HWND h, int index)
{
    struct wnd *w = wnd_from(h);
    if (!w) return 0;
    switch (index) {
    case GWL_WNDPROC:  return (int32_t)(uintptr_t)w->proc;
    case GWL_STYLE:    return (int32_t)w->style;
    case GWL_ID:       return w->id;
    case GWL_USERDATA: return w->userdata;
    }
    if (index < 0 || index + 4 > w->cls->extra)
        return 0;
    int32_t v;
    memcpy(&v, w->extra + index, 4);
    return v;
}

int32_t SetWindowLong(HWND h, int index, int32_t value)
{
    struct wnd *w = wnd_from(h);
    if (!w) return 0;
    int32_t old = GetWindowLong(h, index);
    switch (index) {
    case GWL_WNDPROC:  if (value) w->proc = (WNDPROC)(uintptr_t)value; break;
    case GWL_STYLE:    w->style = (unsigned)value; ui_invalidate_wnd(w); break;
    case GWL_ID:       w->id = value; break;
    case GWL_USERDATA: w->userdata = value; break;
    default:
        if (index >= 0 && index + 4 <= w->cls->extra)
            memcpy(w->extra + index, &value, 4);
    }
    return old;
}

bool SetProp(HWND h, const char *name, void *value)
{
    struct wnd *w = wnd_from(h);
    if (!w || !name) return false;
    int slot = -1;
    for (int i = 0; i < PROP_MAX; i++) {
        if (w->props[i].name[0] && same_name(w->props[i].name, name)) { slot = i; break; }
        if (!w->props[i].name[0] && slot < 0) slot = i;
    }
    if (slot < 0) return false;
    strncpy0(w->props[slot].name, name, sizeof w->props[slot].name);
    w->props[slot].value = value;
    return true;
}

void *GetProp(HWND h, const char *name)
{
    struct wnd *w = wnd_from(h);
    for (int i = 0; w && name && i < PROP_MAX; i++)
        if (w->props[i].name[0] && same_name(w->props[i].name, name))
            return w->props[i].value;
    return NULL;
}

void *RemoveProp(HWND h, const char *name)
{
    struct wnd *w = wnd_from(h);
    for (int i = 0; w && name && i < PROP_MAX; i++)
        if (w->props[i].name[0] && same_name(w->props[i].name, name)) {
            void *v = w->props[i].value;
            w->props[i].name[0] = '\0';
            return v;
        }
    return NULL;
}

/* ---- activation, focus, capture ------------------------------------------ */

void wnd_raise(struct wnd *t)
{
    int i = 0;
    while (i < ntops && tops[i] != t)
        i++;
    if (i == ntops)
        return;
    for (; i < ntops - 1; i++)
        tops[i] = tops[i + 1];
    tops[ntops - 1] = t;
    ui_invalidate_wnd(t);
}

static struct wnd *first_tabstop(struct wnd *w)
{
    for (struct wnd *c = w->first; c; c = c->next) {
        if (c->visible && c->enabled && (c->style & WS_TABSTOP))
            return c;
        struct wnd *d = first_tabstop(c);
        if (d)
            return d;
    }
    return NULL;
}

void wnd_activate(struct wnd *t)
{
    if (!t || t->parent || !t->visible || t->cls->noactivate)
        return;
    wnd_raise(t);
    if (active == t)
        return;
    struct wnd *old = active;
    active = t;
    if (old && wnd_from(old)) {
        SendMessage(old, WM_ACTIVATE, WA_INACTIVE, (LPARAM)t);
        ui_invalidate_wnd(old);
    }
    SendMessage(t, WM_ACTIVATE, WA_ACTIVE, (LPARAM)old);
    struct wnd *f = wnd_from(t->focus_child) ? t->focus_child : first_tabstop(t);
    SetFocus(f ? f : t);
}

HWND GetActiveWindow(void) { return active; }

HWND SetActiveWindow(HWND h)
{
    struct wnd *old = active, *w = wnd_from(h);
    if (w)
        wnd_activate(wnd_top(w));
    return old;
}

HWND GetFocus(void) { return focus; }

HWND SetFocus(HWND h)
{
    struct wnd *w = h ? wnd_from(h) : NULL, *old = focus;
    if ((h && !w) || w == old)
        return old;
    if (w) {
        struct wnd *t = wnd_top(w);
        t->focus_child = w;
        if (t != active) {                      /* activating sets the focus */
            wnd_activate(t);
            return old;
        }
    }
    focus = w;
    if (old && wnd_from(old))
        SendMessage(old, WM_KILLFOCUS, (WPARAM)w, 0);
    if (w)
        SendMessage(w, WM_SETFOCUS, (WPARAM)old, 0);
    return old;
}

HWND SetCapture(HWND h)
{
    struct wnd *old = capture;
    capture = wnd_from(h);
    return old;
}

bool ReleaseCapture(void) { capture = NULL; return true; }
HWND GetCapture(void)     { return capture; }

/* ---- the message queue ------------------------------------------------------ */

#define QMAX 128
static MSG queue[QMAX];
static unsigned qhead, qtail;
static bool quit_posted;
static int quit_code;

bool PostMessage(HWND h, unsigned msg, WPARAM w, LPARAM l)
{
    if (h && !wnd_from(h))
        return false;
    if (msg == WM_MOUSEMOVE && qhead != qtail) {        /* merge moves */
        MSG *last = &queue[(qtail + QMAX - 1) % QMAX];
        if (last->message == WM_MOUSEMOVE && last->hwnd == h) {
            last->wParam = w;
            last->lParam = l;
            return true;
        }
    }
    unsigned next = (qtail + 1) % QMAX;
    if (next == qhead)
        return false;
    queue[qtail] = (MSG){ h, msg, w, l, GetTickCount() };
    qtail = next;
    return true;
}

void PostQuitMessage(int code)
{
    quit_posted = true;
    quit_code = code;
}

static void drop_messages(struct wnd *w)
{
    for (unsigned i = qhead; i != qtail; i = (i + 1) % QMAX)
        if (queue[i].hwnd == w)
            queue[i] = (MSG){ 0 };
}

/* ---- timers ------------------------------------------------------------------ */

#define TIMER_MAX 16
static struct { struct wnd *w; int id; uint32_t interval, due; } timers[TIMER_MAX];

int SetTimer(HWND h, int id, uint32_t ms)
{
    struct wnd *w = wnd_from(h);
    if (!w || !ms) return 0;
    int slot = -1;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (timers[i].w == w && timers[i].id == id) { slot = i; break; }
        if (!timers[i].w && slot < 0) slot = i;
    }
    if (slot < 0) return 0;
    timers[slot].w = w;
    timers[slot].id = id;
    timers[slot].interval = ms;
    timers[slot].due = GetTickCount() + ms;
    return id ? id : 1;
}

bool KillTimer(HWND h, int id)
{
    for (int i = 0; i < TIMER_MAX; i++)
        if (timers[i].w == (struct wnd *)h && timers[i].id == id) {
            timers[i].w = NULL;
            return true;
        }
    return false;
}

static void kill_timers(struct wnd *w)
{
    for (int i = 0; i < TIMER_MAX; i++)
        if (timers[i].w == w)
            timers[i].w = NULL;
}

static void run_timers(void)
{
    uint32_t now = GetTickCount();
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!timers[i].w || (int32_t)(now - timers[i].due) < 0)
            continue;
        timers[i].due = now + timers[i].interval;
        bool pending = false;                       /* WM_TIMERs don't pile up */
        for (unsigned q = qhead; q != qtail; q = (q + 1) % QMAX)
            if (queue[q].message == WM_TIMER && queue[q].hwnd == timers[i].w &&
                (int)queue[q].wParam == timers[i].id)
                pending = true;
        if (!pending)
            PostMessage(timers[i].w, WM_TIMER, (WPARAM)timers[i].id, 0);
    }
}

/* ---- getting and dispatching messages ------------------------------------------ */

static void pump(void)
{
    struct user_event e;
    for (int n = 0; n < 64 && GetInputEvent(&e); n++)
        ui_input_event(&e);
    run_timers();
}

bool PeekMessage(MSG *m, bool remove)
{
    pump();
    while (qhead != qtail) {
        *m = queue[qhead];
        if (m->message == WM_NULL && !m->hwnd) {     /* dropped */
            qhead = (qhead + 1) % QMAX;
            continue;
        }
        if (remove)
            qhead = (qhead + 1) % QMAX;
        return true;
    }
    if (ui_paint_pending())
        ui_paint();
    if (quit_posted) {
        *m = (MSG){ NULL, WM_QUIT, (WPARAM)quit_code, 0, GetTickCount() };
        if (remove)
            quit_posted = false;
        return true;
    }
    return false;
}

bool GetMessage(MSG *m)
{
    while (!PeekMessage(m, true))
        WaitForInput();
    return m->message != WM_QUIT;
}

LRESULT DispatchMessage(const MSG *m)
{
    struct wnd *w = wnd_from(m->hwnd);
    return w ? w->proc(m->hwnd, m->message, m->wParam, m->lParam) : 0;
}

LRESULT SendMessage(HWND h, unsigned msg, WPARAM w, LPARAM l)
{
    struct wnd *n = wnd_from(h);
    return n ? n->proc(h, msg, w, l) : 0;
}

LRESULT CallWindowProc(WNDPROC proc, HWND h, unsigned msg, WPARAM w, LPARAM l)
{
    return proc && wnd_from(h) ? proc(h, msg, w, l) : 0;
}

bool TranslateMessage(const MSG *m)
{
    if (m->message != WM_KEYDOWN)
        return false;
    char ch = kbd_char((int)m->wParam, (m->lParam & (1 << 25)) != 0);
    if (!ch)
        return false;
    return PostMessage(m->hwnd, WM_CHAR, (WPARAM)(uint8_t)ch, m->lParam);
}

/* ---- mouse messages ---------------------------------------------------------------- */

void ui_post_mouse(struct wnd *w, unsigned msg, int sx, int sy, unsigned keys)
{
    int cx, cy;
    wnd_client_screen(w, &cx, &cy);
    PostMessage(w, msg, keys, MAKELPARAM(sx - cx, sy - cy));
}

/* ---- the default window procedure ---------------------------------------------------- */

LRESULT DefWindowProc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    if (!w)
        return 0;
    switch (msg) {
    case WM_SETTEXT:
        strncpy0(w->text, lp ? (const char *)lp : "", TEXT_MAX);
        ui_invalidate_wnd(w);
        return 1;
    case WM_GETTEXT: {
        if (!lp || !wp) return 0;
        strncpy0((char *)lp, w->text, wp);
        return (LRESULT)strlen((char *)lp);
    }
    case WM_GETTEXTLENGTH:
        return (LRESULT)strlen(w->text);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_CLOSE:    SendMessage(h, WM_CLOSE, 0, 0); break;
        case SC_MINIMIZE: frame_minimize(w); break;
        case SC_MAXIMIZE: frame_maximize(w); break;
        case SC_RESTORE:  frame_restore(w); break;
        case SC_MOVE:     ui_keyboard_track(w, false); break;
        case SC_SIZE:     ui_keyboard_track(w, true); break;
        case SC_KEYMENU:  menu_keymenu(w, (int)lp); break;
        }
        return 0;
    case WM_SYSKEYDOWN: {                      /* Alt+key, F10 */
        struct wnd *t = wnd_top(w);
        bool alt = (lp & (1 << 29)) != 0;
        if (wp == VK_F4 && alt)
            SendMessage(t, WM_SYSCOMMAND, SC_CLOSE, 0);
        else if (wp == VK_F10 && !alt)
            SendMessage(t, WM_SYSCOMMAND, SC_KEYMENU, 0);
        else if (alt && wp == VK_SPACE)
            SendMessage(t, WM_SYSCOMMAND, SC_KEYMENU, ' ');
        else if (alt && ((wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9')))
            SendMessage(t, WM_SYSCOMMAND, SC_KEYMENU, (LPARAM)kbd_char((int)wp, false));
        return 0;
    }
    case WM_SYSKEYUP:                          /* Alt pressed and released alone */
        if (wp == VK_MENU && (lp & (1 << 26)))
            SendMessage(wnd_top(w), WM_SYSCOMMAND, SC_KEYMENU, 0);
        return 0;
    case WM_RBUTTONUP: {
        int x, y;
        wnd_client_screen(w, &x, &y);
        SendMessage(h, WM_CONTEXTMENU, (WPARAM)h,
                    MAKELPARAM(x + GET_X_LPARAM(lp), y + GET_Y_LPARAM(lp)));
        return 0;
    }
    case WM_CONTEXTMENU:                       /* unhandled: ask the parent */
        if (w->parent)
            return SendMessage(w->parent, WM_CONTEXTMENU, wp, lp);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return 0;
}
