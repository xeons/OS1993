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
void ata_tick(void);                    /* ata.c */
void cache_tick(void);                  /* cache.c */

#define BDA_TICKS     0x46C
#define BDA_MIDNIGHT  0x470
#define TICKS_PER_DAY 0x1800B0

static volatile uint32_t ticks;
static volatile uint32_t beep_off;      /* the tick the speaker goes quiet (0: it is) */

/* The PC speaker: PIT channel 2 at hz, gated on through port 61h, off
 * again from the tick handler after ms. */
void speaker_beep(unsigned hz, unsigned ms)
{
    if (hz < 20 || hz > 20000 || !ms) {
        outb(0x61, inb(0x61) & ~3);
        beep_off = 0;
        return;
    }
    unsigned div = 1193182u / hz;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)div);
    outb(0x42, (uint8_t)(div >> 8));
    outb(0x61, inb(0x61) | 3);
    uint32_t n = ms * TIMER_HZ_X10 / 10000;
    beep_off = ticks + (n ? n : 1) + 1;
    if (!beep_off) beep_off = 1;
}

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
    if (beep_off && (int32_t)(ticks - beep_off) >= 0) {
        outb(0x61, inb(0x61) & ~3);
        beep_off = 0;
    }
    sched_tick();
    user_tick();
    net_tick();
    ata_tick();
    cache_tick();
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
