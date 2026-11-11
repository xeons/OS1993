/*
 * wnd.c -- window classes, windows, z-order, focus and activation,
 * properties, DefWindowProc, and the calls the shell uses.
 *
 * The model follows Windows: every window belongs to a class whose window
 * procedure handles its messages; DefWindowProc supplies the default
 * behaviour; controls are child windows; anything can be subclassed by
 * swapping its window procedure (SetWindowLong GWL_WNDPROC) and passing
 * the rest on with CallWindowProc. Windows live in the shared arena; each
 * belongs to the process that created it, which alone runs its procedure.
 */
#include "uipriv.h"

/* ---- lookups ------------------------------------------------------------ */

struct wnd *wnd_from(HWND h)
{
    uintptr_t p = (uintptr_t)h, base = (uintptr_t)S->wnds;
    if (p < base || p >= base + sizeof S->wnds || (p - base) % sizeof(struct wnd))
        return NULL;
    struct wnd *w = (struct wnd *)h;
    return w->magic == WND_MAGIC ? w : NULL;
}

bool IsWindow(HWND h) { return wnd_from(h) != NULL; }
bool wnd_mine(struct wnd *w) { return w && w->pid == ui_pid; }

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
    for (int i = 0; i < S->nclasses; i++) {
        struct wclass *c = &S->classes[i];
        if (c->name[0] && (c->pid == 0 || c->pid == ui_pid) && same_name(c->name, name))
            return c;
    }
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
    for (int i = 0; i < S->ntops; i++)
        out[i] = S->tops[i];
    return S->ntops;
}

struct wnd *wnd_active(void) { return S->active; }

struct wnd *wnd_owned_popup(struct wnd *owner)
{
    for (int i = S->ntops - 1; i >= 0; i--)
        if (S->tops[i]->owner == owner && S->tops[i]->visible && S->tops[i]->enabled &&
            !(S->tops[i]->exstyle & WS_EX_NOACTIVATE))
            return S->tops[i];
    return NULL;
}

/* A notification: sent to our own windows, posted to other processes'. */
LRESULT wnd_notify(struct wnd *w, unsigned msg, WPARAM wp, LPARAM lp)
{
    if (!w)
        return 0;
    if (wnd_mine(w))
        return SendMessage(w, msg, wp, lp);
    PostMessage(w, msg, wp, lp);
    return 0;
}

/* Taskbar material: a visible, captioned, unowned top-level window. */
static bool shell_relevant(struct wnd *w)
{
    return w && !w->parent && !w->owner && frame_has(w) && !(w->exstyle & WS_EX_TOOLWINDOW) &&
           w != S->desktop;
}

void ui_shell_notify(int code, struct wnd *w)
{
    struct wnd *hook = S->shell_hook;
    if (hook && hook != w && wnd_from(hook) && (shell_relevant(w) || code == HSHELL_WINDOWDESTROYED))
        PostMessage(hook, WM_SHELLHOOK, (WPARAM)code, (LPARAM)w);
}

/* ---- classes ------------------------------------------------------------ */

bool RegisterClass(const WNDCLASS *wc)
{
    if (!wc || !wc->name || !wc->proc || find_class(wc->name) ||
        wc->extra < 0 || wc->extra > EXTRA_MAX)
        return false;
    struct wclass *c = NULL;
    for (int i = 0; i < S->nclasses; i++)                   /* a freed slot */
        if (!S->classes[i].name[0]) { c = &S->classes[i]; break; }
    if (!c) {
        if (S->nclasses == CLASS_MAX)
            return false;
        c = &S->classes[S->nclasses++];
    }
    memset(c, 0, sizeof *c);
    strncpy0(c->name, wc->name, sizeof c->name);
    c->pid = ui_pid;
    c->proc = wc->proc;
    c->style = wc->style;
    c->extra = wc->extra;
    c->background = wc->background;
    c->icon = wc->icon;
    c->cursor = wc->cursor;
    return true;
}

static struct wclass *builtin(const char *name, WNDPROC proc, bool transparent)
{
    WNDCLASS wc = { .name = name, .proc = proc, .background = C_FACE };
    RegisterClass(&wc);
    struct wclass *c = find_class(name);
    c->pid = 0;                                             /* everyone's */
    c->transparent = transparent;
    return c;
}

