/*
 * os.h -- the VMM's INT 30h system calls, for ring-3 programs.
 */
#ifndef OS1993_USER_OS_H
#define OS1993_USER_OS_H

#include <stddef.h>
#include <stdint.h>

#define USER_SIDE
#include "user.h"

static inline uint32_t syscall3(uint32_t n, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t r;
    __asm__ volatile("int $0x30" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

size_t strlen(const char *s);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);

static inline void sys_print(const char *s)  { syscall3(SYS_PRINT, (uint32_t)s, (uint32_t)strlen(s), 0); }
static inline uint32_t sys_ticks(void)       { return syscall3(SYS_TICKS, 0, 0, 0); }
static inline void sys_yield(void)           { syscall3(SYS_YIELD, 0, 0, 0); }
static inline void sys_wait_tick(void)       { syscall3(SYS_WAIT_TICK, 0, 0, 0); }
static inline int  sys_get_key(void)         { return (int)syscall3(SYS_GET_KEY, 0, 0, 0); }
static inline int  sys_new_session(void)     { return (int)syscall3(SYS_NEW_SESSION, 0, 0, 0); }
static inline int  sys_get_event(struct user_event *e) { return (int)syscall3(SYS_GET_EVENT, (uint32_t)e, 0, 0); }
static inline int  sys_mouse(void)           { return (int)syscall3(SYS_MOUSE, 0, 0, 0); }
__attribute__((noreturn))
static inline void sys_exit(int code)
{
    syscall3(SYS_EXIT, (uint32_t)code, 0, 0);
    for (;;)
        ;
}

#endif
