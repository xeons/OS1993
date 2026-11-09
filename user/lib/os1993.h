/*
 * os1993.h -- the OS1993 programming interface for ring-3 programs.
 *
 *   KERNEL.DLL  processes, time, debug output (wraps the INT 30h calls)
 *   UI.DLL      windows, messages, controls, painting
 *
 * The window system follows the Windows model closely enough that it should
 * feel familiar: window classes with window procedures, a message queue,
 * DefWindowProc, controls as child windows driven by messages, subclassing
 * through SetWindowLong(GWL_WNDPROC) and CallWindowProc. Message numbers
 * match Windows where a Windows equivalent exists.
 */
#ifndef OS1993_H
#define OS1993_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USER_SIDE
#include "user.h"           /* struct user_info, struct user_event */

/* ===================================================================== */
/* KERNEL.DLL                                                            */
/* ===================================================================== */

__attribute__((noreturn)) void ExitProcess(int code);
void     OutputDebugString(const char *s);
void     DebugLog(const char *fmt, ...);        /* %s %d %u %x %c */
uint32_t GetTickCount(void);                    /* milliseconds since boot */
void     Sleep(uint32_t ms);                    /* 0: give up the time slice */
const struct user_info *GetStartupInfo(void);
int      StartDosSession(void);                 /* -> VM number, or -1 */

/* Raw input, for UI.DLL */
bool     GetInputEvent(struct user_event *e);
void     WaitForInput(void);                    /* sleep until the next tick */
bool     HasMouse(void);

/* ===================================================================== */
/* UI.DLL                                                                */
/* ===================================================================== */

typedef struct wnd  *HWND;
typedef struct dc   *HDC;
typedef uint32_t     WPARAM;
typedef int32_t      LPARAM;
typedef int32_t      LRESULT;
typedef LRESULT    (*WNDPROC)(HWND, unsigned, WPARAM, LPARAM);

typedef struct { int left, top, right, bottom; } RECT;
typedef struct { HWND hwnd; unsigned message; WPARAM wParam; LPARAM lParam; uint32_t time; } MSG;
typedef struct { HDC hdc; RECT rcPaint; } PAINTSTRUCT;

typedef struct {
    const char *name;
    WNDPROC     proc;
    unsigned    style;          /* CS_* */
    int         extra;          /* bytes for GetWindowLong(hwnd, 0..), max 32 */
    uint8_t     background;     /* colour the client area is cleared to */
} WNDCLASS;

#define LOWORD(l)        ((uint16_t)((uint32_t)(l) & 0xFFFF))
#define HIWORD(l)        ((uint16_t)((uint32_t)(l) >> 16))
#define MAKELONG(lo, hi) ((int32_t)(((uint32_t)(uint16_t)(lo)) | ((uint32_t)(uint16_t)(hi) << 16)))
#define MAKEWPARAM(l, h) ((WPARAM)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)MAKELONG(l, h))
#define GET_X_LPARAM(l)  ((int)(int16_t)LOWORD(l))
#define GET_Y_LPARAM(l)  ((int)(int16_t)HIWORD(l))

/* Colours (theme palette indices) */
enum {
    C_BLACK, C_GRAY1, C_GRAY2, C_GRAY3, C_GRAY4, C_GRAY5, C_FACE, C_LIGHT,
    C_WHITE, C_ACCENT, C_ACCENT_LT, C_DESKTOP, C_RED, C_GREEN, C_YELLOW, C_ACCENT_DK,
};

/* Window styles */
#define WS_OVERLAPPED       0x00000000u
#define WS_POPUP            0x80000000u
#define WS_CHILD            0x40000000u
#define WS_VISIBLE          0x10000000u
#define WS_DISABLED         0x08000000u
#define WS_CAPTION          0x00C00000u
#define WS_SYSMENU          0x00080000u
#define WS_THICKFRAME       0x00040000u
#define WS_MINIMIZEBOX      0x00020000u
#define WS_MAXIMIZEBOX      0x00010000u
#define WS_GROUP            0x00020000u     /* child windows */
#define WS_TABSTOP          0x00010000u     /* child windows */
#define WS_OVERLAPPEDWINDOW (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_DIALOG           (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

/* Class styles */
#define CS_DBLCLKS          0x0008u

