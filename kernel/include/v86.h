/*
 * v86.h -- the V86 monitor: running real-mode DOS/BIOS code in a VM.
 * All calls act on the VM of the calling thread (vm_current()).
 */
#ifndef OS1993_V86_H
#define OS1993_V86_H

#include <stdbool.h>
#include <stdint.h>

#include "bootinfo.h"
#include "interrupts.h"

struct vm;

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
int  v86_enter(const struct v86_frame *f, uint32_t *save_esp);
__attribute__((noreturn)) void v86_leave(int rc, uint32_t saved_esp);

void v86_init(const struct bootinfo *bi);
void v86_vcpi_rehook(void);             /* the memory manager's vectors back (exit) */
void v86_vcpi_dos_unhook(void);         /* ... and DOS's disk handler pointer */
void v86_vcpi_dos_rehook(void);
void v86_selftest(const struct bootinfo *bi);   /* v86test.c */

/* Simulate INT vec in V86 mode and run until the handler returns.
 * Returns 0, or -1 if the VM crashed (see v86_last_error()). */
int  v86_int(uint8_t vec, struct v86_regs *r);
/* Far-call real-mode code (it must RETF), with interrupts enabled. */
int  v86_call_far(uint16_t seg, uint16_t off, struct v86_regs *r);
const char *v86_last_error(void);
uint32_t v86_delivered(uint8_t vec);    /* hardware IRQs reflected, by vector */

/* Conventional memory for V86 call buffers (in every VM, same address). */
uint16_t v86_buf_seg(void);
uint16_t v86_buf_off(void);
uint16_t v86_buf_size(void);
static inline void *v86_ptr(uint16_t seg, uint16_t off)
{
    return (void *)(uintptr_t)(((uint32_t)seg << 4) + off);
}

/*
 * Monitor breakpoints: ARPL instructions at the end of every VM's transfer
 * buffer (in the loader's segment, which each VM has a private copy of).
 * V86 code that reaches one -- a far call to an entry point the VMM hands
 * out, or a callback returning -- raises #UD, and its service runs.
 */
enum {
    BRK_MOUSE_RET,              /* an INT 33h event handler returned (vmouse.c) */
    BRK_XMS,                    /* the XMS driver's entry point (xms.c) */
    BRK_DPMI_ENTER,             /* DPMI: the mode switch to protected mode */
    BRK_DPMI_RAW,               /* DPMI: raw switch real -> protected */
    BRK_DPMI_SAVE,              /* DPMI: save/restore state (a no-op) */
    BRK_DPMI_RMRET,             /* DPMI: a real-mode call made for the client returned */
    BRK_DPMI_CB,                /* DPMI: real-mode callbacks, BRK_DPMI_CB + n */
    BRK_MAX = BRK_DPMI_CB + 8,
};
typedef void (*v86_brk_fn)(struct vm *vm, struct regs *r, int n);
uint16_t v86_brk_off(int n);    /* offset in v86_buf_seg() */
void v86_set_brk(int n, v86_brk_fn fn);

/* Run real-mode code from the monitor: save the V86 state, far-call
 * seg:off with the registers given, come back through `ret` (a BRK_*). */
void v86_push16(struct regs *r, uint16_t v);
uint16_t v86_pop16(struct regs *r);
uint32_t v86_vm_flags(const struct vm *vm, const struct regs *r);
void v86_set_vm_flags(struct vm *vm, struct regs *r, uint32_t f);
void v86_reflect(struct vm *vm, struct regs *r, uint8_t n);

/* Called from interrupt_dispatch for traps/IRQs that interrupted V86 code. */
void v86_exception(struct regs *r);
void v86_deliver(struct regs *r);

/* dosshell.c */
int  dos_exec_shell(const char *args, uint8_t *exit_code);
void dos_session_body(struct vm *vm);   /* interactive COMMAND.COM */
void dos_shell_selftest(void);
void dos_concurrency_selftest(void);

/* vmtest.c */
void vm_selftest(void);

#endif
