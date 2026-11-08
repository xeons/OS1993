#ifndef OS1993_GDT_H
#define OS1993_GDT_H

#include <stdbool.h>
#include <stdint.h>

/* Keep in sync with kernel/include/selectors.inc */
#define KCODE_SEL 0x08
#define KDATA_SEL 0x10
#define TSS_SEL   0x18

struct dtr {                 /* operand of LGDT / LIDT */
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 386 TSS. We never task-switch; it supplies SS0:ESP0 for traps out of
 * V86 mode and the I/O permission bitmap V86 port accesses are checked
 * against. */
struct tss {
    uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
    uint8_t  iomap[65536 / 8];   /* 1 bit per port: 1 = trap to the monitor */
    uint8_t  iomap_end;          /* must be 0xFF */
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct tss, esp0) == 4, "esp0 offset (v86.asm)");
_Static_assert(__builtin_offsetof(struct tss, iomap) == 104, "386 TSS layout");

extern struct tss kernel_tss;

void gdt_init(void);
void gdt_load(const struct dtr *gdtr);   /* entry.asm */
void idt_load(const struct dtr *idtr);   /* entry.asm */
void tss_trap_port(uint16_t port, bool trap);

#endif