void ui_init_classes(void)
{
    builtin(WC_BUTTON, button_proc, false);
    builtin(WC_STATIC, static_proc, true);
    struct wclass *ec = builtin(WC_EDIT, edit_proc, false);
    ec->style |= CS_DBLCLKS;
    ec->cursor = IDC_IBEAM;
    builtin(WC_LISTBOX, listbox_proc, false)->style |= CS_DBLCLKS;
    builtin(WC_SCROLLBAR, scrollbar_proc, false);
    builtin(WC_TRACKBAR, trackbar_proc, false);
    builtin(WC_PROGRESS, progress_proc, true);
    builtin(WC_COMBOBOX, combo_proc, false);
    builtin(WC_TABCONTROL, tab_proc, false);
    builtin(WC_UPDOWN, updown_proc, false);
    builtin(WC_TREEVIEW, treeview_proc, false)->style |= CS_DBLCLKS;
    builtin(WC_LISTVIEW, listview_proc, false)->style |= CS_DBLCLKS;
    builtin(STATUSCLASSNAME, statusbar_proc, false);
    builtin(TOOLBARCLASSNAME, toolbar_proc, false)->style |= CS_DBLCLKS;
    struct wclass *tc = builtin(TOOLTIPS_CLASS, tooltip_proc, false);
    tc->noactivate = true;
    tc->exstyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    builtin("#32770", dialog_proc, false);
    builtin("#MENU", menu_popup_proc, false)->noactivate = true;   /* menus never activate */
    builtin("#COMBOLBOX", combolist_proc, false)->noactivate = true;   /* nor combo lists */
    builtin("#MSGBOX", msgbox_proc, false);
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

/* ---- z-order -------------------------------------------------------------- */

/* Where a top-level window belongs: the desktop at the bottom, topmost
 * windows above the rest. */
static int band(struct wnd *w)
{
    if (w == S->desktop) return 0;
    if (w->exstyle & WS_EX_TOPMOST) return 2;
    return 1;
}

static void tops_insert(struct wnd *w)          /* at the top of its band */
{
    int i = S->ntops;
    while (i > 0 && band(S->tops[i - 1]) > band(w))
        i--;
    for (int k = S->ntops; k > i; k--)
        S->tops[k] = S->tops[k - 1];
    S->tops[i] = w;
    S->ntops++;
}

static void tops_remove(struct wnd *w)
{
    int i = 0;
    while (i < S->ntops && S->tops[i] != w)
        i++;
    if (i == S->ntops)
        return;
    for (; i < S->ntops - 1; i++)
        S->tops[i] = S->tops[i + 1];
    S->ntops--;
}

/* Alt+Esc: t (and the windows it owns) to the bottom of its band; the
 * next window up becomes active. */
void wnd_lower(struct wnd *t)
{
    if (!t || t->parent || t == S->desktop)
        return;
    struct wnd *move[WND_MAX];
    int n = 0;
    move[n++] = t;
    for (int k = 0; k < S->ntops; k++)
        if (S->tops[k]->owner == t)
            move[n++] = S->tops[k];
    for (int m = n - 1; m >= 0; m--) {          /* owned ones stay above it */
        tops_remove(move[m]);
        int i = 0;
        while (i < S->ntops && band(S->tops[i]) < band(move[m]))
            i++;
        for (int k = S->ntops; k > i; k--)
            S->tops[k] = S->tops[k - 1];
        S->tops[i] = move[m];
        S->ntops++;
        ui_invalidate_wnd(move[m]);
    }
    for (int i = S->ntops - 1; i >= 0; i--) {
        struct wnd *c = S->tops[i];
        if (c->visible && c->enabled && c->state != WS_STATE_MIN && c != S->desktop &&
            !(c->exstyle & (WS_EX_NOACTIVATE | WS_EX_TOPMOST))) {
            if (c != t)
                wnd_activate(c);                /* already the top: no raise */
            return;
        }
    }
}

void wnd_raise(struct wnd *t)
{
    int i = 0;
    while (i < S->ntops && S->tops[i] != t)
        i++;
    if (i == S->ntops)
        return;
    bool top = true;                            /* already the top of its band? */
    for (int k = i + 1; k < S->ntops; k++)
        if (band(S->tops[k]) == band(t))
            top = false;
    if (top)
        return;
    tops_remove(t);
    tops_insert(t);
    ui_invalidate_wnd(t);
    struct wnd *owned[WND_MAX];                 /* the windows it owns stay above it */
    int n = 0;
    for (int k = 0; k < S->ntops; k++)
        if (S->tops[k]->owner == t && S->tops[k]->visible)
            owned[n++] = S->tops[k];
    for (int k = 0; k < n; k++) {
        tops_remove(owned[k]);
        tops_insert(owned[k]);
        ui_invalidate_wnd(owned[k]);
    }
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
    tops_remove(w);
}

HWND CreateWindowEx(unsigned exstyle, const char *cls, const char *text, unsigned style,
                    int x, int y, int w, int h, HWND parent, int id, void *param)
{
    struct wclass *c = find_class(cls ? cls : "");
    struct wnd *p = parent ? wnd_from(parent) : NULL;
    if (!c || (parent && !p) || ((style & WS_CHILD) && (!p || !wnd_mine(p))))
        return NULL;
    struct wnd *n = NULL;
    for (int i = 0; i < WND_MAX; i++)
        if (S->wnds[i].magic != WND_MAGIC) { n = &S->wnds[i]; break; }
    if (!n || (!(style & WS_CHILD) && S->ntops == WND_MAX))
        return NULL;

    memset(n, 0, sizeof *n);
    n->magic = WND_MAGIC;
    n->pid = ui_pid;
    n->cls = c;
    n->proc = c->proc;
    n->style = style;
    n->exstyle = exstyle | (c->noactivate ? WS_EX_NOACTIVATE : 0) | c->exstyle;
    n->id = id;
    n->x = x; n->y = y; n->w = w; n->h = h;
    n->visible = (style & WS_VISIBLE) != 0;
    n->enabled = !(style & WS_DISABLED);
    n->bar_sel = -1;
    if (text)
        strncpy0(n->text, text, TEXT_MAX);
    n->parent = (style & WS_CHILD) ? p : NULL;
    n->owner = (style & WS_CHILD) ? NULL : (p ? wnd_top(p) : NULL);
    if (n->parent) {                                /* top of the siblings */
        struct wnd **pp = &p->first;
        while (*pp)
            pp = &(*pp)->next;
        *pp = n;
    } else {
        tops_insert(n);
    }
    RECT cr;
    GetClientRect(n, &cr);
    n->cw0 = cr.right;
    n->ch0 = cr.bottom;

    if (SendMessage(n, WM_CREATE, 0, (LPARAM)param) == -1) {
        DestroyWindow(n);
        return NULL;
    }
    if (!wnd_from(n))
        return NULL;
    if (n->visible) {
        ui_invalidate_wnd(n);
        if (!n->parent && !(n->exstyle & WS_EX_NOACTIVATE))
            wnd_activate(n);
    }
    if (!n->parent)
        ui_shell_notify(HSHELL_WINDOWCREATED, n);
    return n;
}

HWND CreateWindow(const char *cls, const char *text, unsigned style,
                  int x, int y, int w, int h, HWND parent, int id, void *param)
{
    return CreateWindowEx(0, cls, text, style, x, y, w, h, parent, id, param);
}

/* Activate the next window after w goes (topmost windows such as the
 * taskbar, and the desktop, only if nothing else is left). */
void wnd_activate_next(struct wnd *gone)
{
    S->active = NULL;
    for (int pass = 0; pass < 2; pass++)
        for (int i = S->ntops - 1; i >= 0; i--) {
            struct wnd *t = S->tops[i];
            if (t == gone || !t->visible || t->state == WS_STATE_MIN || t->exstyle & WS_EX_NOACTIVATE ||
                !t->enabled)
                continue;
            if (pass == 0 && (t == S->desktop || (t->exstyle & WS_EX_TOPMOST)))
                continue;
            wnd_activate(t);
            return;
        }
}

static void kill_window_timers(struct wnd *w)
{
    for (int i = 0; i < TIMER_MAX; i++)
        if (S->timers[i].w == w)
            S->timers[i].w = NULL;
}

/* Take w out of every shared structure (no messages: also used for the
 * windows of processes that died). */
static void forget(struct wnd *w)
{
    while (w->first)
        forget(w->first);
    if (w->visible)
        ui_invalidate_wnd(w);
    kill_window_timers(w);
    for (int i = 0; i < PROC_MAX; i++) {            /* drop its posted messages */
        struct pqueue *q = &S->queues[i];
        for (unsigned k = q->head; k != q->tail; k = (k + 1) % QMAX)
            if (q->q[k].hwnd == w)
                q->q[k] = (MSG){ 0 };
    }
    if (S->focus == w) S->focus = NULL;
    if (S->capture == w) S->capture = NULL;
    if (S->frame_pressed_wnd == w) S->frame_pressed_wnd = NULL;
    if (S->in.last_click_wnd == w) S->in.last_click_wnd = NULL;
    struct wnd *t = wnd_top(w);
    if (t && t != w && t->focus_child == w)
        t->focus_child = NULL;
    if (S->shell_hook == w) S->shell_hook = NULL;
    if (S->desktop == w) S->desktop = NULL;
    menu_window_gone(w);
    unlink(w);
    bool was_active = S->active == w;
    bool top = !w->parent;
    w->magic = 0;
    if (top)
        ui_shell_notify(HSHELL_WINDOWDESTROYED, w);
    if (was_active)
        wnd_activate_next(w);
}

void ui_forget_window(struct wnd *w) { forget(w); }

bool DestroyWindow(HWND h)
{
    struct wnd *w = wnd_from(h);
    if (!w || !wnd_mine(w))
        return false;
    if (frame_has(w))
        DebugLog("closed %s", w->text);
    SendMessage(w, WM_DESTROY, 0, 0);
    if (!wnd_from(w))
        return true;
    while (w->first)
        DestroyWindow(w->first);
    for (int i = S->ntops - 1; i >= 0; i--)         /* and the windows it owns */
        if (i < S->ntops && S->tops[i]->owner == w && wnd_mine(S->tops[i]))
            DestroyWindow(S->tops[i]);
    if (wnd_from(w))
        forget(w);
    return true;
}

HWND GetParent(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w ? (w->parent ? w->parent : w->owner) : NULL;
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
    ui_reflow(n);
    RECT c;
    GetClientRect(n, &c);
    wnd_notify(n, WM_SIZE, 0, MAKELPARAM(c.right, c.bottom));
    wnd_notify(n, WM_MOVE, 0, MAKELPARAM(x, y));
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
        if (S->active == w)
            wnd_activate_next(w);
        break;
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED: w->visible = true; frame_minimize(w); break;
    case SW_SHOWMAXIMIZED: w->visible = true; frame_maximize(w); break;
    case SW_RESTORE:       w->visible = true; frame_restore(w); wnd_activate(w); break;
    default:
        w->visible = true;
        ui_invalidate_wnd(w);
        if (!w->parent && !(w->exstyle & WS_EX_NOACTIVATE))
            wnd_activate(w);
    }
    if (!w->parent && was != w->visible)
        ui_shell_notify(w->visible ? HSHELL_WINDOWCREATED : HSHELL_WINDOWDESTROYED, w);
    return was;
}

