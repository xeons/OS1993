/*
 * user/apps/gui.c -- GUI.BIN: the demo applications.
 * Windows, messages and controls come from UI.DLL; system services from
 * KERNEL.DLL. Written the way a Windows program is: register a class,
 * create windows and child controls, run a message loop.
 *
 * Three windows show the window manager and the controls:
 *   Programs       MS-DOS Prompt (a DOS session VM), About, Exit to VMM
 *   Controls       every control type, live
 *   About OS1993   a small dialog
 * With a self-test flag it instead checks that ring 3 is fenced in.
 */
#include "os1993.h"

#define BTN_H   22
#define EDIT_H  22
#define DEF     4               /* a default button's ring */
#define RING    3               /* an edit or list box's focus ring */

#define FRAMED  (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define CHILD   (WS_CHILD | WS_VISIBLE)

static HWND child(HWND parent, const char *cls, const char *text, unsigned style,
                  int x, int y, int w, int h, int id)
{
    return CreateWindow(cls, text, CHILD | style, x, y, w, h, parent, id, NULL);
}

/* ---- About -------------------------------------------------------------- */

static HWND about;

static LRESULT about_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        child(h, WC_STATIC, "OS1993 Virtual Machine Manager", 0, 16, 12, 260, 16, 0);
        child(h, WC_STATIC, "A 386 enhanced mode VMM with", 0, 16, 32, 260, 16, 0);
        child(h, WC_STATIC, "a ring-3 window manager.", 0, 16, 48, 260, 16, 0);
        child(h, WC_BUTTON, "OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
              200 - DEF, 76 - DEF, 80 + 2 * DEF, BTN_H + 2 * DEF, IDOK);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            if (LOWORD(wp) == IDOK)
                DebugLog("button About OK");
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        about = NULL;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void open_about(void)
{
    if (about)
        SetActiveWindow(about);
    else
        about = CreateWindow("About", "About OS1993", FRAMED | WS_VISIBLE,
                             40, 300, 310, 140, NULL, 0, NULL);
}

/* ---- Programs ---------------------------------------------------------- */

enum { ID_DOS = 10, ID_ABOUT, ID_EXIT, ID_CONTENTS };

static HMENU programs_menu(void)
{
    HMENU file = CreatePopupMenu(), help = CreatePopupMenu(), bar = CreateMenu();
    AppendMenu(file, MF_STRING, ID_DOS, "&MS-DOS Prompt");
    AppendMenu(file, MF_STRING, ID_ABOUT, "&About OS1993...");
    AppendMenu(file, MF_SEPARATOR, 0, NULL);
    AppendMenu(file, MF_STRING, ID_EXIT, "E&xit to VMM");
    AppendMenu(help, MF_STRING, ID_CONTENTS, "&Contents\tF1");
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About OS1993");
    AppendMenu(bar, MF_POPUP, (uintptr_t)file, "&File");
    AppendMenu(bar, MF_POPUP, (uintptr_t)help, "&Help");
    return bar;
}

static LRESULT programs_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        child(h, WC_STATIC, "Run a program:", 0, 12, 10, 176, 16, 0);
        child(h, WC_BUTTON, "MS-DOS Prompt", WS_TABSTOP, 12, 36, 176, BTN_H, ID_DOS);
        child(h, WC_BUTTON, "About OS1993", WS_TABSTOP, 12, 66, 176, BTN_H, ID_ABOUT);
        child(h, WC_BUTTON, "Exit to VMM", WS_TABSTOP, 12, 96, 176, BTN_H, ID_EXIT);
        return 0;
    case WM_COMMAND:
        if (!lp) {                      /* from the menu */
            static const char *const names[] = { "MS-DOS Prompt", "About OS1993", "Exit to VMM", "Contents" };
            DebugLog("menu %s", names[LOWORD(wp) - ID_DOS]);
        }
        switch (LOWORD(wp)) {
        case ID_DOS:
            DebugLog("button MS-DOS Prompt");
            StartDosSession();
            break;
        case ID_ABOUT:
            open_about();
            break;
        case ID_EXIT:
            DebugLog("button Exit to VMM");
            PostQuitMessage(0);
            break;
        }
        return 0;
    case WM_CLOSE:                      /* closing Programs ends the session */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---- Controls ----------------------------------------------------------- */

enum { ID_SOUND = 20, ID_CLOCK, ID_SLOW, ID_NORMAL, ID_TURBO, ID_NAME, ID_LIST,
       ID_VOLUME, ID_SCROLL, ID_PROGRESS, ID_TIMER, ID_RESET };

