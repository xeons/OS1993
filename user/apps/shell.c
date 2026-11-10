/*
 * user/apps/shell.c -- SHELL.BIN, the desktop and the taskbar.
 *
 * The first process of a GUI session (the VMM's G key starts it); when it
 * ends, the session ends. It provides, in the style of Windows 95:
 *   the desktop   icons that start programs (double-click, or Enter)
 *   the taskbar   a Start button with the Start menu (Ctrl+Esc), a button
 *                 per program window (click to switch, again to minimize),
 *                 and the notification area: tray icons and a clock
 * Programs run as processes of their own, started with ShellExecute.
 */
#include "os1993.h"
#include "shellpriv.h"

#define BAR_H       28
#define START_W     64
#define TASK_MAX    16
#define TRAY_MAX    8
#define CELL_W      88
#define CELL_H      62
#define SCR_W()     GetSystemMetrics(SM_CXSCREEN)
#define SCR_H()     GetSystemMetrics(SM_CYSCREEN)

static HWND desktop, taskbar;

/* ---- programs -------------------------------------------------------------- */

struct program { const char *name, *file; int icon; };

static const struct program programs[] = {
    { "&Calculator",    "CALC.BIN",    ICON_CALC },
    { "&Paint",         "PAINT.BIN",   ICON_PAINT },
    { "C&lock",         "CLOCK.BIN",   ICON_CLOCK },
    { "&Notepad",       "NOTEPAD.BIN", ICON_NOTEPAD },
    { "Control &Demo",  "DEMO.BIN",    ICON_DEMO },
    { "&MS-DOS Prompt", "COMMAND.COM", ICON_DOS },
};
#define NPROGRAMS (int)(sizeof programs / sizeof programs[0])

static void plain_name(const char *s, char *out, int max)    /* without '&' */
{
    int n = 0;
    for (; *s && n < max - 1; s++)
        if (*s != '&')
            out[n++] = *s;
    out[n] = '\0';
}

static void launch(const char *file, const char *name)
{
    char plain[32];
    plain_name(name, plain, sizeof plain);
    DebugLog("launch %s", plain);
    if (ShellExecute(desktop, "open", file, NULL, NULL, SW_SHOW) <= 32) {
        char text[96] = "Cannot start ";
        strncpy0(text + strlen(text), plain, sizeof text - strlen(text));
        MessageBox(desktop, text, "Shell", MB_OK | MB_ICONERROR);
    }
}

/* ---- the Run dialog ------------------------------------------------------------- */

static HWND run_box;

