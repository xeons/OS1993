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
