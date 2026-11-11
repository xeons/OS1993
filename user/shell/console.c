/*
 * console.c -- a console window for programs that print (PING ...).
 *
 * RunConsoleProgram (os1993.h) runs the program's real main on a second
 * thread and shows what it writes to stdout in a read-only edit control.
 * Only the first thread may use the window system, so the second one
 * leaves its text in a buffer (under a critical section) and the first
 * moves it into the window on a timer. When the program ends the window
 * stays, so its output can be read; closing it earlier ends the program.
 */
#include <stdio.h>
#include "shellpriv.h"

#define CON_CLASS   "ShellConsole"
#define PENDING     4096

struct start { int (*fn)(int, char **); int argc; char **argv; };

static CRITICAL_SECTION lock;
static char pending[PENDING];
static int npending;
static HWND con, edit;
static HANDLE worker;
static volatile int32_t finished;           /* 1: ended, 2: and said so */
static uint32_t exit_code;
static char title[64];

void ConsoleWrite(const char *s, size_t n)
{
    EnterCriticalSection(&lock);
    for (size_t i = 0; i < n && npending < PENDING - 2; i++) {
        if (s[i] == '\n')
            pending[npending++] = '\r';
        pending[npending++] = s[i];
    }
    LeaveCriticalSection(&lock);
}

static void append(const char *text)
{
    int len = (int)SendMessage(edit, WM_GETTEXTLENGTH, 0, 0);
    SendMessage(edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessage(edit, EM_REPLACESEL, 0, (LPARAM)text);
    SendMessage(edit, EM_SCROLLCARET, 0, 0);
}

static void drain(void)
{
    static char text[PENDING + 1];
    EnterCriticalSection(&lock);
    memcpy(text, pending, (size_t)npending);
    text[npending] = '\0';
    npending = 0;
    LeaveCriticalSection(&lock);
    if (text[0])
        append(text);
}

static uint32_t run(void *arg)
{
    struct start *s = arg;
    uint32_t code = (uint32_t)s->fn(s->argc, s->argv);
    exit_code = code;
    InterlockedExchange(&finished, 1);
    return code;
}

static LRESULT console_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        RECT r;
        GetClientRect(h, &r);
        edit = CreateWindow(WC_EDIT, "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                            0, 0, r.right, r.bottom, h, 1, NULL);
        SendMessage(edit, EM_LIMITTEXT, 30000, 0);
        SetTimer(h, 1, 100);
        return 0;
    }
    case WM_SIZE:
        MoveWindow(edit, 0, 0, LOWORD(lp), HIWORD(lp), true);
        return 0;
    case WM_SETFOCUS:
        SetFocus(edit);
        return 0;
    case WM_TIMER:
        drain();
        if (finished == 1) {
            char line[128], t[112];
            snprintf(line, sizeof line, "\r\n[%s has finished: exit code %ld]\r\n", title, (long)exit_code);
            append(line);
            snprintf(t, sizeof t, "Finished - %s", title);
            SetWindowText(h, t);
            finished = 2;
            KillTimer(h, 1);
        }
        return 0;
    case WM_CLOSE:
        if (!finished)                      /* closed while it runs: it ends */
            TerminateThread(worker, 0xC000013Au);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

int RunConsole(const char *name, int (*fn)(int, char **), int argc, char **argv)
{
    InitUI();
    strncpy0(title, name ? name : "Console", sizeof title);
    WNDCLASS wc = { .name = CON_CLASS, .proc = console_proc, .background = C_WHITE, .icon = ICON_DOS };
    RegisterClass(&wc);
    RECT work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    int w = 600, h = 300;
    con = CreateWindow(CON_CLASS, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                       work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 3,
                       w, h, NULL, 0, NULL);
    static struct start s;
    s = (struct start){ fn, argc, argv };
    worker = CreateThread(NULL, 0, run, &s, 0, NULL);
    if (!worker) {
        append("Can't start the program's thread.\r\n");
        finished = 2;
    }
    DebugLog("console %s", title);
    MSG m;
    while (GetMessage(&m)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    if (worker) {
        WaitForSingleObject(worker, 2000);
        CloseHandle(worker);
    }
    return (int)exit_code;
}