static LRESULT run_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindow(WC_STATIC, "Type the name of a program (CALC.BIN,", WS_CHILD | WS_VISIBLE,
                     12, 10, 320, 16, h, 0, NULL);
        CreateWindow(WC_STATIC, "COMMAND.COM ...) and OS1993 will open it.", WS_CHILD | WS_VISIBLE,
                     12, 26, 330, 16, h, 0, NULL);
        CreateWindow(WC_STATIC, "Open:", WS_CHILD | WS_VISIBLE, 12, 56, 48, 16, h, 0, NULL);
        CreateWindow(WC_EDIT, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 60, 49, 270, 28, h, 100, NULL);
        CreateWindow(WC_BUTTON, "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                     160, 88, 84, 30, h, IDOK, NULL);
        CreateWindow(WC_BUTTON, "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                     254, 92, 76, 22, h, IDCANCEL, NULL);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            char file[64];
            GetDlgItemText(h, 100, file, sizeof file);
            if (file[0]) {
                DestroyWindow(h);
                launch(file, file);
            }
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        run_box = NULL;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void open_run(void)
{
    if (run_box) {
        SetForegroundWindow(run_box);
        return;
    }
    run_box = CreateWindow("ShellRun", "Run", WS_DIALOG | WS_VISIBLE, 140, 160, 352, 158,
                           NULL, 0, NULL);
    SetFocus(GetDlgItem(run_box, 100));
}

/* ---- Close Program (Ctrl+Alt+Del) ------------------------------------------------ */

#define ID_TASKS    300
#define ID_ENDTASK  301
#define ID_SHUTDN   302

static HWND task_box;
static int task_pids[PROC_MAX];
static char task_names[PROC_MAX][16];
static int ntask_pids;

static void shut_down(void);

struct find_title { int pid; char *out; };

static bool title_of(HWND h, LPARAM l)                  /* a process's main window */
{
    struct find_title *f = (struct find_title *)l;
    unsigned style = (unsigned)GetWindowLong(h, GWL_STYLE);
    if (GetWindowProcessId(h) != f->pid || !IsWindowVisible(h) || GetParent(h) ||
        (style & WS_CAPTION) != WS_CAPTION)
        return true;
    GetWindowText(h, f->out, 40);
    return false;
}

static void fill_tasks(void)
{
    HWND list = GetDlgItem(task_box, ID_TASKS);
    SendMessage(list, LB_RESETCONTENT, 0, 0);
    PROCESSENTRY p[PROC_MAX];
    ntask_pids = EnumProcesses(p, PROC_MAX);
    for (int i = 0; i < ntask_pids; i++) {
        char text[48] = "";
        task_pids[i] = (int)p[i].pid;
        strncpy0(task_names[i], p[i].name, sizeof task_names[i]);
        if (p[i].flags & PROC_LEADER) {
            strncpy0(text, "Shell", sizeof text);
        } else {
            struct find_title f = { (int)p[i].pid, text };
            EnumWindows(title_of, (LPARAM)&f);
            if (!text[0])
                strncpy0(text, p[i].name, sizeof text);
        }
        SendMessage(list, LB_ADDSTRING, 0, (LPARAM)text);
        DebugLog("task %s: %s", p[i].name, text);
    }
    SendMessage(list, LB_SETCURSEL, 0, 0);
}

static LRESULT task_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindow(WC_LISTBOX, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY,
                     9, 8, 302, 118, h, ID_TASKS, NULL);
        CreateWindow(WC_STATIC, "Ending a program loses its unsaved work.", WS_CHILD | WS_VISIBLE,
                     12, 132, 300, 16, h, 0, NULL);
        CreateWindow(WC_BUTTON, "End Task", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                     8, 156, 92, 30, h, ID_ENDTASK, NULL);
        CreateWindow(WC_BUTTON, "Shut Down", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                     114, 160, 92, 22, h, ID_SHUTDN, NULL);
        CreateWindow(WC_BUTTON, "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                     220, 160, 92, 22, h, IDCANCEL, NULL);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ENDTASK: {
            int i = (int)SendMessage(GetDlgItem(h, ID_TASKS), LB_GETCURSEL, 0, 0);
            if (i < 0 || i >= ntask_pids)
                break;
            if (task_pids[i] == GetCurrentProcessId()) {   /* the shell itself */
                DestroyWindow(h);
                shut_down();
                break;
            }
            DebugLog("end task %s", task_names[i]);
            TerminateProcess(task_pids[i]);
            SetTimer(h, 1, 300);                        /* the list, once it's gone */
            break;
        }
        case ID_SHUTDN:
            DestroyWindow(h);
            shut_down();
            break;
        case IDCANCEL:
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_TIMER:
        KillTimer(h, 1);
        fill_tasks();
        return 0;
    case WM_DESTROY:
        task_box = NULL;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void close_program(void)
{
    TaskListShown();                                    /* the VMM needn't step in */
    if (!task_box) {
        task_box = CreateWindowEx(WS_EX_TOPMOST, "ShellTasks", "Close Program", WS_DIALOG,
                                  155, 110, 330, 222, NULL, 0, NULL);
        fill_tasks();
        ShowWindow(task_box, SW_SHOW);
    }
    SetForegroundWindow(task_box);
    SetFocus(GetDlgItem(task_box, ID_TASKS));
    DebugLog("close program");
}

/* ---- Display Properties ------------------------------------------------------------ */

#define ID_RES      400
#define ID_DEPTH    401
#define ID_KEEPTXT  410

static HWND display_box;
static DEVMODE res_list[16];                /* the sizes offered ... */
static int nres;
static int depth_list[8];                   /* ... and the depths */
static int ndepth;

static const char *depth_name(int bpp)
{
    switch (bpp) {
    case 4:  return "16 colors";
    case 8:  return "256 colors";
    case 16: return "High Color (16 bit)";
    case 32: return "True Color (32 bit)";
    }
    return "?";
}

static bool mode_exists(int w, int h, int bpp)
{
    DEVMODE dm;
    for (int i = 0; EnumDisplaySettings(i, &dm); i++)
        if ((int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h && (int)dm.dmBitsPerPel == bpp)
            return true;
    return false;
}

static void put_int(char *out, int v)
{
    char d[12];
    int n = 0, k = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[k++] = d[--n];
    out[k] = '\0';
}

/* OS1993.INI keeps the mode for the next session. */
static void save_mode(const DEVMODE *dm)
{
    char text[96] = "[display]\r\nwidth=", num[12];
    put_int(num, (int)dm->dmPelsWidth);  strncpy0(text + strlen(text), num, sizeof text - strlen(text));
    strncpy0(text + strlen(text), "\r\nheight=", sizeof text - strlen(text));
    put_int(num, (int)dm->dmPelsHeight); strncpy0(text + strlen(text), num, sizeof text - strlen(text));
    strncpy0(text + strlen(text), "\r\nbits=", sizeof text - strlen(text));
    put_int(num, (int)dm->dmBitsPerPel); strncpy0(text + strlen(text), num, sizeof text - strlen(text));
    strncpy0(text + strlen(text), "\r\n", sizeof text - strlen(text));
    HANDLE f = CreateFile("OS1993.INI", GENERIC_WRITE, CREATE_ALWAYS);
    if (f != INVALID_HANDLE_VALUE) {
        uint32_t done;
        WriteFile(f, text, (uint32_t)strlen(text), &done);
        CloseHandle(f);
    }
}

static bool load_mode(DEVMODE *dm)
{
    char text[256];
    uint32_t got = 0;
    HANDLE f = CreateFile("OS1993.INI", GENERIC_READ, OPEN_EXISTING);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    ReadFile(f, text, sizeof text - 1, &got);
    CloseHandle(f);
    text[got] = '\0';
    *dm = (DEVMODE){ 0, 0, 0 };
    for (char *p = text; *p; ) {
        char *key = p;
        while (*p && *p != '=' && *p != '\n') p++;
        if (*p != '=') { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        *p++ = '\0';
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0');
        if (!strcmp(key, "width"))  dm->dmPelsWidth = v;
        if (!strcmp(key, "height")) dm->dmPelsHeight = v;
        if (!strcmp(key, "bits"))   dm->dmBitsPerPel = v;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return dm->dmPelsWidth && dm->dmPelsHeight && dm->dmBitsPerPel;
}

/* "Keep this setting?": answered, or 15 seconds and it goes back. */
static int keep_left;
static bool keep_done;
static int keep_answer;

static void keep_text(HWND h)
{
    char text[64] = "Reverting in ", num[12];
    put_int(num, keep_left);
    strncpy0(text + strlen(text), num, sizeof text - strlen(text));
    strncpy0(text + strlen(text), " seconds.", sizeof text - strlen(text));
    SetDlgItemText(h, ID_KEEPTXT, text);
}

static LRESULT keep_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindow(WC_STATIC, "Do you want to keep this setting?", WS_CHILD | WS_VISIBLE,
                     12, 12, 290, 16, h, 0, NULL);
        CreateWindow(WC_STATIC, "", WS_CHILD | WS_VISIBLE, 12, 32, 290, 16, h, ID_KEEPTXT, NULL);
        CreateWindow(WC_BUTTON, "Yes", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                     118, 58, 84, 30, h, IDYES, NULL);
        CreateWindow(WC_BUTTON, "No", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 212, 62, 76, 22, h, IDNO, NULL);
        SetTimer(h, 1, 1000);
        return 0;
    case WM_TIMER:
        if (--keep_left <= 0) {
            keep_answer = IDNO;
            keep_done = true;
        } else {
            keep_text(h);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDYES || LOWORD(wp) == IDNO || LOWORD(wp) == IDCANCEL) {
            keep_answer = LOWORD(wp) == IDYES ? IDYES : IDNO;
            keep_done = true;
        }
        return 0;
    case WM_CLOSE:
        keep_answer = IDNO;
        keep_done = true;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static bool keep_setting(void)
{
    keep_left = 15;
    keep_done = false;
    keep_answer = IDNO;
    int w = 320, hh = 130;
    HWND h = CreateWindowEx(WS_EX_TOPMOST, "ShellKeep", "Display Properties", WS_CAPTION | WS_VISIBLE,
                            (SCR_W() - w) / 2, (SCR_H() - hh) / 3, w, hh, NULL, 0, NULL);
    keep_text(h);
    SetFocus(GetDlgItem(h, IDYES));
    MSG m;
    while (!keep_done && GetMessage(&m)) {
        if (!IsDialogMessage(h, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    DestroyWindow(h);
    return keep_answer == IDYES;
}

static void apply_display(int w, int h, int bpp)
{
    DEVMODE old, dm = { (uint32_t)w, (uint32_t)h, (uint32_t)bpp };
    EnumDisplaySettings(ENUM_CURRENT_SETTINGS, &old);
    if (old.dmPelsWidth == dm.dmPelsWidth && old.dmPelsHeight == dm.dmPelsHeight &&
        old.dmBitsPerPel == dm.dmBitsPerPel)
        return;
    DebugLog("display settings %dx%dx%d", w, h, bpp);
    if (ChangeDisplaySettings(&dm, 0) != DISP_CHANGE_SUCCESSFUL) {
        MessageBox(desktop, "The display could not be switched to that mode.", "Display Properties",
                   MB_OK | MB_ICONERROR);
        return;
    }
    if (keep_setting()) {
        save_mode(&dm);
        DebugLog("display kept");
    } else {
        ChangeDisplaySettings(&old, 0);
        DebugLog("display reverted");
    }
}

static LRESULT display_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CreateWindow(WC_STATIC, "Screen area:", WS_CHILD | WS_VISIBLE, 12, 12, 200, 16, h, 0, NULL);
        HWND res = CreateWindow(WC_COMBOBOX, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                12, 30, 236, 24, h, ID_RES, NULL);
        CreateWindow(WC_STATIC, "Colors:", WS_CHILD | WS_VISIBLE, 12, 66, 200, 16, h, 0, NULL);
        HWND dep = CreateWindow(WC_COMBOBOX, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                12, 84, 236, 24, h, ID_DEPTH, NULL);
        CreateWindow(WC_BUTTON, "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                     70, 124, 84, 30, h, IDOK, NULL);
        CreateWindow(WC_BUTTON, "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 164, 128, 84, 22, h, IDCANCEL, NULL);
        DEVMODE cur, dm;
        EnumDisplaySettings(ENUM_CURRENT_SETTINGS, &cur);
        nres = ndepth = 0;
        for (int i = 0; EnumDisplaySettings(i, &dm); i++) {
            bool have = false;
            for (int k = 0; k < nres; k++)
                if (res_list[k].dmPelsWidth == dm.dmPelsWidth && res_list[k].dmPelsHeight == dm.dmPelsHeight)
                    have = true;
            if (!have && nres < 16)
                res_list[nres++] = dm;
            have = false;
            for (int k = 0; k < ndepth; k++)
                if (depth_list[k] == (int)dm.dmBitsPerPel) have = true;
            if (!have && ndepth < 8) {                  /* in order */
                int at = ndepth++;
                while (at > 0 && depth_list[at - 1] > (int)dm.dmBitsPerPel) {
                    depth_list[at] = depth_list[at - 1];
                    at--;
                }
                depth_list[at] = (int)dm.dmBitsPerPel;
            }
        }
        for (int i = 0; i < nres; i++) {
            char text[32], num[12];
            put_int(text, (int)res_list[i].dmPelsWidth);
            strncpy0(text + strlen(text), " by ", sizeof text - strlen(text));
            put_int(num, (int)res_list[i].dmPelsHeight);
            strncpy0(text + strlen(text), num, sizeof text - strlen(text));
            strncpy0(text + strlen(text), " pixels", sizeof text - strlen(text));
            SendMessage(res, CB_ADDSTRING, 0, (LPARAM)text);
            if (res_list[i].dmPelsWidth == cur.dmPelsWidth && res_list[i].dmPelsHeight == cur.dmPelsHeight)
                SendMessage(res, CB_SETCURSEL, (WPARAM)i, 0);
        }
        for (int i = 0; i < ndepth; i++) {
            SendMessage(dep, CB_ADDSTRING, 0, (LPARAM)depth_name(depth_list[i]));
            if (depth_list[i] == (int)cur.dmBitsPerPel)
                SendMessage(dep, CB_SETCURSEL, (WPARAM)i, 0);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int r = (int)SendMessage(GetDlgItem(h, ID_RES), CB_GETCURSEL, 0, 0);
            int d = (int)SendMessage(GetDlgItem(h, ID_DEPTH), CB_GETCURSEL, 0, 0);
            if (r < 0 || d < 0)
                break;
            int w = (int)res_list[r].dmPelsWidth, hh = (int)res_list[r].dmPelsHeight, bpp = depth_list[d];
            if (!mode_exists(w, hh, bpp)) {
                MessageBox(h, "This display can't show that many colors at that size.\n"
                           "Choose another combination.", "Display Properties", MB_OK | MB_ICONINFORMATION);
                break;
            }
            DestroyWindow(h);
            apply_display(w, hh, bpp);
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(h);
        }
        return 0;
    case WM_DESTROY:
        display_box = NULL;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static void open_display(void)
{
    if (display_box) {
        SetForegroundWindow(display_box);
        return;
    }
    int w = 270, hh = 194;
    display_box = CreateWindow("ShellDisplay", "Display Properties", WS_DIALOG | WS_VISIBLE,
                               (SCR_W() - w) / 2, (SCR_H() - hh) / 3, w, hh, NULL, 0, NULL);
    SetFocus(GetDlgItem(display_box, ID_RES));
    DebugLog("display properties");
}

/* The desktop and taskbar fit the screen; the rest of it is the work area. */
static void layout(void)
{
    int w = SCR_W(), h = SCR_H();
    MoveWindow(desktop, 0, 0, w, h, true);
    MoveWindow(taskbar, 0, h - BAR_H, w, BAR_H, true);
    RECT work = { 0, 0, w, h - BAR_H };
    SystemParametersInfo(SPI_SETWORKAREA, 0, &work, 0);
    InvalidateRect(desktop, NULL);
    InvalidateRect(taskbar, NULL);
}

/* ---- the Start menu ------------------------------------------------------------- */

enum { ID_PROGRAM = 100, ID_RUN = 200, ID_ABOUT, ID_SHUTDOWN, ID_DISPLAY };

static bool start_open;

static void shut_down(void)
{
    if (MessageBox(desktop, "Shut down OS1993 and return to the VMM?\nPrograms that are "
                   "still running will be closed.", "Shut Down OS1993",
                   MB_YESNO | MB_ICONQUESTION) == IDYES) {
        DebugLog("shut down");
        PostQuitMessage(0);
    }
}

static void start_menu(void)
{
    HMENU progs = CreatePopupMenu(), menu = CreatePopupMenu();
    for (int i = 0; i < NPROGRAMS; i++)
        AppendMenu(progs, MF_STRING, ID_PROGRAM + i, programs[i].name);
    AppendMenu(menu, MF_POPUP, (uintptr_t)progs, "&Programs");
    AppendMenu(menu, MF_STRING, ID_RUN, "&Run...");
    AppendMenu(menu, MF_STRING, ID_DISPLAY, "&Display Settings...");
    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(menu, MF_STRING, ID_ABOUT, "&About OS1993...");
    AppendMenu(menu, MF_SEPARATOR, 0, NULL);
    AppendMenu(menu, MF_STRING, ID_SHUTDOWN, "Sh&ut Down...");
    start_open = true;
    InvalidateRect(taskbar, NULL);
    int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_BOTTOMALIGN, 2, SCR_H() - BAR_H + 1, taskbar);
    DestroyMenu(menu);
    start_open = false;
    InvalidateRect(taskbar, NULL);
    if (id >= ID_PROGRAM && id < ID_PROGRAM + NPROGRAMS)
        launch(programs[id - ID_PROGRAM].file, programs[id - ID_PROGRAM].name);
    else if (id == ID_RUN)
        open_run();
    else if (id == ID_DISPLAY)
        open_display();
    else if (id == ID_ABOUT)
        ShellAbout(desktop, "OS1993", "Programs run as separate processes,\neach in its own address space.");
    else if (id == ID_SHUTDOWN)
        shut_down();
}

/* ---- the taskbar ------------------------------------------------------------------ */

static HWND tasks[TASK_MAX];
static int ntasks;

struct tray_icon { HWND owner; unsigned id, msg; int icon; char tip[64]; };
static struct tray_icon tray[TRAY_MAX];
static int ntray;
static int clock_minute = -1;

static bool is_task(HWND h)
{
    if (!IsWindow(h) || !IsWindowVisible(h) || GetParent(h) || h == desktop || h == taskbar)
        return false;
    unsigned style = (unsigned)GetWindowLong(h, GWL_STYLE), ex = (unsigned)GetWindowLong(h, GWL_EXSTYLE);
    return (style & WS_CAPTION) == WS_CAPTION && !(ex & WS_EX_TOOLWINDOW);
}

static void task_add(HWND h)
{
    for (int i = 0; i < ntasks; i++)
        if (tasks[i] == h)
            return;
    if (ntasks < TASK_MAX && is_task(h))
        tasks[ntasks++] = h;
}

static void task_remove(HWND h)
{
    for (int i = 0; i < ntasks; i++)
        if (tasks[i] == h) {
            for (int k = i; k < ntasks - 1; k++)
                tasks[k] = tasks[k + 1];
            ntasks--;
            return;
        }
}

static bool collect(HWND h, LPARAM l)
{
    (void)l;
    task_add(h);
    return true;
}

static void tasks_refresh(void)
{
    for (int i = ntasks - 1; i >= 0; i--)
        if (!is_task(tasks[i]))
            task_remove(tasks[i]);
}

static int tray_width(void) { return 8 + ntray * 20 + 64 + 8; }
static int tray_left(void)  { return SCR_W() - 4 - tray_width(); }

static RECT task_rect(int i)
{
    int left = START_W + 8, right = tray_left() - 6;
    int w = ntasks ? (right - left) / ntasks : 0;
    if (w > 160) w = 160;
    int x = left + i * w;
    return (RECT){ x, 3, x + w - 3, BAR_H - 3 };
}

static void bevel_box(HDC dc, RECT r, bool down)
{
    RECT in = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
    FrameRect(dc, &r, C_GRAY2);
    FillRect(dc, &in, down ? C_GRAY5 : C_FACE);
    DrawBevel(dc, &in, !down);
}

static void format_time(char *out)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    int h = t.hour % 12 ? t.hour % 12 : 12;
    out[0] = h >= 10 ? '1' : ' ';
    out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + t.minute / 10);
    out[4] = (char)('0' + t.minute % 10);
    out[5] = ' ';
    out[6] = t.hour < 12 ? 'A' : 'P';
    out[7] = 'M';
    out[8] = '\0';
    clock_minute = t.minute;
}

static void taskbar_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT all = { 0, 0, SCR_W(), BAR_H };
    FillRect(dc, &all, C_FACE);
    DrawLine(dc, 0, 0, SCR_W(), false, C_GRAY4);
    DrawLine(dc, 0, 1, SCR_W(), false, C_WHITE);

    RECT sb = { 2, 3, START_W, BAR_H - 3 };             /* Start */
    bevel_box(dc, sb, start_open);
    DrawSmallIcon(dc, sb.left + 5 + start_open, sb.top + 3 + start_open, ICON_COMPUTER);
    TextOut(dc, sb.left + 23 + start_open, sb.top + 3 + start_open, "Start", C_BLACK, -1);

    HWND fg = GetForegroundWindow();
    for (int i = 0; i < ntasks; i++) {                  /* a button per program window */
        RECT r = task_rect(i);
        bool down = tasks[i] == fg && !IsIconic(tasks[i]);
        bevel_box(dc, r, down);
        DrawSmallIcon(dc, r.left + 4 + down, r.top + 3 + down, GetClassIcon(tasks[i]));
        char title[64];
        GetWindowText(tasks[i], title, sizeof title);
        int room = (r.right - r.left - 28) / 8;
        if (room < 0) room = 0;
        if ((int)strlen(title) > room) title[room] = '\0';
        TextOut(dc, r.left + 23 + down, r.top + 3 + down, title, C_BLACK, -1);
    }

    RECT tr = { tray_left(), 3, SCR_W() - 4, BAR_H - 3 };   /* the notification area */
    DrawBevel(dc, &tr, false);
    for (int i = 0; i < ntray; i++)
        DrawSmallIcon(dc, tr.left + 6 + i * 20, tr.top + 3, tray[i].icon);
    char clock[12];
    format_time(clock);
    TextOut(dc, tr.right - 8 - 64, tr.top + 3, clock, C_BLACK, -1);
    EndPaint(h, &ps);
}

static void tray_message(const struct tray_msg *m)
{
    int i = 0;
    while (i < ntray && !(tray[i].owner == m->nid.hWnd && tray[i].id == m->nid.uID))
        i++;
    if (m->msg == NIM_DELETE) {
        if (i < ntray) {
            for (; i < ntray - 1; i++)
                tray[i] = tray[i + 1];
            ntray--;
        }
    } else {
        if (i == ntray) {
            if (m->msg != NIM_ADD || ntray == TRAY_MAX)
                return;
            ntray++;
            tray[i] = (struct tray_icon){ m->nid.hWnd, m->nid.uID, 0, ICON_APP, "" };
        }
        if (m->nid.uFlags & NIF_MESSAGE) tray[i].msg = m->nid.uCallbackMessage;
        if (m->nid.uFlags & NIF_ICON)    tray[i].icon = m->nid.hIcon;
        if (m->nid.uFlags & NIF_TIP)     strncpy0(tray[i].tip, m->nid.szTip, sizeof tray[i].tip);
    }
    InvalidateRect(taskbar, NULL);
}

static void taskbar_click(int x, int y, unsigned mouse_msg)
{
    if (x < START_W && mouse_msg == WM_LBUTTONDOWN) {
        start_menu();
        return;
    }
    for (int i = 0; i < ntasks; i++) {
        RECT r = task_rect(i);
        if (PtInRect(&r, x, y) && mouse_msg == WM_LBUTTONDOWN) {
            HWND t = tasks[i];
            if (t == GetForegroundWindow() && !IsIconic(t))
                ShowWindow(t, SW_MINIMIZE);
            else
                SetForegroundWindow(t);
            InvalidateRect(taskbar, NULL);
            return;
        }
    }
    int tl = tray_left();
    for (int i = 0; i < ntray; i++) {
        int ix = tl + 6 + i * 20;
        if (x >= ix && x < ix + 18 && tray[i].msg && IsWindow(tray[i].owner)) {
            PostMessage(tray[i].owner, tray[i].msg, tray[i].id, (LPARAM)mouse_msg);
            return;
        }
    }
    if (x >= SCR_W() - 4 - 8 - 64 - 4 && mouse_msg == WM_LBUTTONDBLCLK)
        launch("CLOCK.BIN", "Clock");
}

static LRESULT taskbar_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(h, 1, 1000);
        return 0;
    case WM_PAINT:
        taskbar_paint(h);
        return 0;
    case WM_SHELLHOOK: {
        if (wp == HSHELL_TASKMAN) {
            close_program();
            return 0;
        }
        HWND w = (HWND)lp;
        if (wp == HSHELL_WINDOWCREATED) task_add(w);
        if (wp == HSHELL_WINDOWDESTROYED) task_remove(w);
        tasks_refresh();
        InvalidateRect(h, NULL);
        return 0;
    }
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == TRAY_COPYDATA && cds->cbData >= sizeof(struct tray_msg)) {
            tray_message((const struct tray_msg *)cds->lpData);
            return 1;
        }
        return 0;
    }
    case WM_TIMER: {
        SYSTEMTIME t;
        GetLocalTime(&t);
        if (t.minute != clock_minute) {
            RECT r = { tray_left(), 0, SCR_W(), BAR_H };
            InvalidateRect(h, &r);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONUP:
        taskbar_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), msg);
        return 0;
    case WM_DISPLAYCHANGE:
        layout();
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_TASKLIST) {
            start_menu();
            return 0;
        }
        break;
    case WM_CLOSE:                                      /* not by Alt+F4 */
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---- the desktop -------------------------------------------------------------------- */

struct desk_icon { const char *label; const char *file; int icon; };

static const struct desk_icon icons[] = {
    { "My Computer",    NULL,          ICON_COMPUTER },
    { "Calculator",     "CALC.BIN",    ICON_CALC },
    { "Paint",          "PAINT.BIN",   ICON_PAINT },
    { "Clock",          "CLOCK.BIN",   ICON_CLOCK },
    { "Notepad",        "NOTEPAD.BIN", ICON_NOTEPAD },
    { "Control Demo",   "DEMO.BIN",    ICON_DEMO },
    { "MS-DOS Prompt",  "COMMAND.COM", ICON_DOS },
};
#define NICONS (int)(sizeof icons / sizeof icons[0])

static int selected = -1;

static RECT cell(int i) { return (RECT){ 4, 6 + i * CELL_H, 4 + CELL_W, 6 + (i + 1) * CELL_H }; }

static void open_icon(int i)
{
    if (!icons[i].file)
        ShellAbout(desktop, "My Computer", "The 386 enhanced mode VMM runs DOS\nsessions "
                   "and these ring-3 programs.");
    else
        launch(icons[i].file, icons[i].label);
}

static void label_lines(const char *s, char *a, char *b)    /* wrap at a space */
{
    int n = (int)strlen(s), split = -1;
    if (n * 8 > CELL_W - 4)
        for (int i = 0; i < n; i++)
            if (s[i] == ' ')
                split = i;
    if (split < 0) {
        strncpy0(a, s, 32);
        b[0] = '\0';
    } else {
        memcpy(a, s, (size_t)split);
        a[split] = '\0';
        strncpy0(b, s + split + 1, 32);
    }
}

static void desktop_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    bool focused = GetForegroundWindow() == h;
    DEVMODE mode;
    EnumDisplaySettings(ENUM_CURRENT_SETTINGS, &mode);
    if (mode.dmBitsPerPel >= 16) {                      /* real colours: a gradient */
        int hgt = SCR_H();
        for (int y = ps.rcPaint.top & ~3; y < ps.rcPaint.bottom; y += 4) {
            RECT band = { ps.rcPaint.left, y, ps.rcPaint.right, y + 4 };
            int k = y * 255 / (hgt ? hgt : 1);
            FillRectRGB(dc, &band, (uint8_t)(40 + k * 20 / 255), (uint8_t)(110 - k * 50 / 255),
                        (uint8_t)(170 - k * 90 / 255));
        }
    }
    for (int i = 0; i < NICONS; i++) {
        RECT c = cell(i);
        DrawIcon(dc, c.left + (CELL_W - 32) / 2, c.top + 2, icons[i].icon);
        char a[32], b[32];
        label_lines(icons[i].label, a, b);
        for (int line = 0; line < 2; line++) {
            const char *s = line ? b : a;
            if (!*s)
                continue;
            int w = TextWidth(s), x = c.left + (CELL_W - w) / 2, y = c.top + 36 + line * 14;
            bool sel = i == selected;
            if (sel) {
                RECT r = { x - 2, y, x + w + 2, y + 15 };
                FillRect(dc, &r, focused ? C_ACCENT : C_GRAY3);
            }
            TextOut(dc, x, y - 1, s, C_WHITE, -1);
        }
    }
    EndPaint(h, &ps);
}

static void select_icon(HWND h, int i)
{
    if (i == selected)
        return;
    if (selected >= 0) { RECT r = cell(selected); InvalidateRect(h, &r); }
    selected = i;
    if (selected >= 0) { RECT r = cell(selected); InvalidateRect(h, &r); }
}

static int icon_at(int x, int y)
{
    for (int i = 0; i < NICONS; i++) {
        RECT c = cell(i);
        if (PtInRect(&c, x, y))
            return i;
    }
    return -1;
}

static LRESULT desktop_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        desktop_paint(h);
        return 0;
    case WM_LBUTTONDOWN:
        select_icon(h, icon_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_LBUTTONDBLCLK: {
        int i = icon_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        select_icon(h, i);
        if (i >= 0)
            open_icon(i);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_UP || wp == VK_DOWN)
            select_icon(h, selected < 0 ? 0 : (selected + (wp == VK_DOWN ? 1 : NICONS - 1)) % NICONS);
        else if (wp == VK_RETURN && selected >= 0)
            open_icon(selected);
        return 0;
    case WM_ACTIVATE:
        if (selected >= 0) { RECT r = cell(selected); InvalidateRect(h, &r); }
        return 0;
    case WM_CONTEXTMENU: {
        HMENU m = CreatePopupMenu();
        AppendMenu(m, MF_STRING, ID_RUN, "&Run...");
        AppendMenu(m, MF_STRING, ID_ABOUT, "&About OS1993...");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
        AppendMenu(m, MF_STRING, ID_DISPLAY, "Propert&ies");
        int id = TrackPopupMenu(m, TPM_RETURNCMD, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), h);
        DestroyMenu(m);
        if (id == ID_RUN) open_run();
        if (id == ID_ABOUT) ShellAbout(h, "OS1993", NULL);
        if (id == ID_DISPLAY) open_display();
        return 0;
    }
    case WM_CLOSE:                                      /* Alt+F4 on the desktop */
        shut_down();
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---- main ------------------------------------------------------------------------------ */

int main(void)
{
    InitUI();
    WNDCLASS dc = { .name = DESKTOP_CLASS, .proc = desktop_proc, .style = CS_DBLCLKS,
                    .background = C_DESKTOP, .icon = ICON_COMPUTER };
    WNDCLASS tc = { .name = TRAY_CLASS, .proc = taskbar_proc, .style = CS_DBLCLKS,
                    .background = C_FACE };
    WNDCLASS rc = { .name = "ShellRun", .proc = run_proc, .background = C_FACE, .icon = ICON_APP };
    RegisterClass(&dc);
    RegisterClass(&tc);
    RegisterClass(&rc);
    WNDCLASS kc = { .name = "ShellTasks", .proc = task_proc, .background = C_FACE, .icon = ICON_APP };
    RegisterClass(&kc);
    WNDCLASS pc = { .name = "ShellDisplay", .proc = display_proc, .background = C_FACE, .icon = ICON_COMPUTER };
    RegisterClass(&pc);
    WNDCLASS qc = { .name = "ShellKeep", .proc = keep_proc, .background = C_FACE, .icon = ICON_COMPUTER };
    RegisterClass(&qc);
    DEVMODE saved;                                      /* the mode the last session kept */
    if (load_mode(&saved) && ChangeDisplaySettings(&saved, 0) == DISP_CHANGE_SUCCESSFUL)
        DebugLog("display %dx%dx%d from OS1993.INI", (int)saved.dmPelsWidth, (int)saved.dmPelsHeight,
                 (int)saved.dmBitsPerPel);

    desktop = CreateWindowEx(WS_EX_TOOLWINDOW, DESKTOP_CLASS, "Desktop", WS_POPUP,
                             0, 0, SCR_W(), SCR_H(), NULL, 0, NULL);
    SetShellWindow(desktop);
    ShowWindow(desktop, SW_SHOW);
    taskbar = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TRAY_CLASS, "",
                             WS_POPUP | WS_VISIBLE, 0, SCR_H() - BAR_H, SCR_W(), BAR_H, NULL, 0, NULL);
    RegisterShellHookWindow(taskbar);
    RECT work = { 0, 0, SCR_W(), SCR_H() - BAR_H };
    SystemParametersInfo(SPI_SETWORKAREA, 0, &work, 0);
    EnumWindows(collect, 0);

    MSG msg;
    PeekMessage(&msg, false);                           /* first paint */
    DebugLog("shell ready");
    while (GetMessage(&msg)) {
        if (run_box && IsDialogMessage(run_box, &msg))
            continue;
        if (task_box && IsDialogMessage(task_box, &msg))
            continue;
        if (display_box && IsDialogMessage(display_box, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
