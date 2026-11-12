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
bool     Beep(unsigned freq, unsigned ms);      /* the PC speaker; returns at once */

/* INI files ("[section]", "key=value"), as in Win32; names in any case */
int      GetPrivateProfileString(const char *section, const char *key, const char *def, char *out, int size,
                                 const char *file);    /* key NULL: the keys, NUL-separated */
int      GetPrivateProfileInt(const char *section, const char *key, int def, const char *file);
int      GetPrivateProfileSection(const char *section, char *out, int size, const char *file);  /* k=v\0...\0 */
bool     WritePrivateProfileString(const char *section, const char *key, const char *value,
                                   const char *file);  /* value NULL: remove; key NULL: the section */
const struct user_info *GetStartupInfo(void);
int      StartDosSession(void);                 /* -> VM number, or -1 */
struct vm_settings;                             /* <user.h>: memory, devices, mouse, idle */
int      StartDosSessionEx(const struct vm_settings *s);   /* NULL: OS1993.INI's [DOS] */

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
/* The time zone (the clock keeps local time): UTC = local time + Bias +
 * StandardBias, or + DaylightBias between DaylightDate and StandardDate
 * (month, day_of_week, day = the week 1-5, 5 the last, hour, minute).
 * The shell sets it from OS1993.INI ([timezone] tzbias=, dstbias=,
 * dststart= and dstend= as month,week,day_of_week,hour). */
typedef struct {
    int32_t    Bias;
    char       StandardName[32];
    SYSTEMTIME StandardDate;
    int32_t    StandardBias;
    char       DaylightName[32];
    SYSTEMTIME DaylightDate;
    int32_t    DaylightBias;
} TIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN  0
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2
uint32_t GetTimeZoneInformation(TIME_ZONE_INFORMATION *tz);
bool     SetTimeZoneInformation(const TIME_ZONE_INFORMATION *tz);
bool     SetFileAttributes(const char *path, uint32_t attributes);
uint32_t GetFullPathName(const char *path, uint32_t size, char *buf, char **file_part);
#define DRIVE_UNKNOWN       0
#define DRIVE_NO_ROOT_DIR   1
#define DRIVE_REMOVABLE     2
#define DRIVE_FIXED         3
#define DRIVE_REMOTE        4
uint32_t GetLogicalDrives(void);                        /* bit 0 = A: */
unsigned GetDriveType(const char *root);                /* "C:\\" -> DRIVE_* */
bool     GetVolumeInformation(const char *root, char *label, uint32_t label_size, uint32_t *serial,
                              uint32_t *max_component, uint32_t *flags, char *fs_name, uint32_t fs_size);
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
typedef struct { int width, height; const uint8_t *pixels; } BITMAPIMAGE;  /* 1 byte/pixel, FFh clear */
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

/* Cursors: the stock shapes (IDC_*); LoadCursor(NULL, IDC_...) gives the
 * handle. */
typedef int HCURSOR;
#define IDC_ARROW       32512
#define IDC_IBEAM       32513
#define IDC_WAIT        32514
#define IDC_CROSS       32515
#define IDC_SIZENWSE    32642
#define IDC_SIZENESW    32643
#define IDC_SIZEWE      32644
#define IDC_SIZENS      32645
#define IDC_SIZEALL     32646
#define IDC_NO          32648

typedef struct {
    const char *name;
    WNDPROC     proc;
    unsigned    style;          /* CS_* */
    int         extra;          /* bytes for GetWindowLong(hwnd, 0..), max 32 */
    uint8_t     background;     /* colour the client area is cleared to */
    int         icon;           /* ICON_* (SHELL.DLL's stock icons), for the taskbar */
    HCURSOR     cursor;         /* over the client area (0: the arrow) */
} WNDCLASS;

#define LOWORD(l)        ((uint16_t)((uint32_t)(l) & 0xFFFF))
#define HIWORD(l)        ((uint16_t)((uint32_t)(l) >> 16))
#define MAKELONG(lo, hi) ((int32_t)(((uint32_t)(uint16_t)(lo)) | ((uint32_t)(uint16_t)(hi) << 16)))
#define MAKEWPARAM(l, h) ((WPARAM)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)MAKELONG(l, h))
#define GET_X_LPARAM(l)  ((int)(int16_t)LOWORD(l))
#define GET_Y_LPARAM(l)  ((int)(int16_t)HIWORD(l))

/* RGB colours, as in Win32 (0x00BBGGRR) */
typedef uint32_t COLORREF;
#define RGB(r, g, b)     ((COLORREF)((uint8_t)(r) | ((uint32_t)(uint8_t)(g) << 8) | ((uint32_t)(uint8_t)(b) << 16)))
#define GetRValue(c)     ((uint8_t)(c))
#define GetGValue(c)     ((uint8_t)((c) >> 8))
#define GetBValue(c)     ((uint8_t)((c) >> 16))

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
#define WM_MENUSELECT       0x011F  /* wParam = MAKEWPARAM(id or submenu position, MF_* flags; 0xFFFF:
                                       closed), lParam = the menu */
#define WM_VSCROLL          0x0115
#define WM_COPYDATA         0x004A  /* lParam = COPYDATASTRUCT *: copied across processes */
#define WM_NCLBUTTONDOWN    0x00A1  /* wParam = HT* part, lParam = screen x,y */
#define WM_NCLBUTTONDBLCLK  0x00A3
#define WM_NCHITTEST        0x0084  /* lParam = screen x,y -> HT*: what part of the window it is */
#define WM_DRAWITEM         0x002B  /* owner-drawn control or menu item: lParam = DRAWITEMSTRUCT * */
#define WM_MEASUREITEM      0x002C  /* ... its size asked: lParam = MEASUREITEMSTRUCT * */
#define WM_CTLCOLORMSGBOX   0x0132  /* wParam = HDC, lParam = the control: SetTextColor on the */
#define WM_CTLCOLOREDIT     0x0133  /* DC picks the text's colour, returning CTLCOLOR_BG(c) the */
#define WM_CTLCOLORLISTBOX  0x0134  /* background's; 0: the usual ones */
#define WM_CTLCOLORBTN      0x0135
#define WM_CTLCOLORDLG      0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC   0x0138
#define CTLCOLOR_BG(c)      ((LRESULT)(0x100 | (uint8_t)(c)))

/* WM_NCHITTEST results (DefWindowProc: HTCLIENT). A window may answer
 * HTCAPTION for part of its client area (dragging there moves it), a
 * border code (sizes it), HTTRANSPARENT (the parent gets the mouse) or
 * HTNOWHERE (nobody does). */