/* The right-click menu: a Speed submenu mirroring the radio buttons. */
static void context_menu(HWND h, int x, int y)
{
    HMENU speed = CreatePopupMenu(), menu = CreatePopupMenu();
    AppendMenu(speed, MF_STRING, ID_SLOW, "&Slow");
    AppendMenu(speed, MF_STRING, ID_NORMAL, "&Normal");
    AppendMenu(speed, MF_STRING, ID_TURBO, "&Turbo");
    int cur = IsDlgButtonChecked(h, ID_SLOW) ? ID_SLOW : IsDlgButtonChecked(h, ID_TURBO) ? ID_TURBO : ID_NORMAL;
    CheckMenuRadioItem(speed, ID_SLOW, ID_TURBO, (unsigned)cur, MF_BYCOMMAND);
    AppendMenu(menu, MF_POPUP, (uintptr_t)speed, "&Speed");
    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(menu, MF_STRING | (IsDlgButtonChecked(h, ID_SOUND) ? MF_CHECKED : 0), ID_SOUND, "Sound &effects");
    AppendMenu(menu, MF_STRING, ID_RESET, "&Reset volume");
    int id = TrackPopupMenu(menu, TPM_RETURNCMD, x, y, h);
    DestroyMenu(menu);
    switch (id) {
    case ID_SLOW: case ID_NORMAL: case ID_TURBO:
        SendMessage(GetDlgItem(h, id), BM_SETCHECK, BST_CHECKED, 0);
        DebugLog("speed %s", id == ID_SLOW ? "Slow" : id == ID_TURBO ? "Turbo" : "Normal");
        break;
    case ID_SOUND:
        SendMessage(GetDlgItem(h, ID_SOUND), BM_CLICK, 0, 0);
        break;
    case ID_RESET:
        SendMessage(GetDlgItem(h, ID_VOLUME), TBM_SETPOS, 1, 40);
        DebugLog("volume reset");
        break;
    }
}

static const char *const colours[] = {
    "Platinum", "Graphite", "Lavender", "Teal", "Ocean", "Forest",
    "Sunset", "Sand", "Midnight", "Rose", "Slate", "Copper",
};

