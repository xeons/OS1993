/*
 * v86.h -- the V86 monitor: running real-mode DOS/BIOS code from the VMM.
 */
#ifndef OS1993_V86_H
#define OS1993_V86_H

#include <stdbool.h>
#include <stdint.h>

#include "bootinfo.h"
#include "interrupts.h"

#define EFLAGS_CF   0x00000001u
#define EFLAGS_TF   0x00000100u
#define EFLAGS_IF   0x00000200u
#define EFLAGS_OF   0x00000800u
#define EFLAGS_IOPL 0x00003000u
#define EFLAGS_NT   0x00004000u
#define EFLAGS_VM   0x00020000u

/* Register image for a real-mode call (in and out), like DPMI 0300h. */
struct v86_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint16_t ds, es;
    uint16_t flags;         /* out: FLAGS after the call (test EFLAGS_CF) */
};

/* Full V86 entry state, consumed by v86_enter (v86entry.asm offsets). */
struct v86_frame {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t eip, cs, eflags, esp, ss;
    uint32_t es, ds, fs, gs;
};

/* v86entry.asm */
int  v86_enter(const struct v86_frame *f);
__attribute__((noreturn)) void v86_leave(int rc);

void v86_init(const struct bootinfo *bi);
void v86_selftest(const struct bootinfo *bi);   /* v86test.c */

/* Simulate INT vec in V86 mode and run until the handler returns.
 * Returns 0, or -1 if the VM crashed (see v86_last_error()). */
int  v86_int(uint8_t vec, struct v86_regs *r);
/* Far-call real-mode code (it must RETF), with interrupts enabled. */
int  v86_call_far(uint16_t seg, uint16_t off, struct v86_regs *r);
const char *v86_last_error(void);
uint32_t v86_delivered(uint8_t vec);    /* hardware IRQs reflected, by vector */

/* Conventional memory the kernel can use for V86 call buffers. */
uint16_t v86_buf_seg(void);
uint16_t v86_buf_off(void);
uint16_t v86_buf_size(void);
static inline void *v86_ptr(uint16_t seg, uint16_t off)
{
    return (void *)(uintptr_t)(((uint32_t)seg << 4) + off);
}

/* Called from interrupt_dispatch for traps/IRQs that interrupted V86 code. */
void v86_exception(struct regs *r);
void v86_deliver(struct regs *r);

/* vpic.c -- virtual 8259 pair as seen by the VM */
void vpic_init(uint8_t mask1, uint8_t mask2);
bool vpic_raise(unsigned irq);          /* request; false if already pending/off */
void vpic_hw_irq(unsigned irq);         /* VM-owned hardware IRQ arrived */
int  vpic_ack(void);                    /* vector to deliver now, or -1 */
bool vpic_has_deliverable(void);
bool vpic_port(uint16_t port);
uint8_t vpic_in(uint16_t port);
void vpic_out(uint16_t port, uint8_t val);
void vpic_reset_in_service(void);
void vpic_reclaim(unsigned irq);       /* forget VM state for an IRQ the kernel takes */

/* dosshell.c */
void dos_shell(void);
void dos_shell_selftest(void);

#endif
