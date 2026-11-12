/*
 * kdbg.h -- the kernel debugger on COM1 (WDEB386-style), with OS1993 /D.
 */
#ifndef OS1993_KDBG_H
#define OS1993_KDBG_H

#include <stdbool.h>
#include <stdint.h>

#include "bootinfo.h"

struct regs;

void kdbg_init(const struct bootinfo *bi);
bool kdbg_enabled(void);
void kdbg_poll(struct regs *r);             /* timer IRQ: Ctrl+C on COM1 breaks in */
bool kdbg_trap(struct regs *r);             /* #DB / #BP: true if it was ours */
void kdbg_enter(struct regs *r, const char *why);
void kdbg_panic(const char *msg);           /* panic(): a last look before the end */
void kdbg_set_fault_regs(const struct regs *r);  /* the state a double fault left */

#endif
