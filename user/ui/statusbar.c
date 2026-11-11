/*
 * statusbar.c -- the status bar (STATUSCLASSNAME): a strip along the bottom
 * of its parent in sunken parts, each with a line of text, and with
 * SBARS_SIZEGRIP a grip in the corner that sizes the window.
 *
 * It puts itself at the bottom of the parent's client area when it's made
 * and whenever it gets WM_SIZE (the parent passes its own on). SB_SETPARTS
 * takes the parts' right edges (-1: to the end); SB_SIMPLE shows one part
 * across the whole bar, for menu help, without losing the others' texts.
 */
#include "uipriv.h"

#define PARTS_MAX 8
#define BAR_H     20
#define GRIP      14

struct sbdata {
    int   n, right[PARTS_MAX];
    char *text[PARTS_MAX];
    unsigned style[PARTS_MAX];          /* SBT_* */
    char *simple_text;
    bool  simple;
};

static struct sbdata *sd(struct wnd *w) { return w->ctl; }
static bool grip(struct wnd *w)
{
    struct wnd *t = wnd_top(w);
    return (w->style & SBARS_SIZEGRIP) && t && (t->style & WS_THICKFRAME) && t->state == WS_STATE_NORMAL;
}

static void place(struct wnd *w)
{
    if (!w->parent || (w->style & CCS_NOPARENTALIGN))
        return;
    RECT c;
    GetClientRect(w->parent, &c);
    if (w->x != 0 || w->y != c.bottom - BAR_H || w->w != c.right || w->h != BAR_H)
        MoveWindow(w, 0, c.bottom - BAR_H, c.right, BAR_H, true);   /* (its own WM_SIZE comes back here) */
}

static RECT part_rect(struct wnd *w, int i)
{
    struct sbdata *d = sd(w);
    int right_end = w->w - (grip(w) ? GRIP : 2);
    if (d->simple || d->n == 0)
        return (RECT){ 2, 3, right_end, w->h - 1 };
    int left = i ? d->right[i - 1] + 2 : 2;
    int right = d->right[i] < 0 || d->right[i] > right_end ? right_end : d->right[i];
    return (RECT){ left, 3, right, w->h - 1 };
}

static void set_text(char **slot, const char *s)
{
    LocalFree(*slot);
    *slot = NULL;
    if (s && (*slot = LocalAlloc(0, (uint32_t)strlen(s) + 1)))
        strcpy(*slot, s);
}