#define HTERROR             (-2)
#define HTTRANSPARENT       (-1)
#define HTNOWHERE           0
#define HTCLIENT            1
#define HTCAPTION           2
#define HTSYSMENU           3
#define HTMENU              5
#define HTMINBUTTON         8
#define HTMAXBUTTON         9
#define HTLEFT              10
#define HTRIGHT             11
#define HTTOP               12
#define HTTOPLEFT           13
#define HTTOPRIGHT          14
#define HTBOTTOM            15
#define HTBOTTOMLEFT        16
#define HTBOTTOMRIGHT       17
#define HTBORDER            18
#define HTCLOSE             20
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
#define MK_SHIFT            0x0004
#define MK_CONTROL          0x0008

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
#define MF_BITMAP           0x0004  /* text is a BITMAPIMAGE *: the item is a picture */
#define MF_POPUP            0x0010  /* id is the submenu's HMENU */
#define MF_OWNERDRAW        0x0100  /* the owner draws it; text is its value (WM_DRAWITEM itemData) */
#define MF_SYSMENU          0x2000  /* (WM_MENUSELECT) in the system menu */
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
#define VK_INSERT   0x2D
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
#define BS_3STATE           0x5     /* checked, unchecked or indeterminate (grey) */
#define BS_AUTO3STATE       0x6
#define BS_GROUPBOX         0x7
#define BS_AUTORADIOBUTTON  0x9
#define BS_OWNERDRAW        0xB     /* the parent draws it (WM_DRAWITEM) */
#define BS_PUSHLIKE         0x1000  /* a check box or radio button that looks like a push button */
#define BS_TYPEMASK         0xF
#define BM_GETCHECK         0x00F0
#define BM_SETCHECK         0x00F1
#define BM_GETSTATE         0x00F2
#define BM_SETSTATE         0x00F3  /* wp: shown pushed in */
#define BM_CLICK            0x00F5
#define BN_CLICKED          0
#define BST_UNCHECKED       0
#define BST_CHECKED         1
#define BST_INDETERMINATE   2
#define BST_PUSHED          4
#define BST_FOCUS           8

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
#define EM_EMPTYUNDOBUFFER  0x00CD
#define EM_REDO             (WM_USER + 84)   /* (RichEdit's) step forward again: Ctrl+Y */
#define EM_CANREDO          (WM_USER + 85)
#define ES_MULTILINE        0x0004  /* lines (Enter starts one), tabs, scrolling */
#define ES_AUTOHSCROLL      0x0080  /* (multi-line) scroll sideways; without it or
                                       WS_HSCROLL, words wrap at the width */
#define ES_UPPERCASE        0x0008
#define ES_LOWERCASE        0x0010
#define ES_PASSWORD         0x0020  /* shows stars; won't copy */
#define ES_NOHIDESEL        0x0100  /* the selection shows without the focus */
#define ES_READONLY         0x0800
#define ES_NUMBER           0x2000  /* digits only */
#define EM_SETREADONLY      0x00CF
#define EM_GETLIMITTEXT     0x00D5
#define EN_CHANGE           0x0300

/* Static: text (wrapped at spaces unless SS_LEFTNOWORDWRAP/SS_SIMPLE; '&'
 * underlines the next character) or a shape. */
#define SS_LEFT             0x00
#define SS_CENTER           0x01
#define SS_RIGHT            0x02
#define SS_BLACKRECT        0x04
#define SS_GRAYRECT         0x05
#define SS_WHITERECT        0x06
#define SS_BLACKFRAME       0x07
#define SS_GRAYFRAME        0x08
#define SS_WHITEFRAME       0x09
#define SS_SIMPLE           0x0B
#define SS_LEFTNOWORDWRAP   0x0C
#define SS_OWNERDRAW        0x0D
#define SS_ETCHEDHORZ       0x10
#define SS_ETCHEDVERT       0x11
#define SS_ETCHEDFRAME      0x12
#define SS_TYPEMASK         0x1F
#define SS_NOPREFIX         0x80
#define SS_CENTERIMAGE      0x200   /* centred vertically (one line) */
#define SS_SUNKEN           0x1000
#define SS_ENDELLIPSIS      0x4000

/* List box. Items keep a string (copied into the owner's heap) and a value. */
#define LBS_NOTIFY          0x0001
#define LBS_SORT            0x0002
#define LBS_MULTIPLESEL     0x0008  /* a click toggles an item */
#define LBS_OWNERDRAWFIXED  0x0010  /* the parent draws the rows (WM_DRAWITEM) */
#define LBS_HASSTRINGS      0x0040  /* (owner-drawn) strings kept all the same */
#define LBS_EXTENDEDSEL     0x0800  /* Shift: a range, Ctrl: one more */
#define LB_ADDSTRING        0x0180  /* lp = text (owner-drawn without strings: the value) -> index */
#define LB_INSERTSTRING     0x0181  /* wp = index (-1: the end) */
#define LB_DELETESTRING     0x0182  /* -> items left */
#define LB_SELITEMRANGEEX   0x0183
#define LB_RESETCONTENT     0x0184
#define LB_SETSEL           0x0185  /* (multi) wp = on?, lp = index (-1: all) */
#define LB_SETCURSEL        0x0186
#define LB_GETSEL           0x0187
#define LB_GETCURSEL        0x0188
#define LB_GETTEXT          0x0189  /* wp = index, lp = buffer (LB_GETTEXTLEN + 1) */
#define LB_GETTEXTLEN       0x018A
#define LB_GETCOUNT         0x018B
#define LB_SELECTSTRING     0x018C  /* wp = start after, lp = prefix -> index */
#define LB_GETTOPINDEX      0x018E
#define LB_FINDSTRING       0x018F  /* wp = start after (-1: all), lp = prefix -> index */
#define LB_GETSELCOUNT      0x0190
#define LB_GETSELITEMS      0x0191  /* wp = room, lp = int[] -> how many */
#define LB_SETTOPINDEX      0x0197
#define LB_GETITEMRECT      0x0198
#define LB_GETITEMDATA      0x0199
#define LB_SETITEMDATA      0x019A
#define LB_SELITEMRANGE     0x019B  /* wp = on?, lp = MAKELPARAM(first, last) */
#define LB_SETCARETINDEX    0x019E
#define LB_GETCARETINDEX    0x019F
#define LB_SETITEMHEIGHT    0x01A0  /* lp = pixels */
#define LB_GETITEMHEIGHT    0x01A1
#define LB_FINDSTRINGEXACT  0x01A2
#define LBN_SELCHANGE       1
#define LBN_DBLCLK          2
#define LBN_SETFOCUS        4
#define LBN_KILLFOCUS       5
#define LB_ERR              (-1)
#define LB_ERRSPACE         (-2)

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
 * returns at once; the list opens when the message loop next runs. */
