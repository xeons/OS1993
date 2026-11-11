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

typedef struct systime SYSTEMTIME;
void     GetLocalTime(SYSTEMTIME *t);

/* Processes. Each runs a program (a MOD3 .BIN from the OS1993 directory)
 * in its own address space; the GUI session ends when the shell does. */
int      CreateProcess(const char *program, const char *args);  /* -> pid, or -1 */
int      GetCurrentProcessId(void);
bool     IsProcessAlive(int pid);
bool     TerminateProcess(int pid);
typedef struct proc_entry PROCESSENTRY;          /* pid, flags (PROC_*), name */
int      EnumProcesses(PROCESSENTRY *list, int max);    /* -> how many */
void     TaskListShown(void);                   /* the shell answers Ctrl+Alt+Del (HSHELL_TASKMAN);
                                                   without it the VMM steps in */
uint32_t GetProcessExitCount(void);             /* processes ended so far */

/* Waiting and locks (IPC primitives; UI.DLL builds messages on them) */
bool     WaitForWake(uint32_t ms, unsigned flags);  /* WAIT_INFINITE, WAIT_INPUT */
bool     WakeProcess(int pid);
bool     AcquireLock(int n);                    /* 0-7; 0 is UI.DLL's */
bool     ReleaseLock(int n);
void     SetWaitHooks(void (*before)(void), void (*after)(void));

/* Files: DOS's (FAT, 8.3 names), reached through the VMM. Relative paths
 * start from the process's current directory, at first the OS1993
 * directory. Calls that fail return FALSE / INVALID_HANDLE_VALUE / ~0 and
 * leave the reason (ERROR_*) for GetLastError. */
typedef int HANDLE;
typedef struct find_data WIN32_FIND_DATA;
#define INVALID_HANDLE_VALUE    (-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
HANDLE   CreateFile(const char *path, uint32_t access, uint32_t creation);  /* GENERIC_*, CREATE_* ... */
bool     ReadFile(HANDLE h, void *buf, uint32_t n, uint32_t *done);
bool     WriteFile(HANDLE h, const void *buf, uint32_t n, uint32_t *done);
bool     CloseHandle(HANDLE h);
uint32_t SetFilePointer(HANDLE h, int32_t distance, int method);    /* FILE_BEGIN ... */
uint32_t GetFileSize(HANDLE h);
bool     DeleteFile(const char *path);
bool     MoveFile(const char *from, const char *to);
bool     CreateDirectory(const char *path);
bool     RemoveDirectory(const char *path);
uint32_t GetFileAttributes(const char *path);
uint32_t GetCurrentDirectory(uint32_t size, char *buf);
bool     SetCurrentDirectory(const char *path);
HANDLE   FindFirstFile(const char *pattern, WIN32_FIND_DATA *fd);     /* "*.BMP" */
bool     FindNextFile(HANDLE h, WIN32_FIND_DATA *fd);
bool     FindClose(HANDLE h);
uint32_t GetLastError(void);
void     SetLastError(uint32_t error);

bool     SetEndOfFile(HANDLE h);                        /* the file ends at its pointer */
bool     FlushFileBuffers(HANDLE h);
bool     LockFile(HANDLE h, uint32_t off_lo, uint32_t off_hi, uint32_t len_lo, uint32_t len_hi);
bool     UnlockFile(HANDLE h, uint32_t off_lo, uint32_t off_hi, uint32_t len_lo, uint32_t len_hi);
bool     SetFilePointerEx(HANDLE h, int64_t distance, int64_t *new_pos, int method);  /* past 2 GB */
bool     GetFileSizeEx(HANDLE h, int64_t *size);
/* Links: on ext2 volumes (elsewhere ERROR_NOT_SUPPORTED). A symbolic link
 * reads as the text of its target. */
bool     CreateHardLink(const char *path, const char *existing, void *security);
#define  SYMBOLIC_LINK_FLAG_DIRECTORY 1
bool     CreateSymbolicLink(const char *link, const char *target, uint32_t flags);
/* The time zone (the clock keeps local time): UTC = local time + Bias minutes.
 * The shell sets it from OS1993.INI ([timezone] tzbias=). */
typedef struct { int32_t Bias; char StandardName[32]; } TIME_ZONE_INFORMATION;
uint32_t GetTimeZoneInformation(TIME_ZONE_INFORMATION *tz);
bool     SetTimeZoneInformation(const TIME_ZONE_INFORMATION *tz);
bool     SetFileAttributes(const char *path, uint32_t attributes);
uint32_t GetFullPathName(const char *path, uint32_t size, char *buf, char **file_part);
bool     GetDiskFreeSpace(const char *root, uint32_t *sectors_per_cluster, uint32_t *bytes_per_sector,
                          uint32_t *free_clusters, uint32_t *total_clusters);
