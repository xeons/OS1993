/*
 * vm.h -- virtual machines.
 *
 * The System VM is the DOS that started OS1993; it runs on the original
 * physical memory and is driven by the VMM console thread. Every other VM
 * gets private copies of conventional memory and the HMA, made from a
 * snapshot taken at boot, so each has its own IVT, BIOS data, DOS data, MCB
 * chain and programs. ROM, UMB and adapter space stay shared. Each VM has
 * its own page table for 0-4 MB; switching VMs swaps PDE 0.
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
#define VM_LOW_PAGES    0xA0        /* 0-640 KB: private */
#define VM_HMA_PAGE     0x100
#define VM_HMA_PAGES    0x10        /* HMA: private */
#define VM_PRIV_PAGES   (VM_LOW_PAGES + VM_HMA_PAGES)
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

struct vm {
    int           id;
    enum vm_state state;
    char          name[16];
    struct thread *thread;
    uint32_t      pt0_phys;                 /* page table for 0-4 MB */
    uint32_t      priv[VM_PRIV_PAGES];      /* private frames (System VM: identity) */
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

    /* What the VM's thread runs, and bookkeeping */
    void        (*body)(struct vm *vm);
    void         *arg;
    int           result;
    uint32_t      start_tick, end_tick;
};

extern struct vm *const system_vm;

void vm_system_init(const struct bootinfo *bi);
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

/* keyboard.c: virtual keyboard controller */
void vkbd_put(struct vm *vm, uint8_t scancode);
bool vkbd_port(uint16_t port);
uint8_t vkbd_in(struct vm *vm, uint16_t port);
void vkbd_out(struct vm *vm, uint16_t port, uint8_t val);
void keyboard_forward(struct vm *vm);       /* console type-ahead to a VM */

#endif
