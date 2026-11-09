/*
 * mouse.c -- PS/2 mouse on the 8042's auxiliary port (IRQ 12).
 *
 * The kernel owns the mouse. It keeps an absolute pointer position on the
 * 640x480 screen and queues events for the ring-3 GUI while the GUI has
 * the screen. DOS sessions don't get a mouse yet (that needs a virtual
 * auxiliary device for a DOS mouse driver to talk to).
 *
 * Under an emulator a PS/2 mouse only reports movement, so the guest's
 * pointer drifts away from the host's. If the VMware "backdoor" port
 * answers (QEMU with vmport=on, VMware, VirtualBox), we switch it to
 * absolute mode: the PS/2 interrupt still arrives, but the position is
 * read exactly from the port. Real hardware doesn't answer the probe.
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

#define VMW_PORT        0x5658
#define VMW_MAGIC       0x564D5868u     /* "VMXh" */
#define VMW_GETVERSION  10
#define VMW_ABS_DATA    39
#define VMW_ABS_STATUS  40
#define VMW_ABS_COMMAND 41
#define VMW_CMD_READ_ID 0x45414552u
#define VMW_CMD_DISABLE 0x000000F5u
#define VMW_CMD_ABS     0x53424152u
#define VMW_VERSION_ID  0x3442554Au

static bool     present;
static uint8_t  saved_cmdbyte;
static uint8_t  packet[3];
static unsigned packet_len;
static int      mx = 320, my = 240;
static uint8_t  buttons;
static struct user_event queue[QUEUE];
static unsigned qhead, qtail;
static bool     deliver;            /* queue events (the GUI has the screen) */
static bool     absolute;           /* VMware absolute pointer in use */

static void vmw_call(uint32_t cmd, uint32_t arg, uint32_t r[4])
{
    uint32_t a, b, c, d;
    __asm__ volatile("inl %%dx, %%eax"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(VMW_MAGIC), "b"(arg), "c"(cmd), "d"(VMW_PORT) : "memory");
    r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}

static bool vmmouse_enable(void)
{
    uint32_t r[4];
    vmw_call(VMW_GETVERSION, ~0u, r);
    if (r[1] != VMW_MAGIC)
        return false;                       /* not under a VMware-style VMM */
    vmw_call(VMW_ABS_COMMAND, VMW_CMD_READ_ID, r);
    vmw_call(VMW_ABS_STATUS, 0, r);
    if ((r[0] & 0xFFFF) != 1)
        return false;
    vmw_call(VMW_ABS_DATA, 1, r);
    if (r[0] != VMW_VERSION_ID)
        return false;
    vmw_call(VMW_ABS_COMMAND, VMW_CMD_ABS, r);
    return true;
}

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
    if (!deliver || vm_focus() != NULL)         /* only while the GUI shows */
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

static void vmmouse_poll(void)
{
    uint32_t r[4];
    for (int guard = 0; guard < 16; guard++) {
        vmw_call(VMW_ABS_STATUS, 0, r);
        if (r[0] == 0xFFFF0000u || (r[0] & 0xFFFF) < 4)
            return;
        vmw_call(VMW_ABS_DATA, 4, r);       /* flags, x, y, wheel */
        mx = (int)(r[1] * 640 / 65536);
        my = (int)(r[2] * 480 / 65536);
        buttons = (uint8_t)(((r[0] & 0x20) ? 1 : 0) | ((r[0] & 0x10) ? 2 : 0) |
                            ((r[0] & 0x08) ? 4 : 0));
        struct user_event e = { .type = EV_MOUSE, .buttons = buttons,
                                .x = (int16_t)mx, .y = (int16_t)my };
        enqueue(&e);
    }
}

void mouse_byte(uint8_t b)
{
    if (absolute) {                         /* the PS/2 bytes just signal */
        vmmouse_poll();
        return;
    }
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
        absolute = vmmouse_enable();
        irq_install(12, mouse_irq);
        klog("MOUSE: PS/2 mouse on IRQ 12%s\n",
             absolute ? ", absolute positions via the VMware port" : "");
    } else {
        klog("MOUSE: no PS/2 mouse\n");
    }
}

/* Before returning to DOS: stop the mouse, restore the controller. */
void mouse_shutdown(void)
{
    if (!present)
        return;
    if (absolute) {
        uint32_t r[4];
        vmw_call(VMW_ABS_COMMAND, VMW_CMD_DISABLE, r);
        absolute = false;
    }
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
