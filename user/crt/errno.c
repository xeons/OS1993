/*
 * errno.c -- errno: each thread's own, in its TIB (FS:0).
 */
#include "crt.h"

int *__errno(void)
{
    struct user_tib *t;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(t));
    return &t->err;
}
