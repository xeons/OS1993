/*
 * user/apps/notepad.c -- NOTEPAD.BIN, a text editor.
 *
 * A multi-line EDIT control filling the window, with File (New, Open,
 * Save, Save As), Edit (Undo, Cut, Copy, Paste, Delete, Select All,
 * Time/Date) and Help menus. Files are plain text with CR LF line ends
 * (DOS's), up to 32 KB; the control keeps LF only, so CRs go on the way
 * out. NOTEPAD.BIN name.txt opens a file at start.
 */
#include "os1993.h"

enum { ID_EDIT = 1, ID_NEW = 100, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_EXIT, ID_UNDO, ID_CUT, ID_COPY,
       ID_PASTE, ID_DELETE, ID_SELALL, ID_TIMEDATE, ID_ABOUT };

#define MAX_FILE 32767

static HWND np, edit;
static HMENU edit_menu;
static char file_path[128];

static const char *base_name(const char *path)
{
    const char *b = path;
    for (; *path; path++)
        if (*path == '\\' || *path == ':') b = path + 1;
    return b;
}

static void set_title(void)
{
    char title[64];
    strncpy0(title, file_path[0] ? base_name(file_path) : "Untitled", sizeof title);
    strncpy0(title + strlen(title), " - Notepad", sizeof title - strlen(title));
    SetWindowText(np, title);
}

static void complain(const char *what, const char *path)
{
    char text[160];
    strncpy0(text, what, sizeof text);
    strncpy0(text + strlen(text), path, sizeof text - strlen(text));
    MessageBox(np, text, "Notepad", MB_OK | MB_ICONERROR);
}

/* ---- files ------------------------------------------------------------------- */

static bool load(const char *path)
{
    HANDLE f = CreateFile(path, GENERIC_READ, OPEN_EXISTING);
    if (f == INVALID_HANDLE_VALUE) {
        complain("Cannot open ", path);
        return false;
    }
    uint32_t size = GetFileSize(f), got = 0;
    if (size > MAX_FILE) {
        CloseHandle(f);
        complain("This file is too large for Notepad (32 KB at most):\n", path);
        return false;
    }
    char *text = LocalAlloc(0, size + 1);
    bool ok = text && ReadFile(f, text, size, &got) && got == size;
    CloseHandle(f);
    if (!ok) {
        LocalFree(text);
        complain("Cannot read ", path);
        return false;
    }
    for (uint32_t i = 0; i < size; i++)             /* NUL, ^Z: end of the text */
        if (!text[i] || text[i] == 0x1A) { size = i; break; }
    text[size] = '\0';
    SetWindowText(edit, text);                      /* the control drops the CRs */
    LocalFree(text);
    strncpy0(file_path, path, sizeof file_path);
    set_title();
    DebugLog("notepad opened %s (%d lines)", path, (int)SendMessage(edit, EM_GETLINECOUNT, 0, 0));
    return true;
}

static bool save(const char *path)
{
    int len = (int)SendMessage(edit, WM_GETTEXTLENGTH, 0, 0), lines = 0;
    char *text = LocalAlloc(0, (uint32_t)len + 1);
    if (!text)
        return false;
    GetWindowText(edit, text, len + 1);
    for (int i = 0; i < len; i++)
        if (text[i] == '\n') lines++;
    char *out = LocalAlloc(0, (uint32_t)(len + lines + 1));
    int n = 0;
    for (int i = 0; out && i < len; i++) {          /* LF -> CR LF */
        if (text[i] == '\n') out[n++] = '\r';
        out[n++] = text[i];
    }
    LocalFree(text);
    if (!out)
        return false;
    HANDLE f = CreateFile(path, GENERIC_WRITE, CREATE_ALWAYS);
    uint32_t done = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, out, (uint32_t)n, &done) && done == (uint32_t)n;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    LocalFree(out);
    if (!ok) {
        complain("Cannot save ", path);
        return false;
    }
    strncpy0(file_path, path, sizeof file_path);
    SendMessage(edit, EM_SETMODIFY, 0, 0);
    set_title();
    DebugLog("notepad saved %s (%d bytes)", path, n);
    return true;
}

