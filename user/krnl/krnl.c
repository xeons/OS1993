/*
 * user/krnl/krnl.c -- KERNEL.DLL, the userland kernel library.
 *
 * The only ring-3 module that knows about INT 30h: every other module calls
 * these functions, so the system call interface can change without
 * rebuilding programs (the job KERNEL.EXE did for Win16 programs).
 */
#include "kpriv.h"

#include <stdarg.h>

void ExitProcess(int code)                  { sys_exit(code); }
void OutputDebugString(const char *s)       { sys_print(s); }
uint32_t GetTickCount(void)                 { return sys_ticks() * 55; }  /* 18.2 Hz */
const struct user_info *GetStartupInfo(void){ return (const struct user_info *)USER_INFO; }
int  StartDosSession(void)                  { return sys_new_session(); }
bool GetInputEvent(struct user_event *e)    { return sys_get_event(e) == 1; }
bool HasMouse(void)                         { return sys_mouse() != 0; }
static void (*before_wait)(void), (*after_wait)(void);

/* Only the first thread uses the window system, so only its waits let go
 * of UI.DLL's lock. */
void k_before_wait(void) { if (before_wait && on_main_thread()) before_wait(); }
void k_after_wait(void)  { if (after_wait && on_main_thread()) after_wait(); }

/* Loading takes a while (the VMM reads the files through DOS): let go of
 * any lock meanwhile, as when waiting. */
int CreateProcess(const char *program, const char *args)
{
    k_before_wait();
    int pid = sys_exec(program, args);
    k_after_wait();
    return pid;
}