bool EnableWindow(HWND h, bool enable)
{
    struct wnd *w = wnd_from(h);
    if (!w) return false;
    bool was = !w->enabled;
    if (w->enabled != enable) {
        w->enabled = enable;
        if (!enable && S->focus == w)
            SetFocus(w->parent);
        wnd_notify(w, WM_ENABLE, enable, 0);
        ui_invalidate_wnd(w);
    }
    return was;
}

bool IsWindowEnabled(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w && w->enabled;
}

bool IsWindowVisible(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w && w->visible;
}

bool IsIconic(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w && w->state == WS_STATE_MIN;
}

int GetWindowProcessId(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w ? w->pid : 0;
}

/* Text: another process's window is read and written directly (its
 * procedure can't be called from here), as Windows does for GetWindowText. */
int SetWindowText(HWND h, const char *text)
{
    struct wnd *w = wnd_from(h);
    if (!w) return 0;
    if (wnd_mine(w))
        return (int)SendMessage(h, WM_SETTEXT, 0, (LPARAM)text);
    strncpy0(w->text, text ? text : "", TEXT_MAX);
    ui_invalidate_wnd(w);
    return 1;
}

int GetWindowText(HWND h, char *buf, int max)
{
    struct wnd *w = wnd_from(h);
    if (!w || !buf || max <= 0) return 0;
    if (wnd_mine(w))
        return (int)SendMessage(h, WM_GETTEXT, (WPARAM)max, (LPARAM)buf);
    strncpy0(buf, w->text, (size_t)max);
    return (int)strlen(buf);
}

