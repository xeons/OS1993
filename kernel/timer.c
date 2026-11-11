/*
 * timer.c -- IRQ 0.
 *
 * We keep the BIOS's 18.2 Hz rate. Each tick drives the scheduler and is
 * shared with every VM through its virtual PIC (vm_timer_tick); a VM that
 * can't take it in time gets its BIOS tick count advanced directly. Before
 * the VMs exist we advance DOS's tick count at 0040:006C ourselves.
 */
#include "devices.h"
#include "interrupts.h"
#include "io.h"
#include "sched.h"
#include "vm.h"
#include "user.h"

void net_tick(void);                    /* net.c */

#define BDA_TICKS     0x46C
#define BDA_MIDNIGHT  0x470
#define TICKS_PER_DAY 0x1800B0

static volatile uint32_t ticks;

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
    sched_tick();
    user_tick();
    net_tick();
    if (vm_timer_tick())
        return;                 /* shared with every VM (vm.c) */
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