HANDLE OpenProcess(uint32_t access, bool inherit, int pid)
{
    (void)access; (void)inherit;
    if (pid <= 0 || (int)syscall3(SYS_PROC_WAIT, (uint32_t)pid, 0, 0) < 0) {
        k_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return (HANDLE)(H_PROCESS | (uint32_t)pid);
}

bool GetExitCodeProcess(HANDLE h, uint32_t *code)
{
    int c = 0, rc;
    if (h == GetCurrentProcess()) {
        *code = STILL_ACTIVE;
        return true;
    }
    if (H_TAG(h) != H_PROCESS || (rc = (int)syscall3(SYS_PROC_WAIT, H_ID(h), 0, (uint32_t)(uintptr_t)&c)) < 0) {
        k_error(ERROR_INVALID_HANDLE);
        return false;
    }
    *code = rc == 0 ? (uint32_t)c : STILL_ACTIVE;
    return true;
}

const char *GetCommandLine(void) { return GetStartupInfo()->args; }

/* The network (SYS_SOCKET), for WSOCK.DLL: a call that may wait lets go of
 * the window system's lock meanwhile. -> the result, or -errno. */
int NetCall(uint32_t op, struct sock_args *a)
{
    bool waits = op == SOP_CONNECT || op == SOP_ACCEPT || op == SOP_SEND || op == SOP_RECV || op == SOP_POLL;
    if (waits)
        k_before_wait();
    int rc = (int)syscall3(SYS_SOCKET, op, (uint32_t)(uintptr_t)a, 0);
    if (waits)
        k_after_wait();
    return rc;
}

uint32_t GetModuleFileName(void *module, char *buf, uint32_t size)
{
    (void)module;                               /* the program's */
    const char *name = GetStartupInfo()->name;
    uint32_t n = 0;
    while (name[n] && n + 1 < size) { buf[n] = name[n]; n++; }
    if (size)
        buf[n] = '\0';
    return n;
}
int  GetCurrentProcessId(void)              { return sys_getpid(); }
int  EnumProcesses(PROCESSENTRY *list, int max) { return sys_proc_list(list, max); }
void TaskListShown(void)                    { sys_tasklist_ack(); }
bool GetDisplayInfo(struct display_info *d) { return sys_display_info(d) == 0; }
int  GetDisplayModes(struct display_mode *m, int max) { return sys_display_modes(m, max); }
bool SetDisplayMode(int w, int h, int bpp)  { return sys_display_set(w, h, bpp) == 0; }
bool GetRandomBytes(void *buf, uint32_t n)  { return sys_random(buf, n) == 0; }
bool IsProcessAlive(int pid)                { return sys_alive(pid) == 1; }
bool TerminateProcess(int pid)              { return sys_kill(pid) == 0; }
uint32_t GetProcessExitCount(void)          { return sys_exits(); }
bool WakeProcess(int pid)                   { return sys_wake(pid) == 0; }
bool AcquireLock(int n)                     { return sys_lock(n) == 0; }
bool ReleaseLock(int n)                     { return sys_unlock(n) == 0; }
void GetLocalTime(SYSTEMTIME *t)            { sys_time(t); }

uint32_t GetTimeZoneInformation(TIME_ZONE_INFORMATION *tz)
{
    memset(tz, 0, sizeof *tz);
    tz->Bias = sys_timezone(0, 0);
    return 0;                                           /* TIME_ZONE_ID_UNKNOWN: no daylight saving */
}

bool SetTimeZoneInformation(const TIME_ZONE_INFORMATION *tz)
{
    if (tz->Bias < -14 * 60 || tz->Bias > 14 * 60) {
        k_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    return sys_timezone(1, tz->Bias) == tz->Bias;
}

/* A DLL that holds a lock across calls (UI.DLL's window manager lock) lets
 * go of it whenever its process is about to sleep. */
void SetWaitHooks(void (*before)(void), void (*after)(void))
{
    before_wait = before;
    after_wait = after;
}

bool WaitForWake(uint32_t ms, unsigned flags)
{
    k_before_wait();
    bool woken = sys_wait(ms, flags) == 1;
    k_after_wait();
    return woken;
}

void WaitForInput(void)                     { WaitForWake(55, WAIT_INPUT); }

void k_thread_sleep(uint32_t ms);                    /* thread.c */

void Sleep(uint32_t ms)
{
    if (!on_main_thread()) {
        k_thread_sleep(ms);
        return;
    }
    if (!ms) {
        k_before_wait();
        sys_yield();
        k_after_wait();
        return;
    }
    uint32_t end = GetTickCount() + ms;
    for (int32_t left; (left = (int32_t)(end - GetTickCount())) > 0; )
        WaitForWake((uint32_t)left, 0);
}

/* A line for the debug log (COM1): %s %d %u %x %c */
void DebugLog(const char *fmt, ...)
{
    char buf[160];
    size_t n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt && n < sizeof buf - 12; fmt++) {
        if (*fmt != '%') { buf[n++] = *fmt; continue; }
        fmt++;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && n < sizeof buf - 12) buf[n++] = *s++;
        } else if (*fmt == 'c') {
            buf[n++] = (char)va_arg(ap, int);
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            int v = va_arg(ap, int);
            unsigned base = *fmt == 'x' ? 16 : 10;
            unsigned u = (*fmt == 'd' && v < 0) ? (unsigned)-v : (unsigned)v;
            char d[12];
            int m = 0;
            do { d[m++] = "0123456789abcdef"[u % base]; u /= base; } while (u);
            if (*fmt == 'd' && v < 0) buf[n++] = '-';
            while (m) buf[n++] = d[--m];
        } else if (!*fmt) {
            break;
        }
    }
    va_end(ap);
    buf[n] = '\0';
    sys_print(buf);
}

/* ---- files ------------------------------------------------------------- */

uint32_t GetLastError(void)        { return tib()->last_error; }   /* each thread's own */
void     SetLastError(uint32_t e)  { tib()->last_error = e; }

/* The VMM does it through DOS, which takes a while: let go of locks. */
static int file_call(uint32_t op, struct file_args *a)
{
    k_before_wait();
    int rc = sys_file(op, a);
    k_after_wait();
    k_error(rc < 0 ? (uint32_t)-rc : 0);
    return rc;
}

HANDLE CreateFile(const char *path, uint32_t access, uint32_t creation)
{
    struct file_args a = { .path = path, .a = access, .b = creation };
    int rc = file_call(FOP_OPEN, &a);
    return rc < 0 ? INVALID_HANDLE_VALUE : rc;
}

bool ReadFile(HANDLE h, void *buf, uint32_t n, uint32_t *done)
{
    struct file_args a = { .handle = (uint32_t)h, .buf = buf, .len = n };
    int rc = file_call(FOP_READ, &a);
    if (done) *done = rc < 0 ? 0 : (uint32_t)rc;
    return rc >= 0;
}

