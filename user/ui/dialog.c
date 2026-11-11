/*
 * dialog.c -- keyboard navigation in dialog-like windows, and helpers.
 *
 * IsDialogMessage, called from the message loop, gives a window the dialog
 * keys: Tab / Shift+Tab move the focus through WS_TABSTOP controls, Enter
 * presses the focused push button or else the default one, Esc sends
 * WM_COMMAND IDCANCEL. The arrow keys move between the buttons of a
 * group (siblings from a WS_GROUP to the next) and, for radio buttons,
 * check the one they land on; tabbing into a group lands on its checked
 * radio button.
 */
#include "uipriv.h"

static bool is_descendant(struct wnd *w, struct wnd *ancestor)
{
    for (; w; w = w->parent)
        if (w == ancestor)
            return true;
    return false;
}

/* The WS_TABSTOP controls of dlg, depth first. */
static int tabstops(struct wnd *w, struct wnd **out, int n, int max)
{
    for (struct wnd *c = w->first; c && n < max; c = c->next) {
        if (c->visible && c->enabled && (c->style & WS_TABSTOP))
            out[n++] = c;
        n = tabstops(c, out, n, max);
    }
    return n;
}

static struct wnd *find_default(struct wnd *w)
{
    for (struct wnd *c = w->first; c; c = c->next) {
        if (c->cls->proc == button_proc && (c->style & BS_TYPEMASK) == BS_DEFPUSHBUTTON &&
            c->visible && c->enabled)
            return c;
        struct wnd *d = find_default(c);
        if (d)
            return d;
    }
    return NULL;
}

static bool is_radio(struct wnd *c)
{
    int t = (int)(c->style & BS_TYPEMASK);
    return c->cls->proc == button_proc && (t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON);
}

/* The group around c: its first and the one after its last sibling. */
static struct wnd *group_start(struct wnd *c)
{
    struct wnd *start = c->parent ? c->parent->first : c;
    for (struct wnd *k = start; k; k = k->next) {
        if (k->style & WS_GROUP) start = k;
        if (k == c) break;
    }
    return start;
}

static struct wnd *group_item(struct wnd *c, bool prev)
{
    struct wnd *start = group_start(c), *items[WND_MAX];
    int n = 0, at = 0;
    for (struct wnd *k = start; k && n < WND_MAX; k = k->next) {
        if (k != start && (k->style & WS_GROUP))
            break;
        if (k == c) at = n;
        if (k == c || (k->visible && k->enabled && k->cls->proc == button_proc &&
                       (k->style & BS_TYPEMASK) != BS_GROUPBOX))
            items[n++] = k;
    }
    if (n < 2)
        return c;
    return items[(at + (prev ? n - 1 : 1)) % n];
}

HWND GetNextDlgGroupItem(HWND dlg, HWND ctl, bool prev)
{
    (void)dlg;
    struct wnd *c = wnd_from(ctl);
    return c ? group_item(c, prev) : NULL;
}

HWND GetNextDlgTabItem(HWND dlg, HWND ctl, bool prev)
{
    struct wnd *d = wnd_from(dlg), *c = wnd_from(ctl), *list[WND_MAX];
    if (!d) return NULL;
    int n = tabstops(d, list, 0, WND_MAX), i = 0;
    if (!n) return NULL;
    while (i < n && list[i] != c) i++;
    return list[i == n ? 0 : (i + (prev ? n - 1 : 1)) % n];
}

static struct wnd *checked_in_group(struct wnd *c)
{
    if (!is_radio(c))
        return c;
    struct wnd *start = group_start(c);
    for (struct wnd *k = start; k; k = k->next) {
        if (k != start && (k->style & WS_GROUP))
            break;
        if (is_radio(k) && k->value && k->visible && k->enabled)
            return k;
    }
    return c;
}

static struct wnd *find_tabs(struct wnd *w)
{
    for (struct wnd *c = w->first; c; c = c->next) {
        if (c->visible && c->enabled && c->cls->proc == tab_proc)
            return c;
        struct wnd *t = find_tabs(c);
        if (t) return t;
    }
    return NULL;
}