static void controls_create(HWND h)
{
    child(h, WC_BUTTON, "Options", BS_GROUPBOX, 10, 6, 170, 76, 0);
    child(h, WC_BUTTON, "Sound effects", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 28, 150, 20, ID_SOUND);
    child(h, WC_BUTTON, "Show clock", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 52, 150, 20, ID_CLOCK);
    child(h, WC_BUTTON, "Speed", BS_GROUPBOX, 190, 6, 170, 100, 0);
    child(h, WC_BUTTON, "Slow", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 202, 28, 150, 20, ID_SLOW);
    child(h, WC_BUTTON, "Normal", BS_AUTORADIOBUTTON, 202, 52, 150, 20, ID_NORMAL);
    child(h, WC_BUTTON, "Turbo", BS_AUTORADIOBUTTON, 202, 76, 150, 20, ID_TURBO);
    child(h, WC_STATIC, "Name:", WS_GROUP, 12, 96, 48, 16, 0);
    child(h, WC_EDIT, "Hello", WS_TABSTOP, 64 - RING, 92 - RING, 116 + 2 * RING, EDIT_H + 2 * RING, ID_NAME);
    child(h, WC_STATIC, "Colour scheme:", 0, 12, 124, 168, 16, 0);
    HWND list = child(h, WC_LISTBOX, "", LBS_NOTIFY | WS_TABSTOP,
                      12 - RING, 144 - RING, 168 + 2 * RING, 100 + 2 * RING, ID_LIST);
    child(h, WC_STATIC, "Volume:", 0, 192, 124, 166, 16, 0);
    HWND vol = child(h, WC_TRACKBAR, "", WS_TABSTOP, 190, 144, 170, 26, ID_VOLUME);
    child(h, WC_STATIC, "Progress:", 0, 192, 180, 166, 16, 0);
    child(h, WC_PROGRESS, "", 0, 192, 200, 166, 16, ID_PROGRESS);
    HWND sb = child(h, WC_SCROLLBAR, "", SBS_HORZ | WS_TABSTOP, 192, 228, 166, 16, ID_SCROLL);
    child(h, WC_BUTTON, "OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
          196 - DEF, 262 - DEF, 76 + 2 * DEF, BTN_H + 2 * DEF, IDOK);
    child(h, WC_BUTTON, "Cancel", WS_TABSTOP, 282, 262, 76, BTN_H, IDCANCEL);

    CheckDlgButton(h, ID_CLOCK, BST_CHECKED);
    CheckDlgButton(h, ID_NORMAL, BST_CHECKED);
    for (unsigned i = 0; i < sizeof colours / sizeof colours[0]; i++)
        SendMessage(list, LB_ADDSTRING, 0, (LPARAM)colours[i]);
    SendMessage(list, LB_SETCURSEL, 0, 0);
    SendMessage(vol, TBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(vol, TBM_SETPOS, 1, 40);
    SendMessage(sb, SBM_SETRANGE, 0, 50);
    SendMessage(sb, SBM_SETPAGE, 10, 0);
    SendMessage(sb, SBM_SETPOS, 10, 0);
    SetTimer(h, ID_TIMER, 55);
}

static void controls_ok(HWND h)
{
    char name[64], colour[32];
    const char *speed = IsDlgButtonChecked(h, ID_SLOW) ? "Slow" :
                        IsDlgButtonChecked(h, ID_TURBO) ? "Turbo" : "Normal";
    GetDlgItemText(h, ID_NAME, name, sizeof name);
    HWND list = GetDlgItem(h, ID_LIST);
    colour[0] = '\0';
    SendMessage(list, LB_GETTEXT, (WPARAM)SendMessage(list, LB_GETCURSEL, 0, 0), (LPARAM)colour);
    DebugLog("OK: sound=%d clock=%d speed=%s name=%s colour=%s volume=%d",
             IsDlgButtonChecked(h, ID_SOUND), IsDlgButtonChecked(h, ID_CLOCK), speed, name,
             colour, (int)SendMessage(GetDlgItem(h, ID_VOLUME), TBM_GETPOS, 0, 0));
}

static LRESULT controls_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        controls_create(h);
        return 0;
    case WM_COMMAND: {
        HWND c = (HWND)lp;
        char text[32];
        switch (LOWORD(wp)) {
        case ID_SOUND: case ID_CLOCK:
            GetWindowText(c, text, sizeof text);
            DebugLog("check %s %s", text, SendMessage(c, BM_GETCHECK, 0, 0) ? "on" : "off");
            break;
        case ID_SLOW: case ID_NORMAL: case ID_TURBO:
            GetWindowText(c, text, sizeof text);
            DebugLog("radio %s", text);
            break;
        case ID_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                SendMessage(c, LB_GETTEXT, (WPARAM)SendMessage(c, LB_GETCURSEL, 0, 0), (LPARAM)text);
                DebugLog("list %s", text);
            }
            break;
        case IDOK:
            controls_ok(h);
            break;
        case IDCANCEL:
            if (lp)                     /* the button, not the Esc key */
                DebugLog("button Cancel");
            break;
        }
        return 0;
    }
    case WM_HSCROLL: {
        HWND c = (HWND)lp;
        int code = LOWORD(wp);
        if (GetDlgCtrlID(c) == ID_VOLUME) {
            if (code != TB_ENDTRACK)
                DebugLog("slider %d", (int)SendMessage(c, TBM_GETPOS, 0, 0));
            return 0;
        }
        int pos = (int)SendMessage(c, SBM_GETPOS, 0, 0);
        switch (code) {                 /* a scroll bar leaves moving to us */
        case SB_LINEUP:        pos -= 1; break;
        case SB_LINEDOWN:      pos += 1; break;
        case SB_PAGEUP:        pos -= 10; break;
        case SB_PAGEDOWN:      pos += 10; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = (int16_t)HIWORD(wp); break;
        default:               return 0;
        }
        int old = (int)SendMessage(c, SBM_SETPOS, (WPARAM)pos, 0);
        pos = (int)SendMessage(c, SBM_GETPOS, 0, 0);
        if (pos != old)
            DebugLog("scroll %d", pos);
        return 0;
    }
    case WM_CONTEXTMENU:
        context_menu(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_TIMER: {                    /* animate the progress bar */
        HWND p = GetDlgItem(h, ID_PROGRESS);
        SendMessage(p, PBM_SETPOS, (WPARAM)((SendMessage(p, PBM_GETPOS, 0, 0) + 1) % 101), 0);
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---- self-test ------------------------------------------------------------ */

static int selftest(const struct user_info *info)
{
    OutputDebugString("ring-3 self-test running");
    uint32_t t0 = GetTickCount();
    Sleep(110);
    if (GetTickCount() > t0)
        OutputDebugString("timer ticks advance");
    if (info->flags & UINFO_TEST_CLI)
        __asm__ volatile("cli");                            /* must #GP */
    if (info->flags & UINFO_TEST_KERNEL)
        *(volatile uint32_t *)0x80000000u = 0;              /* must #PF */
    OutputDebugString("privileged operation was allowed!");
    return 1;
}

/* ---- main ------------------------------------------------------------------- */

static void register_class(const char *name, WNDPROC proc, uint8_t background)
{
    WNDCLASS wc = { .name = name, .proc = proc, .background = background };
    RegisterClass(&wc);
}

int main(void)
{
    const struct user_info *info = GetStartupInfo();
    if (info->flags & (UINFO_TEST_CLI | UINFO_TEST_KERNEL))
        return selftest(info);

    InitUI();
    register_class("Programs", programs_proc, C_FACE);
    register_class("Controls", controls_proc, C_FACE);
    register_class("About", about_proc, C_FACE);

    HWND programs = CreateWindow("Programs", "Programs", FRAMED, 16, 16, 210, 181, NULL, 0, NULL);
    SetMenu(programs, programs_menu());
    ShowWindow(programs, SW_SHOW);
    open_about();
    HWND controls = CreateWindow("Controls", "Controls", FRAMED | WS_THICKFRAME | WS_VISIBLE,
                                 246, 40, 380, 330, NULL, 0, NULL);
    SetFocus(GetDlgItem(controls, ID_NAME));

    MSG msg;
    PeekMessage(&msg, false);                               /* first paint */
    OutputDebugString("desktop ready");
    if (HasMouse())
        OutputDebugString("mouse ready");

    while (GetMessage(&msg)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {  /* Esc leaves the GUI */
            PostQuitMessage(0);
            continue;
        }
        if (!IsDialogMessage(GetActiveWindow(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
