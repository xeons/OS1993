/*
 * filedlg.c -- the Open and Save As dialogs (SHELL.DLL).
 *
 * The current folder, a file name field, and one list of its subfolders
 * ("[NAME]", "[..]" to go up) and the files matching the filter. A click on
 * a file copies its name to the field; a double-click opens a folder or
 * picks a file; typing a folder's name or a pattern ("*.*") in the field
 * goes there. Save As asks before replacing a file, Open checks that it
 * exists. Modal to the owner, as a MessageBox is. The dialog moves the
 * process's current directory (as Windows does without OFN_NOCHANGEDIR).
 */
#include "os1993.h"

#define ID_NAME   100
#define ID_LIST   101
#define ID_FOLDER 102
#define ITEMS     96
#define NAME_LEN  64                            /* long names, cut at 61 */

struct dlg {
    OPENFILENAME *ofn;
    bool          save;
    char          filter[16];
    bool          done, ok;
};

static struct dlg *dlg_of(HWND h) { return (struct dlg *)(uintptr_t)GetWindowLong(h, GWL_USERDATA); }

static bool wild(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return true;
    return false;
}

static int compare(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y || !x) return x - y;
    }
}

static void fill(HWND h)
{
    struct dlg *d = dlg_of(h);
    char cwd[MAX_PATH];
    GetCurrentDirectory(sizeof cwd, cwd);
    SetDlgItemText(h, ID_FOLDER, cwd);
    DebugLog("folder %s", cwd);
    HWND list = GetDlgItem(h, ID_LIST);
    SendMessage(list, LB_RESETCONTENT, 0, 0);

    static char names[ITEMS][NAME_LEN];
    int n = 0, ndirs = 0;
    WIN32_FIND_DATA fd;
    for (int pass = 0; pass < 2; pass++) {              /* folders, then files */
        HANDLE f = FindFirstFile(pass ? d->filter : "*.*", &fd);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        do {
            bool dir = fd.attrib & FILE_ATTRIBUTE_DIRECTORY;
            if (dir != !pass || !strcmp(fd.name, ".") || n == ITEMS)
                continue;
            if (dir && !strcmp(fd.name, "..") && strlen(cwd) <= 3)
                continue;
            int at = pass ? n : ndirs, start = pass ? ndirs : 0;  /* sorted insert */
            while (at > start && compare(names[at - 1], fd.name) > 0) {
                strncpy0(names[at], names[at - 1], NAME_LEN);
                at--;
            }
            if (pass) {
                strncpy0(names[at], fd.name, NAME_LEN);
            } else {
                names[at][0] = '[';
                strncpy0(names[at] + 1, fd.name, NAME_LEN - 2);
                size_t k = strlen(names[at]);
                names[at][k] = ']';
                names[at][k + 1] = '\0';
                ndirs++;
            }
            n++;
        } while (FindNextFile(f, &fd));
        FindClose(f);
    }
    for (int i = 0; i < n; i++)
        SendMessage(list, LB_ADDSTRING, 0, (LPARAM)names[i]);
}

/* Go into a folder; -> false if it isn't one */
static bool go(HWND h, const char *name)
{
    uint32_t attr = GetFileAttributes(name);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        !SetCurrentDirectory(name))
        return false;
    fill(h);
    return true;
}

