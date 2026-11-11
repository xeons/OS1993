/*
 * ata.c -- IDE/ATA hard disks, PIO, polled.
 *
 * Both legacy channels (1F0h/3F6h and 170h/376h) are probed with IDENTIFY.
 * Each disk found is matched to a BIOS drive number by reading its first
 * sector both ways. The channel holding BIOS drive 80h (DOS's boot disk)
 * is left to the BIOS; the VMM claims every other channel: interrupts off
 * (nIEN, so it is polled and IRQ 14/15 never reaches a VM), its ports
 * hidden from V86 code, and INT 13h for its disks refused (v86.c). Two
 * masters of one channel would trample each other's register writes, so
 * it is all or nothing per channel.
 *
 * LBA28 only (disks up to 128 GB; a bigger one shows its first 128 GB).
 */
#include "kernel.h"
#include "devices.h"
#include "disk.h"
#include "gdt.h"
#include "io.h"
#include "sched.h"
#include "v86.h"

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_RDY  0x40
#define ST_BSY  0x80

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

static const uint16_t io_base[2] = { 0x1F0, 0x170 };
static const uint16_t ctl_base[2] = { 0x3F6, 0x376 };
static bool chan_owned[2], claimed;

struct disk disks[DISK_MAX];

static inline uint16_t reg(int ch, int r) { return (uint16_t)(io_base[ch] + r); }

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

static void select_lba(struct disk *d, uint32_t lba, unsigned n)
{
    int ch = d->channel;
    outb(reg(ch, 6), (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
    delay400(ch);
    outb(reg(ch, 2), (uint8_t)n);                      /* 256 is 0 */
    outb(reg(ch, 3), (uint8_t)lba);
    outb(reg(ch, 4), (uint8_t)(lba >> 8));
    outb(reg(ch, 5), (uint8_t)(lba >> 16));
}

static int check_range(struct disk *d, uint32_t lba, unsigned n)
{
    if (!d->present || n == 0 || n > 256 || lba >= d->sectors || d->sectors - lba < n)
        return -1;
    return 0;
}

int ata_read(struct disk *d, uint32_t lba, unsigned n, void *buf)
{
    if (check_range(d, lba, n) < 0)
        return -1;
    int ch = d->channel;
    if (wait(ch, false) < 0)
        return -1;
    select_lba(d, lba, n);
    outb(reg(ch, 7), CMD_READ);
    uint16_t *p = buf;
    for (unsigned s = 0; s < n; s++) {
        delay400(ch);
        if (wait(ch, true) < 0) {
            klog("ATA: read error, disk %d LBA %u (status %02X, error %02X)\n",
                 ch * 2 + d->slave, lba + s, inb(reg(ch, 7)), inb(reg(ch, 1)));
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
    select_lba(d, lba, n);
    outb(reg(ch, 7), CMD_WRITE);
    const uint16_t *p = buf;
    for (unsigned s = 0; s < n; s++) {
        delay400(ch);
        if (wait(ch, true) < 0) {
            klog("ATA: write error, disk %d LBA %u (status %02X, error %02X)\n",
                 ch * 2 + d->slave, lba + s, inb(reg(ch, 7)), inb(reg(ch, 1)));
            return -1;
        }
        __asm__ volatile("rep outsw" : "+S"(p), "+c"((uint32_t){ 256 }) : "d"(reg(ch, 0)) : "memory");
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
    outb(reg(ch, 7), CMD_FLUSH);
    delay400(ch);
    wait(ch, false);                                    /* old drives: not supported, fine */
    return 0;
}

/* ---- probing ------------------------------------------------------------ */

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
    uint32_t n;
    for (n = 0; n < 1000000 && (inb(reg(ch, 7)) & ST_BSY); n++)
        ;
    if (inb(reg(ch, 4)) || inb(reg(ch, 5)))
        return;                                         /* ATAPI or SATA: not a disk we drive */
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
    ata_string(d->model, id, 27, 20);
}

/* Which BIOS drive is it? Compare the first sector both ways. */
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
        for (int i = 0; i < DISK_MAX; i++)
            if (disks[i].present && !disks[i].bios && ata_read(&disks[i], 0, 1, mine) == 0 &&
                memcmp(mine, bios, SECTOR) == 0) {
                disks[i].bios = drive;
                break;
            }
    }
}

void ata_init(void)
{
    for (int ch = 0; ch < 2; ch++) {
        if (inb(reg(ch, 7)) == 0xFF)
            continue;                                   /* floating bus: no channel */
        outb(ctl_base[ch], 0x02);                       /* nIEN: polled */
        identify(ch, 0);
        identify(ch, 1);
    }
    for (int ch = 0; ch < 2; ch++)
        outb(ctl_base[ch], 0x00);                       /* the BIOS's way, for match_bios */
    match_bios();

    for (int ch = 0; ch < 2; ch++) {
        bool any = false, boot = false;
        for (int s = 0; s < 2; s++) {
            struct disk *d = &disks[ch * 2 + s];
            any |= d->present;
            boot |= d->present && d->bios == 0x80;
        }
        if (!any || boot)
            continue;
        chan_owned[ch] = true;
        for (int s = 0; s < 2; s++)
            disks[ch * 2 + s].owned = disks[ch * 2 + s].present;
    }
    for (int i = 0; i < DISK_MAX; i++) {
        struct disk *d = &disks[i];
        if (!d->present)
            continue;
        char bios[8] = "none";
        if (d->bios)
            ksnprintf(bios, sizeof bios, "%02Xh", d->bios);
        klog("ATA: disk %d (%s %s): %s, %u MB, BIOS %s, %s\n", i,
             d->channel ? "secondary" : "primary", d->slave ? "slave" : "master", d->model,
             d->sectors / 2048, bios, d->owned ? "the VMM's" : "left to DOS");
    }
}

/* From here on the channels are the VMM's alone. Until now DOS could still
 * read them through the BIOS (redir.c asks it for volume serial numbers). */
void ata_claim(void)
{
    for (int ch = 0; ch < 2; ch++) {
        if (!chan_owned[ch])
            continue;
        outb(ctl_base[ch], 0x02);
        for (int p = 0; p < 8; p++)
            tss_trap_port((uint16_t)(io_base[ch] + p), true);
        tss_trap_port(ctl_base[ch], true);
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

bool ata_bios_owned(uint8_t drive)
{
    if (!claimed)
        return false;
    for (int i = 0; i < DISK_MAX; i++)
        if (disks[i].owned && disks[i].bios == drive)
            return true;
    return false;
}

void ata_release(void)
{
    for (int ch = 0; ch < 2; ch++)
        if (chan_owned[ch])
            outb(ctl_base[ch], 0x00);                   /* interrupts back on for the BIOS */
}
