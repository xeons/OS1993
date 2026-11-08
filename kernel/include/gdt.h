#ifndef OS1993_GDT_H
#define OS1993_GDT_H

#include <stdint.h>

/* Keep in sync with kernel/include/selectors.inc */
#define KCODE_SEL 0x08
#define KDATA_SEL 0x10

struct dtr {                 /* operand of LGDT / LIDT */
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

void gdt_init(void);
void gdt_load(const struct dtr *gdtr);   /* entry.asm */
void idt_load(const struct dtr *idtr);   /* entry.asm */

#endif
