/*
 * findrepl.c -- the Find and Replace dialogs (SHELL.DLL).
 *
 * Modeless, owned by the program's window, as Win32's FindText and
 * ReplaceText: each button copies what was typed and ticked into the
 * caller's FINDREPLACE and sends the owner WM_FINDMSG; the owner does the
 * searching (only it knows its text). Closing sends FR_DIALOGTERM.
 */
#include "os1993.h"

enum { ID_WHAT = 100, ID_WITH, ID_CASE, ID_WORD, ID_UP, ID_DOWN,
       ID_NEXT, ID_REPLACE, ID_ALL };

static FINDREPLACE *fr_of(HWND h) { return (FINDREPLACE *)(uintptr_t)GetWindowLong(h, GWL_USERDATA); }

static HWND add(HWND h, const char *cls, const char *text, unsigned style,
                int x, int y, int w, int hh, int id)
{
    return CreateWindow(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hh, h, id, NULL);
}

static void report(HWND h, unsigned what)
{
    FINDREPLACE *fr = fr_of(h);
    GetDlgItemText(h, ID_WHAT, fr->find, (int)fr->max);
    if (fr->replace && GetDlgItem(h, ID_WITH))
        GetDlgItemText(h, ID_WITH, fr->replace, (int)fr->max);
    bool down = GetDlgItem(h, ID_DOWN) ? IsDlgButtonChecked(h, ID_DOWN) : true;
    fr->flags = what | (down ? FR_DOWN : 0) |
                (IsDlgButtonChecked(h, ID_CASE) ? FR_MATCHCASE : 0) |
                (IsDlgButtonChecked(h, ID_WORD) ? FR_WHOLEWORD : 0);
    if (what != FR_DIALOGTERM && !fr->find[0])
        return;                                         /* nothing to look for */
    SendMessage(fr->owner, WM_FINDMSG, 0, (LPARAM)fr);
}

static LRESULT dlg_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        FINDREPLACE *fr = (FINDREPLACE *)lp;
        bool rep = GetWindowLong(h, GWL_ID) == 1;
        SetWindowLong(h, GWL_USERDATA, (int32_t)lp);
        add(h, WC_STATIC, "Find what:", 0, 10, 12, 104, 16, 0);
        add(h, WC_EDIT, fr->find, WS_TABSTOP, 112, 5, 128, 28, ID_WHAT);
        int y = 40;
        if (rep) {
            add(h, WC_STATIC, "Replace with:", 0, 10, 44, 104, 16, 0);
            add(h, WC_EDIT, fr->replace ? fr->replace : "", WS_TABSTOP, 112, 37, 128, 28, ID_WITH);
            y = 76;
        }
        add(h, WC_BUTTON, "Match case", BS_AUTOCHECKBOX | WS_TABSTOP, 10, y, 130, 20, ID_CASE);
        add(h, WC_BUTTON, "Whole words", BS_AUTOCHECKBOX | WS_TABSTOP, 10, y + 22, 130, 20, ID_WORD);
        if (!rep) {
            add(h, WC_BUTTON, "Direction", BS_GROUPBOX, 136, 38, 104, 48, 0);
            add(h, WC_BUTTON, "Up", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 144, 58, 44, 20, ID_UP);
            add(h, WC_BUTTON, "Down", BS_AUTORADIOBUTTON, 188, 58, 50, 20, ID_DOWN);
            CheckDlgButton(h, (fr->flags & FR_DOWN) || !(fr->flags & ~0u) ? ID_DOWN : ID_UP, BST_CHECKED);
        }
        if (fr->flags & FR_MATCHCASE) CheckDlgButton(h, ID_CASE, BST_CHECKED);
        if (fr->flags & FR_WHOLEWORD) CheckDlgButton(h, ID_WORD, BST_CHECKED);
        add(h, WC_BUTTON, "Find Next", BS_DEFPUSHBUTTON | WS_TABSTOP, 248, 3, 96, 30, ID_NEXT);
        int by = 40;
        if (rep) {
            add(h, WC_BUTTON, "Replace", WS_TABSTOP, 252, by, 88, 22, ID_REPLACE);
            add(h, WC_BUTTON, "Replace All", WS_TABSTOP, 252, by + 26, 88, 22, ID_ALL);
            by += 52;
        }
        add(h, WC_BUTTON, "Cancel", WS_TABSTOP, 252, by, 88, 22, IDCANCEL);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_NEXT:    report(h, FR_FINDNEXT); break;
        case ID_REPLACE: report(h, FR_REPLACE); break;
        case ID_ALL:     report(h, FR_REPLACEALL); break;
        case IDCANCEL:   SendMessage(h, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_CLOSE:
        report(h, FR_DIALOGTERM);
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static HWND open(FINDREPLACE *fr, bool replace)
{
    static bool registered;
    if (!registered) {
        WNDCLASS wc = { .name = "#FINDDLG", .proc = dlg_proc, .background = C_FACE };
        RegisterClass(&wc);
        registered = true;
    }
    if (!fr || !fr->find || !fr->max || !IsWindow(fr->owner))
        return NULL;
    RECT o;
    GetWindowRect(fr->owner, &o);
    int w = 350 + 10, h = (replace ? 124 : 94) + 30;
    int x = o.left + (o.right - o.left - w) / 2, y = o.top + 40;
    if (x < 0) x = 0;
    if (x + w > 640) x = 640 - w;
    if (y + h > 452) y = 452 - h;
    /* the id says which dialog to build (WM_CREATE reads it) */
    HWND d = CreateWindow("#FINDDLG", replace ? "Replace" : "Find", WS_CAPTION | WS_SYSMENU,
                          x, y, w, h, fr->owner, replace ? 1 : 0, fr);
    if (!d)
        return NULL;
    ShowWindow(d, SW_SHOW);
    HWND what = GetDlgItem(d, ID_WHAT);
    SetFocus(what);
    SendMessage(what, EM_SETSEL, 0, -1);
    return d;
}

HWND FindText(FINDREPLACE *fr)    { return open(fr, false); }
HWND ReplaceText(FINDREPLACE *fr) { return open(fr, true); }
