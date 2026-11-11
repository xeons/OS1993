/*
 * tooltip.c -- tooltips (TOOLTIPS_CLASS): a small window of text that
 * appears by the pointer when it rests over a tool.
 *
 * A tool is a rectangle of a window (in its client coordinates), or the
 * whole window with TTF_IDISHWND (uId is then the HWND). The tooltip
 * watches the pointer itself on a timer -- nobody needs to relay mouse
 * messages (TTM_RELAYEVENT is accepted and ignored) -- and shows a tool's
 * text half a second after the pointer stops over it, at once when it
 * moves on to another tool while one is showing, and hides it when the
 * pointer leaves, a button goes down, or after a few seconds.
 * LPSTR_TEXTCALLBACK asks the tool's window for the text (TTN_GETDISPINFO).
 */
#include "uipriv.h"

#define TOOL_MAX  48
#define POLL_MS   60
#define INITIAL   500
#define RESHOW    100
#define AUTOPOP   5000
#define TIMER_ID  1

struct tool { HWND hwnd; unsigned id, flags; RECT r; char *text; };

struct ttdata {
    struct tool t[TOOL_MAX];
    int n, cur, shown;                  /* the tool under the pointer, shown; -1 */
    uint32_t since, shown_at;
    int lx, ly;
    bool active, popped;                /* popped: hidden until the pointer leaves the tool */
    char text[128];
};

static struct ttdata *tt(struct wnd *w) { return w->ctl; }

static int find(struct ttdata *d, HWND hwnd, unsigned id)
{
    for (int i = 0; i < d->n; i++)
        if (d->t[i].hwnd == hwnd && d->t[i].id == id)
            return i;
    return -1;
}

static void set_text(struct tool *t, const char *s)
{
    if (t->text != LPSTR_TEXTCALLBACK)
        LocalFree(t->text);
    t->text = NULL;
    if (s == LPSTR_TEXTCALLBACK)
        t->text = LPSTR_TEXTCALLBACK;
    else if (s && (t->text = LocalAlloc(0, (uint32_t)strlen(s) + 1)))
        strcpy(t->text, s);
}

static bool is_inside(HWND a, HWND b)               /* a is b or inside it */
{
    for (struct wnd *w = wnd_from(a); w; w = w->parent)
        if (w == b) return true;
    return false;
}

static int tool_at(struct ttdata *d, int x, int y)
{
    POINT p = { x, y };
    HWND under = WindowFromPoint(p);
    for (int i = 0; i < d->n; i++) {
        struct tool *t = &d->t[i];
        struct wnd *tw = wnd_from(t->hwnd);
        if (!tw || !tw->visible)
            continue;
        if (t->flags & TTF_IDISHWND) {
            if (is_inside(under, (HWND)(uintptr_t)t->id))
                return i;
            continue;
        }
        if (under != t->hwnd)
            continue;
        int ox, oy;
        wnd_client_screen(tw, &ox, &oy);
        if (rect_contains(&t->r, x - ox, y - oy))
            return i;
    }
    return -1;
}

static void hide(struct wnd *w)
{
    struct ttdata *d = tt(w);
    if (d->shown >= 0)
        ShowWindow(w, SW_HIDE);
    d->shown = -1;
}

