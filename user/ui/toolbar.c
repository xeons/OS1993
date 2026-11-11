/*
 * toolbar.c -- the toolbar (TOOLBARCLASSNAME): a row of picture buttons
 * along the top of its parent, the pictures from an image list.
 *
 * TBSTYLE_BUTTON items send WM_COMMAND (their idCommand) when clicked;
 * TBSTYLE_CHECK ones stay in until clicked again; TBSTYLE_CHECKGROUP ones
 * are radio buttons among their neighbours; TBSTYLE_SEP is a gap with a
 * groove. With TBSTYLE_FLAT the buttons are flat until the pointer is
 * over them. With TBSTYLE_TOOLTIPS a tooltip shows each button's string
 * (TBBUTTON.iString: a char *). Like the status bar, it places itself
 * (across the top) when made and on WM_SIZE or TB_AUTOSIZE.
 */
#include "uipriv.h"

#define BTN_MAX 32
#define SEP_W   8

struct tbtn { int image, id; uint8_t state, style; char *tip; };

struct tbdata {
    struct tbtn b[BTN_MAX];
    int n, bw, bh;                      /* button size */
    HIMAGELIST il;
    int hot, press;                     /* the button under the pointer, held down; -1 */
    bool inside;
    HWND tips;
};

static struct tbdata *td(struct wnd *w) { return w->ctl; }

static int bar_h(struct wnd *w) { return td(w)->bh + 6; }

static void place(struct wnd *w)
{
    if (!w->parent || (w->style & CCS_NOPARENTALIGN))
        return;
    RECT c;
    GetClientRect(w->parent, &c);
    if (w->x != 0 || w->y != 0 || w->w != c.right || w->h != bar_h(w))
        MoveWindow(w, 0, 0, c.right, bar_h(w), true);       /* (its own WM_SIZE comes back here) */
}

static RECT btn_rect(struct wnd *w, int i)
{
    struct tbdata *d = td(w);
    int x = 4;
    for (int k = 0; k < i; k++)
        x += (d->b[k].style & TBSTYLE_SEP) ? SEP_W : d->bw;
    int bw = (d->b[i].style & TBSTYLE_SEP) ? SEP_W : d->bw;
    return (RECT){ x, 3, x + bw, 3 + d->bh };
}

static int btn_at(struct wnd *w, int x, int y)
{
    for (int i = 0; i < td(w)->n; i++) {
        RECT r = btn_rect(w, i);
        if (rect_contains(&r, x, y) && !(td(w)->b[i].style & TBSTYLE_SEP) &&
            !(td(w)->b[i].state & TBSTATE_HIDDEN))
            return i;
    }
    return -1;
}

static int find_cmd(struct wnd *w, int id)
{
    for (int i = 0; i < td(w)->n; i++)
        if (td(w)->b[i].id == id && !(td(w)->b[i].style & TBSTYLE_SEP))
            return i;
    return -1;
}

static void sync_tips(struct wnd *w)               /* a tool per button with a string */
{
    struct tbdata *d = td(w);
    if (!d->tips)
        return;
    for (int i = 0; i < BTN_MAX; i++) {
        TOOLINFO ti = { sizeof ti, 0, w, (unsigned)(i + 1), { 0, 0, 0, 0 }, NULL };
        SendMessage(d->tips, TTM_DELTOOL, 0, (LPARAM)&ti);
    }
    for (int i = 0; i < d->n; i++) {
        if (!d->b[i].tip || (d->b[i].style & TBSTYLE_SEP))
            continue;
        TOOLINFO ti = { sizeof ti, 0, w, (unsigned)(i + 1), btn_rect(w, i), d->b[i].tip };
        SendMessage(d->tips, TTM_ADDTOOL, 0, (LPARAM)&ti);
    }
}

static void click(struct wnd *w, int i)
{
    struct tbdata *d = td(w);
    struct tbtn *b = &d->b[i];
    if (b->style & TBSTYLE_GROUP && b->style & TBSTYLE_CHECK) {     /* radio: clear the run */
        int a = i, z = i;
        while (a > 0 && (d->b[a - 1].style & TBSTYLE_CHECKGROUP) == TBSTYLE_CHECKGROUP) a--;
        while (z < d->n - 1 && (d->b[z + 1].style & TBSTYLE_CHECKGROUP) == TBSTYLE_CHECKGROUP) z++;
        for (int k = a; k <= z; k++) d->b[k].state &= (uint8_t)~TBSTATE_CHECKED;
        b->state |= TBSTATE_CHECKED;
    } else if (b->style & TBSTYLE_CHECK) {
        b->state ^= TBSTATE_CHECKED;
    }
    InvalidateRect(w, NULL);
    if (w->parent)
        SendMessage(w->parent, WM_COMMAND, MAKEWPARAM(b->id, BN_CLICKED), (LPARAM)w);
}

