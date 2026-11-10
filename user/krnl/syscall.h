/*
 * syscall.h -- the VMM's INT 30h system calls. Only KERNEL.DLL uses these.
 */
#ifndef OS1993_USER_OS_H
#define OS1993_USER_OS_H

#include <stddef.h>
#include <stdint.h>

#ifndef USER_SIDE
#define USER_SIDE
#endif
#include "user.h"

static inline uint32_t syscall3(uint32_t n, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t r;
    __asm__ volatile("int $0x30" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

size_t strlen(const char *s);

static inline void sys_print(const char *s)  { syscall3(SYS_PRINT, (uint32_t)s, (uint32_t)strlen(s), 0); }
static inline uint32_t sys_ticks(void)       { return syscall3(SYS_TICKS, 0, 0, 0); }
static inline void sys_yield(void)           { syscall3(SYS_YIELD, 0, 0, 0); }
static inline void sys_wait_tick(void)       { syscall3(SYS_WAIT_TICK, 0, 0, 0); }
static inline int  sys_get_key(void)         { return (int)syscall3(SYS_GET_KEY, 0, 0, 0); }
static inline int  sys_new_session(void)     { return (int)syscall3(SYS_NEW_SESSION, 0, 0, 0); }
static inline int  sys_get_event(struct user_event *e) { return (int)syscall3(SYS_GET_EVENT, (uint32_t)e, 0, 0); }
static inline int  sys_mouse(void)           { return (int)syscall3(SYS_MOUSE, 0, 0, 0); }
static inline int  sys_exec(const char *p, const char *a) { return (int)syscall3(SYS_EXEC, (uint32_t)p, (uint32_t)a, 0); }
static inline int  sys_wait(uint32_t ms, uint32_t f)     { return (int)syscall3(SYS_WAIT, ms, f, 0); }
static inline int  sys_wake(int pid)         { return (int)syscall3(SYS_WAKE, (uint32_t)pid, 0, 0); }
static inline int  sys_getpid(void)          { return (int)syscall3(SYS_GETPID, 0, 0, 0); }
static inline int  sys_alive(int pid)        { return (int)syscall3(SYS_PROC_ALIVE, (uint32_t)pid, 0, 0); }
static inline int  sys_kill(int pid)         { return (int)syscall3(SYS_KILL, (uint32_t)pid, 0, 0); }
static inline int  sys_lock(int n)           { return (int)syscall3(SYS_LOCK, (uint32_t)n, 0, 0); }
static inline int  sys_unlock(int n)         { return (int)syscall3(SYS_UNLOCK, (uint32_t)n, 0, 0); }
static inline int  sys_time(struct systime *t) { return (int)syscall3(SYS_TIME, (uint32_t)t, 0, 0); }
static inline uint32_t sys_exits(void)       { return syscall3(SYS_PROC_EXITS, 0, 0, 0); }
static inline int  sys_tasklist_ack(void)    { return (int)syscall3(SYS_TASKLIST_ACK, 0, 0, 0); }
static inline int  sys_proc_list(struct proc_entry *p, int n) { return (int)syscall3(SYS_PROC_LIST, (uint32_t)p, (uint32_t)n, 0); }
static inline int  sys_display_info(struct display_info *d) { return (int)syscall3(SYS_DISPLAY_INFO, (uint32_t)d, 0, 0); }
static inline int  sys_display_modes(struct display_mode *m, int n) { return (int)syscall3(SYS_DISPLAY_MODES, (uint32_t)m, (uint32_t)n, 0); }
static inline int  sys_display_set(int w, int h, int bpp) { return (int)syscall3(SYS_DISPLAY_SET, (uint32_t)w, (uint32_t)h, (uint32_t)bpp); }
static inline uint32_t sys_sbrk(uint32_t n)  { return syscall3(SYS_SBRK, n, 0, 0); }
static inline int  sys_file(uint32_t op, struct file_args *a) { return (int)syscall3(SYS_FILE, op, (uint32_t)a, 0); }
__attribute__((noreturn))
static inline void sys_exit(int code)
{
    syscall3(SYS_EXIT, (uint32_t)code, 0, 0);
    for (;;)
        ;
}

#endif