static bool save_as(void)
{
    char path[128];
    strncpy0(path, file_path[0] ? file_path : "untitled.txt", sizeof path);
    OPENFILENAME ofn = { np, "*.TXT", path, sizeof path, NULL, "TXT", OFN_OVERWRITEPROMPT };
    return GetSaveFileName(&ofn) && save(path);
}

static bool save_current(void) { return file_path[0] ? save(file_path) : save_as(); }

static bool may_discard(void)
{
    if (!SendMessage(edit, EM_GETMODIFY, 0, 0))
        return true;
    char text[96] = "The text in the ";
    strncpy0(text + strlen(text), file_path[0] ? base_name(file_path) : "Untitled",
             sizeof text - strlen(text));
    strncpy0(text + strlen(text), " file has changed.\nDo you want to save the changes?",
             sizeof text - strlen(text));
    int r = MessageBox(np, text, "Notepad", MB_YESNOCANCEL | MB_ICONWARNING);
    return r == IDNO || (r == IDYES && save_current());
}

/* ---- commands ------------------------------------------------------------------ */

static void time_date(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[32];
    int h = t.hour % 12 ? t.hour % 12 : 12, k = 0;
    if (h >= 10) s[k++] = '1';
    s[k++] = (char)('0' + h % 10);
    s[k++] = ':';
    s[k++] = (char)('0' + t.minute / 10);
    s[k++] = (char)('0' + t.minute % 10);
    s[k++] = ' ';
    s[k++] = t.hour < 12 ? 'A' : 'P';
    s[k++] = 'M';
    s[k++] = ' ';
    if (t.month >= 10) s[k++] = '1';
    s[k++] = (char)('0' + t.month % 10);
    s[k++] = '/';
    if (t.day >= 10) s[k++] = (char)('0' + t.day / 10);
    s[k++] = (char)('0' + t.day % 10);
    s[k++] = '/';
    s[k++] = (char)('0' + t.year / 1000);
    s[k++] = (char)('0' + t.year / 100 % 10);
    s[k++] = (char)('0' + t.year / 10 % 10);
    s[k++] = (char)('0' + t.year % 10);
    s[k] = '\0';
    SendMessage(edit, EM_REPLACESEL, 1, (LPARAM)s);
}

static void command(int id)
{
    switch (id) {
    case ID_NEW:
        if (may_discard()) {
            SetWindowText(edit, "");
            file_path[0] = '\0';
            set_title();
            DebugLog("notepad new");
        }
        break;
    case ID_OPEN: {
        if (!may_discard())
            break;
        char path[128] = "";
        OPENFILENAME ofn = { np, "*.TXT", path, sizeof path, NULL, "TXT", OFN_FILEMUSTEXIST };
        if (GetOpenFileName(&ofn))
            load(path);
        break;
    }
    case ID_SAVE:     save_current(); break;
    case ID_SAVEAS:   save_as(); break;
    case ID_EXIT:     SendMessage(np, WM_CLOSE, 0, 0); break;
    case ID_UNDO:     SendMessage(edit, WM_UNDO, 0, 0); break;
    case ID_CUT:      SendMessage(edit, WM_CUT, 0, 0); break;
    case ID_COPY:     SendMessage(edit, WM_COPY, 0, 0); break;
    case ID_PASTE:    SendMessage(edit, WM_PASTE, 0, 0); break;
    case ID_DELETE:   SendMessage(edit, WM_CLEAR, 0, 0); break;
    case ID_SELALL:   SendMessage(edit, EM_SETSEL, 0, -1); break;
    case ID_TIMEDATE: time_date(); break;
    case ID_ABOUT:    ShellAbout(np, "Notepad", NULL); break;
    }
}

