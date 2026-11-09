/*
 * user/krnl/krnl.c -- KERNEL.DLL, the userland kernel library.
 *
 * The only ring-3 module that knows about INT 30h: every other module calls
 * these functions, so the system call interface can change without
 * rebuilding programs (the job KERNEL.EXE did for Win16 programs).
 */
#include "os1993.h"
#include "syscall.h"

#include <stdarg.h>

void ExitProcess(int code)                  { sys_exit(code); }
void OutputDebugString(const char *s)       { sys_print(s); }
uint32_t GetTickCount(void)                 { return sys_ticks() * 55; }  /* 18.2 Hz */
const struct user_info *GetStartupInfo(void){ return (const struct user_info *)USER_INFO; }
int  StartDosSession(void)                  { return sys_new_session(); }
bool GetInputEvent(struct user_event *e)    { return sys_get_event(e) == 1; }
bool HasMouse(void)                         { return sys_mouse() != 0; }
static void (*before_wait)(void), (*after_wait)(void);

/* Loading takes a while (the VMM reads the files through DOS): let go of
 * any lock meanwhile, as when waiting. */
int CreateProcess(const char *program, const char *args)
{
    if (before_wait) before_wait();
    int pid = sys_exec(program, args);
    if (after_wait) after_wait();
    return pid;
}
int  GetCurrentProcessId(void)              { return sys_getpid(); }
int  EnumProcesses(PROCESSENTRY *list, int max) { return sys_proc_list(list, max); }
void TaskListShown(void)                    { sys_tasklist_ack(); }
bool IsProcessAlive(int pid)                { return sys_alive(pid) == 1; }
bool TerminateProcess(int pid)              { return sys_kill(pid) == 0; }
uint32_t GetProcessExitCount(void)          { return sys_exits(); }
bool WakeProcess(int pid)                   { return sys_wake(pid) == 0; }
bool AcquireLock(int n)                     { return sys_lock(n) == 0; }
bool ReleaseLock(int n)                     { return sys_unlock(n) == 0; }
void GetLocalTime(SYSTEMTIME *t)            { sys_time(t); }

/* A DLL that holds a lock across calls (UI.DLL's window manager lock) lets
 * go of it whenever its process is about to sleep. */
void SetWaitHooks(void (*before)(void), void (*after)(void))
{
    before_wait = before;
    after_wait = after;
}

bool WaitForWake(uint32_t ms, unsigned flags)
{
    if (before_wait) before_wait();
    bool woken = sys_wait(ms, flags) == 1;
    if (after_wait) after_wait();
    return woken;
}

void WaitForInput(void)                     { WaitForWake(55, WAIT_INPUT); }

void Sleep(uint32_t ms)
{
    if (!ms) {
        if (before_wait) before_wait();
        sys_yield();
        if (after_wait) after_wait();
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

static uint32_t last_error;

uint32_t GetLastError(void)        { return last_error; }
void     SetLastError(uint32_t e)  { last_error = e; }

/* The VMM does it through DOS, which takes a while: let go of locks. */
static int file_call(uint32_t op, struct file_args *a)
{
    if (before_wait) before_wait();
    int rc = sys_file(op, a);
    if (after_wait) after_wait();
    last_error = rc < 0 ? (uint32_t)-rc : 0;
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

/* ---- memory: a first-fit heap over SYS_SBRK ------------------------------- */

struct block { uint32_t size; uint32_t free; };   /* size includes the header */

#define GROW (16u * 1024)

static struct block *heap_first, *heap_end;

static struct block *next_block(struct block *b) { return (struct block *)((uint8_t *)b + b->size); }

static bool grow(uint32_t need)
{
    uint32_t n = need > GROW ? (need + GROW - 1) / GROW * GROW : GROW;
    uint32_t old = sys_sbrk(n);
    if (!old)
        return false;
    struct block *b = (struct block *)old;
    if (!heap_first)
        heap_first = b;
    b->size = n;
    b->free = 1;
    heap_end = next_block(b);
    /* merge with a free block just before it */
    for (struct block *p = heap_first; p != b; p = next_block(p))
        if (next_block(p) == b && p->free) {
            p->size += b->size;
            break;
        }
    return true;
}

void *LocalAlloc(unsigned flags, uint32_t size)
{
    if (size > 0x3FFFFFFF)
        return NULL;
    uint32_t need = (size + sizeof(struct block) + 7) & ~7u;
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = heap_first; b && b != heap_end; b = next_block(b)) {
            if (!b->free || b->size < need)
                continue;
            if (b->size - need >= 32) {                 /* split */
                struct block *rest = (struct block *)((uint8_t *)b + need);
                rest->size = b->size - need;
                rest->free = 1;
                b->size = need;
            }
            b->free = 0;
            void *p = b + 1;
            if (flags & LMEM_ZEROINIT)
                memset(p, 0, b->size - sizeof *b);
            return p;
        }
        if (!grow(need))
            return NULL;
    }
    return NULL;
}

void *LocalFree(void *p)
{
    if (!p)
        return NULL;
    ((struct block *)p - 1)->free = 1;
    for (struct block *b = heap_first; b && b != heap_end; b = next_block(b))
        while (b->free && next_block(b) != heap_end && next_block(b)->free)
            b->size += next_block(b)->size;             /* coalesce */
    return NULL;
}

uint32_t LocalSize(void *p)
{
    return p ? ((struct block *)p - 1)->size - sizeof(struct block) : 0;
}

void *LocalReAlloc(void *p, uint32_t size, unsigned flags)
{
    if (!p)
        return LocalAlloc(flags, size);
    uint32_t have = LocalSize(p);
    if (size <= have)
        return p;
    void *q = LocalAlloc(flags, size);
    if (!q)
        return NULL;
    memcpy(q, p, have);
    LocalFree(p);
    return q;
}