bool WriteFile(HANDLE h, const void *buf, uint32_t n, uint32_t *done)
{
    struct file_args a = { .handle = (uint32_t)h, .buf = (void *)buf, .len = n };
    int rc = file_call(FOP_WRITE, &a);
    if (done) *done = rc < 0 ? 0 : (uint32_t)rc;
    return rc >= 0;
}

bool CloseHandle(HANDLE h)
{
    if (H_TAG(h))                                   /* a thread, process, event ... */
        return k_close_object(h);
    struct file_args a = { .handle = (uint32_t)h };
    return file_call(FOP_CLOSE, &a) >= 0;
}

uint32_t SetFilePointer(HANDLE h, int32_t distance, int method)
{
    struct file_args a = { .handle = (uint32_t)h, .a = (uint32_t)distance, .b = (uint32_t)method };
    int rc = file_call(FOP_SEEK, &a);
    return rc < 0 ? INVALID_SET_FILE_POINTER : (uint32_t)rc;
}

uint32_t GetFileSize(HANDLE h)
{
    struct file_args a = { .handle = (uint32_t)h };
    int rc = file_call(FOP_SIZE, &a);
    return rc < 0 ? 0xFFFFFFFFu : (uint32_t)rc;
}

static bool path_op(uint32_t op, const char *path, const char *path2)
{
    struct file_args a = { .path = path, .path2 = path2 };
    return file_call(op, &a) >= 0;
}

bool DeleteFile(const char *path)                   { return path_op(FOP_DELETE, path, NULL); }
bool MoveFile(const char *from, const char *to)     { return path_op(FOP_RENAME, from, to); }
bool CreateDirectory(const char *path)              { return path_op(FOP_MKDIR, path, NULL); }
bool RemoveDirectory(const char *path)              { return path_op(FOP_RMDIR, path, NULL); }
bool SetCurrentDirectory(const char *path)          { return path_op(FOP_SETCWD, path, NULL); }

uint32_t GetFileAttributes(const char *path)
{
    struct file_args a = { .path = path };
    int rc = file_call(FOP_ATTRIB, &a);
    return rc < 0 ? INVALID_FILE_ATTRIBUTES : (uint32_t)rc;
}

uint32_t GetCurrentDirectory(uint32_t size, char *buf)
{
    struct file_args a = { .buf = buf, .len = size };
    int rc = file_call(FOP_GETCWD, &a);
    return rc < 0 ? 0 : (uint32_t)rc;
}

HANDLE FindFirstFile(const char *pattern, WIN32_FIND_DATA *fd)
{
    struct file_args a = { .path = pattern, .buf = fd };
    int rc = file_call(FOP_FIND_FIRST, &a);
    return rc < 0 ? INVALID_HANDLE_VALUE : rc;
}

bool FindNextFile(HANDLE h, WIN32_FIND_DATA *fd)
{
    struct file_args a = { .handle = (uint32_t)h, .buf = fd };
    return file_call(FOP_FIND_NEXT, &a) >= 0;
}

bool FindClose(HANDLE h)
{
    struct file_args a = { .handle = (uint32_t)h };
    return file_call(FOP_FIND_CLOSE, &a) >= 0;
}

/* ---- more about files ------------------------------------------------------- */

static bool handle_op(uint32_t op, HANDLE h)
{
    struct file_args a = { .handle = (uint32_t)h };
    return file_call(op, &a) >= 0;
}

bool SetEndOfFile(HANDLE h)      { return handle_op(FOP_TRUNCATE, h); }
bool FlushFileBuffers(HANDLE h)  { return handle_op(FOP_FLUSH, h); }

static bool lock_op(uint32_t op, HANDLE h, uint32_t off_lo, uint32_t off_hi, uint32_t len_lo, uint32_t len_hi)
{
    struct file_args a = { .handle = (uint32_t)h, .a = off_lo, .c = off_hi, .b = len_lo, .d = len_hi };
    return file_call(op, &a) >= 0;
}

bool LockFile(HANDLE h, uint32_t off_lo, uint32_t off_hi, uint32_t len_lo, uint32_t len_hi)
{
    return lock_op(FOP_LOCK, h, off_lo, off_hi, len_lo, len_hi);
}

bool UnlockFile(HANDLE h, uint32_t off_lo, uint32_t off_hi, uint32_t len_lo, uint32_t len_hi)
{
    return lock_op(FOP_UNLOCK, h, off_lo, off_hi, len_lo, len_hi);
}

