#ifndef OS1993_INTERRUPTS_H
#define OS1993_INTERRUPTS_H

#include <stdint.h>

#define IRQ_BASE 0x20        /* PIC remapped to vectors 20h-2Fh */
#define SYSCALL_VECTOR 0x30  /* INT 30h: ring-3 calls into the VMM */

/* Stack frame built by kernel/isr.asm. */
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_pushad, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
    /* Only valid when the interrupt came from ring 3 or V86 mode: */
    uint32_t user_esp, user_ss;
    uint32_t v86_es, v86_ds, v86_fs, v86_gs;
};

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void irq_install(unsigned irq, irq_handler_t h);
void irq_uninstall(unsigned irq);
void interrupt_dispatch(struct regs *r);       /* called from isr.asm */

/* pic.c */
void pic_init(uint8_t master_base, uint8_t slave_base);
void pic_mask_all(void);
void pic_unmask(unsigned irq);
void pic_mask(unsigned irq);
void pic_eoi(unsigned irq);
int  pic_is_spurious(unsigned irq);

/* panic.c */
__attribute__((noreturn))
void panic_exception(const struct regs *r);

#endif
