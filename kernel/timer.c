/*
 * timer.c -- IRQ 0.
 *
 * We keep the BIOS's 18.2 Hz rate and share each tick with the DOS VM: it
 * is raised on the virtual PIC so the BIOS INT 8 handler runs when the VM
 * next can. If the previous tick is still undelivered (the VM isn't running,
 * or has interrupts off), this one would be lost, so we advance the BIOS
 * tick count at 0040:006C ourselves. Either way DOS's clock stays right.
 */
#include "devices.h"
#include "interrupts.h"
#include "io.h"
#include "v86.h"

#define BDA_TICKS     0x46C
#define BDA_MIDNIGHT  0x470
#define TICKS_PER_DAY 0x1800B0

static volatile uint32_t ticks;

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
    if (vpic_raise(0))
        return;
    uint32_t t = LOWMEM32(BDA_TICKS) + 1;
    if (t >= TICKS_PER_DAY) {
        t = 0;
        LOWMEM8(BDA_MIDNIGHT) = 1;
    }
    LOWMEM32(BDA_TICKS) = t;
}

void timer_init(void)
{
    irq_install(0, timer_irq);
}

uint32_t timer_ticks(void)
{
    return ticks;
}
