/*
 * crt.h -- inside the C runtime (not for programs).
 */
#ifndef OS1993_CRT_H
#define OS1993_CRT_H

#define __CRT_BUILD 1                   /* no _FORTIFY_SOURCE macros on our own names */
#include <crtdefs.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef USER_SIDE
#define USER_SIDE
#endif
#include "user.h"                       /* SYS_FAIL, FAIL_* */

/* End the process now: it found its own memory corrupted, so nothing in
 * it can be trusted to clean up (as Win32's __fastfail, a trap of its own
 * rather than a call into KERNEL.DLL). */
__attribute__((noreturn, cold))
static inline void __crt_fail(int reason, const char *msg)
{
    __asm__ volatile("int $0x30" : : "a"(SYS_FAIL), "b"(reason), "c"(msg) : "memory");
    __builtin_unreachable();
}

/* Annex K: a broken constraint (safe.c). */
void __crt_constraint(const char *msg, errno_t e);

/* Locks for what a process's threads share (the heap, the open files,
 * atexit's list): critical sections, recursive (lock.c). */
enum { CRT_LOCK_HEAP, CRT_LOCK_FILES, CRT_LOCK_EXIT, CRT_LOCKS };
void __crt_lock(int n);
void __crt_unlock(int n);

/* printf's engine (printf.c): output goes to put(ctx, s, n). */
#define FMT_SECURE  1                   /* the _s rules: null %s is a broken constraint */
struct fmt_out {
    void  (*put)(struct fmt_out *o, const char *s, size_t n);
    size_t  count;                      /* characters produced (stored or not) */
};
int __crt_format(struct fmt_out *o, const char *fmt, va_list ap, unsigned flags);

/* KERNEL.DLL's error code -> errno */
int __crt_errno_from_win(uint32_t err);

#endif