static void accept(HWND h)
{
    struct dlg *d = dlg_of(h);
    char name[64];
    GetDlgItemText(h, ID_NAME, name, sizeof name);
    if (!name[0])
        return;
    if (wild(name)) {                                   /* a new filter */
        strncpy0(d->filter, name, sizeof d->filter);
        SetDlgItemText(h, ID_NAME, "");
        fill(h);
        return;
    }
    if (go(h, name)) {
        SetDlgItemText(h, ID_NAME, "");
        return;
    }
    bool dot = false;
    for (char *c = name; *c; c++)
        if (*c == '.') dot = true; else if (*c == '\\') dot = false;
    if (!dot && d->ofn->defext) {
        size_t n = strlen(name);
        if (n + 1 + strlen(d->ofn->defext) < sizeof name) {
            name[n] = '.';
            strncpy0(name + n + 1, d->ofn->defext, sizeof name - n - 1);
        }
    }
    bool exists = GetFileAttributes(name) != INVALID_FILE_ATTRIBUTES;
    char text[128];
    if (!d->save && (d->ofn->flags & OFN_FILEMUSTEXIST) && !exists) {
        strncpy0(text, name, sizeof text);
        strncpy0(text + strlen(text), "\nFile not found. Check the name and try again.",
                 sizeof text - strlen(text));
        MessageBox(h, text, d->ofn->title ? d->ofn->title : "Open", MB_OK | MB_ICONWARNING);
        return;
    }
    if (d->save && (d->ofn->flags & OFN_OVERWRITEPROMPT) && exists) {
        strncpy0(text, name, sizeof text);
        strncpy0(text + strlen(text), " already exists.\nDo you want to replace it?",
                 sizeof text - strlen(text));
        if (MessageBox(h, text, d->ofn->title ? d->ofn->title : "Save As",
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    char path[128];                                     /* the full path */
    if (name[1] == ':') {
        strncpy0(path, name, sizeof path);
    } else {
        GetCurrentDirectory(sizeof path, path);
        size_t n = strlen(path);
        if (path[n - 1] != '\\' && n + 1 < sizeof path) path[n++] = '\\';
        strncpy0(path + n, name[0] == '\\' ? name + 1 : name, sizeof path - n);
    }
    for (char *c = path; *c; c++)                       /* DOS names: upper case */
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 32);
    strncpy0(d->ofn->file, path, d->ofn->max);
    d->ok = d->done = true;
}

static LRESULT dlg_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        struct dlg *d = (struct dlg *)lp;
        SetWindowLong(h, GWL_USERDATA, (int32_t)lp);
        CreateWindow(WC_STATIC, "Folder:", WS_CHILD | WS_VISIBLE, 12, 10, 64, 16, h, 0, NULL);
        CreateWindow(WC_STATIC, "", WS_CHILD | WS_VISIBLE, 76, 10, 220, 16, h, ID_FOLDER, NULL);
        CreateWindow(WC_STATIC, "File name:", WS_CHILD | WS_VISIBLE, 12, 38, 80, 16, h, 0, NULL);
        const char *base = d->ofn->file;                /* just the name part */
        for (const char *c = d->ofn->file; *c; c++)
            if (*c == '\\' || *c == ':') base = c + 1;
        CreateWindow(WC_EDIT, base, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 94, 31, 198, 28, h, ID_NAME, NULL);
        CreateWindow(WC_LISTBOX, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY,
                     9, 64, 286, 150, h, ID_LIST, NULL);
        CreateWindow(WC_BUTTON, d->save ? "Save" : "Open", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                     BS_DEFPUSHBUTTON, 122, 222, 84, 30, h, IDOK, NULL);
        CreateWindow(WC_BUTTON, "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                     216, 226, 76, 22, h, IDCANCEL, NULL);
        return 0;
    }
    case WM_COMMAND: {
        struct dlg *d = dlg_of(h);
        switch (LOWORD(wp)) {
        case IDOK:
            accept(h);
            break;
        case IDCANCEL:
            d->done = true;
            break;
        case ID_LIST: {
            HWND list = (HWND)lp;
            char item[NAME_LEN];
            if (SendMessage(list, LB_GETTEXT, (WPARAM)SendMessage(list, LB_GETCURSEL, 0, 0),
                            (LPARAM)item) == LB_ERR)
                break;
            bool dir = item[0] == '[';
            if (dir) {
                item[strlen(item) - 1] = '\0';
                if (HIWORD(wp) == LBN_DBLCLK)
                    go(h, item + 1);
            } else {
                SetDlgItemText(h, ID_NAME, item);
                if (HIWORD(wp) == LBN_DBLCLK)
                    accept(h);
            }
            break;
        }
        }
        return 0;
    }
    case WM_CLOSE:
        dlg_of(h)->done = true;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static bool run(OPENFILENAME *ofn, bool save)
{
    static bool registered;
    if (!registered) {
        WNDCLASS wc = { .name = "#FILEDLG", .proc = dlg_proc, .background = C_FACE };
        RegisterClass(&wc);
        registered = true;
    }
    if (!ofn || !ofn->file || !ofn->max)
        return false;
    struct dlg d = { .ofn = ofn, .save = save };
    strncpy0(d.filter, ofn->filter ? ofn->filter : "*.*", sizeof d.filter);

    HWND owner = ofn->owner;
    while (owner && GetParent(owner))
        owner = GetParent(owner);
    RECT o, work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    o = work;
    if (owner)
        GetWindowRect(owner, &o);
    int w = 304 + 10, hh = 262 + 25 + 5;
    int x = o.left + (o.right - o.left - w) / 2, y = o.top + (o.bottom - o.top - hh) / 3;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > work.right) x = work.right - w;
    if (y + hh > work.bottom) y = work.bottom - hh;

    bool reenable = owner && IsWindowEnabled(owner);
    if (owner)
        EnableWindow(owner, false);
    const char *title = ofn->title ? ofn->title : save ? "Save As" : "Open";
    HWND h = CreateWindow("#FILEDLG", title, WS_DIALOG | WS_VISIBLE, x, y, w, hh, owner, 0, &d);
    if (h) {
        fill(h);
        SetFocus(GetDlgItem(h, ID_NAME));
        SendMessage(GetDlgItem(h, ID_NAME), EM_SETSEL, 0, -1);
        MSG m;
        while (!d.done && IsWindow(h)) {
            if (!GetMessage(&m)) {
                PostQuitMessage((int)m.wParam);
                break;
            }
            if (!IsDialogMessage(h, &m)) {
                TranslateMessage(&m);
                DispatchMessage(&m);
            }
        }
        if (IsWindow(h)) {
            if (reenable) EnableWindow(owner, true);
            DestroyWindow(h);
        }
    }
    if (reenable && IsWindow(owner)) {
        EnableWindow(owner, true);
        SetActiveWindow(owner);
    }
    return d.ok;
}

bool GetOpenFileName(OPENFILENAME *ofn) { return run(ofn, false); }
bool GetSaveFileName(OPENFILENAME *ofn) { return run(ofn, true); }
