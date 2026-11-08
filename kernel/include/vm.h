/*
 * vm.h -- virtual machines.
 *
 * The System VM is the DOS that started OS1993; it runs on the original
 * physical memory and is driven by the VMM console thread. Memory is split
 * the way Win386 split it:
 *
 *   shared     everything below OS1993's loader (DOS, its buffers and file
 *              tables, drivers, the parent shell) and the HMA, plus ROM,
 *              UMB and adapter space. One DOS serves every VM (dosmgr.c).
 *   instance   shared addresses whose contents are swapped on every VM
 *              switch: IVT + BIOS data (0000-05FF), DOS's swappable data
 *              area and current directory structures.
 *   private    the loader's segment and everything above it up to 640 KB:
 *              each VM's own memory arena, MCB chain and programs; and the
 *              BIOS's working RAM in the E000 segment (SeaBIOS keeps
 *              variables and an internal stack there). Copied from a
 *              snapshot taken at boot.
 *
 * Each VM has its own page table for 0-4 MB; switching VMs swaps PDE 0.
 *
 * Text-mode video (B800:0000-BFFF) is virtualized: only the focused VM is
 * mapped to the real video RAM, the others draw into private buffers. The
 * VMM console itself is a focus target too (focus == NULL).
 */
#ifndef OS1993_VM_H
#define OS1993_VM_H

#include <stdbool.h>
#include <stdint.h>

#include "bootinfo.h"

#define VM_MAX          8
#define VM_LOW_PAGES    0xA0        /* conventional memory ends at 640 KB */
#define VM_BIOS_RAM_PAGE  0xE0      /* E000 segment: BIOS working RAM */
#define VM_BIOS_RAM_PAGES 0x10
#define VM_PRIV_MAX     (VM_LOW_PAGES + VM_BIOS_RAM_PAGES)
#define VM_INST_MAX     48          /* instance data regions */
#define VM_DOS_NEST     8           /* nested DOS calls tracked per VM */
#define VM_VIDEO_PAGE   0xB8
#define VM_VIDEO_PAGES  8           /* B800:0000-BFFF: virtualized */
#define VM_V86_PAGES    0x110       /* what V86 code can address */

struct v86_regs;
struct thread;

/* Virtual 8259A pair (vpic.c) */
struct vpic {
    uint16_t irr, isr, imr;
    uint16_t inflight;          /* masked on the real PIC until virtual EOI */
    uint8_t  vbase[2];
    uint8_t  icw_step[2];
    bool     icw4[2];
    bool     read_isr[2];
};

/* Virtual keyboard controller (keyboard.c) */
struct vkbd {
    uint8_t q[32];
    uint8_t head, tail;
    uint8_t latch;              /* byte the VM reads from port 60h */
    bool    full;               /* status bit 0: output buffer full */
};

enum vm_state { VM_FREE, VM_READY, VM_RUNNING, VM_FINISHED };

/* A DOS call in progress (dosmgr.c) */
struct dos_call {
    uint16_t ret_cs, ret_ip;    /* where the caller continues */
    uint16_t sp;                /* caller's SP before the INT */
    uint16_t psp;               /* process that made the call */
    uint8_t  kind;              /* DC_* */
};

struct vm {
    int           id;
    enum vm_state state;
    char          name[16];
    struct thread *thread;
    uint32_t      pt0_phys;                 /* page table for 0-4 MB */
    uint32_t      priv[VM_PRIV_MAX];        /* private frames (System VM: identity) */
    uint8_t      *inst;                     /* instance data while swapped out */
    uint32_t      video[VM_VIDEO_PAGES];    /* background video buffer frames */

    /* V86 execution state (v86.c) */
    bool          active;                   /* inside a V86 call */
    uint32_t      kernel_esp;               /* TSS.ESP0 while in V86 mode */
    struct v86_regs *out;
    bool          vif;                      /* virtual IF */
    uint16_t      vflags;                   /* virtual IOPL/NT */
    char          error[96];
    uint32_t      delivered[256];           /* reflected hardware IRQs, by vector */

    struct vpic   pic;
    struct vkbd   kbd;

