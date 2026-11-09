/*
 * updown.c -- the UPDOWN class (a spin box's arrows): two bevelled arrow
 * buttons, stacked (side by side with UDS_HORZ), that step w->value
 * between w->min and w->max. Held down, a button repeats (a timer).
 *
 * As in Windows, it moves itself and tells its parent: WM_VSCROLL
 * (WM_HSCROLL) SB_THUMBPOSITION with the new position, SB_ENDSCROLL when
 * the mouse lets go. A buddy window (usually an EDIT) can show the number
 * (UDS_SETBUDDYINT; typed numbers are read back) and lend it its Up/Down
 * keys (UDS_ARROWKEYS, by subclassing the buddy). w->ctl holds the buddy
 * and its old window procedure; w->part is the arrow being held (1 the
 * one toward max, 2 the other), w->pressed whether the pointer is on it.
 */
#include "uipriv.h"

#define TIMER_ID    1
#define FIRST_DELAY 500
#define REPEAT      80
#define PROP        "#UPDOWN"

static const uint16_t g_up[4]    = { 0x1000, 0x3800, 0x7C00, 0xFE00 };
static const uint16_t g_down[4]  = { 0xFE00, 0x7C00, 0x3800, 0x1000 };
static const uint16_t g_left[7]  = { 0x1000, 0x3000, 0x7000, 0xF000, 0x7000, 0x3000, 0x1000 };
static const uint16_t g_right[7] = { 0x8000, 0xC000, 0xE000, 0xF000, 0xE000, 0xC000, 0x8000 };

enum { PART_NONE, PART_INC, PART_DEC };

struct updown { struct wnd *buddy; WNDPROC buddy_proc; };

static struct updown *data_of(struct wnd *w) { return (struct updown *)w->ctl; }

static bool horz(struct wnd *w) { return (w->style & UDS_HORZ) != 0; }

/* The arrow's rectangle: the top (right) half steps toward max. */
static RECT part_rect(struct wnd *w, int part)
{
    if (horz(w)) {
        int half = w->w / 2;
        return part == PART_INC ? (RECT){ w->w - half, 0, w->w, w->h } : (RECT){ 0, 0, half, w->h };
    }
    int half = w->h / 2;
    return part == PART_INC ? (RECT){ 0, 0, w->w, half } : (RECT){ 0, w->h - half, w->w, w->h };
}

static int hit(struct wnd *w, int x, int y)
{
    RECT a = part_rect(w, PART_INC), b = part_rect(w, PART_DEC);
    return rect_contains(&a, x, y) ? PART_INC : rect_contains(&b, x, y) ? PART_DEC : PART_NONE;
}

/* ---- the buddy ----------------------------------------------------------- */

static struct wnd *buddy_of(struct wnd *w)
{
    struct updown *d = data_of(w);
    return d ? wnd_from(d->buddy) : NULL;
}

static void show_pos(struct wnd *w)                 /* UDS_SETBUDDYINT: the number */
{
    struct wnd *b = buddy_of(w);
    if (!b || !(w->style & UDS_SETBUDDYINT))
        return;
    char buf[12], *p = buf + sizeof buf;
    int v = w->value;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    *--p = '\0';
    do *--p = (char)('0' + u % 10); while (u /= 10);
    if (v < 0) *--p = '-';
    SetWindowText(b, p);
}

/* The buddy's number, if it holds one (UDS_SETBUDDYINT). */
static bool read_pos(struct wnd *w, int *out)
{
    struct wnd *b = buddy_of(w);
    if (!b || !(w->style & UDS_SETBUDDYINT))
        return false;
    char buf[TEXT_MAX];
    GetWindowText(b, buf, sizeof buf);
    const char *s = buf;
    bool neg = *s == '-';
    if (neg) s++;
    if (*s < '0' || *s > '9')
        return false;
    int v = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        if (v < 100000) v = v * 10 + (*s - '0');
    if (*s)
        return false;
    *out = neg ? -v : v;
    return true;
}

static int lo(struct wnd *w) { return w->min < w->max ? w->min : w->max; }
static int hi(struct wnd *w) { return w->min < w->max ? w->max : w->min; }

static LRESULT buddy_sub(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *u = wnd_from((HWND)GetProp(h, PROP));
    struct updown *d = u ? data_of(u) : NULL;
    if (!d)
        return DefWindowProc(h, msg, wp, lp);
    if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN))
        return SendMessage(u, msg, wp, lp);
    return CallWindowProc(d->buddy_proc, h, msg, wp, lp);
}

static void unhook(struct wnd *w)
{
    struct updown *d = data_of(w);
    struct wnd *b = buddy_of(w);
    if (b && d->buddy_proc && b->proc == buddy_sub) {
        SetWindowLong(b, GWL_WNDPROC, (int32_t)(uintptr_t)d->buddy_proc);
        RemoveProp(b, PROP);
    }
    d->buddy = NULL;
    d->buddy_proc = NULL;
}