static LRESULT np_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        edit = CreateWindow(WC_EDIT, "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                            ES_MULTILINE, 0, 0, 100, 100, h, ID_EDIT, NULL);
        return edit ? 0 : -1;
    case WM_SIZE:
        MoveWindow(edit, 0, 0, LOWORD(lp), HIWORD(lp), true);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            SetFocus(edit);
        return 0;
    case WM_INITMENUPOPUP:
        if ((HMENU)wp == edit_menu) {               /* grey what can't be done */
            int a, z;
            SendMessage(edit, EM_GETSEL, (WPARAM)&a, (LPARAM)&z);
            unsigned sel = z > a ? MF_ENABLED : MF_GRAYED;
            EnableMenuItem(edit_menu, ID_UNDO, SendMessage(edit, EM_CANUNDO, 0, 0) ? MF_ENABLED : MF_GRAYED);
            EnableMenuItem(edit_menu, ID_CUT, sel);
            EnableMenuItem(edit_menu, ID_COPY, sel);
            EnableMenuItem(edit_menu, ID_DELETE, sel);
            EnableMenuItem(edit_menu, ID_PASTE, GetClipboardTextLength() ? MF_ENABLED : MF_GRAYED);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) != ID_EDIT)
            command(LOWORD(wp));
        return 0;
    case WM_CLOSE:
        if (may_discard())
            PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

int main(void)
{
    InitUI();
    WNDCLASS wc = { .name = "NotepadWindow", .proc = np_proc, .background = C_FACE, .icon = ICON_NOTEPAD };
    RegisterClass(&wc);
    HMENU file = CreatePopupMenu(), help = CreatePopupMenu(), bar = CreateMenu();
    edit_menu = CreatePopupMenu();
    AppendMenu(file, MF_STRING, ID_NEW, "&New\tCtrl+N");
    AppendMenu(file, MF_STRING, ID_OPEN, "&Open...\tCtrl+O");
    AppendMenu(file, MF_STRING, ID_SAVE, "&Save\tCtrl+S");
    AppendMenu(file, MF_STRING, ID_SAVEAS, "Save &As...");
    AppendMenu(file, MF_SEPARATOR, 0, NULL);
    AppendMenu(file, MF_STRING, ID_EXIT, "E&xit");
    AppendMenu(edit_menu, MF_STRING, ID_UNDO, "&Undo\tCtrl+Z");
    AppendMenu(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(edit_menu, MF_STRING, ID_CUT, "Cu&t\tCtrl+X");
    AppendMenu(edit_menu, MF_STRING, ID_COPY, "&Copy\tCtrl+C");
    AppendMenu(edit_menu, MF_STRING, ID_PASTE, "&Paste\tCtrl+V");
    AppendMenu(edit_menu, MF_STRING, ID_DELETE, "De&lete\tDel");
    AppendMenu(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(edit_menu, MF_STRING, ID_SELALL, "Select &All\tCtrl+A");
    AppendMenu(edit_menu, MF_STRING, ID_TIMEDATE, "Time/&Date\tF5");
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About Notepad");
    AppendMenu(bar, MF_POPUP, (uintptr_t)file, "&File");
    AppendMenu(bar, MF_POPUP, (uintptr_t)edit_menu, "&Edit");
    AppendMenu(bar, MF_POPUP, (uintptr_t)help, "&Help");
    np = CreateWindow("NotepadWindow", "Untitled - Notepad", WS_OVERLAPPEDWINDOW, 70, 40, 480, 340,
                      NULL, 0, NULL);
    SetMenu(np, bar);
    ShowWindow(np, SW_SHOW);
    SetFocus(edit);
    static const ACCEL keys[] = {
        { FVIRTKEY | FCONTROL, 'N', ID_NEW }, { FVIRTKEY | FCONTROL, 'O', ID_OPEN },
        { FVIRTKEY | FCONTROL, 'S', ID_SAVE }, { FVIRTKEY, VK_F1 + 4, ID_TIMEDATE },
    };
    HACCEL acc = CreateAcceleratorTable(keys, 4);
    const char *arg = GetStartupInfo()->args;
    if (arg[0])
        load(arg);
    DebugLog("notepad ready");

    MSG msg;
    while (GetMessage(&msg)) {
        if (TranslateAccelerator(np, acc, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