#define CBS_DROPDOWN        0x0002      /* an editable field and the arrow */
#define CBS_SIMPLE          0x0001      /* a field with the list always shown below it */
#define CBS_DROPDOWNLIST    0x0003      /* a box showing the selection and the arrow */
#define CBS_OWNERDRAWFIXED  0x0010      /* the parent draws the items (WM_DRAWITEM) */
#define CBS_SORT            0x0100
#define CBS_NOAUTOCOMPLETE  0x8000      /* (OS1993) an editable box doesn't complete what's typed */
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
#define CB_GETEDITSEL       0x0140      /* as EM_GETSEL, the field's */
#define CB_LIMITTEXT        0x0141
#define CB_SETEDITSEL       0x0142      /* lp = MAKELPARAM(start, end) */
#define CB_SELECTSTRING     0x014D      /* wp = start after, lp = prefix -> index (selected) */
#define CB_GETITEMDATA      0x0150
#define CB_SETITEMDATA      0x0151      /* wp = index, lp = value */
#define CB_FINDSTRINGEXACT  0x0158      /* as CB_FINDSTRING, the whole text (any case) */
#define CBN_SELCHANGE       1
#define CBN_DBLCLK          2           /* CBS_SIMPLE */
#define CBN_SELENDOK        9           /* the list closed with a choice */
#define CBN_SELENDCANCEL    10          /* ... without one */
#define CBN_EDITCHANGE      5           /* CBS_DROPDOWN: the text was typed */
#define CBN_DROPDOWN        7
#define CBN_CLOSEUP         8
#define CB_ERR              (-1)

/* Tab control. Tabs along the top; the program shows the page itself
 * (on TCN_SELCHANGE: hide one set of controls, show another). It notifies
 * with WM_NOTIFY (NMHDR code TCN_*) and then, as it did before WM_NOTIFY,
 * with WM_COMMAND (HIWORD(wParam) = TCN_*, lParam = the tab control);
 * nonzero from either to TCN_SELCHANGING keeps the current tab. Texts
 * keep 31 characters. TCS_MULTILINE wraps tabs that don't fit onto more
 * rows; otherwise arrows scroll them. Ctrl+Tab in a dialog goes to the
 * next tab. */