static void set_buddy(struct wnd *w, struct wnd *b)
{
    struct updown *d = data_of(w);
    unhook(w);
    if (!b)
        return;
    d->buddy = b;
    if (w->style & UDS_ALIGNRIGHT) {                /* an edit: as tall as its field */
        int ring = b->cls->proc == edit_proc && !(b->style & ES_MULTILINE) ? 3 : 0;
        MoveWindow(w, b->x + b->w, b->y + ring, w->w, b->h - 2 * ring, true);
    }
    if ((w->style & UDS_ARROWKEYS) && wnd_mine(b)) {
        SetProp(b, PROP, w);
        d->buddy_proc = (WNDPROC)(uintptr_t)SetWindowLong(b, GWL_WNDPROC, (int32_t)(uintptr_t)buddy_sub);
    }
    show_pos(w);
}

/* ---- stepping --------------------------------------------------------------- */

static void notify(struct wnd *w, int code)
{
    if (w->parent)
        SendMessage(w->parent, horz(w) ? WM_HSCROLL : WM_VSCROLL,
                    MAKEWPARAM(code, w->value), (LPARAM)w);
}

static void step(struct wnd *w, int part)
{
    int v = w->value;
    read_pos(w, &v);
    int dir = (part == PART_INC) == (w->max >= w->min) ? 1 : -1;
    int n = v + dir;
    if (n > hi(w)) n = (w->style & UDS_WRAP) ? lo(w) : hi(w);
    if (n < lo(w)) n = (w->style & UDS_WRAP) ? hi(w) : lo(w);
    w->value = n;
    show_pos(w);
    if (n != v)
        notify(w, SB_THUMBPOSITION);
}

/* ---- painting ------------------------------------------------------------------ */

static void paint_part(HDC dc, struct wnd *w, int part)
{
    RECT r = part_rect(w, part);
    bool down = w->part == part && w->pressed, dis = !w->enabled;
    FrameRect(dc, &r, dis ? C_GRAY4 : C_GRAY1);
    RECT in = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
    FillRect(dc, &in, down ? C_GRAY4 : C_FACE);
    if (!down)
        DrawBevel(dc, &in, true);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    uint8_t fg = dis ? C_GRAY4 : C_BLACK;
    if (horz(w))
        DrawBitmap(dc, cx - 2, cy - 3, part == PART_INC ? g_right : g_left, 7, fg);
    else
        DrawBitmap(dc, cx - 3, cy - 2, part == PART_INC ? g_up : g_down, 4, fg);
}

LRESULT updown_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    if (!data_of(w) && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        w->max = 100;
        if (!(w->ctl = LocalAlloc(LMEM_ZEROINIT, sizeof(struct updown))))
            return -1;
        if ((w->style & UDS_AUTOBUDDY) && w->parent) {      /* the sibling before it */
            struct wnd *prev = NULL;
            for (struct wnd *c = w->parent->first; c && c != w; c = c->next)
                prev = c;
            set_buddy(w, prev);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_ID);
        unhook(w);
        LocalFree(w->ctl);
        w->ctl = NULL;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint_part(dc, w, PART_INC);
        paint_part(dc, w, PART_DEC);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ENABLE:
        InvalidateRect(h, NULL);
        return 0;
    case WM_LBUTTONDOWN:
        w->part = hit(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!w->part)
            return 0;
        if (buddy_of(w))
            SetFocus(buddy_of(w));
        SetCapture(h);
        w->tracking = w->pressed = true;
        InvalidateRect(h, NULL);
        step(w, w->part);
        SetTimer(h, TIMER_ID, FIRST_DELAY);
        return 0;
    case WM_MOUSEMOVE:
        if (w->tracking) {
            bool in = hit(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == w->part;
            if (in != w->pressed) {
                w->pressed = in;
                InvalidateRect(h, NULL);
            }
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID && w->tracking) {
            if (w->pressed)
                step(w, w->part);
            SetTimer(h, TIMER_ID, REPEAT);
        }
        return 0;
    case WM_LBUTTONUP:
        if (w->tracking) {
            ReleaseCapture();
            KillTimer(h, TIMER_ID);
            w->tracking = w->pressed = false;
            w->part = PART_NONE;
            InvalidateRect(h, NULL);
            notify(w, SB_ENDSCROLL);
        }
        return 0;
    case WM_KEYDOWN:                                /* also the buddy's, with UDS_ARROWKEYS */
        if (wp == VK_UP || wp == VK_DOWN) {
            step(w, wp == VK_UP ? PART_INC : PART_DEC);
            return 0;
        }
        break;
    case UDM_SETRANGE:
        w->max = (int16_t)LOWORD(lp);
        w->min = (int16_t)HIWORD(lp);
        w->value = clampi(w->value, lo(w), hi(w));
        show_pos(w);
        return 0;
    case UDM_GETRANGE:
        return MAKELONG(w->max, w->min);
    case UDM_SETPOS: {
        int old = w->value;
        w->value = clampi((int16_t)LOWORD(lp), lo(w), hi(w));
        show_pos(w);
        return old;
    }
    case UDM_GETPOS: {
        int v = w->value;
        bool bad = buddy_of(w) && (w->style & UDS_SETBUDDYINT) && !read_pos(w, &v);
        if (!bad)
            w->value = clampi(v, lo(w), hi(w));
        return MAKELONG(w->value, bad);
    }
    case UDM_SETBUDDY: {
        struct wnd *old = buddy_of(w);
        set_buddy(w, wnd_from((HWND)wp));
        return (LRESULT)(uintptr_t)old;
    }
    case UDM_GETBUDDY:
        return (LRESULT)(uintptr_t)buddy_of(w);
    }
    return DefWindowProc(h, msg, wp, lp);
}
