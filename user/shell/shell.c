/*
 * shell.c -- SHELL.DLL, the shell API (the job SHELL32 does in Windows):
 * starting programs, the About box, and tray icons.
 *
 * Tray icons belong to the taskbar, which lives in the shell's process: a
 * program's Shell_NotifyIcon reaches it as WM_COPYDATA (the data is copied
 * across address spaces) addressed to the "Shell_TrayWnd" window.
 */
#include "os1993.h"
#include "shellpriv.h"

static bool ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), t = strlen(tail);
    if (n < t)
        return false;
    for (size_t i = 0; i < t; i++) {
        char a = s[n - t + i], b = tail[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != b)
            return false;
    }
    return true;
}

int ShellExecute(HWND hwnd, const char *verb, const char *file, const char *params,
                 const char *dir, int show)
{
    (void)hwnd; (void)dir; (void)show;
    if (verb && strcmp(verb, "open"))
        return 31;                                  /* SE_ERR_NOASSOC */
    if (!file)
        return 2;
    if (ends_with(file, "COMMAND.COM"))
        return StartDosSession() >= 0 ? 33 : 8;     /* 8: out of memory */
    if (!ends_with(file, ".BIN"))
        return 31;
    int pid = CreateProcess(file, params ? params : "");
    return pid > 0 ? 32 + pid : 2;                  /* 2: file not found */
}

int ShellAbout(HWND hwnd, const char *app, const char *other)
{
    char title[64] = "About ", text[256];
    size_t n = strlen(title);
    strncpy0(title + n, app ? app : "OS1993", sizeof title - n);
    strncpy0(text, app ? app : "OS1993", sizeof text);
    n = strlen(text);
    strncpy0(text + n, "\nOS1993 Virtual Machine Manager 0.1\n"
                       "A 386 enhanced mode VMM with a ring-3\nwindow system and shell.", sizeof text - n);
    if (other) {
        n = strlen(text);
        strncpy0(text + n, "\n\n", sizeof text - n);
        n = strlen(text);
        strncpy0(text + n, other, sizeof text - n);
    }
    return MessageBox(hwnd, text, title, MB_OK | MB_ICONINFORMATION);
}

bool Shell_NotifyIcon(unsigned msg, const NOTIFYICONDATA *data)
{
    HWND tray = FindWindow(TRAY_CLASS, NULL);
    if (!tray || !data || msg > NIM_DELETE)
        return false;
    struct tray_msg tm = { msg, *data };
    COPYDATASTRUCT cds = { TRAY_COPYDATA, sizeof tm, &tm };
    return SendMessage(tray, WM_COPYDATA, (WPARAM)data->hWnd, (LPARAM)&cds) != 0;
}