#define TCS_MULTILINE       0x0200
#define TCM_FIRST           0x1300
#define TCM_GETIMAGELIST    (TCM_FIRST + 2)
#define TCM_SETIMAGELIST    (TCM_FIRST + 3)     /* lp = HIMAGELIST -> the old one */
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
#define TCM_GETROWCOUNT     (TCM_FIRST + 44)
#define TCM_GETCURFOCUS     (TCM_FIRST + 47)
#define TCM_SETCURFOCUS     (TCM_FIRST + 48)    /* as a click: notifies */
#define TCIF_TEXT           0x0001
#define TCIF_IMAGE          0x0002
#define TCIF_PARAM          0x0008
#define TCN_SELCHANGE       0xFDD9      /* Windows' -551, as a 16-bit WM_COMMAND code */
#define TCN_SELCHANGING     0xFDD8      /* -552 */
typedef struct {
    unsigned    mask;                   /* TCIF_* */
    unsigned    dwState, dwStateMask;   /* (unused) */
    char       *pszText;
    int         cchTextMax;             /* TCM_GETITEM: the buffer's size */
    int         iImage;                 /* TCIF_IMAGE: in the image list (-1 none) */
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
#define UDS_NOTHOUSANDS     0x0080      /* no "," every three digits */
#define UDM_SETRANGE        (WM_USER + 101)     /* lp = MAKELPARAM(max, min) */
#define UDM_GETRANGE        (WM_USER + 102)     /* -> MAKELONG(max, min) */
#define UDM_SETPOS          (WM_USER + 103)     /* lp = position -> the old one */
#define UDM_GETPOS          (WM_USER + 104)     /* -> MAKELONG(position, error) */
#define UDM_SETBUDDY        (WM_USER + 105)     /* wp = HWND -> the old buddy */
#define UDM_GETBUDDY        (WM_USER + 106)
#define UDM_SETACCEL        (WM_USER + 107)     /* wp = count, lp = UDACCEL[] */
#define UDM_GETACCEL        (WM_USER + 108)
#define UDM_SETBASE         (WM_USER + 109)     /* wp = 10 or 16 */
#define UDM_GETBASE         (WM_USER + 110)
#define UDM_SETRANGE32      (WM_USER + 111)     /* wp = min, lp = max */
#define UDM_GETRANGE32      (WM_USER + 112)     /* wp, lp = int * */
#define UDM_SETPOS32        (WM_USER + 113)     /* lp = position */
#define UDM_GETPOS32        (WM_USER + 114)     /* lp = int * (error) -> position */
#define UDN_DELTAPOS        (-722)              /* WM_NOTIFY, NMUPDOWN: nonzero stops the step */
typedef struct { unsigned nSec, nInc; } UDACCEL;

/* WM_NOTIFY: a control tells its parent something, lParam = NMHDR * (or a
 * bigger structure starting with one), wParam = its id. The pointer is the
 * control's process's memory: the parent must be in the same process (as
 * a dialog's controls always are). */
#define WM_NOTIFY           0x004E
typedef struct { int x, y; } POINT;
typedef struct { HWND hwndFrom; unsigned idFrom; int code; } NMHDR;
typedef struct { NMHDR hdr; int iPos, iDelta; } NMUPDOWN;
#define NM_CLICK            (-2)
#define NM_DBLCLK           (-3)
#define NM_RETURN           (-4)
#define NM_RCLICK           (-5)
#define NM_SETFOCUS         (-7)
#define NM_KILLFOCUS        (-8)

/* Common control placement: status bars and toolbars put themselves along
 * the parent's bottom / top edge (on creation and on WM_SIZE, which the
 * parent passes on) unless CCS_NOPARENTALIGN. */
#define CCS_NOPARENTALIGN   0x0008

/* Status bar: sunken parts with a line of text each. SB_SETTEXT's wParam
 * is the part | SBT_*; a text starting "\t" is centred, "\t\t" right-aligned. */
#define STATUSCLASSNAME     "msctls_statusbar32"
#define SBARS_SIZEGRIP      0x0100      /* a grip in the corner sizes the window */
#define SB_SETTEXT          (WM_USER + 1)   /* wp = part | SBT_*, lp = text */
#define SB_GETTEXT          (WM_USER + 2)   /* wp = part, lp = buffer -> MAKELONG(len, SBT_*) */
#define SB_GETTEXTLENGTH    (WM_USER + 3)
#define SB_SETPARTS         (WM_USER + 4)   /* wp = count, lp = int[] right edges (-1: the end) */
#define SB_GETPARTS         (WM_USER + 6)
#define SB_GETRECT          (WM_USER + 10)
#define SB_SIMPLE           (WM_USER + 9)   /* wp: one part across (menu help) */
#define SB_ISSIMPLE         (WM_USER + 14)
#define SB_SIMPLEID         0xFF            /* SB_SETTEXT's part for the simple text */
#define SBT_OWNERDRAW       0x1000          /* lp is the value for WM_DRAWITEM */
#define SBT_NOBORDERS       0x0100
#define SBT_POPOUT          0x0200
HWND    CreateStatusWindow(unsigned style, const char *text, HWND parent, int id);

/* Toolbar: picture buttons from an image list (TB_SETIMAGELIST) across
 * the top; a click sends the parent WM_COMMAND with the button's id. */
#define TOOLBARCLASSNAME    "ToolbarWindow32"
#define TBSTYLE_BUTTON      0x00
#define TBSTYLE_SEP         0x01
#define TBSTYLE_CHECK       0x02        /* stays in until clicked again */
#define TBSTYLE_GROUP       0x04
#define TBSTYLE_CHECKGROUP  (TBSTYLE_GROUP | TBSTYLE_CHECK)   /* one of its neighbours */
#define TBSTYLE_TOOLTIPS    0x0100      /* (window style) tooltips from iString */
#define TBSTYLE_FLAT        0x0800      /* (window style) flat until pointed at */
#define TBSTATE_CHECKED     0x01
#define TBSTATE_PRESSED     0x02
#define TBSTATE_ENABLED     0x04
#define TBSTATE_HIDDEN      0x08
typedef struct {
    int      iBitmap;                   /* image list index (-1 none) */
    int      idCommand;
    uint8_t  fsState, fsStyle;
    uint32_t dwData;
    intptr_t iString;                   /* the tooltip: a char * (or 0) */
} TBBUTTON;
#define TB_ENABLEBUTTON     (WM_USER + 1)   /* wp = id, lp = MAKELONG(on, 0) */
#define TB_CHECKBUTTON      (WM_USER + 2)
#define TB_PRESSBUTTON      (WM_USER + 3)
#define TB_HIDEBUTTON       (WM_USER + 4)
#define TB_ISBUTTONENABLED  (WM_USER + 9)
#define TB_ISBUTTONCHECKED  (WM_USER + 10)
#define TB_ISBUTTONPRESSED  (WM_USER + 11)
#define TB_ISBUTTONHIDDEN   (WM_USER + 12)
#define TB_SETSTATE         (WM_USER + 17)
#define TB_GETSTATE         (WM_USER + 18)
#define TB_ADDBUTTONS       (WM_USER + 20)  /* wp = count, lp = TBBUTTON[] */
#define TB_INSERTBUTTON     (WM_USER + 21)  /* wp = index, lp = TBBUTTON * */
#define TB_DELETEBUTTON     (WM_USER + 22)
#define TB_BUTTONCOUNT      (WM_USER + 24)
#define TB_GETITEMRECT      (WM_USER + 29)
#define TB_SETBUTTONSIZE    (WM_USER + 31)  /* lp = MAKELONG(w, h) */
#define TB_AUTOSIZE         (WM_USER + 33)
#define TB_GETTOOLTIPS      (WM_USER + 35)
#define TB_SETIMAGELIST     (WM_USER + 48)  /* lp = HIMAGELIST -> the old one */

/* Tooltips: a tool is part of a window (rect, in its client coordinates)
 * or all of it (TTF_IDISHWND, uId = the HWND). The text shows by the
 * pointer when it rests on the tool. */
#define TOOLTIPS_CLASS      "tooltips_class32"
#define TTF_IDISHWND        0x0001
#define TTF_SUBCLASS        0x0010      /* (not needed: tooltips watch the pointer themselves) */
#define LPSTR_TEXTCALLBACK  ((char *)-1)    /* ask the window: TTN_GETDISPINFO */
typedef struct {
    unsigned cbSize, uFlags;
    HWND     hwnd;
    unsigned uId;
    RECT     rect;
    char    *lpszText;
} TOOLINFO;
typedef struct { NMHDR hdr; char *lpszText; char szText[80]; int hinst; unsigned uFlags; } NMTTDISPINFO;
#define TTN_GETDISPINFO     (-520)
#define TTM_ACTIVATE        (WM_USER + 1)   /* wp: on */
#define TTM_NEWTOOLRECT     (WM_USER + 6)
#define TTM_RELAYEVENT      (WM_USER + 7)
#define TTM_SETTOOLINFO     (WM_USER + 9)
#define TTM_GETTOOLCOUNT    (WM_USER + 13)
#define TTM_POP             (WM_USER + 28)
#define TTM_ADDTOOL         (WM_USER + 50)  /* lp = TOOLINFO * */
#define TTM_DELTOOL         (WM_USER + 51)
#define TTM_UPDATETIPTEXT   (WM_USER + 57)
bool    ScreenToClient(HWND hwnd, POINT *pt);
bool    ClientToScreen(HWND hwnd, POINT *pt);
HWND    WindowFromPoint(POINT pt);          /* the deepest visible window there */

/* Image lists: pictures of one size (16x16 small icons, 32x32 large ones)
 * for tree and list views, one byte per pixel in the theme's colours, FFh
 * transparent. In the process's own heap. SHELL.DLL's ImageList_AddIcon
 * adds a stock icon. */
typedef struct imagelist *HIMAGELIST;
#define ILD_NORMAL          0x0000
#define ILD_SELECTED        0x0004      /* blended with the selection colour */
#define ILD_DISABLED        0x0100      /* grey, embossed (a disabled button's) */
#define ILD_BLEND25         0x0002      /* faded with white (cut items) */
HIMAGELIST ImageList_Create(int cx, int cy, unsigned flags, int initial, int grow);
bool       ImageList_Destroy(HIMAGELIST il);
int        ImageList_AddPixels(HIMAGELIST il, const uint8_t *pixels);   /* cx * cy bytes -> index */
int        ImageList_GetImageCount(HIMAGELIST il);
bool       ImageList_GetIconSize(HIMAGELIST il, int *cx, int *cy);
bool       ImageList_Draw(HIMAGELIST il, int i, HDC dc, int x, int y, unsigned style);

/* Tree view: items under items, expanded and collapsed with the +/- boxes
 * (TVS_HASBUTTONS), joined by dotted lines (TVS_HASLINES), each with an
 * image and a selected image from its image list. Items live in the
 * owning process's heap; an HTREEITEM is a pointer to one. cChildren = 1
 * gives an item a + box before it has children: the program fills them in
 * when it hears TVN_ITEMEXPANDING (nonzero keeps it collapsed). Keys: the
 * arrows (Left closes or goes up, Right opens or goes down), Home, End,
 * PgUp, PgDn, Enter (NM_RETURN), '+', '-', '*' (all below), and a letter
 * goes to the next item starting with it. */
#define WC_TREEVIEW         "SysTreeView32"
#define TVS_HASBUTTONS      0x0001
#define TVS_HASLINES        0x0002
#define TVS_LINESATROOT     0x0004
#define TVS_SHOWSELALWAYS   0x0020      /* the selection stays (grey) without the focus */
#define TVS_EDITLABELS      0x0008      /* F2 (or TVM_EDITLABEL) edits a label in place */
#define TVS_CHECKBOXES      0x0100      /* a check box before each item: state image 1 / 2 */
typedef struct tree_item *HTREEITEM;
#define TVI_ROOT            ((HTREEITEM)(uintptr_t)0xFFFF0000u)
#define TVI_FIRST           ((HTREEITEM)(uintptr_t)0xFFFF0001u)
#define TVI_LAST            ((HTREEITEM)(uintptr_t)0xFFFF0002u)
#define TVI_SORT            ((HTREEITEM)(uintptr_t)0xFFFF0003u)
#define TVIF_TEXT           0x0001
#define TVIF_IMAGE          0x0002
#define TVIF_PARAM          0x0004
#define TVIF_STATE          0x0008
#define TVIF_HANDLE         0x0010
#define TVIF_SELECTEDIMAGE  0x0020
#define TVIF_CHILDREN       0x0040
#define TVIS_SELECTED       0x0002
#define TVIS_BOLD           0x0010
#define TVIS_EXPANDED       0x0020
#define TVIS_DROPHILITED    0x0008      /* a drop target under a drag (TVGN_DROPHILITE) */
#define TVIS_STATEIMAGEMASK 0xF000
typedef struct {
    unsigned  mask;                     /* TVIF_* */
    HTREEITEM hItem;
    unsigned  state, stateMask;         /* TVIS_* */
    char     *pszText;
    int       cchTextMax;               /* TVM_GETITEM: the buffer's size */
    int       iImage, iSelectedImage;
    int       cChildren;                /* 1: it has (or will have) children */
    LPARAM    lParam;
} TVITEM;
typedef struct { HTREEITEM hParent, hInsertAfter; TVITEM item; } TVINSERTSTRUCT;
typedef struct { POINT pt; unsigned flags; HTREEITEM hItem; } TVHITTESTINFO;
#define TVHT_NOWHERE        0x0001
#define TVHT_ONITEMICON     0x0002
#define TVHT_ONITEMLABEL    0x0004
#define TVHT_ONITEM         (TVHT_ONITEMICON | TVHT_ONITEMLABEL)
#define TVHT_ONITEMINDENT   0x0008
#define TVHT_ONITEMBUTTON   0x0010
#define TVHT_ONITEMSTATEICON 0x0040
#define TV_FIRST            0x1100
#define TVM_INSERTITEM      (TV_FIRST + 0)      /* lp = TVINSERTSTRUCT * -> HTREEITEM */
#define TVM_DELETEITEM      (TV_FIRST + 1)      /* lp = item (TVI_ROOT: all), with its children */
#define TVM_EXPAND          (TV_FIRST + 2)      /* wp = TVE_*, lp = item */
#define TVM_GETITEMRECT     (TV_FIRST + 4)      /* wp = text only, lp = RECT * holding the item */
#define TVM_GETCOUNT        (TV_FIRST + 5)
#define TVM_GETIMAGELIST    (TV_FIRST + 8)
#define TVM_SETIMAGELIST    (TV_FIRST + 9)      /* lp = HIMAGELIST -> the old one */
#define TVM_GETNEXTITEM     (TV_FIRST + 10)     /* wp = TVGN_*, lp = item */
#define TVM_SELECTITEM      (TV_FIRST + 11)     /* wp = TVGN_CARET, lp = item (NULL: none) */
#define TVM_GETITEM         (TV_FIRST + 12)     /* lp = TVITEM * (hItem set) */
#define TVM_SETITEM         (TV_FIRST + 13)
#define TVM_HITTEST         (TV_FIRST + 17)     /* lp = TVHITTESTINFO * -> item */
#define TVM_SORTCHILDREN    (TV_FIRST + 19)     /* lp = item: by text, any case */
#define TVM_ENSUREVISIBLE   (TV_FIRST + 20)     /* lp = item: its parents opened, scrolled to */
#define TVM_EDITLABEL       (TV_FIRST + 14)     /* lp = item -> the edit control */
#define TVM_ENDEDITLABELNOW (TV_FIRST + 22)     /* wp = cancel */
#define TVM_GETITEMSTATE    (TV_FIRST + 39)     /* wp = item, lp = mask */
#define TreeView_GetCheckState(h, it) ((((unsigned)SendMessage((h), TVM_GETITEMSTATE, (WPARAM)(it), TVIS_STATEIMAGEMASK)) >> 12) == 2)
#define TVE_COLLAPSE        0x0001
#define TVE_EXPAND          0x0002
#define TVE_TOGGLE          0x0003
#define TVE_COLLAPSERESET   0x8000              /* ... and its children deleted */
#define TVGN_ROOT           0x0000
#define TVGN_NEXT           0x0001
#define TVGN_PREVIOUS       0x0002
#define TVGN_PARENT         0x0003
#define TVGN_CHILD          0x0004
#define TVGN_FIRSTVISIBLE   0x0005
#define TVGN_NEXTVISIBLE    0x0006
#define TVGN_PREVIOUSVISIBLE 0x0007
#define TVGN_CARET          0x0009
#define TVGN_DROPHILITE     0x0008      /* (TVM_SELECTITEM) the drop target, NULL: none */
#define TVC_UNKNOWN         0x0000
#define TVC_BYMOUSE         0x0001
#define TVC_BYKEYBOARD      0x0002
#define TVN_SELCHANGING     (-401)      /* nonzero keeps the old selection */
#define TVN_SELCHANGED      (-402)
#define TVN_ITEMEXPANDING   (-405)      /* nonzero: it doesn't open (or close) */
#define TVN_ITEMEXPANDED    (-406)
#define TVN_DELETEITEM      (-409)      /* itemOld: the item going (its lParam to free) */
#define TVN_KEYDOWN         (-412)
#define TVN_BEGINDRAG       (-407)      /* NMTREEVIEW itemNew, ptDrag: an item dragged */
#define TVN_BEGINLABELEDIT  (-410)      /* NMTVDISPINFO; nonzero: no editing */
#define TVN_ENDLABELEDIT    (-411)      /* NMTVDISPINFO, item.pszText NULL if cancelled; nonzero keeps it */
typedef struct {
    NMHDR    hdr;
    unsigned action;                    /* TVC_*, or TVE_* for expanding */
    TVITEM   itemOld, itemNew;          /* hItem, state, lParam filled */
    POINT    ptDrag;
} NMTREEVIEW;
typedef struct { NMHDR hdr; TVITEM item; } NMTVDISPINFO;
#define HTREEITEM_OF(nm) (((NMTREEVIEW *)(nm))->itemNew.hItem)

/* List view: items with an image, a label and sub-items, shown four ways
 * (the LVS_TYPEMASK part of the style, changed with SetWindowLong or
 * LVM_SETVIEW): large icons in a grid, labels wrapped below; small icons
 * in rows; a list in columns, scrolled sideways; and details, a row each
 * under column headers (click: LVN_COLUMNCLICK; drag an edge to resize).
 * Several items may be selected (Ctrl, Shift) unless LVS_SINGLESEL. Keys:
 * the arrows in the view's directions, Home, End, PgUp, PgDn, Space,
 * Enter (NM_RETURN, LVN_ITEMACTIVATE), F2 edits the label (LVS_EDITLABELS:
 * LVN_BEGINLABELEDIT, then LVN_ENDLABELEDIT with the new text, nonzero
 * keeps it), a letter goes to the next label starting with it; every key
 * is offered as LVN_KEYDOWN first. */
#define WC_LISTVIEW         "SysListView32"
#define LVS_ICON            0x0000
#define LVS_REPORT          0x0001
#define LVS_SMALLICON       0x0002
#define LVS_LIST            0x0003
#define LVS_TYPEMASK        0x0003
#define LVS_SINGLESEL       0x0004
#define LVS_SHOWSELALWAYS   0x0008
#define LVS_EDITLABELS      0x0200
#define LVS_OWNERDATA       0x1000  /* a virtual list: LVM_SETITEMCOUNT, texts from LVN_GETDISPINFO */
#define LVS_NOCOLUMNHEADER  0x4000
#define LV_VIEW_ICON        0
#define LV_VIEW_DETAILS     1
#define LV_VIEW_SMALLICON   2
#define LV_VIEW_LIST        3
#define LVIF_TEXT           0x0001
#define LVIF_IMAGE          0x0002
#define LVIF_PARAM          0x0004
#define LVIF_STATE          0x0008
#define LVIS_FOCUSED        0x0001
#define LVIS_SELECTED       0x0002
#define LVIS_CUT            0x0004  /* drawn faded (cut, waiting to be pasted) */
#define LVIS_DROPHILITED    0x0008  /* a drop target under a drag */
#define LVIS_STATEIMAGEMASK 0xF000  /* LVS_EX_CHECKBOXES: 1 unchecked, 2 checked */
#define INDEXTOSTATEIMAGEMASK(i) ((unsigned)(i) << 12)
#define LVHT_ONITEMSTATEICON 0x0008
#define LVCFMT_JUSTIFYMASK  0x0003
#define LVCFMT_SORTDOWN     0x0200  /* an arrow in the header (as the header's HDF_SORT*) */
#define LVCFMT_SORTUP       0x0400
#define LVS_EX_GRIDLINES    0x0001
#define LVS_EX_CHECKBOXES   0x0004
#define LVS_EX_FULLROWSELECT 0x0020
#define LVNI_ALL            0x0000
#define LVNI_FOCUSED        0x0001
#define LVNI_SELECTED       0x0002
#define LVSIL_NORMAL        0
#define LVSIL_SMALL         1
#define LVCF_FMT            0x0001
#define LVCF_WIDTH          0x0002
#define LVCF_TEXT           0x0004
#define LVCF_SUBITEM        0x0008
#define LVCFMT_LEFT         0
#define LVCFMT_RIGHT        1
#define LVCFMT_CENTER       2
#define LVIR_BOUNDS         0
#define LVIR_ICON           1
#define LVIR_LABEL          2
#define LVHT_NOWHERE        0x0001
#define LVHT_ONITEMICON     0x0002
#define LVHT_ONITEMLABEL    0x0004
#define LVHT_ONITEM         (LVHT_ONITEMICON | LVHT_ONITEMLABEL)
typedef struct {
    unsigned mask;                      /* LVIF_* */
    int      iItem, iSubItem;
    unsigned state, stateMask;          /* LVIS_* */
    char    *pszText;
    int      cchTextMax;
    int      iImage;
    LPARAM   lParam;
} LVITEM;
typedef struct {
    unsigned mask;                      /* LVCF_* */
    int      fmt;                       /* LVCFMT_* */
    int      cx;
    char    *pszText;
    int      cchTextMax;
    int      iSubItem;
} LVCOLUMN;
typedef struct { POINT pt; unsigned flags; int iItem, iSubItem; } LVHITTESTINFO;
typedef int (*PFNLVCOMPARE)(LPARAM a, LPARAM b, LPARAM sort);
#define LVM_FIRST           0x1000
#define LVM_GETIMAGELIST    (LVM_FIRST + 2)     /* wp = LVSIL_* */
#define LVM_SETIMAGELIST    (LVM_FIRST + 3)     /* wp = LVSIL_*, lp = HIMAGELIST -> the old one */
#define LVM_GETITEMCOUNT    (LVM_FIRST + 4)
#define LVM_GETITEM         (LVM_FIRST + 5)     /* lp = LVITEM * (iItem, iSubItem set) */
#define LVM_SETITEM         (LVM_FIRST + 6)
#define LVM_INSERTITEM      (LVM_FIRST + 7)     /* lp = LVITEM * -> index */
#define LVM_DELETEITEM      (LVM_FIRST + 8)     /* wp = index */
#define LVM_DELETEALLITEMS  (LVM_FIRST + 9)
#define LVM_GETNEXTITEM     (LVM_FIRST + 12)    /* wp = after (-1: from the start), lp = LVNI_* */
#define LVM_GETITEMRECT     (LVM_FIRST + 14)    /* wp = index, lp = RECT * (left = LVIR_*) */
#define LVM_HITTEST         (LVM_FIRST + 18)    /* lp = LVHITTESTINFO * -> index or -1 */
#define LVM_ENSUREVISIBLE   (LVM_FIRST + 19)    /* wp = index */
#define LVM_EDITLABEL       (LVM_FIRST + 23)    /* wp = index -> the edit control */
#define LVM_GETCOLUMN       (LVM_FIRST + 25)    /* wp = index, lp = LVCOLUMN * */
#define LVM_SETCOLUMN       (LVM_FIRST + 26)
#define LVM_INSERTCOLUMN    (LVM_FIRST + 27)    /* wp = index, lp = LVCOLUMN * -> index */
#define LVM_DELETECOLUMN    (LVM_FIRST + 28)
#define LVM_GETCOLUMNWIDTH  (LVM_FIRST + 29)
#define LVM_SETCOLUMNWIDTH  (LVM_FIRST + 30)    /* wp = index, lp = width */
#define LVM_SETITEMSTATE    (LVM_FIRST + 43)    /* wp = index (-1: all), lp = LVITEM * (state, stateMask) */
#define LVM_GETITEMSTATE    (LVM_FIRST + 44)    /* wp = index, lp = mask */
#define LVM_GETITEMTEXT     (LVM_FIRST + 45)    /* wp = index, lp = LVITEM * (iSubItem, pszText, cchTextMax) */
#define LVM_SETITEMTEXT     (LVM_FIRST + 46)    /* wp = index, lp = LVITEM * (iSubItem, pszText) */
#define LVM_SORTITEMS       (LVM_FIRST + 48)    /* wp = sort, lp = PFNLVCOMPARE (lParams compared) */
#define LVM_GETSELECTEDCOUNT (LVM_FIRST + 50)
#define LVM_SETVIEW         (LVM_FIRST + 142)   /* wp = LV_VIEW_* */
#define LVM_GETVIEW         (LVM_FIRST + 143)
#define LVM_ARRANGE         (LVM_FIRST + 22)    /* icon views: back on the grid */
#define LVM_SETITEMPOSITION (LVM_FIRST + 15)    /* wp = index, lp = MAKELPARAM(x, y): icon views */
#define LVM_GETITEMPOSITION (LVM_FIRST + 16)    /* wp = index, lp = POINT * */
#define LVM_SETITEMCOUNT    (LVM_FIRST + 47)    /* LVS_OWNERDATA: wp = how many */
#define LVM_SETEXTENDEDLISTVIEWSTYLE (LVM_FIRST + 54)   /* wp = mask (0: all), lp = LVS_EX_* */
#define LVM_GETEXTENDEDLISTVIEWSTYLE (LVM_FIRST + 55)
#define LVN_BEGINDRAG       (-109)      /* NMLISTVIEW: items dragged; nonzero: the parent takes it over */
#define LVN_GETDISPINFO     (-150)      /* LVS_OWNERDATA: NMLVDISPINFO, fill in item.pszText, iImage */
#define ListView_GetCheckState(h, i) ((((unsigned)SendMessage((h), LVM_GETITEMSTATE, (i), LVIS_STATEIMAGEMASK)) >> 12) == 2)
#define LVN_ITEMCHANGED     (-101)
#define LVN_DELETEITEM      (-103)
#define LVN_COLUMNCLICK     (-108)
#define LVN_ITEMACTIVATE    (-114)
#define LVN_KEYDOWN         (-155)      /* NMLVKEYDOWN */
#define LVN_BEGINLABELEDIT  (-175)      /* NMLVDISPINFO; nonzero: no editing */
#define LVN_ENDLABELEDIT    (-176)      /* NMLVDISPINFO, item.pszText NULL if cancelled; nonzero keeps it */
typedef struct {
    NMHDR    hdr;
    int      iItem, iSubItem;
    unsigned uNewState, uOldState, uChanged;
    POINT    ptAction;
    LPARAM   lParam;
} NMLISTVIEW;
typedef NMLISTVIEW NMITEMACTIVATE;
typedef struct { NMHDR hdr; uint16_t wVKey; unsigned flags; } NMLVKEYDOWN;
typedef struct { NMHDR hdr; LVITEM item; } NMLVDISPINFO;

/* ShowWindow */
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOWMINIMIZED    2
#define SW_SHOWMAXIMIZED    3
#define SW_SHOWNOACTIVATE   4
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
/* Anchors: a child keeps its distance from the parent's edges it is
 * anchored to as the parent is resized (left and right: it stretches). */
#define ANCHOR_LEFT         0x01
#define ANCHOR_TOP          0x02
#define ANCHOR_RIGHT        0x04
#define ANCHOR_BOTTOM       0x08
#define ANCHOR_ALL          0x0F
bool    SetWindowAnchor(HWND hwnd, unsigned anchors);
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
/* Dialog templates: a dialog and its controls from a table. The DLGPROC
 * gets each message first and returns nonzero if it dealt with it; then
 * WM_CLOSE means Cancel and an OK or Cancel it didn't take ends the dialog.
 * WM_INITDIALOG (wParam = the first tab stop, lParam = init) comes once
 * the controls exist; nonzero gives the first tab stop the focus. Sizes in
 * pixels; the template's w and h are the client area's (with a caption);
 * x = CW_USEDEFAULT centres it on the owner. */
#define WM_INITDIALOG       0x0110
#define CW_USEDEFAULT       ((int)0x80000000)
typedef intptr_t (*DLGPROC)(HWND, unsigned, WPARAM, LPARAM);
typedef struct {
    const char *cls, *text;
    unsigned    style, exstyle;             /* WS_CHILD | WS_VISIBLE are added */
    int         x, y, w, h, id;
} DLGITEM;
typedef struct {
    const char    *title;
    unsigned       style, exstyle;          /* 0: WS_DIALOG */
    int            x, y, w, h;
    int            nitems;
    const DLGITEM *items;
    int            id;                      /* (WS_CHILD dialogs: a page in another window) */
} DLGTEMPLATE;
int     DialogBox(const DLGTEMPLATE *t, HWND owner, DLGPROC proc, LPARAM init);    /* modal -> EndDialog's */
HWND    CreateDialog(const DLGTEMPLATE *t, HWND owner, DLGPROC proc, LPARAM init); /* modeless */
bool    EndDialog(HWND dlg, int result);
LRESULT DefDlgProc(HWND dlg, unsigned msg, WPARAM wp, LPARAM lp);
LRESULT SendDlgItemMessage(HWND dlg, int id, unsigned msg, WPARAM wp, LPARAM lp);
bool    SetDlgItemInt(HWND dlg, int id, int value, bool is_signed);
int     GetDlgItemInt(HWND dlg, int id, bool *ok, bool is_signed);
HWND    GetNextDlgGroupItem(HWND dlg, HWND ctl, bool prev);   /* arrows in a group */
HWND    GetNextDlgTabItem(HWND dlg, HWND ctl, bool prev);
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

/* Fonts: FONT_FIXED is the 8x16 VGA font every DC starts with (edit
 * controls keep it); FONT_GUI is the window-text font, the same glyphs
 * set proportionally (labels, buttons, menus, lists). */
#define FONT_FIXED          0
#define FONT_GUI            1
int     SelectFont(HDC dc, int font);               /* -> the old one */
int     GetTextWidth(HDC dc, const char *s, int n); /* in the DC's font; n -1: all */
int     TextWidthFont(int font, const char *s, int n);
int     SetTextColor(HDC dc, int color);            /* DrawText's (-1: black) -> the old one */
int     SetBkColor(HDC dc, int color);              /* DrawText fills behind (-1: doesn't) */

/* DrawText: text in a rectangle, in the DC's font and colours. '&' marks
 * the next character as the mnemonic (underlined; "&&" is an '&'). */
#define DT_LEFT             0x0000
#define DT_CENTER           0x0001
#define DT_RIGHT            0x0002
#define DT_VCENTER          0x0004  /* (DT_SINGLELINE) */
#define DT_BOTTOM           0x0008  /* (DT_SINGLELINE) */
#define DT_WORDBREAK        0x0010  /* wrap at spaces to the width */
#define DT_SINGLELINE       0x0020
#define DT_EXPANDTABS       0x0040
#define DT_NOCLIP           0x0100
#define DT_CALCRECT         0x0400  /* only measure: r gets the size */
#define DT_NOPREFIX         0x0800  /* '&' is just a character */
#define DT_HIDEPREFIX       0x00100000  /* no underline (the '&' still goes) */
#define DT_END_ELLIPSIS     0x8000  /* cut with "..." */
int     DrawText(HDC dc, const char *s, int n, RECT *r, unsigned flags);   /* -> height */

/* Owner drawing (WM_DRAWITEM, WM_MEASUREITEM): BS_OWNERDRAW buttons,
 * SS_OWNERDRAW statics, LBS_OWNERDRAWFIXED list boxes, CBS_OWNERDRAWFIXED
 * combo boxes and MF_OWNERDRAW menu items are drawn by their parent (the
 * menu's owner), in the same process. hDC is the control's own; rcItem is
 * in its coordinates. */
#define ODT_MENU            1
#define ODT_LISTBOX         2
#define ODT_COMBOBOX        3
#define ODT_BUTTON          4
#define ODT_STATIC          5
#define ODA_DRAWENTIRE      0x0001
#define ODA_SELECT          0x0002
#define ODA_FOCUS           0x0004
#define ODS_SELECTED        0x0001
#define ODS_GRAYED          0x0002
#define ODS_DISABLED        0x0004
#define ODS_CHECKED         0x0008
#define ODS_FOCUS           0x0010
#define ODS_COMBOBOXEDIT    0x1000
typedef struct {
    unsigned CtlType, CtlID, itemID, itemAction, itemState;
    HWND     hwndItem;                  /* the control (menus: the HMENU, cast) */
    HDC      hDC;
    RECT     rcItem;
    LPARAM   itemData;
} DRAWITEMSTRUCT;
typedef struct {
    unsigned CtlType, CtlID, itemID;
    unsigned itemWidth, itemHeight;     /* filled in by the parent */
    LPARAM   itemData;
} MEASUREITEMSTRUCT;

/* Windows of every process (the shell's view) */
bool    EnumWindows(WNDENUMPROC proc, LPARAM l);    /* top-level, top first */
HWND    FindWindow(const char *cls, const char *title);   /* either may be NULL */
int     GetWindowProcessId(HWND hwnd);
bool    IsWindowVisible(HWND hwnd);
bool    IsIconic(HWND hwnd);                        /* minimized */
bool    IsZoomed(HWND hwnd);                        /* maximized */
bool    IsHungAppWindow(HWND hwnd);                 /* its program hasn't read its messages for 5 s */
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
COLORREF GetThemeColor(int index);                  /* a theme colour's RGB */
void    FillRectRGB(HDC dc, const RECT *r, uint8_t red, uint8_t green, uint8_t blue);

/* The clipboard: text or a picture, shared by every process */
bool    SetClipboardText(const char *text, int len);        /* len -1: up to the NUL */
int     GetClipboardText(char *buf, int max);               /* -> length copied */
#define CF_TEXT             1
#define CF_BITMAP           2
#define CF_FILES            3       /* Explorer's: file paths (as CF_HDROP) */
bool    SetClipboardFiles(const char *paths, bool cut);    /* "a\0b\0\0", full paths */
int     GetClipboardFiles(char *out, int size, bool *cut); /* -> bytes (with the last NUL) */
void    EmptyClipboard(void);
bool    SetClipboardBitmap(int w, int h, const uint8_t *pixels);   /* theme colours, 1 byte a pixel; up to 640x480 */
bool    GetClipboardBitmap(int *w, int *h, uint8_t *out, int room);
bool    IsClipboardFormatAvailable(unsigned format);
int     GetClipboardTextLength(void);
HCURSOR LoadCursor(void *instance, int idc);         /* instance NULL: IDC_* */
HCURSOR SetCursor(HCURSOR cursor);                  /* -> the old one */
HCURSOR GetCursor(void);
bool    GetCursorPos(POINT *pt);                    /* screen coordinates */
HCURSOR SetWindowCursor(HWND hwnd, HCURSOR cursor); /* its own shape (0: the class's) */
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
       ICON_COMPUTER, ICON_INFO, ICON_SHUTDOWN, ICON_NOTEPAD, ICON_FOLDER, ICON_FOLDER_OPEN,
       ICON_DRIVE, ICON_FLOPPY, ICON_DOCUMENT, ICON_TEXT, ICON_EXPLORER, ICON_COUNT };
