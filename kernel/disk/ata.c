/*
 * ata.c -- IDE/ATA hard disks, PIO.
 *
 * Both legacy channels (1F0h/3F6h, IRQ 14 and 170h/376h, IRQ 15) are
 * probed with IDENTIFY. Each disk found is matched to a BIOS drive number
 * by reading its first sector both ways, and the BIOS's geometry for it
 * is noted, so that INT 13h can be done here instead (int13.c).
 *
 * The VMM claims every channel that has a disk and no ATAPI device (a CD
 * driver in DOS would drive that itself): its ports read as empty from
 * V86 code and its interrupt is the VMM's, and DOS's INT 13h calls for its
 * disks are answered by int13.c through this driver -- "32-bit disk
 * access", as Windows 3.1's WDCTRL had it. A channel with a BIOS disk
 * whose geometry the BIOS didn't give stays the BIOS's.
 *
 * Transfers wait for the drive's interrupt (the thread sleeps; a timer
 * tick wakes it if the interrupt never comes), or poll when interrupts
 * are off. LBA28, and LBA48 for sectors past 2^28 (disks over 128 GB, up
 * to 2 TB here: sector numbers are 32-bit).
 */
#include "kernel.h"
#include "devices.h"
#include "disk.h"
#include "gdt.h"
#include "interrupts.h"
#include "io.h"
#include "sched.h"
#include "v86.h"

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ       0x20
#define CMD_READ_EXT   0x24
#define CMD_WRITE      0x30
#define CMD_WRITE_EXT  0x34
#define CMD_FLUSH      0xE7
#define CMD_FLUSH_EXT  0xEA
#define CMD_IDENTIFY   0xEC

static const uint16_t io_base[2] = { 0x1F0, 0x170 };
static const uint16_t ctl_base[2] = { 0x3F6, 0x376 };
static const uint8_t chan_irq[2] = { 14, 15 };
static bool chan_owned[2], chan_atapi[2], claimed;

static struct {
    volatile bool seen;
    struct thread *waiter;
    uint32_t deadline;
    bool on;                            /* interrupts in use on this channel */
} irqs[2];

struct disk disks[DISK_MAX];

static inline uint16_t reg(int ch, int r) { return (uint16_t)(io_base[ch] + r); }

/* ---- waiting --------------------------------------------------------------- */

static void irq_common(int ch)
{
    (void)inb(reg(ch, 7));                              /* reading the status acknowledges it */
    irqs[ch].seen = true;
    if (irqs[ch].waiter)
        thread_wake(irqs[ch].waiter);
}

static void irq14(struct regs *r) { (void)r; irq_common(0); }
static void irq15(struct regs *r) { (void)r; irq_common(1); }

void ata_tick(void)
{
    for (int ch = 0; ch < 2; ch++)
        if (irqs[ch].waiter && (int32_t)(timer_ticks() - irqs[ch].deadline) >= 0)
            thread_wake(irqs[ch].waiter);
}

/* Sleep until the channel's interrupt (or a second goes by). */
static void wait_irq(int ch)
{
    uint32_t f = irq_save();
    if (irqs[ch].on && (f & 0x200) && !irqs[ch].seen && current) {
        irqs[ch].waiter = current;
        irqs[ch].deadline = timer_ticks() + 19;
        current->state = T_BLOCKED;
        schedule();
        irqs[ch].waiter = NULL;
    }
    irqs[ch].seen = false;
    irq_restore(f);
}

/* Wait for BSY to clear (and, with drq, for DRQ). 0, or -1 on error/timeout. */
static int wait(int ch, bool drq)
{
    uint32_t start = timer_ticks();
    bool timed = (read_eflags() & 0x200) != 0;
    for (uint32_t n = 0;; n++) {
        uint8_t s = inb(reg(ch, 7));
        if (s == 0xFF)
            return -1;                                  /* nothing there */
        if (!(s & ST_BSY)) {
            if (s & (ST_ERR | ST_DF))
                return -1;
            if (!drq || (s & ST_DRQ))
                return 0;
        }
        if (timed ? timer_ticks() - start > 5 * 18 : n > 20000000)
            return -1;
        if (timed && n > 1000 && !(n & 255))
            sched_yield();                              /* a slow drive: let others run */
    }
}

