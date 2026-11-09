/*
 * dialog.c -- keyboard navigation in dialog-like windows, and helpers.
 *
 * IsDialogMessage, called from the message loop, gives a window the dialog
 * keys: Tab / Shift+Tab move the focus through WS_TABSTOP controls, Enter
 * presses the focused push button or else the default one, Esc sends
 * WM_COMMAND IDCANCEL.
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

bool IsDialogMessage(HWND dlg, const MSG *m)
{
    struct wnd *d = wnd_from(dlg), *target = wnd_from(m->hwnd);
    if (!d || !target || !is_descendant(target, d))
        return false;
    if (m->message == WM_CHAR && (m->wParam == 9 || m->wParam == 13 || m->wParam == 27))
        return true;                            /* consumed as dialog keys */
    if (m->message != WM_KEYDOWN)
        return false;

    switch (m->wParam) {
    case VK_TAB: {
        struct wnd *list[WND_MAX];
        int n = tabstops(d, list, 0, WND_MAX);
        if (!n)
            return true;
        struct wnd *f = wnd_from(GetFocus());
        int i = 0;
        while (i < n && list[i] != f)
            i++;
        int dir = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
        i = i == n ? 0 : (i + dir + n) % n;
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