bool CreateHardLink(const char *path, const char *existing, void *security)
{
    (void)security;
    struct file_args a = { .path = existing, .path2 = path };
    return file_call(FOP_LINK, &a) >= 0;
}

bool CreateSymbolicLink(const char *link, const char *target, uint32_t flags)
{
    (void)flags;                                        /* a file or a directory: the same here */
    struct file_args a = { .path = link, .path2 = target };
    return file_call(FOP_SYMLINK, &a) >= 0;
}

bool SetFilePointerEx(HANDLE h, int64_t distance, int64_t *new_pos, int method)
{
    int64_t pos = 0;
    struct file_args a = { .handle = (uint32_t)h, .a = (uint32_t)distance, .c = (uint32_t)((uint64_t)distance >> 32),
                           .b = (uint32_t)method, .buf = &pos };
    if (file_call(FOP_SEEK64, &a) < 0)
        return false;
    if (new_pos)
        *new_pos = pos;
    return true;
}

bool GetFileSizeEx(HANDLE h, int64_t *size)
{
    struct file_args a = { .handle = (uint32_t)h, .buf = size };
    return file_call(FOP_SIZE64, &a) >= 0;
}

bool SetFileAttributes(const char *path, uint32_t attributes)
{
    struct file_args a = { .path = path, .a = attributes };
    return file_call(FOP_SETATTR, &a) >= 0;
}

/* -> the length of the full path, or the size needed if size is too small
 * (0 on an error). *file_part: where its last part starts. */
uint32_t GetFullPathName(const char *path, uint32_t size, char *buf, char **file_part)
{
    struct file_args a = { .path = path, .buf = buf, .len = size };
    int rc = file_call(FOP_FULLPATH, &a);
    if (rc < 0)
        return 0;
    if ((uint32_t)rc >= size)
        return (uint32_t)rc;                        /* the size needed */
    if (file_part) {
        char *last = buf;
        for (char *c = buf; *c; c++)
            if (*c == '\\')
                last = c + 1;
        *file_part = *last ? last : NULL;
    }
    return (uint32_t)rc;
}

bool GetDiskFreeSpace(const char *root, uint32_t *sectors_per_cluster, uint32_t *bytes_per_sector,
                      uint32_t *free_clusters, uint32_t *total_clusters)
{
    struct disk_free df;
    struct file_args a = { .path = root, .buf = &df };
    if (file_call(FOP_DISKFREE, &a) < 0)
        return false;
    if (sectors_per_cluster) *sectors_per_cluster = df.sectors_per_cluster;
    if (bytes_per_sector)    *bytes_per_sector = df.bytes_per_sector;
    if (free_clusters)       *free_clusters = df.free_clusters;
    if (total_clusters)      *total_clusters = df.total_clusters;
    return true;
}

bool CopyFile(const char *from, const char *to, bool fail_if_exists)
{
    HANDLE in = CreateFile(from, GENERIC_READ, OPEN_EXISTING);
    if (in == INVALID_HANDLE_VALUE)
        return false;
    HANDLE out = CreateFile(to, GENERIC_WRITE, fail_if_exists ? CREATE_NEW : CREATE_ALWAYS);
    if (out == INVALID_HANDLE_VALUE) {
        uint32_t e = GetLastError();
        CloseHandle(in);
        k_error(e);
        return false;
    }
    static char buf[2048];                          /* not on a thread's small stack */
    static CRITICAL_SECTION buf_lock;
    bool ok = true;
    EnterCriticalSection(&buf_lock);
    for (;;) {
        uint32_t got, put;
        if (!ReadFile(in, buf, sizeof buf, &got)) { ok = false; break; }
        if (!got)
            break;
        if (!WriteFile(out, buf, got, &put) || put != got) { ok = false; break; }
    }
    LeaveCriticalSection(&buf_lock);
    uint32_t e = GetLastError();
    uint32_t when;
    struct file_args t = { .handle = (uint32_t)in, .buf = &when };
    if (ok && file_call(FOP_GETTIME, &t) >= 0) {    /* the copy keeps the date */
        struct file_args st = { .handle = (uint32_t)out, .a = when };
        file_call(FOP_SETTIME, &st);
    }
    CloseHandle(in);
    CloseHandle(out);
    if (!ok) {
        DeleteFile(to);
        k_error(e ? e : ERROR_ACCESS_DENIED);
    }
    return ok;
}

