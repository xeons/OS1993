#include "devices.h"
#include "interrupts.h"
#include "io.h"

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
