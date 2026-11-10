/*
 * ssp.c -- the stack protector's canary and its failure, linked into every
 * module (each has its own canary: data can't be imported from a DLL).
 *
 * The module header points at __stack_chk_guard and the loader stores a
 * random value there for each process (its low byte 0, so a string copy
 * can't reproduce it). Compiled without the stack protector itself.
 */
#include "crt.h"

uint32_t __stack_chk_guard;

__attribute__((noreturn)) void __stack_chk_fail(void)
{
    __crt_fail(FAIL_STACK_OVERRUN, NULL);
}

__attribute__((noreturn, visibility("hidden"))) void __stack_chk_fail_local(void)
{
    __crt_fail(FAIL_STACK_OVERRUN, NULL);
}
