#include "kernel.h"
#include "gdt.h"
#include "interrupts.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t off_hi;
} __attribute__((packed));

#define IDT_INT_GATE32 0x8E      /* present, DPL 0, 32-bit interrupt gate */

static struct idt_entry idt[256];
static irq_handler_t irq_handlers[16];

extern const uint32_t isr_stub_table[48];

static void idt_set(int vec, uint32_t handler, uint8_t flags)
{
    idt[vec].off_lo = handler & 0xFFFF;
    idt[vec].sel    = KCODE_SEL;
    idt[vec].zero   = 0;
    idt[vec].flags  = flags;
    idt[vec].off_hi = handler >> 16;
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i], IDT_INT_GATE32);

    static struct dtr idtr;
    idtr.limit = sizeof idt - 1;
    idtr.base  = (uint32_t)(uintptr_t)idt;
    idt_load(&idtr);
}

void irq_install(unsigned irq, irq_handler_t h)
{
    irq_handlers[irq] = h;
    pic_unmask(irq);
}

void interrupt_dispatch(struct regs *r)
{
    if (r->int_no < IRQ_BASE) {
        /* Future: #GP from V86 mode goes to the V86 monitor, #PF to the
         * pager. For now every exception is fatal. */
        panic_exception(r);
    }

    unsigned irq = r->int_no - IRQ_BASE;
    if (irq < 16) {
        if (pic_is_spurious(irq))
            return;
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        pic_eoi(irq);
    }
}
