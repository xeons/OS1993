/*
 * errno.c -- errno, and the C runtime's locks.
 */
#include "crt.h"

static int errno_value;

int *__errno(void) { return &errno_value; }

void __crt_lock(int n)   { (void)n; }
void __crt_unlock(int n) { (void)n; }
