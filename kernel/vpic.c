/*
 * vpic.c -- virtual 8259A pair for the DOS VM (a small VPICD).
 *
 * The real PICs belong to the kernel (remapped to 20h-2Fh). The VM sees a
 * pair of virtual PICs at the usual ports 20h/21h/A0h/A1h (trapped through
 * the TSS I/O bitmap) with the BIOS vector bases 08h/70h.
 *
 * A VM-owned hardware IRQ (one without a kernel handler) is acknowledged on
 * the real PIC at once and masked there until the VM's handler sends its
 * (virtual) EOI, then unmasked. Meanwhile it sits in the virtual IRR until
 * the VM can take it: virtual IF set and nothing of higher or equal
 * priority in service. Priority is the standard cascaded order
 * 0 1 (2: 8-15) 3 4 5 6 7.
 */
#include "kernel.h"
#include "v86.h"

static const uint8_t prio_order[16] = { 0, 1, 2, 8, 9, 10, 11, 12, 13, 14, 15, 3, 4, 5, 6, 7 };

static struct {
    bool     on;
    uint16_t irr, isr, imr;
    uint16_t inflight;          /* masked on the real PIC until virtual EOI */
    uint8_t  vbase[2];
    uint8_t  icw_step[2];       /* 0 = idle, else next ICW expected (2..4) */
    bool     icw4[2];
    bool     read_isr[2];       /* OCW3: port 20h/A0h reads ISR, not IRR */
} p;

void vpic_init(uint8_t mask1, uint8_t mask2)
{
    p.imr = (uint16_t)(mask1 | mask2 << 8);
    p.vbase[0] = 0x08;
    p.vbase[1] = 0x70;
    p.on = true;
}

bool vpic_raise(unsigned irq)
{
    uint16_t bit = (uint16_t)(1u << irq);
    if (!p.on || (p.irr & bit))
        return false;
    p.irr |= bit;
    return true;
}

void vpic_hw_irq(unsigned irq)
{
    if (!p.on || irq == 2) {
        return;
    }
    p.irr |= (uint16_t)(1u << irq);
    p.inflight |= (uint16_t)(1u << irq);
    pic_mask(irq);
}

static int next_irq(void)
{
    uint16_t imr = p.imr;
    if (imr & 0x0004)               /* cascade masked => whole slave masked */
        imr |= 0xFF00;
    uint16_t pend = p.irr & ~imr;
    if (!p.on || !pend)
        return -1;
    for (int i = 0; i < 16; i++) {
        unsigned irq = prio_order[i];
        uint16_t bit = (uint16_t)(1u << irq);
        if (p.isr & bit)
            return -1;              /* everything below is blocked */
        if (irq != 2 && (pend & bit))
            return (int)irq;
    }
    return -1;
}

bool vpic_has_deliverable(void)
{
    return next_irq() >= 0;
}

int vpic_ack(void)
{
    int irq = next_irq();
    if (irq < 0)
        return -1;
    p.irr &= (uint16_t)~(1u << irq);
    p.isr |= (uint16_t)(1u << irq);
    if (irq >= 8)
        p.isr |= 0x0004;            /* cascade line in service on the master */
    return irq < 8 ? p.vbase[0] + irq : p.vbase[1] + (irq - 8);
}

static void eoi(unsigned irq)
{
    uint16_t bit = (uint16_t)(1u << irq);
    p.isr &= (uint16_t)~bit;
    if (p.inflight & bit) {
        p.inflight &= (uint16_t)~bit;
        pic_unmask(irq);
    }
}

static void eoi_nonspecific(int pic)
{
    uint8_t in_service = (uint8_t)(p.isr >> (pic * 8));
    for (unsigned b = 0; b < 8; b++) {
        if (in_service & (1u << b)) {
            eoi(b + (unsigned)pic * 8);
            return;
        }
    }
}

void vpic_reset_in_service(void)
{
    for (unsigned irq = 0; irq < 16; irq++)
        if (p.isr & (1u << irq))
            eoi(irq);
    p.isr = 0;
}

void vpic_reclaim(unsigned irq)
{
    uint16_t bit = (uint16_t)(1u << irq);
    p.irr &= (uint16_t)~bit;
    p.isr &= (uint16_t)~bit;
    p.inflight &= (uint16_t)~bit;
    pic_unmask(irq);
}

bool vpic_port(uint16_t port)
{
    return port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1;
}

uint8_t vpic_in(uint16_t port)
{
    int pic = port >= 0xA0;
    if (port & 1)
        return (uint8_t)(p.imr >> (pic * 8));
    return (uint8_t)((p.read_isr[pic] ? p.isr : p.irr) >> (pic * 8));
}

void vpic_out(uint16_t port, uint8_t val)
{
    int pic = port >= 0xA0;

    if (!(port & 1)) {                          /* command port */
        if (val & 0x10) {                       /* ICW1: (re)initialise */
            p.icw_step[pic] = 2;
            p.icw4[pic] = val & 1;
            p.isr &= (uint16_t)~(0xFFu << (pic * 8));
            return;
        }
        if ((val & 0x18) == 0x08) {             /* OCW3 */
            if (val & 0x02)
                p.read_isr[pic] = val & 1;
            return;
        }
        switch (val & 0xE0) {                   /* OCW2 */
        case 0x20: eoi_nonspecific(pic); break;
        case 0x60: eoi((val & 7u) + (unsigned)pic * 8); break;
        default:   break;                       /* rotation modes: ignored */
        }
        return;
    }

    switch (p.icw_step[pic]) {                  /* data port */
    case 2:
        p.vbase[pic] = val & 0xF8;
        p.icw_step[pic] = 3;
        return;
    case 3:
        p.icw_step[pic] = p.icw4[pic] ? 4 : 0;
        return;
    case 4:
        p.icw_step[pic] = 0;
        return;
    default:                                    /* OCW1: mask */
        if (pic) p.imr = (uint16_t)((p.imr & 0x00FF) | val << 8);
        else     p.imr = (uint16_t)((p.imr & 0xFF00) | val);
    }
}