    /* DOS critical section (dosmgr.c) */
    struct dos_call dos[VM_DOS_NEST];
    int           dos_depth;
    int           int28_depth;              /* idling inside DOS */
    uint32_t      keys;                     /* scancodes delivered */
    struct { uint16_t ax, bx, cx, dx, ret_ax, ret_fl, cs, ip; uint32_t tick; } trace[48];
    unsigned      trace_n;                  /* DOS call trace (debugging) */

    /* What the VM's thread runs, and bookkeeping */
    void        (*body)(struct vm *vm);
    void         *arg;
    int           result;
    uint32_t      start_tick, end_tick;
};

extern struct vm *const system_vm;

void vm_system_init(const struct bootinfo *bi);   /* the System VM */
bool vm_add_instance(uint32_t linear, uint32_t len);
void vm_finish_init(void);                  /* snapshot; VMs can be created */
struct vm *vm_create(const char *name, void (*body)(struct vm *), void *arg);
void vm_start(struct vm *vm);
void vm_reap(void);                         /* free finished VMs */
int  vm_count(void);                        /* DOS VMs, excluding the System VM */
struct vm *vm_get(int id);

struct vm *vm_current(void);
void vm_switch_to(struct vm *vm);           /* scheduler: PDE 0, TSS.ESP0 */
void *vm_ptr(struct vm *vm, uint32_t linear);   /* kernel view of VM memory */

struct vm *vm_focus(void);                  /* NULL = VMM console */
void vm_set_focus(struct vm *vm);
void vm_focus_next(void);

/* Interrupt routing */
void vm_raise_irq(struct vm *vm, unsigned irq);
void vm_hw_irq(unsigned irq);               /* a VM-owned hardware IRQ fired */
bool vm_timer_tick(void);                   /* false before VMs exist */

void vm_restore_dos(void);                  /* undo everything before exit */
void vm_check(const char *where);           /* panic on state mismatch */

/* vpic.c */
void vpic_init(struct vpic *p, uint8_t mask1, uint8_t mask2);
bool vpic_raise(struct vpic *p, unsigned irq);
void vpic_hw_irq(struct vpic *p, unsigned irq);
int  vpic_ack(struct vpic *p);
bool vpic_has_deliverable(struct vpic *p);
bool vpic_port(uint16_t port);
uint8_t vpic_in(struct vpic *p, uint16_t port);
void vpic_out(struct vpic *p, uint16_t port, uint8_t val);
void vpic_reset_in_service(struct vpic *p);

/* dosmgr.c: one DOS shared by all VMs */
#define DC_RETURN   0           /* returns to the caller (hooked) */
#define DC_RET_FLAGS 1          /* INT 25h/26h: RETF leaves FLAGS on the stack */
#define DC_NORETURN 2           /* terminate: never returns */
#define DC_KERNEL   3           /* a v86_int() from the kernel */
#define DC_IDLE     4           /* INT 28h: DOS idle, others may use DOS */

#define DOSMGR_PASS 0
#define DOSMGR_HOOK 1
#define DOSMGR_DONE 2

struct regs;
void dosmgr_init(void);
void dosmgr_started(void);                  /* INT 2Fh 1608h */
void dosmgr_shutdown(void);                 /* INT 2Fh 1609h + 1606h */
int  dosmgr_int(struct vm *vm, struct regs *r, uint8_t n, uint16_t next_ip);
bool dosmgr_return(struct vm *vm, struct regs *r);
int  dosmgr_kernel_enter(struct vm *vm, uint8_t vec, uint8_t ah);
void dosmgr_kernel_leave(struct vm *vm, int depth);
bool dosmgr_enabled(void);
bool dosmgr_is_dos_int(uint8_t vec, uint8_t ah);
bool dosmgr_busy_for(struct vm *vm);        /* another VM is inside DOS */
bool dosmgr_holds(struct vm *vm);
uint16_t dosmgr_current_psp(struct vm *vm);
uint32_t dosmgr_waits(void);
void dosmgr_dump_trace(struct vm *vm);

/* keyboard.c: virtual keyboard controller */
void vkbd_put(struct vm *vm, uint8_t scancode);
bool vkbd_port(uint16_t port);
uint8_t vkbd_in(struct vm *vm, uint16_t port);
void vkbd_out(struct vm *vm, uint16_t port, uint8_t val);
void keyboard_forward(struct vm *vm);       /* console type-ahead to a VM */

#endif