/* Messages */
#define WM_NULL             0x0000
#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_ENABLE           0x000A
#define WM_SETTEXT          0x000C
#define WM_GETTEXT          0x000D
#define WM_GETTEXTLENGTH    0x000E
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUIT             0x0012
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_COMMAND          0x0111
#define WM_SYSCOMMAND       0x0112
#define WM_TIMER            0x0113
#define WM_HSCROLL          0x0114
#define WM_VSCROLL          0x0115
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_LBUTTONDBLCLK    0x0203
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_USER             0x0400

#define WA_INACTIVE         0
#define WA_ACTIVE           1
#define MK_LBUTTON          0x0001
#define MK_RBUTTON          0x0002

/* WM_SYSCOMMAND */
#define SC_SIZE             0xF000
#define SC_MOVE             0xF010
#define SC_MINIMIZE         0xF020
#define SC_MAXIMIZE         0xF030
#define SC_CLOSE            0xF060
#define SC_RESTORE          0xF120

/* WM_HSCROLL / WM_VSCROLL codes */
#define SB_LINEUP           0
#define SB_LINEDOWN         1
#define SB_PAGEUP           2
#define SB_PAGEDOWN         3
#define SB_THUMBPOSITION    4
#define SB_THUMBTRACK       5
#define SB_ENDSCROLL        8
#define TB_THUMBTRACK       5
#define TB_ENDTRACK         8

/* Virtual keys (WM_KEYDOWN wParam) */
#define VK_BACK     0x08
#define VK_TAB      0x09
#define VK_RETURN   0x0D
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_PRIOR    0x21
#define VK_NEXT     0x22
#define VK_END      0x23
#define VK_HOME     0x24
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_DELETE   0x2E
#define VK_F1       0x70
#define VK_F4       0x73
#define VK_F10      0x79

/* Dialog command ids */
#define IDOK        1
#define IDCANCEL    2

/* Control classes */
#define WC_BUTTON     "BUTTON"
#define WC_STATIC     "STATIC"
#define WC_EDIT       "EDIT"
#define WC_LISTBOX    "LISTBOX"
#define WC_SCROLLBAR  "SCROLLBAR"
#define WC_TRACKBAR   "TRACKBAR"
#define WC_PROGRESS   "PROGRESS"

/* Button styles and messages */
#define BS_PUSHBUTTON       0x0
#define BS_DEFPUSHBUTTON    0x1
#define BS_CHECKBOX         0x2
#define BS_AUTOCHECKBOX     0x3
#define BS_RADIOBUTTON      0x4
#define BS_GROUPBOX         0x7
#define BS_AUTORADIOBUTTON  0x9
#define BS_TYPEMASK         0xF
#define BM_GETCHECK         0x00F0
#define BM_SETCHECK         0x00F1
#define BM_CLICK            0x00F5
#define BN_CLICKED          0
#define BST_UNCHECKED       0
#define BST_CHECKED         1

/* Edit */
#define EN_CHANGE           0x0300

/* List box */
#define LBS_NOTIFY          0x0001
#define LB_ADDSTRING        0x0180
#define LB_RESETCONTENT     0x0184
#define LB_SETCURSEL        0x0186
#define LB_GETCURSEL        0x0188
#define LB_GETTEXT          0x0189
#define LB_GETCOUNT         0x018B
#define LBN_SELCHANGE       1
#define LB_ERR              (-1)

/* Scroll bar */
#define SBS_HORZ            0x0
#define SBS_VERT            0x1
#define SBM_SETPOS          0x00E0
#define SBM_GETPOS          0x00E1
#define SBM_SETRANGE        0x00E2      /* wParam = min, lParam = max */
#define SBM_SETPAGE         (WM_USER + 1)

/* Track bar (slider) */
#define TBM_GETPOS          (WM_USER)
#define TBM_SETPOS          (WM_USER + 5)
#define TBM_SETRANGE        (WM_USER + 6)   /* lParam = MAKELPARAM(min, max) */

/* Progress bar */
#define PBM_SETRANGE        (WM_USER + 1)   /* lParam = MAKELPARAM(min, max) */
#define PBM_SETPOS          (WM_USER + 2)
#define PBM_GETPOS          (WM_USER + 8)

/* ShowWindow */
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOWMINIMIZED    2
#define SW_SHOWMAXIMIZED    3
#define SW_SHOW             5
#define SW_RESTORE          9

/* GetWindowLong / SetWindowLong */
#define GWL_WNDPROC         (-4)
#define GWL_STYLE           (-16)
#define GWL_ID              (-12)
#define GWL_USERDATA        (-21)