int32_t GetWindowLong(HWND h, int index)
{
    struct wnd *w = wnd_from(h);
    if (!w) return 0;
    switch (index) {
    case GWL_WNDPROC:  return wnd_mine(w) ? (int32_t)(uintptr_t)w->proc : 0;
    case GWL_STYLE:    return (int32_t)w->style;
    case GWL_ID:       return w->id;
    case GWL_USERDATA: return w->userdata;
    case GWL_EXSTYLE:  return (int32_t)w->exstyle;
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
    if (!w || !wnd_mine(w)) return 0;
    int32_t old = GetWindowLong(h, index);
    switch (index) {
    case GWL_WNDPROC:  if (value) w->proc = (WNDPROC)(uintptr_t)value; break;
    case GWL_STYLE:
        w->style = (unsigned)value;
        ui_invalidate_wnd(w);
        SendMessage(w, WM_STYLECHANGED, (WPARAM)GWL_STYLE, 0);
        break;
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

static void set_focus(struct wnd *w)
{
    struct wnd *old = S->focus;
    if (w == old)
        return;
    S->focus = w;
    if (old && wnd_from(old))
        wnd_notify(old, WM_KILLFOCUS, (WPARAM)w, 0);
    if (w)
        wnd_notify(w, WM_SETFOCUS, (WPARAM)old, 0);
}

void wnd_activate(struct wnd *t)
{
    if (!t || t->parent || !t->visible || t->exstyle & WS_EX_NOACTIVATE)
        return;
    wnd_raise(t);
    if (S->active == t)
        return;
    struct wnd *old = S->active;
    S->active = t;
    if (old && wnd_from(old)) {
        wnd_notify(old, WM_ACTIVATE, WA_INACTIVE, (LPARAM)t);
        ui_invalidate_wnd(old);
    }
    ui_invalidate_wnd(t);
    wnd_notify(t, WM_ACTIVATE, WA_ACTIVE, (LPARAM)old);
    struct wnd *f = wnd_from(t->focus_child) ? t->focus_child : first_tabstop(t);
    set_focus(f ? f : t);
    ui_shell_notify(HSHELL_WINDOWACTIVATED, t);
}

HWND GetActiveWindow(void) { return S->active; }
HWND GetForegroundWindow(void) { return S->active; }

HWND SetActiveWindow(HWND h)
{
    struct wnd *old = S->active, *w = wnd_from(h);
    if (w)
        wnd_activate(wnd_top(w));
    return old;
}

bool SetForegroundWindow(HWND h)
{
    struct wnd *w = wnd_from(h);
    if (!w)
        return false;
    w = wnd_top(w);
    if (w->state == WS_STATE_MIN)
        frame_restore(w);
    wnd_activate(w);
    return true;
}

HWND GetFocus(void) { return S->focus; }

HWND SetFocus(HWND h)
{
    struct wnd *w = h ? wnd_from(h) : NULL, *old = S->focus;
    if ((h && !w) || w == old)
        return old;
    if (w) {
        struct wnd *t = wnd_top(w);
        t->focus_child = w;
        if (t != S->active) {                   /* activating sets the focus */
            wnd_activate(t);
            return old;
        }
    }
    set_focus(w);
    return old;
}

HWND SetCapture(HWND h)
{
    struct wnd *old = S->capture;
    S->capture = wnd_from(h);
    return old;
}

bool ReleaseCapture(void) { S->capture = NULL; S->in.cur_key = 0; return true; }
HWND GetCapture(void)     { return S->capture; }

/* ---- the shell's view ------------------------------------------------------ */

bool EnumWindows(WNDENUMPROC proc, LPARAM l)
{
    struct wnd *list[WND_MAX];
    int n = wnd_toplevels(list);
    for (int i = n - 1; i >= 0; i--)
        if (wnd_from(list[i]) && !(list[i]->exstyle & WS_EX_NOACTIVATE) && !proc(list[i], l))
            return false;
    return true;
}

HWND FindWindow(const char *cls, const char *title)
{
    for (int i = S->ntops - 1; i >= 0; i--) {
        struct wnd *t = S->tops[i];
        if ((!cls || same_name(t->cls->name, cls)) && (!title || !strcmp(t->text, title)))
            return t;
    }
    return NULL;
}

bool RegisterShellHookWindow(HWND h)
{
    struct wnd *w = wnd_from(h);
    if (!w) return false;
    S->shell_hook = w;
    return true;
}

bool SetShellWindow(HWND h)
{
    struct wnd *w = wnd_from(h);
    if (!w || w->parent) return false;
    S->desktop = w;
    tops_remove(w);
    tops_insert(w);
    ui_invalidate_wnd(w);
    return true;
}

HWND GetShellWindow(void) { return S->desktop; }

bool SetClipboardText(const char *text, int len)
{
    if (!text) return false;
    if (len < 0) len = (int)strlen(text);
    if (len > CLIP_MAX - 1) len = CLIP_MAX - 1;
    memcpy(S->clip, text, (size_t)len);
    S->clip[len] = '\0';
    S->clip_len = len;
    S->clipbmp_w = S->clipbmp_h = 0;            /* one thing at a time */
    S->clip_files = false;
    return true;
}

/* Files (full paths, each NUL-ended, a NUL after the last) copied or cut
 * by Explorer, to be pasted somewhere else. */
bool SetClipboardFiles(const char *paths, bool cut)
{
    if (!paths) return false;
    int n = 0;
    while (paths[n]) n += (int)strlen(paths + n) + 1;
    if (n + 1 > CLIP_MAX) return false;
    memcpy(S->clip, paths, (size_t)n + 1);
    S->clip_len = 0;                            /* (not text) */
    S->clipbmp_w = S->clipbmp_h = 0;
    S->clip_files = true;
    S->clip_cut = cut;
    S->clip_n = n + 1;
    return true;
}

int GetClipboardFiles(char *out, int size, bool *cut)
{
    if (!S->clip_files) return 0;
    if (cut) *cut = S->clip_cut;
    if (out && size >= S->clip_n) memcpy(out, S->clip, (size_t)S->clip_n);
    return S->clip_n;
}

void EmptyClipboard(void)
{
    S->clip_len = 0;
    S->clip[0] = '\0';
    S->clipbmp_w = S->clipbmp_h = 0;
    S->clip_files = false;
}

/* A picture in the theme's 16 colours, one byte a pixel (FFh: white). */
bool SetClipboardBitmap(int w, int h, const uint8_t *pixels)
{
    if (!pixels || w <= 0 || h <= 0 || (w + 1) / 2 * h > CLIPBMP_MAX)
        return false;
    int row = (w + 1) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += 2) {
            uint8_t a = pixels[y * w + x], b = x + 1 < w ? pixels[y * w + x + 1] : 0;
            S->clipbmp[y * row + x / 2] = (uint8_t)((a & 15) << 4 | (b & 15));
        }
    S->clipbmp_w = w;
    S->clipbmp_h = h;
    S->clip_len = 0;
    S->clip[0] = '\0';
    S->clip_files = false;
    return true;
}

/* -> true if there is one; its size; the pixels if out has room (w * h). */
bool GetClipboardBitmap(int *w, int *h, uint8_t *out, int room)
{
    if (!S->clipbmp_w)
        return false;
    if (w) *w = S->clipbmp_w;
    if (h) *h = S->clipbmp_h;
    int bw = S->clipbmp_w, bh = S->clipbmp_h, row = (bw + 1) / 2;
    if (out && room >= bw * bh)
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                uint8_t v = S->clipbmp[y * row + x / 2];
                out[y * bw + x] = x & 1 ? v & 15 : v >> 4;
            }
    return true;
}

bool IsClipboardFormatAvailable(unsigned format)
{
    return format == CF_TEXT ? S->clip_len > 0 : format == CF_BITMAP ? S->clipbmp_w > 0 :
           format == CF_FILES ? S->clip_files : false;
}

int GetClipboardText(char *buf, int max)
{
    if (!buf || max <= 0) return 0;
    int n = S->clip_len < max - 1 ? S->clip_len : max - 1;
    memcpy(buf, S->clip, (size_t)n);
    buf[n] = '\0';
    return n;
}

int GetClipboardTextLength(void) { return S->clip_len; }

int GetClassIcon(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w ? w->cls->icon : 0;
}

bool SystemParametersInfo(unsigned action, unsigned param, void *pv, unsigned flags)
{
    (void)param; (void)flags;
    if (!pv) return false;
    if (action == SPI_GETWORKAREA) { *(RECT *)pv = S->work; return true; }
    if (action == SPI_SETWORKAREA) { S->work = *(const RECT *)pv; return true; }
    return false;
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
        if (!w->parent)
            ui_shell_notify(HSHELL_REDRAW, w);
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
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_NCLBUTTONDOWN:                     /* a click on the frame */
        ui_track_frame(w, (int)wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_NCLBUTTONDBLCLK:
        if (wp == HT_CAPTION && (w->style & WS_MAXIMIZEBOX))
            SendMessage(h, WM_SYSCOMMAND, w->state == WS_STATE_MAX ? SC_RESTORE : SC_MAXIMIZE, 0);
        return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case SC_CLOSE:    SendMessage(h, WM_CLOSE, 0, 0); break;
        case SC_MINIMIZE: frame_minimize(w); break;
        case SC_MAXIMIZE: frame_maximize(w); break;
        case SC_RESTORE:  frame_restore(w); wnd_activate(w); break;
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

HCURSOR SetWindowCursor(HWND h, HCURSOR cursor)
{
    struct wnd *w = wnd_from(h);
    if (!w) return 0;
    HCURSOR old = w->cursor;
    w->cursor = cursor;
    S->in.cur_key = 0;                          /* look again at the next move */
    return old;
}

HWND WindowFromPoint(POINT p)
{
    for (int i = S->ntops - 1; i >= 0; i--) {
        struct wnd *t = S->tops[i];
        if (!t->visible || !frame_shown(t) || t->cls->proc == tooltip_proc)
            continue;
        RECT o, c;
        frame_outer(t, &o);
        if (!rect_contains(&o, p.x, p.y))
            continue;
        frame_client(t, &c);
        if (t->state == WS_STATE_MIN || !rect_contains(&c, p.x, p.y))
            return t;
        int cx, cy;
        wnd_client_screen(t, &cx, &cy);
        return wnd_child_at(t, p.x - cx, p.y - cy);
    }
    return NULL;
}

HWND CreateStatusWindow(unsigned style, const char *text, HWND parent, int id)
{
    return CreateWindow(STATUSCLASSNAME, text ? text : "", style | WS_CHILD, 0, 0, 0, 0, parent, id, NULL);
}

/* Children with anchors keep their distance from the parent's edges they
 * are anchored to: right (or bottom) alone moves them, left and right (top
 * and bottom) together stretch them. */
void ui_reflow(struct wnd *p)
{
    RECT c;
    GetClientRect(p, &c);
    int dw = c.right - p->cw0, dh = c.bottom - p->ch0;
    p->cw0 = c.right;
    p->ch0 = c.bottom;
    if (!dw && !dh)
        return;
    for (struct wnd *k = p->first; k; k = k->next) {
        unsigned a = k->anchor;
        if (!(a & (ANCHOR_RIGHT | ANCHOR_BOTTOM)))
            continue;
        int x = k->x, y = k->y, w = k->w, h = k->h;
        if (a & ANCHOR_RIGHT) { if (a & ANCHOR_LEFT) w += dw; else x += dw; }
        if (a & ANCHOR_BOTTOM) { if (a & ANCHOR_TOP) h += dh; else y += dh; }
        if (w < 0) w = 0;
        if (h < 0) h = 0;
        MoveWindow(k, x, y, w, h, true);
    }
}

bool SetWindowAnchor(HWND h, unsigned anchors)
{
    struct wnd *w = wnd_from(h);
    if (!w || !w->parent) return false;
    w->anchor = (uint8_t)anchors;
    return true;
}

bool ScreenToClient(HWND h, POINT *p)
{
    struct wnd *w = wnd_from(h);
    if (!w || !p) return false;
    int ox, oy;
    wnd_client_screen(w, &ox, &oy);
    p->x -= ox;
    p->y -= oy;
    return true;
}

bool ClientToScreen(HWND h, POINT *p)
{
    struct wnd *w = wnd_from(h);
    if (!w || !p) return false;
    int ox, oy;
    wnd_client_screen(w, &ox, &oy);
    p->x += ox;
    p->y += oy;
    return true;
}

bool IsZoomed(HWND h)
{
    struct wnd *w = wnd_from(h);
    return w && w->state == WS_STATE_MAX;
}
