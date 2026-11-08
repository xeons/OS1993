/*
 * mouse.c -- PS/2 mouse on the 8042's auxiliary port (IRQ 12).
 *
 * The kernel owns the mouse. It keeps an absolute pointer position on the
 * 640x480 screen and queues events for the ring-3 GUI while the GUI has
 * the screen. DOS sessions don't get a mouse yet (that needs a virtual
 * auxiliary device for a DOS mouse driver to talk to).
 */
#include "kernel.h"
#include "devices.h"
#include "interrupts.h"
#include "io.h"
#include "user.h"
#include "vm.h"

#define KBD_DATA    0x60
#define KBD_STATUS  0x64
#define KBD_CMD     0x64
#define ST_OBF      0x01            /* output buffer full */
#define ST_IBF      0x02            /* input buffer full */
#define ST_AUX      0x20            /* the byte came from the mouse */
#define ACK         0xFA

#define QUEUE 32

static bool     present;
static uint8_t  saved_cmdbyte;
static uint8_t  packet[3];
static unsigned packet_len;
static int      mx = 320, my = 240;
static uint8_t  buttons;
static struct user_event queue[QUEUE];
static unsigned qhead, qtail;
static bool     deliver;            /* queue events (the GUI has the screen) */

static bool wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(KBD_STATUS) & ST_IBF))
            return true;
    return false;
}

static int read_byte(void)
{
    for (int i = 0; i < 200000; i++)
        if (inb(KBD_STATUS) & ST_OBF)
            return inb(KBD_DATA);
    return -1;
}

static void ctl_cmd(uint8_t c)  { wait_write(); outb(KBD_CMD, c); }
static void ctl_data(uint8_t d) { wait_write(); outb(KBD_DATA, d); }

static bool mouse_cmd(uint8_t c)
{
    ctl_cmd(0xD4);                  /* next data byte goes to the mouse */
    ctl_data(c);
    return read_byte() == ACK;
}

static void enqueue(const struct user_event *e)
{
    if (!deliver)
        return;
    if (qhead != qtail) {           /* merge with a queued move */
        struct user_event *last = &queue[(qhead + QUEUE - 1) % QUEUE];
        if (e->type == EV_MOUSE && last->type == EV_MOUSE && last->buttons == e->buttons) {
            *last = *e;
            return;
        }
    }
    unsigned next = (qhead + 1) % QUEUE;
    if (next != qtail) {
        queue[qhead] = *e;
        qhead = next;
    }
}

void mouse_byte(uint8_t b)
{
    if (packet_len == 0 && !(b & 0x08))
        return;                     /* not a first byte: resynchronise */
    packet[packet_len++] = b;
    if (packet_len < 3)
        return;
    packet_len = 0;
    if (packet[0] & 0xC0)
        return;                     /* overflow: drop */

    int dx = packet[1] - ((packet[0] << 4) & 0x100);
    int dy = packet[2] - ((packet[0] << 3) & 0x100);
    mx += dx;
    my -= dy;                       /* PS/2 y grows upwards */
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > 639) mx = 639;
    if (my > 479) my = 479;
    buttons = packet[0] & 0x07;

    struct user_event e = { .type = EV_MOUSE, .buttons = buttons,
                            .x = (int16_t)mx, .y = (int16_t)my };
    enqueue(&e);
}

static void mouse_irq(struct regs *r)
{
    (void)r;
    if (inb(KBD_STATUS) & ST_AUX)
        mouse_byte(inb(KBD_DATA));
}

void mouse_init(void)
{
    uint32_t f = irq_save();
    ctl_cmd(0xA8);                  /* enable the auxiliary port */
    ctl_cmd(0x20);                  /* read the command byte */
    int cb = read_byte();
    if (cb < 0) {
        irq_restore(f);
        klog("MOUSE: no response from the keyboard controller\n");
        return;
    }
    saved_cmdbyte = (uint8_t)cb;
    ctl_cmd(0x60);
    ctl_data((uint8_t)((cb | 0x02) & ~0x20));   /* IRQ 12 on, aux clock on */

    present = mouse_cmd(0xF6) && mouse_cmd(0xF4);   /* defaults, stream on */
    if (!present) {
        ctl_cmd(0x60);
        ctl_data(saved_cmdbyte);
        ctl_cmd(0xA7);              /* disable the auxiliary port again */
        while (inb(KBD_STATUS) & ST_OBF)
            (void)inb(KBD_DATA);
    }
    irq_restore(f);

    if (present) {
        irq_install(12, mouse_irq);
        klog("MOUSE: PS/2 mouse on IRQ 12\n");
    } else {
        klog("MOUSE: no PS/2 mouse\n");
    }
}

/* Before returning to DOS: stop the mouse, restore the controller. */
void mouse_shutdown(void)
{
    if (!present)
        return;
    mouse_cmd(0xF5);                /* stop streaming */
    ctl_cmd(0x60);
    ctl_data(saved_cmdbyte);
    if (saved_cmdbyte & 0x20)
        ctl_cmd(0xA7);
    while (inb(KBD_STATUS) & ST_OBF)
        (void)inb(KBD_DATA);
    present = false;
}

bool mouse_present(void) { return present; }

void mouse_set_delivery(bool on)
{
    uint32_t f = irq_save();
    deliver = on;
    qhead = qtail = 0;
    if (on) {                       /* start with the current position */
        struct user_event e = { .type = EV_MOUSE, .buttons = buttons,
                                .x = (int16_t)mx, .y = (int16_t)my };
        enqueue(&e);
    }
    irq_restore(f);
}

bool mouse_get_event(struct user_event *e)
{
    uint32_t f = irq_save();
    bool got = qhead != qtail;
    if (got) {
        *e = queue[qtail];
        qtail = (qtail + 1) % QUEUE;
    }
    irq_restore(f);
    return got;
}
