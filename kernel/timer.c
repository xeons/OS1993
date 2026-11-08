/*
 * timer.c -- IRQ 0.
 *
 * We keep the BIOS's 18.2 Hz rate and also advance the BIOS tick count at
 * 0040:006C, so DOS's clock hasn't lost time when we return. (Windows did
 * the same by reflecting timer ticks into the DOS VM.)
 */
#include "devices.h"
#include "interrupts.h"
#include "io.h"

#define BDA_TICKS     0x46C
#define BDA_MIDNIGHT  0x470
#define TICKS_PER_DAY 0x1800B0

static volatile uint32_t ticks;

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
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