bool     CopyFile(const char *from, const char *to, bool fail_if_exists);
typedef struct { uint32_t dwLowDateTime, dwHighDateTime; } FILETIME;   /* 100 ns since 1601 */
bool     GetFileTime(HANDLE h, FILETIME *created, FILETIME *accessed, FILETIME *written);
bool     SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written);
bool     FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st);
bool     SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft);

/* Memory: the process heap (it grows as needed, up to 1 GB) */
#define LMEM_FIXED      0x0000
#define LMEM_ZEROINIT   0x0040
void    *LocalAlloc(unsigned flags, uint32_t size);     /* NULL if out of memory */
void    *LocalReAlloc(void *p, uint32_t size, unsigned flags);
void    *LocalFree(void *p);                            /* -> NULL */
uint32_t LocalSize(void *p);

/* Heaps, as in Win32: the process heap (LocalAlloc's), or private ones in
 * regions of their own. A damaged block header ends the process. */
#define HEAP_NO_SERIALIZE           0x0001
#define HEAP_ZERO_MEMORY            0x0008
#define HEAP_REALLOC_IN_PLACE_ONLY  0x0010
HANDLE   GetProcessHeap(void);
HANDLE   HeapCreate(uint32_t flags, uint32_t initial, uint32_t max);   /* max 0: up to 16 MB */
bool     HeapDestroy(HANDLE heap);
void    *HeapAlloc(HANDLE heap, uint32_t flags, uint32_t size);
void    *HeapReAlloc(HANDLE heap, uint32_t flags, void *p, uint32_t size);
bool     HeapFree(HANDLE heap, uint32_t flags, void *p);
uint32_t HeapSize(HANDLE heap, uint32_t flags, const void *p);
bool     HeapValidate(HANDLE heap, uint32_t flags, const void *p);

/* Virtual memory: regions of the address space (0x62000000-0x7F000000),
 * reserved in 64 KB steps and committed page by page. No paging to disk:
 * a committed page has its memory at once. */
#define MEM_COMMIT      0x1000
#define MEM_RESERVE     0x2000
#define MEM_DECOMMIT    0x4000
#define MEM_RELEASE     0x8000
#define MEM_FREE        0x10000
#define MEM_PRIVATE     0x20000
#define PAGE_NOACCESS           0x01            /* only by not committing */
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_WRITECOPY          0x08
#define PAGE_EXECUTE            0x10            /* a 486 can't refuse to run data: */
#define PAGE_EXECUTE_READ       0x20            /* these are READONLY ... */
#define PAGE_EXECUTE_READWRITE  0x40            /* ... and READWRITE */
#define PAGE_EXECUTE_WRITECOPY  0x80
typedef struct {
    void    *BaseAddress, *AllocationBase;
    uint32_t AllocationProtect, RegionSize, State, Protect, Type;
} MEMORY_BASIC_INFORMATION;
typedef struct {
    uint32_t dwLength, dwMemoryLoad;
    uint32_t dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS;
void    *VirtualAlloc(void *addr, uint32_t size, uint32_t type, uint32_t protect);
bool     VirtualFree(void *addr, uint32_t size, uint32_t type);   /* MEM_RELEASE: size 0 */
bool     VirtualProtect(void *addr, uint32_t size, uint32_t protect, uint32_t *old);
uint32_t VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, uint32_t len);
void     GlobalMemoryStatus(MEMORYSTATUS *ms);

/* Threads, as in Win32. Each has its own stack (64 KB by default, a guard
 * page below it) and its own GetLastError, errno and TLS slots. Only the
 * first thread may use the window system. Handles: CloseHandle. */
#define INFINITE                0xFFFFFFFFu
#define STILL_ACTIVE            259
#define CREATE_SUSPENDED        0x04
#define WAIT_OBJECT_0           0x000
#define WAIT_TIMEOUT            0x102
#define WAIT_FAILED             0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS    32
#define TLS_OUT_OF_INDEXES      0xFFFFFFFFu
#define ERROR_TOO_MANY_POSTS    298
#define ERROR_NOT_OWNER         288
typedef uint32_t (*LPTHREAD_START_ROUTINE)(void *arg);
typedef struct {
    volatile int32_t  lock;
    volatile uint32_t owner;            /* its thread id, or 0 */
    int32_t           recursion;
} CRITICAL_SECTION;                     /* all zero is a ready one */
HANDLE   CreateThread(void *security, uint32_t stack_size, LPTHREAD_START_ROUTINE fn, void *arg,
                      uint32_t flags, uint32_t *tid);