/* Start-up: call once, before creating windows */
void    InitUI(void);

/* Classes and windows */
bool    RegisterClass(const WNDCLASS *wc);
HWND    CreateWindow(const char *cls, const char *text, unsigned style,
                     int x, int y, int w, int h, HWND parent, int id, void *param);
bool    DestroyWindow(HWND hwnd);
bool    IsWindow(HWND hwnd);
HWND    GetParent(HWND hwnd);
HWND    GetDlgItem(HWND parent, int id);
int     GetDlgCtrlID(HWND hwnd);
bool    MoveWindow(HWND hwnd, int x, int y, int w, int h, bool repaint);
bool    ShowWindow(HWND hwnd, int cmd);
bool    EnableWindow(HWND hwnd, bool enable);
bool    IsWindowEnabled(HWND hwnd);
bool    GetClientRect(HWND hwnd, RECT *r);
bool    GetWindowRect(HWND hwnd, RECT *r);          /* screen coordinates */
int     SetWindowText(HWND hwnd, const char *text);
int     GetWindowText(HWND hwnd, char *buf, int max);
int32_t GetWindowLong(HWND hwnd, int index);
int32_t SetWindowLong(HWND hwnd, int index, int32_t value);
bool    SetProp(HWND hwnd, const char *name, void *value);
void   *GetProp(HWND hwnd, const char *name);
void   *RemoveProp(HWND hwnd, const char *name);

/* Activation, focus, capture */
HWND    GetActiveWindow(void);
HWND    SetActiveWindow(HWND hwnd);
HWND    GetFocus(void);
HWND    SetFocus(HWND hwnd);
HWND    SetCapture(HWND hwnd);
bool    ReleaseCapture(void);
HWND    GetCapture(void);
int     GetKeyState(int vk);                        /* < 0: the key is down */

/* Messages */
bool    GetMessage(MSG *msg);                       /* false on WM_QUIT */
bool    PeekMessage(MSG *msg, bool remove);
bool    TranslateMessage(const MSG *msg);
LRESULT DispatchMessage(const MSG *msg);
LRESULT SendMessage(HWND hwnd, unsigned msg, WPARAM w, LPARAM l);
bool    PostMessage(HWND hwnd, unsigned msg, WPARAM w, LPARAM l);
void    PostQuitMessage(int code);
LRESULT DefWindowProc(HWND hwnd, unsigned msg, WPARAM w, LPARAM l);
LRESULT CallWindowProc(WNDPROC proc, HWND hwnd, unsigned msg, WPARAM w, LPARAM l);
bool    IsDialogMessage(HWND dlg, const MSG *msg);
int     SetTimer(HWND hwnd, int id, uint32_t ms);
bool    KillTimer(HWND hwnd, int id);

/* Dialog helpers */
bool    CheckDlgButton(HWND dlg, int id, int check);
int     IsDlgButtonChecked(HWND dlg, int id);
bool    SetDlgItemText(HWND dlg, int id, const char *text);
int     GetDlgItemText(HWND dlg, int id, char *buf, int max);

/* Painting */
bool    InvalidateRect(HWND hwnd, const RECT *r);   /* client coords; NULL: all */
HDC     BeginPaint(HWND hwnd, PAINTSTRUCT *ps);
bool    EndPaint(HWND hwnd, const PAINTSTRUCT *ps);
void    FillRect(HDC dc, const RECT *r, uint8_t color);
void    FrameRect(HDC dc, const RECT *r, uint8_t color);
void    DrawLine(HDC dc, int x, int y, int len, bool vertical, uint8_t color);
void    SetPixel(HDC dc, int x, int y, uint8_t color);
void    TextOut(HDC dc, int x, int y, const char *s, uint8_t fg, int bg);   /* bg -1: none */
void    DrawBitmap(HDC dc, int x, int y, const uint16_t *rows, int h, uint8_t color);
void    DrawBevel(HDC dc, const RECT *r, bool raised);
void    DrawRoundRect(HDC dc, const RECT *r, uint8_t color);
int     TextWidth(const char *s);

/* Small runtime helpers (each module has its own copy: user/lib/lib.c) */
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
char   *strncpy0(char *d, const char *s, size_t size);   /* always terminates */
void   *memset(void *d, int c, size_t n);
void   *memcpy(void *d, const void *s, size_t n);

#endif
