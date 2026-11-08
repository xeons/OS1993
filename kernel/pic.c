/*
 * pic.c -- 8259A pair. Remapped away from the BIOS vectors (08h/70h), which
 * collide with CPU exceptions in protected mode. The loader restores the
 * BIOS layout when we return to DOS.
 */
#include "interrupts.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_init(uint8_t master_base, uint8_t slave_base)
{
    outb(PIC1_CMD, 0x11);  io_wait();     /* ICW1: edge, cascade, ICW4 */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, master_base); io_wait();
    outb(PIC2_DATA, slave_base);  io_wait();
    outb(PIC1_DATA, 0x04); io_wait();     /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();     /* 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFB);                /* all masked except cascade */
    outb(PIC2_DATA, 0xFF);
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(unsigned irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1u << (irq & 7)));
}

void pic_mask(unsigned irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1u << (irq & 7)));
}

void pic_eoi(unsigned irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* IRQ 7/15 can fire with nothing in service (line noise). Don't EOI those,
 * except that a spurious IRQ 15 still needs an EOI to the master. */
int pic_is_spurious(unsigned irq)
{
    if (irq != 7 && irq != 15)
        return 0;
    uint16_t cmd = irq == 7 ? PIC1_CMD : PIC2_CMD;
    outb(cmd, 0x0B);                      /* OCW3: read ISR */
    if (inb(cmd) & 0x80)
        return 0;
    if (irq == 15)
        outb(PIC1_CMD, 0x20);
    return 1;
}