void    DrawIcon(HDC dc, int x, int y, int icon);
bool    GetIconPixels(int icon, int size, uint8_t *out);   /* 16 or 32 square, 1 byte a pixel, FFh clear */

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
    const char *types;                  /* the "Files of type" list, as Windows' lpstrFilter:
                                           "Bitmaps (*.BMP)\0*.BMP\0All files\0*.*\0" (and
                                           a final \0); NULL: just filter */
    int         type_index;             /* in/out: which (1 the first; 0 as 1) */
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

/* The colour dialog: the theme's 16 colours, 16 custom ones (cc->custom,
 * kept by the program; white slots are free) and RGB sliders. rgb is
 * the answer; index the theme colour nearest it. */
#define CC_RGBINIT          0x0001  /* start at rgb (else at theme colour index) */
#define CC_FULLOPEN         0x0002  /* with the sliders showing */
#define CC_PREVENTFULLOPEN  0x0004
typedef struct {
    HWND      owner;
    COLORREF  rgb;
    int       index;
    COLORREF *custom;                   /* 16, or NULL */
    uint32_t  flags;
} CHOOSECOLOR;
bool    ChooseColor(CHOOSECOLOR *cc);
void    DrawSmallIcon(HDC dc, int x, int y, int icon);
int     ImageList_AddIcon(HIMAGELIST il, int icon);  /* a stock icon, at the list's size -> index */

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
bool    InsertMenu(HMENU menu, unsigned item, unsigned flags, uintptr_t id, const char *text);  /* before item */
bool    DeleteMenu(HMENU menu, unsigned item, unsigned flags);   /* a submenu is destroyed */
bool    RemoveMenu(HMENU menu, unsigned item, unsigned flags);   /* ... kept */
bool    ModifyMenu(HMENU menu, unsigned item, unsigned flags, uintptr_t id, const char *text);
unsigned GetMenuItemID(HMENU menu, int pos);                     /* -1: a submenu or none */
int     GetMenuString(HMENU menu, unsigned item, char *buf, int max, unsigned flags);
int     GetMenuState(HMENU menu, unsigned item, unsigned flags); /* MF_* (-1: none) */
bool    SetMenuItemImage(HMENU menu, unsigned item, unsigned flags, const uint8_t *pixels, int size);
bool    SetMenuBanner(HMENU menu, const char *text, uint8_t bg); /* up the left side */
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
#define MB_SYSTEMMODAL      0x1000  /* every program's windows wait; the box stays on top */
#define MB_TASKMODAL        0x2000  /* every window of this program waits */
#define MB_TOPMOST          0x40000
bool    MessageBeep(unsigned type);                 /* MB_ICON*: the PC speaker */
int     MessageBox(HWND owner, const char *text, const char *caption, unsigned type);  /* -> IDOK... */

/* Small runtime helpers (each module has its own copy: user/lib/lib.c) */
#include <string.h>          /* the C runtime's (user/crt/include) */

#endif