/* ---- file times: FILETIME is 100 ns units since 1601-01-01 ------------------------- */

/* v / d (d < 2^24), 8 bits at a time: no 64-bit division in libgcc here. */
static uint32_t div64(uint64_t *v, uint32_t d)
{
    uint32_t rem = 0;
    uint64_t q = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
        uint32_t cur = rem << 8 | (uint32_t)(*v >> shift & 0xFF);
        q = q << 8 | cur / d;
        rem = cur % d;
    }
    *v = q;
    return rem;
}

static bool leap(uint32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

bool SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft)
{
    if (st->year < 1601 || st->month < 1 || st->month > 12 || st->day < 1 || st->day > 31 ||
        st->hour > 23 || st->minute > 59 || st->second > 59 || st->ms > 999) {
        k_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    uint32_t days = 0;
    for (uint32_t y = 1601; y < st->year; y++)
        days += leap(y) ? 366 : 365;
    days += before_month[st->month - 1] + (st->month > 2 && leap(st->year)) + st->day - 1u;
    uint64_t t = (uint64_t)days * 86400u + st->hour * 3600u + st->minute * 60u + st->second;
    t = t * 10000000u + (uint64_t)st->ms * 10000u;
    ft->dwLowDateTime = (uint32_t)t;
    ft->dwHighDateTime = (uint32_t)(t >> 32);
    return true;
}

bool FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st)
{
    uint64_t t = (uint64_t)ft->dwHighDateTime << 32 | ft->dwLowDateTime;
    uint32_t sub = div64(&t, 10000);                /* -> ms */
    (void)sub;
    uint32_t ms = div64(&t, 1000);                  /* -> seconds */
    uint32_t secs = div64(&t, 86400);               /* -> days */
    uint32_t days = (uint32_t)t, y = 1601;
    st->day_of_week = (uint16_t)((days + 1) % 7);   /* 1601-01-01 was a Monday */
    while (days >= (leap(y) ? 366u : 365u)) {
        days -= leap(y) ? 366 : 365;
        y++;
    }
    int m = 11;
    while (m > 0 && days < (uint32_t)(before_month[m] + (m > 1 && leap(y))))
        m--;
    *st = (SYSTEMTIME){ .year = (uint16_t)y, .month = (uint16_t)(m + 1), .day_of_week = st->day_of_week,
                        .day = (uint16_t)(days - before_month[m] - (m > 1 && leap(y)) + 1),
                        .hour = (uint16_t)(secs / 3600), .minute = (uint16_t)(secs / 60 % 60),
                        .second = (uint16_t)(secs % 60), .ms = (uint16_t)ms };
    return true;
}

/* DOS keeps the time of the last write only, to 2 seconds. */
bool GetFileTime(HANDLE h, FILETIME *created, FILETIME *accessed, FILETIME *written)
{
    uint32_t v;
    struct file_args a = { .handle = (uint32_t)h, .buf = &v };
    if (file_call(FOP_GETTIME, &a) < 0)
        return false;
    uint32_t date = v & 0xFFFF, time = v >> 16;
    SYSTEMTIME st = { .year = (uint16_t)(1980 + (date >> 9)), .month = (uint16_t)(date >> 5 & 15),
                      .day = (uint16_t)(date & 31), .hour = (uint16_t)(time >> 11),
                      .minute = (uint16_t)(time >> 5 & 63), .second = (uint16_t)((time & 31) * 2) };
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
        return false;
    if (created)  *created = ft;
    if (accessed) *accessed = ft;
    if (written)  *written = ft;
    return true;
}

bool SetFileTime(HANDLE h, const FILETIME *created, const FILETIME *accessed, const FILETIME *written)
{
    (void)created; (void)accessed;
    if (!written)
        return true;
    SYSTEMTIME st;
    FileTimeToSystemTime(written, &st);
    if (st.year < 1980 || st.year > 2107) {
        k_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    uint32_t date = (uint32_t)(st.year - 1980) << 9 | (uint32_t)st.month << 5 | st.day;
    uint32_t time = (uint32_t)st.hour << 11 | (uint32_t)st.minute << 5 | st.second / 2u;
    struct file_args a = { .handle = (uint32_t)h, .a = time << 16 | date };
    return file_call(FOP_SETTIME, &a) >= 0;
}