LRESULT statusbar_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct sbdata *d = sd(w);
    if (!d && msg != WM_CREATE)
        return DefWindowProc(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        if (!(d = LocalAlloc(LMEM_ZEROINIT, sizeof *d)))
            return -1;
        w->ctl = d;
        d->n = 1;
        d->right[0] = -1;
        place(w);
        return 0;
    case WM_DESTROY:
        for (int i = 0; i < PARTS_MAX; i++) LocalFree(d->text[i]);
        LocalFree(d->simple_text);
        LocalFree(d);
        w->ctl = NULL;
        return 0;
    case WM_SIZE:
        place(w);
        InvalidateRect(h, NULL);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SelectFont(dc, FONT_GUI);
        RECT all = { 0, 0, w->w, w->h };
        FillRect(dc, &all, C_FACE);
        DrawLine(dc, 0, 0, w->w, false, C_GRAY4);
        DrawLine(dc, 0, 1, w->w, false, C_WHITE);
        int n = d->simple ? 1 : d->n;
        for (int i = 0; i < n; i++) {
            RECT r = part_rect(w, i);
            unsigned st = d->simple ? 0 : d->style[i];
            if (!(st & SBT_NOBORDERS))
                DrawBevel(dc, &r, (st & SBT_POPOUT) != 0);
            if (st & SBT_OWNERDRAW) {
                RECT in = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
                owner_draw(w, dc, 0, (unsigned)i, ODA_DRAWENTIRE, 0, in, (LPARAM)d->text[i]);
                SelectFont(dc, FONT_GUI);
                continue;
            }
            const char *t = d->simple ? d->simple_text : d->text[i];
            if (!t) continue;
            RECT tr = { r.left + 4, r.top + 1, r.right - 3, r.bottom - 1 };
            unsigned f = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            if (t[0] == '\t') {                 /* tabs centre or right-align, as in Windows */
                t++;
                f |= DT_CENTER;
                if (t[0] == '\t') { t++; f = (f & ~(unsigned)DT_CENTER) | DT_RIGHT; }
            }
            SetTextColor(dc, w->enabled ? C_BLACK : C_GRAY3);
            DrawText(dc, t, -1, &tr, f);
        }
        if (grip(w))                            /* three diagonal ridges */
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 2; j++) {
                    int o = 4 + k * 4 + j;
                    DrawLineEx(dc, w->w - o, w->h - 2, w->w - 2, w->h - o, j ? C_WHITE : C_GRAY4);
                }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        bool on = grip(w) && GET_X_LPARAM(lp) >= w->w - GRIP;
        SetCursor(LoadCursor(NULL, on ? IDC_SIZENWSE : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (grip(w) && GET_X_LPARAM(lp) >= w->w - GRIP) {   /* the window's corner */
            struct wnd *t = wnd_top(w);
            int ox, oy;
            wnd_client_screen(w, &ox, &oy);
            ui_reserve_input(ui_pid);
            SendMessage(t, WM_NCLBUTTONDOWN, (WPARAM)(HT_BORDER | (E_R | E_B) << 8),
                        MAKELPARAM(ox + GET_X_LPARAM(lp), oy + GET_Y_LPARAM(lp)));
        }
        return 0;
    case SB_SETPARTS: {
        int n = clampi((int)wp, 1, PARTS_MAX);
        const int *edges = (const int *)lp;
        for (int i = 0; i < n; i++)
            d->right[i] = edges ? edges[i] : -1;
        for (int i = n; i < d->n; i++)
            set_text(&d->text[i], NULL);
        d->n = n;
        InvalidateRect(h, NULL);
        return 1;
    }
    case SB_GETPARTS: {
        int n = (int)wp < d->n ? (int)wp : d->n;
        for (int i = 0; i < n && lp; i++)
            ((int *)lp)[i] = d->right[i];
        return d->n;
    }
    case SB_SETTEXT: {
        int i = (int)(wp & 0xFF);
        if (i == SB_SIMPLEID) {
            set_text(&d->simple_text, (const char *)lp);
        } else if (i < d->n) {
            d->style[i] = (unsigned)wp & 0xFF00;
            if (wp & SBT_OWNERDRAW) { LocalFree(d->text[i]); d->text[i] = (char *)lp; }
            else set_text(&d->text[i], (const char *)lp);
        } else {
            return 0;
        }
        InvalidateRect(h, NULL);
        return 1;
    }
    case SB_GETTEXT:
    case SB_GETTEXTLENGTH: {
        int i = (int)wp;
        const char *t = i == SB_SIMPLEID ? d->simple_text : i >= 0 && i < d->n ? d->text[i] : NULL;
        int len = t ? (int)strlen(t) : 0;
        if (msg == SB_GETTEXT && lp) strcpy((char *)lp, t ? t : "");
        return MAKELONG(len, i >= 0 && i < d->n ? d->style[i] : 0);
    }
    case SB_SIMPLE:
        d->simple = wp != 0;
        InvalidateRect(h, NULL);
        return 0;
    case SB_ISSIMPLE:
        return d->simple;
    case SB_GETRECT:
        if ((int)wp < 0 || (int)wp >= d->n || !lp) return 0;
        *(RECT *)lp = part_rect(w, (int)wp);
        return 1;
    case WM_SETTEXT:                            /* the first part's text */
        DefWindowProc(h, msg, wp, lp);
        set_text(&d->text[0], (const char *)lp);
        InvalidateRect(h, NULL);
        return 1;
    }
    return DefWindowProc(h, msg, wp, lp);
}
