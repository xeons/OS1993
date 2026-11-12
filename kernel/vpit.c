/*
 * vpit.c -- the virtual timer (a small VTD): each VM's own 8253 channel 0.
 *
 * The kernel owns the real PIT. A VM sees ports 40h-43h trapped: channel 0
 * is virtual per VM (reload value, access mode, counting mode, latches and
 * the read-back command), so a program that speeds the timer up for itself
 * -- music, games, a profiler -- gets its IRQ 0 at the rate it asked for,
 * and the other VMs and the kernel keep theirs. Channel 2 (the speaker)
 * goes to the hardware; channel 1 (refresh) is left alone.
 *
 * The real channel 0 runs at the fastest rate any running VM asked for
 * (but no faster than MIN_DIV allows), and the kernel's own 18.2 Hz tick
 * is counted out of it (timer.c). Every real interrupt is shared out: a VM
 * whose period has passed gets a virtual IRQ 0. A count read from port 40h
 * is worked out from the PIT clocks gone by since the VM loaded its
 * counter.
 */
#include "kernel.h"
#include "io.h"
#include "vm.h"

#define PIT_CH0  0x40
#define PIT_CH2  0x42
#define PIT_CMD  0x43
#define MIN_DIV  596                /* ~2 kHz: the fastest the real timer runs */

static uint32_t real_div = 65536;   /* what the real channel 0 counts */
static volatile uint32_t clock_base;    /* PIT clocks at the start of this period */

static void program_real(uint32_t div)
{
    uint32_t f = irq_save();
    real_div = div;
    outb(PIT_CMD, 0x34);            /* channel 0, lo/hi, mode 2 */
    outb(PIT_CH0, (uint8_t)div);
    outb(PIT_CH0, (uint8_t)(div >> 8));
    irq_restore(f);
}

uint32_t vpit_real_div(void) { return real_div; }

/* The timer interrupt: one period of real_div clocks has passed. */
void vpit_period(void) { clock_base += real_div; }

/* PIT clocks (1.193182 MHz) since boot, modulo 2^32. */
uint32_t vpit_clock(void)
{
    uint32_t f = irq_save();
    outb(PIT_CMD, 0x00);            /* latch channel 0 */
    uint32_t c = inb(PIT_CH0);
    c |= (uint32_t)inb(PIT_CH0) << 8;
    if (!c) c = 65536;
    uint32_t elapsed = real_div >= c ? real_div - c : 0;
    uint32_t v = clock_base + elapsed;
    irq_restore(f);
    return v;
}

/* The fastest rate any VM wants, for the real timer. */
static void retune(void)
{
    uint32_t want = 65536;
    for (int i = 0; i < VM_MAX; i++) {
        struct vm *vm = vm_get(i);
        if (vm && vm->state == VM_RUNNING && vm->pit.div < want)
            want = vm->pit.div;
    }
    if (want < MIN_DIV)
        want = MIN_DIV;
    if (want != real_div) {
        klog("VPIT: real timer at %u Hz\n", 1193182u / want);
        program_real(want);
    }
}

void vpit_init(struct vpit *p)
{
    memset(p, 0, sizeof *p);
    p->div = 65536;
    p->access = 3;
    p->mode = 3;
    p->start = vpit_clock();
}

/* A VM ended: the real timer may slow down again. */
void vpit_vm_gone(void) { retune(); }

void vpit_shutdown(void)
{
    uint32_t f = irq_save();
    outb(PIT_CMD, 0x36);            /* what the BIOS set: channel 0, lo/hi, mode 3, 18.2 Hz */
    outb(PIT_CH0, 0);
    outb(PIT_CH0, 0);
    real_div = 65536;
    irq_restore(f);
}

bool vpit_port(uint16_t port) { return port == PIT_CH0 || port == PIT_CMD; }

