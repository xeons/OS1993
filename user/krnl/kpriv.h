/*
 * kpriv.h -- inside KERNEL.DLL.
 */
#ifndef OS1993_KPRIV_H
#define OS1993_KPRIV_H

#include "os1993.h"
#include "syscall.h"

/* This thread's information block (FS:0). */
static inline struct user_tib *tib(void)
{
    struct user_tib *t;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(t));
    return t;
}

static inline bool on_main_thread(void) { return (tib()->flags & TIB_MAIN) != 0; }

/* Around anything that may sleep: UI.DLL lets go of the window system's
 * lock (only the first thread uses the window system). */
void k_before_wait(void);
void k_after_wait(void);

static inline void k_error(uint32_t e) { tib()->last_error = e; }

/* Handles: files are small numbers from the VMM; the rest are tagged. */
#define H_TAG(h)    ((uint32_t)(h) & 0xF0000000u)
#define H_ID(h)     ((uint32_t)(h) & 0x0FFFFFFFu)
#define H_THREAD    0x40000000u
#define H_PROCESS   0x50000000u
#define H_OBJECT    0x60000000u
#define H_HEAP      0x70000000u
bool k_close_object(HANDLE h);              /* thread.c: a tagged handle */

/* thread.c: signalled things wake WaitForMultipleObjects */
void k_signal(void);

#endif
