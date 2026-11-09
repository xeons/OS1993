/*
 * user/apps/hang.c -- HANG.BIN, a program that stops responding on purpose.
 *
 * For trying (and testing) Ctrl+Alt+Del: press H or click the button and
 * it loops forever inside a message handler, still holding the window
 * system's lock, which freezes the whole GUI -- until the VMM's emergency
 * screen ends it. Start it from Run (it isn't on the Start menu).
 */
#include "os1993.h"

static LRESULT hang_proc(HWND h, unsigned msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindow(WC_STATIC, "Press H or the button: this program will", WS_CHILD | WS_VISIBLE,
                     12, 10, 330, 16, h, 0, NULL);
        CreateWindow(WC_STATIC, "stop responding. Ctrl+Alt+Del ends it.", WS_CHILD | WS_VISIBLE,
                     12, 26, 330, 16, h, 0, NULL);
        CreateWindow(WC_BUTTON, "Hang", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 120, 54, 96, 22, h, 1, NULL);
        return 0;
    case WM_CHAR:
        if (wp != 'h' && wp != 'H')
            return 0;
        /* fall through */
    case WM_COMMAND:
        DebugLog("hanging");
        for (volatile int forever = 1; forever; )
            ;
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

int main(void)
{
    InitUI();
    WNDCLASS wc = { .name = "HangWindow", .proc = hang_proc, .background = C_FACE, .icon = ICON_APP };
    RegisterClass(&wc);
    HWND h = CreateWindow("HangWindow", "Hang Test", WS_DIALOG | WS_VISIBLE, 150, 150, 350, 116,
                          NULL, 0, NULL);
    SetFocus(h);
    DebugLog("hang ready");
    MSG msg;
    while (GetMessage(&msg)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