static void show(struct wnd *w, int i)
{
    struct ttdata *d = tt(w);
    struct tool *t = &d->t[i];
    const char *s = t->text;
    if (s == LPSTR_TEXTCALLBACK) {                  /* ask its window */
        NMTTDISPINFO di = { { w, t->id, TTN_GETDISPINFO }, NULL, "", 0, 0 };
        struct wnd *tw = wnd_from(t->hwnd);
        if (tw && wnd_mine(tw))
            SendMessage(tw, WM_NOTIFY, t->id, (LPARAM)&di);
        s = di.lpszText ? di.lpszText : di.szText;
    }
    if (!s || !*s)
        return;
    strncpy0(d->text, s, sizeof d->text);
    RECT m = { 0, 0, 300, 0 };
    struct dc tmp = { 0 };
    tmp.font = FONT_GUI;
    DrawText(&tmp, d->text, -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int cw = m.right - m.left + 10, ch = m.bottom - m.top + 4;
    POINT p;
    GetCursorPos(&p);
    int x = p.x, y = p.y + 20;
    if (x + cw > S->scr_w) x = S->scr_w - cw;
    if (y + ch > S->scr_h) y = p.y - ch - 4;
    if (x < 0) x = 0;
    MoveWindow(w, x, y, cw, ch, false);
    ShowWindow(w, SW_SHOWNOACTIVATE);
    InvalidateRect(w, NULL);
    d->shown = i;
    d->shown_at = GetTickCount();
    DebugLog("tooltip %s", d->text);
}

static void poll(struct wnd *w)
{
    struct ttdata *d = tt(w);
    if (!d->active || S->background) {
        hide(w);
        return;
    }
    POINT p;
    GetCursorPos(&p);
    uint32_t now = GetTickCount();
    int i = (S->in.last_buttons & 3) ? -1 : tool_at(d, p.x, p.y);
    if (S->in.last_buttons & 3) d->popped = true;
    if (i != d->cur || p.x != d->lx || p.y != d->ly) {
        if (i != d->cur) {
            d->popped = false;
            if (d->shown >= 0 && i >= 0) {          /* moving along a row of tools: quickly */
                d->cur = i;
                d->since = now - (INITIAL - RESHOW);
                hide(w);
            } else {
                hide(w);
                d->cur = i;
                d->since = now;
            }
        } else if (d->shown < 0) {
            d->since = now;                         /* still moving: wait */
        }
        d->lx = p.x;
        d->ly = p.y;
    }
    if (d->cur >= 0 && d->shown < 0 && !d->popped && now - d->since >= INITIAL)
        show(w, d->cur);
    else if (d->shown >= 0 && now - d->shown_at >= AUTOPOP) {
        hide(w);
        d->popped = true;
    }
}

LRESULT tooltip_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct ttdata *d = tt(w);
    if (!d && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        if (!(d = LocalAlloc(LMEM_ZEROINIT, sizeof *d)))
            return -1;
        w->ctl = d;
        d->cur = d->shown = -1;
        d->active = true;
        w->visible = false;
        SetTimer(h, TIMER_ID, POLL_MS);
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_ID);
        for (int i = 0; i < d->n; i++) set_text(&d->t[i], NULL);
        LocalFree(d);
        w->ctl = NULL;
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID) {
            for (int i = 0; i < d->n; i++)          /* tools of windows that went */
                if (!wnd_from(d->t[i].hwnd)) {
                    set_text(&d->t[i], NULL);
                    d->t[i--] = d->t[--d->n];
                    d->cur = d->shown = -1;
                }
            poll(w);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT all = { 0, 0, w->w, w->h }, r = { 5, 2, w->w - 4, w->h - 2 };
        FillRect(dc, &all, C_LIGHT);
        FrameRect(dc, &all, C_BLACK);
        SelectFont(dc, FONT_GUI);
        SetTextColor(dc, C_BLACK);
        DrawText(dc, d->text, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case TTM_ADDTOOL:
    case TTM_SETTOOLINFO: {
        const TOOLINFO *ti = (const TOOLINFO *)lp;
        if (!ti) return 0;
        int i = find(d, ti->hwnd, ti->uId);
        if (i < 0) {
            if (msg == TTM_SETTOOLINFO || d->n == TOOL_MAX) return 0;
            i = d->n++;
            memset(&d->t[i], 0, sizeof d->t[i]);
        }
        struct tool *t = &d->t[i];
        t->hwnd = ti->hwnd;
        t->id = ti->uId;
        t->flags = ti->uFlags;
        t->r = ti->rect;
        set_text(t, ti->lpszText);
        return 1;
    }
    case TTM_DELTOOL: {
        const TOOLINFO *ti = (const TOOLINFO *)lp;
        int i = ti ? find(d, ti->hwnd, ti->uId) : -1;
        if (i >= 0) {
            set_text(&d->t[i], NULL);
            d->t[i] = d->t[--d->n];
            if (d->shown >= 0) hide(w);
            d->cur = -1;
        }
        return 0;
    }
    case TTM_NEWTOOLRECT: {
        const TOOLINFO *ti = (const TOOLINFO *)lp;
        int i = ti ? find(d, ti->hwnd, ti->uId) : -1;
        if (i >= 0) d->t[i].r = ti->rect;
        return 0;
    }
    case TTM_UPDATETIPTEXT: {
        const TOOLINFO *ti = (const TOOLINFO *)lp;
        int i = ti ? find(d, ti->hwnd, ti->uId) : -1;
        if (i >= 0) {
            set_text(&d->t[i], ti->lpszText);
            if (d->shown == i) show(w, i);
        }
        return 0;
    }
    case TTM_GETTOOLCOUNT:
        return d->n;
    case TTM_ACTIVATE:
        d->active = wp != 0;
        if (!d->active) hide(w);
        return 0;
    case TTM_POP:
        hide(w);
        d->popped = true;
        return 0;
    case TTM_RELAYEVENT:                            /* not needed: it watches the pointer */
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}