__attribute__((noreturn)) void ExitThread(uint32_t code);
bool     TerminateThread(HANDLE thread, uint32_t code);
uint32_t ResumeThread(HANDLE thread);                   /* one CREATE_SUSPENDED */
bool     GetExitCodeThread(HANDLE thread, uint32_t *code);   /* STILL_ACTIVE while it runs */
uint32_t GetCurrentThreadId(void);
HANDLE   GetCurrentThread(void);
HANDLE   GetCurrentProcess(void);
void    *GetThreadInformationBlock(void);               /* struct user_tib, at FS:0 */
void     InitializeCriticalSection(CRITICAL_SECTION *cs);
void     DeleteCriticalSection(CRITICAL_SECTION *cs);
void     EnterCriticalSection(CRITICAL_SECTION *cs);
bool     TryEnterCriticalSection(CRITICAL_SECTION *cs);
void     LeaveCriticalSection(CRITICAL_SECTION *cs);
HANDLE   CreateEvent(void *security, bool manual_reset, bool initial, const char *name);
bool     SetEvent(HANDLE event);
bool     ResetEvent(HANDLE event);
HANDLE   CreateMutex(void *security, bool initial_owner, const char *name);
bool     ReleaseMutex(HANDLE mutex);
HANDLE   CreateSemaphore(void *security, int32_t initial, int32_t max, const char *name);
bool     ReleaseSemaphore(HANDLE sem, int32_t n, int32_t *previous);
uint32_t WaitForSingleObject(HANDLE h, uint32_t ms);    /* a thread, process, event ... */
uint32_t WaitForMultipleObjects(uint32_t n, const HANDLE *hs, bool wait_all, uint32_t ms);
uint32_t TlsAlloc(void);
bool     TlsFree(uint32_t index);
void    *TlsGetValue(uint32_t index);
bool     TlsSetValue(uint32_t index, void *value);
int32_t  InterlockedIncrement(volatile int32_t *p);
int32_t  InterlockedDecrement(volatile int32_t *p);
int32_t  InterlockedExchange(volatile int32_t *p, int32_t v);
int32_t  InterlockedExchangeAdd(volatile int32_t *p, int32_t v);
int32_t  InterlockedCompareExchange(volatile int32_t *p, int32_t v, int32_t compare);

/* Processes beyond CreateProcess (-> a pid): a handle to wait on */
HANDLE   OpenProcess(uint32_t access, bool inherit, int pid);
bool     GetExitCodeProcess(HANDLE process, uint32_t *code);   /* STILL_ACTIVE while it runs */
const char *GetCommandLine(void);

/* The network, for WSOCK.DLL (programs use its Winsock calls) */
int      NetCall(uint32_t op, struct sock_args *a);    /* SOP_* -> result, or -errno */
uint32_t GetModuleFileName(void *module, char *buf, uint32_t size);

/* The display, for UI.DLL (programs use EnumDisplaySettings and
 * ChangeDisplaySettings): the current mode, the modes there are, a switch. */
bool     GetDisplayInfo(struct display_info *d);
int      GetDisplayModes(struct display_mode *list, int max);  /* -> how many */
bool     SetDisplayMode(int width, int height, int bpp);

/* Random bytes from the kernel's generator (ChaCha20 keyed from interrupt
 * timing), at most 256 per call: for keys, cookies, sequence numbers. */
bool     GetRandomBytes(void *buf, uint32_t n);

/* Raw input, for UI.DLL */
bool     GetInputEvent(struct user_event *e);
void     WaitForInput(void);                    /* sleep until input or a tick */
bool     HasMouse(void);

/* ===================================================================== */
/* UI.DLL                                                                */
/* ===================================================================== */

typedef struct wnd  *HWND;
typedef struct dc   *HDC;
typedef struct menu *HMENU;
typedef struct accel_table *HACCEL;
typedef uint32_t     WPARAM;
typedef int32_t      LPARAM;
typedef int32_t      LRESULT;
typedef LRESULT    (*WNDPROC)(HWND, unsigned, WPARAM, LPARAM);

