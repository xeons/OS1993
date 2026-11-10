/*
 * lock.c -- the C runtime's locks, for what a process's threads share
 * (the open files, atexit's list): KERNEL.DLL's critical sections, which
 * cost no system call unless a thread has to wait.
 */
#include "crt.h"
#include "os1993.h"

static CRITICAL_SECTION locks[CRT_LOCKS];          /* all zero: ready */

void __crt_lock(int n)   { EnterCriticalSection(&locks[n]); }
void __crt_unlock(int n) { LeaveCriticalSection(&locks[n]); }