static void delay400(int ch)
{
    for (int i = 0; i < 4; i++)
        inb(ctl_base[ch]);
}

/* ---- commands ----------------------------------------------------------------- */

static void issue(struct disk *d, uint32_t lba, unsigned n, bool write)
{
    int ch = d->channel;
    bool ext = d->lba48 && (lba + n > 0x0FFFFFFF || lba > 0x0FFFFFFF);
    irqs[ch].seen = false;
    if (ext) {
        outb(reg(ch, 6), (uint8_t)(0x40 | (d->slave << 4)));
        delay400(ch);
        outb(reg(ch, 2), (uint8_t)(n >> 8));            /* the high bytes first */
        outb(reg(ch, 3), (uint8_t)(lba >> 24));
        outb(reg(ch, 4), 0);
        outb(reg(ch, 5), 0);
        outb(reg(ch, 2), (uint8_t)n);
        outb(reg(ch, 3), (uint8_t)lba);
        outb(reg(ch, 4), (uint8_t)(lba >> 8));
        outb(reg(ch, 5), (uint8_t)(lba >> 16));
        outb(reg(ch, 7), write ? CMD_WRITE_EXT : CMD_READ_EXT);
    } else {
        outb(reg(ch, 6), (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
        delay400(ch);
        outb(reg(ch, 2), (uint8_t)n);                   /* 256 is 0 */
        outb(reg(ch, 3), (uint8_t)lba);
        outb(reg(ch, 4), (uint8_t)(lba >> 8));
        outb(reg(ch, 5), (uint8_t)(lba >> 16));
        outb(reg(ch, 7), write ? CMD_WRITE : CMD_READ);
    }
}

static int check_range(struct disk *d, uint32_t lba, unsigned n)
{
    if (!d->present || n == 0 || n > 256 || lba >= d->sectors || d->sectors - lba < n)
        return -1;
    return 0;
}

static void error(struct disk *d, const char *what, uint32_t lba)
{
    int ch = d->channel;
    klog("ATA: %s error, disk %d LBA %u (status %02X, error %02X)\n", what,
         ch * 2 + d->slave, lba, inb(reg(ch, 7)), inb(reg(ch, 1)));
}

int ata_read(struct disk *d, uint32_t lba, unsigned n, void *buf)
{
    if (check_range(d, lba, n) < 0)
        return -1;
    int ch = d->channel;
    if (wait(ch, false) < 0)
        return -1;
    issue(d, lba, n, false);
    uint16_t *p = buf;
    for (unsigned s = 0; s < n; s++) {
        wait_irq(ch);
        delay400(ch);
        if (wait(ch, true) < 0) {
            error(d, "read", lba + s);
            return -1;
        }
        __asm__ volatile("rep insw" : "+D"(p), "+c"((uint32_t){ 256 }) : "d"(reg(ch, 0)) : "memory");
    }
    return 0;
}

int ata_write(struct disk *d, uint32_t lba, unsigned n, const void *buf)
{
    if (check_range(d, lba, n) < 0)
        return -1;
    int ch = d->channel;
    if (wait(ch, false) < 0)
        return -1;
    issue(d, lba, n, true);
    const uint16_t *p = buf;
    for (unsigned s = 0; s < n; s++) {
        delay400(ch);
        if (wait(ch, true) < 0) {                       /* the first sector goes without an interrupt */
            error(d, "write", lba + s);
            return -1;
        }
        __asm__ volatile("rep outsw" : "+S"(p), "+c"((uint32_t){ 256 }) : "d"(reg(ch, 0)) : "memory");
        wait_irq(ch);                                   /* ... each one after it with one */
    }
    delay400(ch);
    return wait(ch, false);
}

int ata_flush(struct disk *d)
{
    int ch = d->channel;
    if (wait(ch, false) < 0)
        return -1;
    outb(reg(ch, 6), (uint8_t)(0xE0 | (d->slave << 4)));
    delay400(ch);
    irqs[ch].seen = false;
    outb(reg(ch, 7), d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    wait_irq(ch);
    delay400(ch);
    wait(ch, false);                                    /* old drives: not supported, fine */
    return 0;
}

/* ---- probing ------------------------------------------------------------------ */

static void ata_string(char *out, const uint16_t *id, int first, int words)
{
    int n = 0;
    for (int i = 0; i < words; i++) {
        out[n++] = (char)(id[first + i] >> 8);
        out[n++] = (char)id[first + i];
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

static void identify(int ch, int slave)
{
    struct disk *d = &disks[ch * 2 + slave];
    outb(reg(ch, 6), (uint8_t)(0xA0 | slave << 4));
    delay400(ch);
    if (inb(reg(ch, 7)) == 0xFF || inb(reg(ch, 7)) == 0)
        return;                                         /* no device */
    outb(reg(ch, 2), 0);
    outb(reg(ch, 3), 0);
    outb(reg(ch, 4), 0);
    outb(reg(ch, 5), 0);
    outb(reg(ch, 7), CMD_IDENTIFY);
    delay400(ch);
    if (inb(reg(ch, 7)) == 0)
        return;
    for (uint32_t n = 0; n < 1000000 && (inb(reg(ch, 7)) & ST_BSY); n++)
        ;
    uint8_t mid = inb(reg(ch, 4)), high = inb(reg(ch, 5));
    if (mid || high) {
        if ((mid == 0x14 && high == 0xEB) || (mid == 0x69 && high == 0x96)) {
            chan_atapi[ch] = true;                      /* a CD-ROM: DOS's driver needs the channel */
            klog("ATA: an ATAPI device on the %s channel: left to DOS\n", ch ? "secondary" : "primary");
        }
        return;
    }
    if (wait(ch, true) < 0)
        return;
    uint16_t id[256];
    uint16_t *p = id;
    __asm__ volatile("rep insw" : "+D"(p), "+c"((uint32_t){ 256 }) : "d"(reg(ch, 0)) : "memory");
    if (!(id[49] & 0x0200)) {
        klog("ATA: disk %d has no LBA: not used\n", ch * 2 + slave);
        return;
    }
    d->present = true;
    d->channel = (uint8_t)ch;
    d->slave = (uint8_t)slave;
    d->sectors = (uint32_t)id[60] | (uint32_t)id[61] << 16;
    if (id[83] & 0x0400) {                              /* LBA48 */
        d->lba48 = true;
        uint32_t lo = (uint32_t)id[100] | (uint32_t)id[101] << 16;
        bool big = id[102] || id[103];
        d->sectors = big ? 0xFFFFFFFFu : (lo > d->sectors ? lo : d->sectors);
    }
    ata_string(d->model, id, 27, 20);
}

/* Which BIOS drive is each disk, and what geometry does the BIOS give it? */
static void match_bios(void)
{
    uint8_t count = *(volatile uint8_t *)0x475;         /* BDA: hard disks */
    static uint8_t mine[SECTOR], bios[SECTOR];
    for (uint8_t drive = 0x80; drive < 0x80 + count && drive < 0x88; drive++) {
        struct v86_regs r = { .eax = 0x0201, .ecx = 0x0001, .edx = drive,
                              .ebx = v86_buf_off(), .es = v86_buf_seg() };
        if (v86_int(0x13, &r) != 0 || (r.flags & EFLAGS_CF))
            continue;
        memcpy(bios, v86_ptr(v86_buf_seg(), v86_buf_off()), SECTOR);
        struct disk *d = NULL;
        for (int i = 0; i < DISK_MAX && !d; i++)
            if (disks[i].present && !disks[i].bios && ata_read(&disks[i], 0, 1, mine) == 0 &&
                memcmp(mine, bios, SECTOR) == 0)
                d = &disks[i];
        if (!d)
            continue;
        d->bios = drive;
        struct v86_regs g = { .eax = 0x0800, .edx = drive };
        if (v86_int(0x13, &g) == 0 && !(g.flags & EFLAGS_CF) && (g.ecx & 0x3F)) {
            d->spt = (uint8_t)(g.ecx & 0x3F);
            d->heads = (uint16_t)(((g.edx >> 8) & 0xFF) + 1);
            d->cyls = (uint16_t)((((g.ecx >> 8) & 0xFF) | ((g.ecx & 0xC0) << 2)) + 1);
            d->bios_cx = (uint16_t)g.ecx;
            d->bios_dh = (uint8_t)(g.edx >> 8);
        }
    }
}

void ata_init(void)
{
    for (int ch = 0; ch < 2; ch++) {
        if (inb(reg(ch, 7)) == 0xFF)
            continue;                                   /* floating bus: no channel */
        outb(ctl_base[ch], 0x02);                       /* nIEN while probing */
        identify(ch, 0);
        identify(ch, 1);
    }
    for (int ch = 0; ch < 2; ch++)
        outb(ctl_base[ch], 0x00);                       /* the BIOS's way, for match_bios */
    match_bios();

    for (int ch = 0; ch < 2; ch++) {
        bool any = false, ok = !chan_atapi[ch];
        for (int s = 0; s < 2; s++) {
            struct disk *d = &disks[ch * 2 + s];
            any |= d->present;
            if (d->present && d->bios && !d->spt)
                ok = false;                             /* the BIOS gave no geometry: keep its */
        }
        if (!any || !ok)
            continue;
        chan_owned[ch] = true;
        for (int s = 0; s < 2; s++)
            disks[ch * 2 + s].owned = disks[ch * 2 + s].present;
    }
    for (int i = 0; i < DISK_MAX; i++) {
        struct disk *d = &disks[i];
        if (!d->present)
            continue;
        char bios[40] = "none";
        if (d->bios)
            ksnprintf(bios, sizeof bios, "%02Xh (%u/%u/%u)", d->bios, d->cyls, d->heads, d->spt);
        klog("ATA: disk %d (%s %s): %s, %u MB%s, BIOS %s, %s\n", i,
             d->channel ? "secondary" : "primary", d->slave ? "slave" : "master", d->model,
             d->sectors / 2048, d->lba48 ? ", LBA48" : "", bios, d->owned ? "the VMM's" : "left to DOS");
    }
}

/* From here on the channels are the VMM's alone. Until now DOS could still
 * read them through the BIOS (redir.c asks it for volume serial numbers). */
void ata_claim(void)
{
    for (int ch = 0; ch < 2; ch++) {
        if (!chan_owned[ch])
            continue;
        for (int p = 0; p < 8; p++)
            tss_trap_port((uint16_t)(io_base[ch] + p), true);
        tss_trap_port(ctl_base[ch], true);
        irqs[ch].on = true;
        irq_install(chan_irq[ch], ch ? irq15 : irq14);
        outb(ctl_base[ch], 0x00);                       /* interrupts on */
    }
    claimed = true;
}

bool ata_port_owned(uint16_t port)
{
    if (!claimed)
        return false;
    for (int ch = 0; ch < 2; ch++)
        if (chan_owned[ch] && ((port >= io_base[ch] && port < io_base[ch] + 8) || port == ctl_base[ch]))
            return true;
    return false;
}

struct disk *ata_bios_disk(uint8_t drive)
{
    if (!claimed)
        return NULL;
    for (int i = 0; i < DISK_MAX; i++)
        if (disks[i].owned && disks[i].bios == drive)
            return &disks[i];
    return NULL;
}

void ata_release(void)
{
    for (int ch = 0; ch < 2; ch++)
        if (chan_owned[ch] && irqs[ch].on) {
            irqs[ch].on = false;
            irq_uninstall(chan_irq[ch]);                /* the BIOS's again */
            outb(ctl_base[ch], 0x00);
        }
}