typedef struct { int left, top, right, bottom; } RECT;
typedef struct { HWND hwnd; unsigned message; WPARAM wParam; LPARAM lParam; uint32_t time; } MSG;
typedef struct { HDC hdc; RECT rcPaint; } PAINTSTRUCT;
typedef struct { uint32_t dwData; uint32_t cbData; void *lpData; } COPYDATASTRUCT;
typedef bool (*WNDENUMPROC)(HWND, LPARAM);
static inline bool PtInRect(const RECT *r, int x, int y)
{
    return x >= r->left && y >= r->top && x < r->right && y < r->bottom;
}

typedef struct {
    const char *name;
    WNDPROC     proc;
    unsigned    style;          /* CS_* */
    int         extra;          /* bytes for GetWindowLong(hwnd, 0..), max 32 */
    uint8_t     background;     /* colour the client area is cleared to */
    int         icon;           /* ICON_* (SHELL.DLL's stock icons), for the taskbar */
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
#define WS_HSCROLL          0x00100000u     /* (multi-line edit) scroll bars */
#define WS_VSCROLL          0x00200000u
#define WS_BORDER           0x00800000u     /* (multi-line edit) a sunken border */
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
#define WM_CONTEXTMENU      0x007B
#define WM_STYLECHANGED     0x007D  /* after SetWindowLong(GWL_STYLE): wParam = GWL_STYLE */
#define WM_DISPLAYCHANGE    0x007E  /* the display mode changed: wParam = bits, lParam = w, h */  /* wParam = window clicked, lParam = screen x,y */
#define WM_COMMAND          0x0111
#define WM_SYSCOMMAND       0x0112
#define WM_TIMER            0x0113
#define WM_HSCROLL          0x0114
#define WM_INITMENU         0x0116  /* wParam = menu bar or system menu */
#define WM_INITMENUPOPUP    0x0117  /* wParam = menu, lParam = MAKELPARAM(pos, system) */
#define WM_VSCROLL          0x0115
#define WM_COPYDATA         0x004A  /* lParam = COPYDATASTRUCT *: copied across processes */
#define WM_NCLBUTTONDOWN    0x00A1  /* wParam = HT* part, lParam = screen x,y */
#define WM_NCLBUTTONDBLCLK  0x00A3
#define WM_SHELLHOOK        0x0380  /* to the shell hook window: wParam HSHELL_*, lParam HWND */
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_LBUTTONDBLCLK    0x0203
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_CUT              0x0300
#define WM_COPY             0x0301
#define WM_PASTE            0x0302
#define WM_CLEAR            0x0303
#define WM_UNDO             0x0304
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
#define SC_KEYMENU          0xF100  /* lParam = mnemonic, ' ' system menu, 0 menu bar */
#define SC_TASKLIST         0xF130  /* Ctrl+Esc: to the shell hook window (the Start menu) */

/* Menus */
#define MF_STRING           0x0000
#define MF_ENABLED          0x0000
#define MF_UNCHECKED        0x0000
#define MF_BYCOMMAND        0x0000
#define MF_GRAYED           0x0001
#define MF_DISABLED         0x0002
#define MF_CHECKED          0x0008
#define MF_POPUP            0x0010  /* id is the submenu's HMENU */
#define MFT_RADIOCHECK      0x0200  /* checked shows a dot, not a tick */
#define MF_BYPOSITION       0x0400
#define MF_SEPARATOR        0x0800
#define TPM_RETURNCMD       0x0100  /* return the id instead of posting WM_COMMAND */
#define TPM_BOTTOMALIGN     0x0020  /* y is the menu's bottom edge */

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
#define IDABORT     3
#define IDRETRY     4
#define IDIGNORE    5
#define IDYES       6
#define IDNO        7

/* Control classes */
#define WC_BUTTON     "BUTTON"
#define WC_STATIC     "STATIC"
#define WC_EDIT       "EDIT"
#define WC_LISTBOX    "LISTBOX"
#define WC_SCROLLBAR  "SCROLLBAR"
#define WC_TRACKBAR   "TRACKBAR"
#define WC_PROGRESS   "PROGRESS"
#define WC_COMBOBOX   "COMBOBOX"
#define WC_TABCONTROL "TABCONTROL"
#define WC_UPDOWN     "UPDOWN"

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
#define EM_GETSEL           0x00B0  /* -> MAKELONG(start, end); also *(int *)wp, *(int *)lp */
#define EM_SETSEL           0x00B1  /* (0, -1) selects all the text; (-1, 0) none */
#define EM_SCROLLCARET      0x00B7
#define EM_GETMODIFY        0x00B8
#define EM_SETMODIFY        0x00B9
#define EM_GETLINECOUNT     0x00BA
#define EM_LINEINDEX        0x00BB  /* wp = line (-1: the caret's) -> its first character */
#define EM_REPLACESEL       0x00C2  /* lp = text */
#define EM_LIMITTEXT        0x00C5  /* wp = most characters */
#define EM_CANUNDO          0x00C6
#define EM_UNDO             0x00C7
#define EM_LINEFROMCHAR     0x00C9  /* wp = character (-1: the caret) -> its line */
#define ES_MULTILINE        0x0004  /* lines (Enter starts one), tabs, scrolling */
#define ES_AUTOHSCROLL      0x0080  /* (multi-line) scroll sideways; without it or
                                       WS_HSCROLL, words wrap at the width */
#define ES_READONLY         0x0800
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
#define LBN_DBLCLK          2
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

/* Combo box. The window's height is the closed box's (Windows counts the
 * list too); the list drops below it, sized to at most 8 rows. Item
 * strings are copied into the owning process's heap. CB_SHOWDROPDOWN 1
 * returns once the list has closed again (the list runs a modal loop). */
#define CBS_DROPDOWN        0x0002      /* an editable field and the arrow */
#define CBS_DROPDOWNLIST    0x0003      /* a box showing the selection and the arrow */
#define CB_ADDSTRING        0x0143      /* lp = text -> index */
#define CB_DELETESTRING     0x0144      /* wp = index -> items left */
#define CB_GETCOUNT         0x0146
#define CB_GETCURSEL        0x0147
#define CB_GETLBTEXT        0x0148      /* wp = index, lp = buffer (big enough) -> length */
#define CB_GETLBTEXTLEN     0x0149
#define CB_INSERTSTRING     0x014A      /* wp = index (-1: the end), lp = text */
#define CB_RESETCONTENT     0x014B
#define CB_FINDSTRING       0x014C      /* wp = start after (-1: all), lp = prefix -> index */
#define CB_SETCURSEL        0x014E      /* wp = index (-1: none) */
#define CB_SHOWDROPDOWN     0x014F      /* wp = 1 opens the list, 0 closes it */
#define CB_GETDROPPEDSTATE  0x0157
#define CB_FINDSTRINGEXACT  0x0158      /* as CB_FINDSTRING, the whole text (any case) */
#define CBN_SELCHANGE       1
#define CBN_EDITCHANGE      5           /* CBS_DROPDOWN: the text was typed */
#define CBN_DROPDOWN        7
#define CBN_CLOSEUP         8
#define CB_ERR              (-1)

/* Tab control. Tabs along the top; the program shows the page itself
 * (on TCN_SELCHANGE: hide one set of controls, show another). Windows
 * notifies with WM_NOTIFY, which OS1993 doesn't have: here it is
 * WM_COMMAND with HIWORD(wParam) = TCN_* and lParam = the tab control;
 * returning nonzero to TCN_SELCHANGING keeps the current tab. TCIF_TEXT
 * and TCIF_PARAM are the item fields there are; texts keep 31 characters. */
#define TCM_FIRST           0x1300
#define TCM_GETITEMCOUNT    (TCM_FIRST + 4)
#define TCM_GETITEM         (TCM_FIRST + 5)     /* wp = index, lp = TCITEM * */
#define TCM_SETITEM         (TCM_FIRST + 6)
#define TCM_INSERTITEM      (TCM_FIRST + 7)     /* wp = index, lp = TCITEM * -> index */
#define TCM_DELETEITEM      (TCM_FIRST + 8)
#define TCM_DELETEALLITEMS  (TCM_FIRST + 9)
#define TCM_GETITEMRECT     (TCM_FIRST + 10)    /* wp = index, lp = RECT * (its client coords) */
#define TCM_GETCURSEL       (TCM_FIRST + 11)
#define TCM_SETCURSEL       (TCM_FIRST + 12)    /* no notification -> the previous one */
#define TCM_ADJUSTRECT      (TCM_FIRST + 40)    /* lp = RECT *; wp 0: window -> page, 1: back */
#define TCM_GETCURFOCUS     (TCM_FIRST + 47)
#define TCM_SETCURFOCUS     (TCM_FIRST + 48)    /* as a click: notifies */
#define TCIF_TEXT           0x0001
#define TCIF_PARAM          0x0008
#define TCN_SELCHANGE       0xFDD9      /* Windows' -551, as a 16-bit WM_COMMAND code */
#define TCN_SELCHANGING     0xFDD8      /* -552 */
typedef struct {
    unsigned    mask;                   /* TCIF_* */
    unsigned    dwState, dwStateMask;   /* (unused) */
    char       *pszText;
    int         cchTextMax;             /* TCM_GETITEM: the buffer's size */
    int         iImage;                 /* (unused) */
    LPARAM      lParam;
} TCITEM;

/* Up-down (spin) control: two arrow buttons that step a position through
 * a range, repeating while held. The parent hears WM_VSCROLL (WM_HSCROLL
 * with UDS_HORZ) SB_THUMBPOSITION with the new position in HIWORD(wParam),
 * and SB_ENDSCROLL when the mouse lets go. The range starts as 0..100
 * (Windows' is 100..0); max below min makes the up arrow count down. */
#define UDS_WRAP            0x0001      /* past one end to the other */
#define UDS_SETBUDDYINT     0x0002      /* the position is the buddy's text */
#define UDS_ALIGNRIGHT      0x0004      /* sit at the buddy's right, as tall as it */
#define UDS_AUTOBUDDY       0x0010      /* the buddy is the sibling created before it */
#define UDS_ARROWKEYS       0x0020      /* Up/Down in the buddy step it */
#define UDS_HORZ            0x0040      /* arrows side by side */
#define UDM_SETRANGE        (WM_USER + 101)     /* lp = MAKELPARAM(max, min) */
#define UDM_GETRANGE        (WM_USER + 102)     /* -> MAKELONG(max, min) */
#define UDM_SETPOS          (WM_USER + 103)     /* lp = position -> the old one */
#define UDM_GETPOS          (WM_USER + 104)     /* -> MAKELONG(position, error) */
#define UDM_SETBUDDY        (WM_USER + 105)     /* wp = HWND -> the old buddy */
#define UDM_GETBUDDY        (WM_USER + 106)

/* ShowWindow */
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOWMINIMIZED    2
#define SW_SHOWMAXIMIZED    3
#define SW_SHOW             5
#define SW_RESTORE          9
#define SW_MINIMIZE         6

/* Extended styles (CreateWindowEx) */
#define WS_EX_TOPMOST       0x00000008  /* above all other windows (the taskbar) */
#define WS_EX_TOOLWINDOW    0x00000080  /* no taskbar button */
#define WS_EX_NOACTIVATE    0x08000000  /* clicks don't activate it (the taskbar) */

/* Shell hook codes (WM_SHELLHOOK) */
#define HSHELL_WINDOWCREATED   1
#define HSHELL_WINDOWDESTROYED 2
#define HSHELL_WINDOWACTIVATED 4
#define HSHELL_REDRAW          6        /* title or state changed */
#define HSHELL_TASKMAN         7        /* Ctrl+Alt+Del: show the task list */

/* SystemParametersInfo */
#define SPI_SETWORKAREA     0x002F      /* pv = RECT *: where windows maximize */
#define SPI_GETWORKAREA     0x0030

/* GetWindowLong / SetWindowLong */
#define GWL_WNDPROC         (-4)
#define GWL_STYLE           (-16)
#define GWL_ID              (-12)
#define GWL_USERDATA        (-21)
#define GWL_EXSTYLE         (-20)

/* Start-up: call once, before creating windows */
void    InitUI(void);

/* Classes and windows */
bool    RegisterClass(const WNDCLASS *wc);
HWND    CreateWindowEx(unsigned exstyle, const char *cls, const char *text, unsigned style,
                       int x, int y, int w, int h, HWND parent, int id, void *param);
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
bool    UpdateWindow(HWND hwnd);                    /* paint it now if it needs it */
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
void    DrawLineEx(HDC dc, int x0, int y0, int x1, int y1, uint8_t color);
void    DrawEllipse(HDC dc, const RECT *r, uint8_t color, bool filled);
void    DrawPixels(HDC dc, int x, int y, int w, int h, const uint8_t *pixels, int stride);  /* 1 byte/pixel, FFh transparent */

/* Windows of every process (the shell's view) */
bool    EnumWindows(WNDENUMPROC proc, LPARAM l);    /* top-level, top first */
HWND    FindWindow(const char *cls, const char *title);   /* either may be NULL */
int     GetWindowProcessId(HWND hwnd);
bool    IsWindowVisible(HWND hwnd);
bool    IsIconic(HWND hwnd);                        /* minimized */
HWND    GetForegroundWindow(void);
bool    SetForegroundWindow(HWND hwnd);
bool    RegisterShellHookWindow(HWND hwnd);         /* gets WM_SHELLHOOK */
bool    SetShellWindow(HWND hwnd);                  /* the desktop: kept at the bottom */
HWND    GetShellWindow(void);
bool    SystemParametersInfo(unsigned action, unsigned param, void *pv, unsigned flags);

/* The screen and its modes, as in Win32 */
#define SM_CXSCREEN         0
#define SM_CYSCREEN         1
#define ENUM_CURRENT_SETTINGS (-1)
#define DISP_CHANGE_SUCCESSFUL 0
#define DISP_CHANGE_FAILED  (-1)
#define DISP_CHANGE_BADMODE (-2)
typedef struct { uint32_t dmPelsWidth, dmPelsHeight, dmBitsPerPel; } DEVMODE;
int     GetSystemMetrics(int index);
bool    EnumDisplaySettings(int index, DEVMODE *dm);        /* index 0..: the modes */
int     ChangeDisplaySettings(const DEVMODE *dm, unsigned flags);
void    FillRectRGB(HDC dc, const RECT *r, uint8_t red, uint8_t green, uint8_t blue);

/* The clipboard: text, shared by every process */
bool    SetClipboardText(const char *text, int len);        /* len -1: up to the NUL */
int     GetClipboardText(char *buf, int max);               /* -> length copied */
int     GetClipboardTextLength(void);
int     GetClassIcon(HWND hwnd);                    /* WNDCLASS.icon */

/* ===================================================================== */
/* SHELL.DLL                                                             */
/* ===================================================================== */

/* Start a program ("CALC.BIN"; "COMMAND.COM" opens a DOS session).
 * Returns more than 32 on success, as in Windows. */
/* A console window (SHELL.DLL), for programs that print: main runs on a
 * second thread and what it writes to stdout shows in the window, which
 * stays open after it ends. In main: return RunConsoleProgram("Ping",
 * real_main, argc, argv); real_main must not use the window system. */
void    ConsoleWrite(const char *s, size_t n);
int     RunConsole(const char *title, int (*fn)(int, char **), int argc, char **argv);
void    __crt_set_output(void (*fn)(const char *s, size_t n));      /* CRT.DLL */
static inline int RunConsoleProgram(const char *title, int (*fn)(int, char **), int argc, char **argv)
{
    __crt_set_output(ConsoleWrite);         /* this program's C runtime's stdout */
    return RunConsole(title, fn, argc, argv);
}

int     ShellExecute(HWND hwnd, const char *verb, const char *file, const char *params,
                     const char *dir, int show);
int     ShellAbout(HWND hwnd, const char *app, const char *other);

/* Icons in the taskbar's notification area (the tray). The taskbar
 * sends uCallbackMessage to hWnd (wParam = uID, lParam = the mouse
 * message) when one is clicked. */
#define NIM_ADD     0
#define NIM_MODIFY  1
#define NIM_DELETE  2
#define NIF_MESSAGE 1
#define NIF_ICON    2
#define NIF_TIP     4
typedef struct {
    uint32_t cbSize;
    HWND     hWnd;
    unsigned uID, uFlags, uCallbackMessage;
    int      hIcon;                                 /* ICON_* */
    char     szTip[64];
} NOTIFYICONDATA;
bool    Shell_NotifyIcon(unsigned msg, const NOTIFYICONDATA *data);

/* Stock icons: 32x32 (desktop) and 16x16 (taskbar, menus) */
enum { ICON_NONE, ICON_APP, ICON_CALC, ICON_PAINT, ICON_CLOCK, ICON_DEMO, ICON_DOS,
       ICON_COMPUTER, ICON_INFO, ICON_SHUTDOWN, ICON_NOTEPAD, ICON_COUNT };
void    DrawIcon(HDC dc, int x, int y, int icon);

/* The Open and Save As dialogs */
#define OFN_OVERWRITEPROMPT 0x0002      /* Save: ask before replacing a file */
#define OFN_FILEMUSTEXIST   0x1000      /* Open: the file must exist */
typedef struct {
    HWND        owner;
    const char *filter;                 /* the files listed: "*.BMP" */
    char       *file;                   /* in: a name to start with; out: the full path */
    uint32_t    max;
    const char *title;                  /* NULL: "Open" / "Save As" */
    const char *defext;                 /* "BMP": added to a name without one */
    uint32_t    flags;
} OPENFILENAME;
bool    GetOpenFileName(OPENFILENAME *ofn);

/* Find and Replace: modeless dialogs owned by fr->owner, which gets
 * WM_FINDMSG (lParam = fr) for each button with fr->flags saying which and
 * how, and does the searching itself; FR_DIALOGTERM when the dialog closes.
 * Pass the dialog to IsDialogMessage in the message loop. */
#define WM_FINDMSG      0x0381
#define FR_DOWN         0x0001
#define FR_WHOLEWORD    0x0002
#define FR_MATCHCASE    0x0004
#define FR_FINDNEXT     0x0008
#define FR_REPLACE      0x0010
#define FR_REPLACEALL   0x0020
#define FR_DIALOGTERM   0x0040
typedef struct {
    HWND        owner;
    unsigned    flags;                  /* in: FR_DOWN etc. to start with */
    char       *find, *replace;         /* in: the text to start with; out: what was typed */
    uint32_t    max;                    /* size of each buffer */
} FINDREPLACE;
HWND    FindText(FINDREPLACE *fr);
HWND    ReplaceText(FINDREPLACE *fr);
bool    GetSaveFileName(OPENFILENAME *ofn);
void    DrawSmallIcon(HDC dc, int x, int y, int icon);

/* Menus. Item text marks its mnemonic with '&' and may carry a
 * right-aligned accelerator after a tab: "E&xit\tAlt+F4". Choosing an item
 * sends WM_COMMAND (id, 0) -- WM_SYSCOMMAND from the system menu. */
HMENU   CreateMenu(void);                           /* a menu bar */
HMENU   CreatePopupMenu(void);
bool    DestroyMenu(HMENU menu);                    /* and its submenus */
bool    AppendMenu(HMENU menu, unsigned flags, uintptr_t id, const char *text);
bool    SetMenu(HWND hwnd, HMENU menu);
HMENU   GetMenu(HWND hwnd);
HMENU   GetSubMenu(HMENU menu, int pos);
int     GetMenuItemCount(HMENU menu);
int     CheckMenuItem(HMENU menu, unsigned item, unsigned flags);   /* -> old, or -1 */
int     EnableMenuItem(HMENU menu, unsigned item, unsigned flags);  /* -> old, or -1 */
bool    CheckMenuRadioItem(HMENU menu, unsigned first, unsigned last, unsigned check, unsigned flags);
HMENU   GetSystemMenu(HWND hwnd, bool revert);
int     TrackPopupMenu(HMENU menu, unsigned flags, int x, int y, HWND owner);
bool    DrawMenuBar(HWND hwnd);

/* Keyboard accelerators: TranslateAccelerator, called in the message loop
 * before TranslateMessage, turns a matching key into WM_COMMAND
 * (HIWORD(wParam) = 1) for hwnd, and returns true if it did. */
#define FVIRTKEY    0x01        /* key is a VK_ code (else a character) */
#define FSHIFT      0x04
#define FCONTROL    0x08
#define FALT        0x10
typedef struct { uint8_t fVirt; uint16_t key; uint16_t cmd; } ACCEL;
HACCEL  CreateAcceleratorTable(const ACCEL *accels, int count);
bool    DestroyAcceleratorTable(HACCEL table);
int     TranslateAccelerator(HWND hwnd, HACCEL table, MSG *msg);

/* Message boxes: modal to their owner (which is disabled meanwhile). */
#define MB_OK               0x0000
#define MB_OKCANCEL         0x0001
#define MB_ABORTRETRYIGNORE 0x0002
#define MB_YESNOCANCEL      0x0003
#define MB_YESNO            0x0004
#define MB_RETRYCANCEL      0x0005
#define MB_TYPEMASK         0x000F
#define MB_ICONHAND         0x0010
#define MB_ICONERROR        MB_ICONHAND
#define MB_ICONQUESTION     0x0020
#define MB_ICONEXCLAMATION  0x0030
#define MB_ICONWARNING      MB_ICONEXCLAMATION
#define MB_ICONASTERISK     0x0040
#define MB_ICONINFORMATION  MB_ICONASTERISK
#define MB_ICONMASK         0x00F0
#define MB_DEFBUTTON1       0x0000
#define MB_DEFBUTTON2       0x0100
#define MB_DEFBUTTON3       0x0200
#define MB_DEFMASK          0x0F00
int     MessageBox(HWND owner, const char *text, const char *caption, unsigned type);  /* -> IDOK... */

/* Small runtime helpers (each module has its own copy: user/lib/lib.c) */
#include <string.h>          /* the C runtime's (user/crt/include) */

#endif
