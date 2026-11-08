#include "devices.h"
#include "interrupts.h"
#include "io.h"
#include "v86.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

static volatile uint8_t buf[32];
static volatile unsigned head, tail;

static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(KBD_DATA);
    unsigned next = (head + 1) % sizeof buf;
    if (next != tail) {
        buf[head] = sc;
        head = next;
    }
}

void keyboard_init(void)
{
    while (inb(KBD_STATUS) & 1)           /* drain anything pending */
        (void)inb(KBD_DATA);
    irq_install(1, keyboard_irq);
}

/* Give IRQ 1 to the DOS VM (its BIOS INT 9 handler gets the keys). */
void keyboard_release(void)
{
    irq_uninstall(1);
}

/* Take IRQ 1 back from the VM. */
void keyboard_claim(void)
{
    vpic_reclaim(1);
    head = tail = 0;
    keyboard_init();
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