LRESULT toolbar_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct tbdata *d = td(w);
    if (!d && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        if (!(d = LocalAlloc(LMEM_ZEROINIT, sizeof *d)))
            return -1;
        w->ctl = d;
        d->bw = 24;
        d->bh = 22;
        d->hot = d->press = -1;
        if (w->style & TBSTYLE_TOOLTIPS)
            d->tips = CreateWindow(TOOLTIPS_CLASS, "", WS_POPUP, 0, 0, 0, 0, NULL, 0, NULL);
        place(w);
        return 0;
    case WM_DESTROY:
        for (int i = 0; i < d->n; i++) LocalFree(d->b[i].tip);
        if (d->tips) DestroyWindow(d->tips);
        LocalFree(d);
        w->ctl = NULL;
        return 0;
    case WM_SIZE:
    case TB_AUTOSIZE:
        place(w);
        InvalidateRect(h, NULL);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT all = { 0, 0, w->w, w->h };
        FillRect(dc, &all, C_FACE);
        DrawLine(dc, 0, w->h - 2, w->w, false, C_GRAY4);
        DrawLine(dc, 0, w->h - 1, w->w, false, C_WHITE);
        bool flat = (w->style & TBSTYLE_FLAT) != 0;
        int cx = 16, cy = 16;
        if (d->il) ImageList_GetIconSize(d->il, &cx, &cy);
        for (int i = 0; i < d->n; i++) {
            struct tbtn *b = &d->b[i];
            if (b->state & TBSTATE_HIDDEN)
                continue;
            RECT r = btn_rect(w, i);
            if (b->style & TBSTYLE_SEP) {
                DrawLine(dc, r.left + SEP_W / 2 - 1, r.top + 1, d->bh - 2, true, C_GRAY4);
                DrawLine(dc, r.left + SEP_W / 2, r.top + 1, d->bh - 2, true, C_WHITE);
                continue;
            }
            bool on = (b->state & TBSTATE_ENABLED) && w->enabled;
            bool down = (i == d->press && d->inside) || (b->state & (TBSTATE_CHECKED | TBSTATE_PRESSED));
            bool hot = on && i == d->hot;
            if (down && (b->state & TBSTATE_CHECKED) && !(i == d->press && d->inside)) {
                RECT in = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };   /* checked: dotted */
                for (int y = in.top; y < in.bottom; y++)
                    for (int x = in.left + ((y ^ in.left) & 1); x < in.right; x += 2)
                        SetPixel(dc, x, y, C_WHITE);
            }
            if (down || !flat || hot) {
                if (!flat) FrameRect(dc, &r, C_GRAY1);
                RECT in = flat ? r : (RECT){ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
                DrawBevel(dc, &in, !down);
            }
            int ix = r.left + (d->bw - cx) / 2 + (down ? 1 : 0), iy = r.top + (d->bh - cy) / 2 + (down ? 1 : 0);
            if (d->il && b->image >= 0)
                ImageList_Draw(d->il, b->image, dc, ix, iy, on ? ILD_NORMAL : ILD_DISABLED);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i = btn_at(w, x, y);
        if (d->press >= 0) {
            bool in = i == d->press;
            if (in != d->inside) { d->inside = in; InvalidateRect(h, NULL); }
        } else if (i != d->hot) {
            d->hot = i;
            InvalidateRect(h, NULL);
            if (i >= 0 && !w->tracking) {           /* watch for the pointer leaving */
                w->tracking = true;
                SetTimer(h, 1, 100);
            }
        }
        return 0;
    }
    case WM_TIMER: {                                /* the pointer gone: no hot button */
        POINT p;
        GetCursorPos(&p);
        int ox, oy;
        wnd_client_screen(w, &ox, &oy);
        if (WindowFromPoint(p) != h || btn_at(w, p.x - ox, p.y - oy) < 0) {
            KillTimer(h, 1);
            w->tracking = false;
            if (d->hot >= 0 && d->press < 0) { d->hot = -1; InvalidateRect(h, NULL); }
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int i = btn_at(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i < 0 || !(d->b[i].state & TBSTATE_ENABLED))
            return 0;
        d->press = i;
        d->inside = true;
        SetCapture(h);
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_LBUTTONUP:
        if (d->press >= 0) {
            int i = d->press;
            bool fire = d->inside;
            d->press = -1;
            d->inside = false;
            ReleaseCapture();
            InvalidateRect(h, NULL);
            if (fire) click(w, i);
        }
        return 0;
    case TB_SETIMAGELIST: {
        HIMAGELIST old = d->il;
        d->il = (HIMAGELIST)lp;
        if (d->il) {
            int cx, cy;
            ImageList_GetIconSize(d->il, &cx, &cy);
            d->bw = cx + 8;
            d->bh = cy + 6;
            place(w);
        }
        InvalidateRect(h, NULL);
        return (LRESULT)old;
    }
    case TB_SETBUTTONSIZE:
        d->bw = clampi(LOWORD(lp), 8, 100);
        d->bh = clampi(HIWORD(lp), 8, 100);
        place(w);
        InvalidateRect(h, NULL);
        return 1;
    case TB_ADDBUTTONS:
    case TB_INSERTBUTTON: {
        const TBBUTTON *src = (const TBBUTTON *)lp;
        int n = msg == TB_ADDBUTTONS ? (int)wp : 1, at = msg == TB_ADDBUTTONS ? d->n : clampi((int)wp, 0, d->n);
        if (!src || d->n + n > BTN_MAX)
            return 0;
        memmove(&d->b[at + n], &d->b[at], (size_t)(d->n - at) * sizeof d->b[0]);
        for (int k = 0; k < n; k++) {
            struct tbtn *b = &d->b[at + k];
            *b = (struct tbtn){ src[k].iBitmap, src[k].idCommand, src[k].fsState, src[k].fsStyle, NULL };
            const char *t = (const char *)src[k].iString;
            if (t && (b->tip = LocalAlloc(0, (uint32_t)strlen(t) + 1)))
                strcpy(b->tip, t);
        }
        d->n += n;
        sync_tips(w);
        InvalidateRect(h, NULL);
        return 1;
    }
    case TB_DELETEBUTTON: {
        int i = (int)wp;
        if (i < 0 || i >= d->n) return 0;
        LocalFree(d->b[i].tip);
        memmove(&d->b[i], &d->b[i + 1], (size_t)(d->n - i - 1) * sizeof d->b[0]);
        d->n--;
        sync_tips(w);
        InvalidateRect(h, NULL);
        return 1;
    }
    case TB_BUTTONCOUNT:
        return d->n;
    case TB_ENABLEBUTTON:
    case TB_CHECKBUTTON:
    case TB_PRESSBUTTON:
    case TB_HIDEBUTTON: {
        int i = find_cmd(w, (int)wp);
        if (i < 0) return 0;
        uint8_t bit = msg == TB_ENABLEBUTTON ? TBSTATE_ENABLED : msg == TB_CHECKBUTTON ? TBSTATE_CHECKED :
                      msg == TB_PRESSBUTTON ? TBSTATE_PRESSED : TBSTATE_HIDDEN;
        uint8_t old = d->b[i].state;
        if (LOWORD(lp)) d->b[i].state |= bit; else d->b[i].state &= (uint8_t)~bit;
        if (d->b[i].state != old) {
            if (msg == TB_HIDEBUTTON) sync_tips(w);
            InvalidateRect(h, NULL);
        }
        return 1;
    }
    case TB_ISBUTTONENABLED:
    case TB_ISBUTTONCHECKED:
    case TB_ISBUTTONPRESSED:
    case TB_ISBUTTONHIDDEN: {
        int i = find_cmd(w, (int)wp);
        if (i < 0) return 0;
        uint8_t bit = msg == TB_ISBUTTONENABLED ? TBSTATE_ENABLED : msg == TB_ISBUTTONCHECKED ? TBSTATE_CHECKED :
                      msg == TB_ISBUTTONPRESSED ? TBSTATE_PRESSED : TBSTATE_HIDDEN;
        return (d->b[i].state & bit) != 0;
    }
    case TB_GETSTATE: {
        int i = find_cmd(w, (int)wp);
        return i < 0 ? -1 : d->b[i].state;
    }
    case TB_SETSTATE: {
        int i = find_cmd(w, (int)wp);
        if (i < 0) return 0;
        d->b[i].state = (uint8_t)LOWORD(lp);
        InvalidateRect(h, NULL);
        return 1;
    }
    case TB_GETITEMRECT:
        if ((int)wp < 0 || (int)wp >= d->n || !lp) return 0;
        *(RECT *)lp = btn_rect(w, (int)wp);
        return 1;
    case TB_GETTOOLTIPS:
        return (LRESULT)d->tips;
    }
    return DefWindowProc(h, msg, wp, lp);
}