/* The count channel 0 of this VM shows now. */
static uint16_t count_now(struct vpit *p)
{
    uint32_t pos = (vpit_clock() - p->start) % p->div;
    uint32_t c;
    if (p->mode == 3) {             /* square wave: down by two, twice a period */
        uint32_t half = p->div / 2 ? p->div / 2 : 1;
        c = p->div - 2 * (pos % half);
    } else {
        c = p->div - pos;
    }
    return (uint16_t)c;             /* 65536 reads as 0 */
}

static void latch(struct vpit *p)
{
    if (p->latched)
        return;                     /* a latch holds until it is read */
    p->latch = count_now(p);
    p->latched = true;
    p->latch_hi = false;
}

uint8_t vpit_in(struct vm *vm, uint16_t port)
{
    struct vpit *p = &vm->pit;
    if (port != PIT_CH0)
        return 0xFF;                /* the command port can't be read */
    if (p->status_latched) {
        p->status_latched = false;
        return p->status;
    }
    uint16_t v = p->latched ? p->latch : count_now(p);
    uint8_t b;
    switch (p->access) {
    case 1: b = (uint8_t)v; p->latched = false; break;
    case 2: b = (uint8_t)(v >> 8); p->latched = false; break;
    default:                        /* lo then hi */
        if (!p->latch_hi) {
            b = (uint8_t)v;
            p->latch_hi = true;
            if (!p->latched) {      /* free-running: keep the high byte consistent */
                p->latch = v;
                p->latched = true;
            }
        } else {
            b = (uint8_t)(p->latch >> 8);
            p->latch_hi = false;
            p->latched = false;
        }
    }
    return b;
}

static void reload(struct vm *vm, uint32_t v)
{
    struct vpit *p = &vm->pit;
    p->div = v ? v : 65536;
    p->start = vpit_clock();
    p->acc = 0;
    if (p->div != 65536)
        klog("VPIT: VM %d timer at %u Hz (divisor %u)\n", vm->id, 1193182u / p->div, p->div);
    retune();
}

void vpit_out(struct vm *vm, uint16_t port, uint8_t v)
{
    struct vpit *p = &vm->pit;
    if (port == PIT_CMD) {
        unsigned ch = v >> 6;
        if (ch == 3) {                          /* read-back */
            if (v & 0x02) {                     /* counter 0 selected */
                if (!(v & 0x10)) {              /* status */
                    p->status = (uint8_t)(p->access << 4 | p->mode << 1);
                    p->status_latched = true;
                }
                if (!(v & 0x20))                /* count */
                    latch(p);
            }
            return;
        }
        if (ch == 2) {                          /* the speaker's channel: the hardware */
            outb(PIT_CMD, v);
            return;
        }
        if (ch == 1)
            return;
        if (!(v & 0x30)) {                      /* counter latch */
            latch(p);
            return;
        }
        p->access = (v >> 4) & 3;
        p->mode = (v >> 1) & 7;
        if (p->mode > 5) p->mode -= 4;          /* 6, 7 are 2, 3 */
        p->write_hi = false;
        p->latched = false;
        p->latch_hi = false;
        return;
    }
    switch (p->access) {                        /* port 40h: a new count */
    case 1: reload(vm, v); break;
    case 2: reload(vm, (uint32_t)v << 8); break;
    default:
        if (!p->write_hi) {
            p->low = v;
            p->write_hi = true;
        } else {
            p->write_hi = false;
            reload(vm, p->low | (uint32_t)v << 8);
        }
    }
}

/* A real timer interrupt: does this VM's period end? Returns the number of
 * its IRQ 0s that fell in it. */
unsigned vpit_due(struct vpit *p, uint32_t clocks)
{
    p->acc += clocks;
    if (p->acc < p->div)
        return 0;
    unsigned n = 0;
    while (p->acc >= p->div && n < 64) {
        p->acc -= p->div;
        n++;
    }
    if (p->acc >= p->div)
        p->acc %= p->div;
    return n;
}
