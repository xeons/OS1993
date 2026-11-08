/*
 * keyboard.c -- the real keyboard (IRQ 1) and each VM's virtual one (VKD).
 *
 * The kernel always owns IRQ 1. Every scancode goes to whoever has the
 * focus: the VMM console's buffer, or the focused VM's virtual keyboard
 * controller, which raises that VM's virtual IRQ 1. A VM reads ports 60h
 * and 64h (trapped) and sees only its own keys. Alt+Tab is the VMM's
 * hotkey and switches the focus.
 */
#include "kernel.h"
#include "devices.h"
#include "interrupts.h"
#include "io.h"
#include "vm.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

#define SC_ALT      0x38
#define SC_TAB      0x0F
#define SC_RELEASE  0x80

static volatile uint8_t buf[32];            /* the console's scancodes */
static volatile unsigned head, tail;
static bool alt_down, eat_tab_release;

static void console_put(uint8_t sc)
{
    unsigned next = (head + 1) % sizeof buf;
    if (next != tail) {
        buf[head] = sc;
        head = next;
    }
}

static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(KBD_DATA);

    if (sc == SC_ALT)                     alt_down = true;
    if (sc == (SC_ALT | SC_RELEASE))      alt_down = false;

    if (alt_down && sc == SC_TAB) {       /* Alt+Tab: next VM */
        struct vm *old = vm_focus();
        if (old)
            vkbd_put(old, SC_ALT | SC_RELEASE);   /* don't leave Alt stuck */
        eat_tab_release = true;
        vm_focus_next();
        return;
    }
    if (sc == (SC_TAB | SC_RELEASE) && eat_tab_release) {
        eat_tab_release = false;
        return;
    }

    struct vm *f = vm_focus();
    if (f) vkbd_put(f, sc);
    else   console_put(sc);
}

void keyboard_init(void)
{
    while (inb(KBD_STATUS) & 1)           /* drain anything pending */
        (void)inb(KBD_DATA);
    irq_install(1, keyboard_irq);
}

/* Hand keys still queued for the console to a VM that just got the focus,
 * so type-ahead after starting a session isn't lost. */
void keyboard_forward(struct vm *vm)
{
    int sc;
    while ((sc = keyboard_read()) >= 0)
        vkbd_put(vm, (uint8_t)sc);
}

int keyboard_read(void)
{
    if (head == tail)
        return -1;
    int sc = buf[tail];
    tail = (tail + 1) % sizeof buf;
    return sc;
}

int keyboard_poll_key(void)
{
    while (inb(KBD_STATUS) & 1)
        (void)inb(KBD_DATA);
    for (;;) {
        if (inb(KBD_STATUS) & 1) {
            uint8_t sc = inb(KBD_DATA);
            if (!(sc & 0x80))
                return sc;
        }
    }
}

/* ---- virtual keyboard controller --------------------------------------- */

static void latch_next(struct vm *vm)
{
    struct vkbd *k = &vm->kbd;
    if (k->head == k->tail)
        return;
    k->latch = k->q[k->tail];
    k->tail = (uint8_t)((k->tail + 1) % sizeof k->q);
    k->full = true;
    vm_raise_irq(vm, 1);
}

void vkbd_put(struct vm *vm, uint8_t sc)
{
    uint32_t f = irq_save();
    struct vkbd *k = &vm->kbd;
    uint8_t next = (uint8_t)((k->head + 1) % sizeof k->q);
    if (next != k->tail) {
        k->q[k->head] = sc;
        k->head = next;
    }
    vm->keys++;
    if (!k->full)
        latch_next(vm);
    irq_restore(f);
}

bool vkbd_port(uint16_t port)
{
    return port == KBD_DATA || port == KBD_STATUS;
}

uint8_t vkbd_in(struct vm *vm, uint16_t port)
{
    struct vkbd *k = &vm->kbd;
    if (port == KBD_STATUS)
        return (uint8_t)(0x14 | (k->full ? 0x01 : 0x00));  /* sys flag, unlocked */
    uint8_t v = k->latch;
    if (k->full) {
        k->full = false;
        latch_next(vm);
    }
    return v;
}

/* Commands (LEDs, enable/disable) reach the real controller only from the
 * focused VM; replies come back through IRQ 1 into its queue. */
void vkbd_out(struct vm *vm, uint16_t port, uint8_t val)
{
    if (vm == vm_focus())
        outb(port, val);
}
