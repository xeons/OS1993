/*
 * setjmp.h -- non-local jumps. The saved stack and code pointers are
 * mangled with a per-process secret, so a buffer overrun that reaches a
 * jmp_buf can't aim longjmp anywhere useful.
 */
#ifndef _SETJMP_H
#define _SETJMP_H

#include <crtdefs.h>

typedef unsigned long jmp_buf[6];       /* EBX ESI EDI EBP ESP EIP */

__CRT_BEGIN
int setjmp(jmp_buf env) __attribute__((returns_twice));
__crt_noreturn void longjmp(jmp_buf env, int value);
__CRT_END

#endif