static char mnemonic(const char *t)
{
    for (; *t; t++)
        if (*t == '&') {
            if (t[1] == '&') { t++; continue; }
            char c = t[1];
            return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        }
    return 0;
}

/* The control whose label has the mnemonic ch (a static's: the next
 * control after it, as a label names the field beside it). */
static struct wnd *find_mnemonic(struct wnd *w, char ch, bool *is_label)
{
    for (struct wnd *c = w->first; c; c = c->next) {
        if (!c->visible || !c->enabled)
            continue;
        bool btn = c->cls->proc == button_proc, label = c->cls->proc == static_proc;
        if ((btn || label) && mnemonic(c->text) == ch) {
            *is_label = label || (c->style & BS_TYPEMASK) == BS_GROUPBOX;
            return c;
        }
        struct wnd *d = find_mnemonic(c, ch, is_label);
        if (d) return d;
    }
    return NULL;
}

bool IsDialogMessage(HWND dlg, const MSG *m)
{
    struct wnd *d = wnd_from(dlg), *target = wnd_from(m->hwnd);
    if (!d || !target || !is_descendant(target, d))
        return false;
    if (m->message == WM_CHAR && (m->wParam == 9 || m->wParam == 13 || m->wParam == 27))
        return true;                            /* consumed as dialog keys */
    if (m->message == WM_SYSKEYDOWN && (m->lParam & (1 << 29)) &&
        ((m->wParam >= 'A' && m->wParam <= 'Z') || (m->wParam >= '0' && m->wParam <= '9'))) {
        bool label = false;
        struct wnd *c = find_mnemonic(d, (char)m->wParam, &label);
        if (!c)
            return false;                       /* the menu bar's, perhaps */
        if (label) {                            /* the next control that takes the focus */
            struct wnd *n = c->next;
            while (n && (!n->visible || !n->enabled || !(n->style & WS_TABSTOP)))
                n = n->next;
            if (n) {
                SetFocus(n);
                if (n->cls->proc == edit_proc)
                    SendMessage(n, EM_SETSEL, 0, -1);
            }
        } else {
            SetFocus(c);
            SendMessage(c, BM_CLICK, 0, 0);
        }
        return true;
    }
    if (m->message != WM_KEYDOWN)
        return false;

    switch (m->wParam) {
    case VK_TAB:
        if (GetKeyState(VK_CONTROL) < 0) {      /* Ctrl+Tab: the next tab of a tab control */
            struct wnd *t = find_tabs(d);
            if (t) {
                int n = (int)SendMessage(t, TCM_GETITEMCOUNT, 0, 0), cur = (int)SendMessage(t, TCM_GETCURSEL, 0, 0);
                int dir = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
                if (n > 1)
                    SendMessage(t, TCM_SETCURFOCUS, (WPARAM)((cur + dir + n) % n), 0);
            }
            return true;
        }
        {
        struct wnd *list[WND_MAX];
        int n = tabstops(d, list, 0, WND_MAX);
        if (!n)
            return true;
        struct wnd *f = wnd_from(GetFocus());
        int i = 0;
        while (i < n && list[i] != f)
            i++;
        if (i == n && f)                            /* inside one (a combo box's field) */
            for (i = 0; i < n && !is_descendant(f, list[i]); i++)
                ;
        int dir = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
        i = i == n ? 0 : (i + dir + n) % n;
        list[i] = checked_in_group(list[i]);
        SetFocus(list[i]);
        if (list[i]->cls->proc == edit_proc)        /* tabbing into a field selects it */
            SendMessage(list[i], EM_SETSEL, 0, -1);
        return true;
    }
    case VK_RETURN: {
        struct wnd *f = wnd_from(GetFocus());
        int type = f && f->cls->proc == button_proc ? (int)(f->style & BS_TYPEMASK) : -1;
        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
            SendMessage(f, BM_CLICK, 0, 0);
            return true;
        }
        struct wnd *def = find_default(d);
        if (def)
            SendMessage(def, BM_CLICK, 0, 0);
        else
            SendMessage(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        return true;
    }
    case VK_ESCAPE:
        SendMessage(d, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        return true;
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: {
        struct wnd *f = wnd_from(GetFocus());
        if (!f || f->cls->proc != button_proc || (f->style & BS_TYPEMASK) == BS_GROUPBOX)
            return false;                       /* the control's own arrows */
        struct wnd *to = group_item(f, m->wParam == VK_LEFT || m->wParam == VK_UP);
        if (to == f)
            return true;
        SetFocus(to);
        if (is_radio(to) && to->cls->proc == button_proc)
            SendMessage(to, BM_CLICK, 0, 0);
        return true;
    }
    }
    return false;
}

bool CheckDlgButton(HWND dlg, int id, int check)
{
    HWND b = GetDlgItem(dlg, id);
    if (!b) return false;
    SendMessage(b, BM_SETCHECK, (WPARAM)check, 0);
    return true;
}

int IsDlgButtonChecked(HWND dlg, int id)
{
    HWND b = GetDlgItem(dlg, id);
    return b ? (int)SendMessage(b, BM_GETCHECK, 0, 0) : 0;
}

bool SetDlgItemText(HWND dlg, int id, const char *text)
{
    HWND c = GetDlgItem(dlg, id);
    return c && SetWindowText(c, text);
}

int GetDlgItemText(HWND dlg, int id, char *buf, int max)
{
    HWND c = GetDlgItem(dlg, id);
    return c ? GetWindowText(c, buf, max) : 0;
}

/* ---- dialog templates ------------------------------------------------------ */

/* A dialog made from a DLGTEMPLATE: a window of the "#32770" class whose
 * procedure gives each message to the program's DLGPROC first (nonzero:
 * dealt with) and otherwise does the usual: WM_CLOSE is Cancel, and OK or
 * Cancel that the DLGPROC lets through end the dialog. DialogBox runs a
 * modal loop until EndDialog; CreateDialog's dialog is modeless (the
 * program passes it to IsDialogMessage). Coordinates are pixels, the
 * items' in the client area; CW_USEDEFAULT centres the dialog on its
 * owner. */
struct dlgdata { DLGPROC proc; int result; bool ended, modal; LPARAM init; };

static struct dlgdata *dd(struct wnd *w) { return w && w->cls->proc == dialog_proc ? w->ctl : NULL; }

LRESULT DefDlgProc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        SendMessage(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
            EndDialog(h, LOWORD(wp));
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

LRESULT dialog_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    struct wnd *w = wnd_from(h);
    struct dlgdata *d = w->ctl;
    if (msg == WM_CREATE || !d)
        return msg == WM_CREATE ? 0 : DefWindowProc(h, msg, wp, lp);
    if (msg == WM_DESTROY) {
        if (d->proc) d->proc(h, msg, wp, lp);
        LocalFree(d);
        w->ctl = NULL;
        return 0;
    }
    if (d->proc && d->proc(h, msg, wp, lp))
        return 0;
    return wnd_from(h) ? DefDlgProc(h, msg, wp, lp) : 0;
}

static HWND make_dialog(const DLGTEMPLATE *t, HWND owner, DLGPROC proc, LPARAM init, bool modal)
{
    if (!t)
        return NULL;
    struct dlgdata *d = LocalAlloc(LMEM_ZEROINIT, sizeof *d);
    if (!d)
        return NULL;
    *d = (struct dlgdata){ proc, 0, false, modal, init };
    struct wnd *ow = wnd_top(wnd_from(owner));
    unsigned style = t->style ? t->style : WS_DIALOG;
    bool child = (style & WS_CHILD) != 0;
    int ow_ = t->w, oh = t->h, x = t->x, y = t->y;
    if (!child && (style & WS_CAPTION)) {           /* the template gives the client size */
        ow_ += 2 * FRAME;
        oh += CLIENT_TOP + FRAME;
    }
    if (!child && x == CW_USEDEFAULT) {
        RECT area = S->work;
        if (ow && ow->visible && ow->state != WS_STATE_MIN)
            frame_outer(ow, &area);
        x = clampi(area.left + (area.right - area.left - ow_) / 2, 0, S->scr_w - ow_);
        y = clampi(area.top + (area.bottom - area.top - oh) / 3, 0, S->scr_h - oh);
    }
    HWND h = CreateWindowEx(t->exstyle, "#32770", t->title ? t->title : "", style & ~WS_VISIBLE,
                            x, y, ow_, oh, child ? owner : (HWND)ow, child ? t->id : 0, NULL);
    struct wnd *w = wnd_from(h);
    if (!w) {
        LocalFree(d);
        return NULL;
    }
    w->ctl = d;
    for (int i = 0; i < t->nitems; i++) {
        const DLGITEM *it = &t->items[i];
        CreateWindowEx(it->exstyle, it->cls, it->text ? it->text : "", it->style | WS_CHILD | WS_VISIBLE,
                       it->x, it->y, it->w, it->h, h, it->id, NULL);
    }
    HWND first = NULL;
    for (struct wnd *c = w->first; c; c = c->next)
        if (c->visible && c->enabled && (c->style & WS_TABSTOP)) { first = c; break; }
    LRESULT r = proc ? proc(h, WM_INITDIALOG, (WPARAM)first, init) : 1;
    if (!wnd_from(h))
        return NULL;
    if (style & WS_VISIBLE || modal)
        ShowWindow(h, SW_SHOW);
    if (r && first) {
        SetFocus(first);
        if (((struct wnd *)first)->cls->proc == edit_proc)
            SendMessage(first, EM_SETSEL, 0, -1);
    }
    return h;
}

HWND CreateDialog(const DLGTEMPLATE *t, HWND owner, DLGPROC proc, LPARAM init)
{
    return make_dialog(t, owner, proc, init, false);
}

bool EndDialog(HWND h, int result)
{
    struct dlgdata *d = dd(wnd_from(h));
    if (!d)
        return false;
    d->result = result;
    d->ended = true;
    if (!d->modal)
        DestroyWindow(h);
    return true;
}

int DialogBox(const DLGTEMPLATE *t, HWND owner, DLGPROC proc, LPARAM init)
{
    struct wnd *ow = wnd_top(wnd_from(owner));
    bool reenable = ow && ow->enabled;
    if (ow)
        EnableWindow(ow, false);
    HWND h = make_dialog(t, ow, proc, init, true);
    int result = -1;
    if (h) {
        MSG m;
        for (;;) {
            struct dlgdata *d = dd(wnd_from(h));
            if (!d) break;
            if (d->ended) { result = d->result; break; }
            if (!GetMessage(&m)) {
                PostQuitMessage((int)m.wParam);
                break;
            }
            if (!IsDialogMessage(h, &m)) {
                TranslateMessage(&m);
                DispatchMessage(&m);
            }
        }
    }
    if (reenable && wnd_from(ow))
        EnableWindow(ow, true);
    if (h && wnd_from(h))
        DestroyWindow(h);
    if (reenable && wnd_from(ow))
        SetActiveWindow(ow);
    return result;
}

LRESULT SendDlgItemMessage(HWND dlg, int id, unsigned msg, WPARAM wp, LPARAM lp)
{
    HWND c = GetDlgItem(dlg, id);
    return c ? SendMessage(c, msg, wp, lp) : 0;
}

bool SetDlgItemInt(HWND dlg, int id, int value, bool is_signed)
{
    char buf[16], *p = buf + sizeof buf;
    unsigned u = is_signed && value < 0 ? 0u - (unsigned)value : (unsigned)value;
    *--p = '\0';
    do *--p = (char)('0' + u % 10); while (u /= 10);
    if (is_signed && value < 0) *--p = '-';
    return SetDlgItemText(dlg, id, p);
}

int GetDlgItemInt(HWND dlg, int id, bool *ok, bool is_signed)
{
    char buf[24];
    GetDlgItemText(dlg, id, buf, sizeof buf);
    const char *s = buf;
    while (*s == ' ') s++;
    bool neg = is_signed && *s == '-';
    if (neg) s++;
    int v = 0, n = 0;
    for (; *s >= '0' && *s <= '9'; s++, n++)
        v = v * 10 + (*s - '0');
    while (*s == ' ') s++;
    if (ok) *ok = n > 0 && !*s;
    return neg ? -v : v;
}
