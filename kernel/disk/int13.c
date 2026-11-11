/*
 * int13.c -- INT 13h for the VMM's disks, done by the VMM ("32-bit disk
 * access").
 *
 * DOS (and any program) in any VM still calls INT 13h for its hard disks;
 * the V86 monitor sends the calls for a disk the VMM owns here, and they
 * go through the VMM's IDE driver instead of the BIOS, so the BIOS never
 * touches a channel the VMM drives. CHS addresses use the geometry the
 * BIOS gave the disk at boot (ata.c), so they mean what they meant to the
 * BIOS; the EDD calls (41h-48h) give LBA access.
 *
 * Writes drop the cache's copies of their sectors; reads first write what
 * the cache holds dirty in their range, so both views agree. Interrupts
 * are on while a call runs: transfers take time.
 */
#include "kernel.h"
#include "disk.h"
#include "fs.h"
#include "interrupts.h"
#include "io.h"
#include "v86.h"

#define BDA_STATUS 0x474

static uint8_t *at(uint32_t seg, uint32_t off) { return (uint8_t *)(uintptr_t)(((seg & 0xFFFF) << 4) + (off & 0xFFFF)); }
static void set8h(uint32_t *reg, uint8_t v)    { *reg = (*reg & 0xFFFF00FFu) | (uint32_t)v << 8; }
static void set16(uint32_t *reg, uint32_t v)   { *reg = (*reg & 0xFFFF0000u) | (v & 0xFFFF); }

static int transfer(struct disk *d, uint32_t lba, uint32_t n, uint8_t *buf, bool write, uint32_t *done)
{
    *done = 0;
    if (lba >= d->sectors || d->sectors - lba < n)
        return 0x04;                                    /* sector not found */
    while (*done < n) {
        uint32_t k = n - *done > 128 ? 128 : n - *done;
        uint32_t at_lba = lba + *done;
        int rc;
        fs_lock();
        if (write) {
            rc = ata_write(d, at_lba, k, buf + *done * SECTOR);
            cache_forget(d, at_lba, k);
        } else {
            cache_flush_range(d, at_lba, k);
            rc = ata_read(d, at_lba, k, buf + *done * SECTOR);
        }
        fs_unlock();
        if (rc < 0)
            return write ? 0xCC : 0x10;                 /* write fault / read error */
        *done += k;
    }
    return 0;
}

static uint32_t chs_lba(const struct disk *d, uint32_t cx, uint32_t dx, bool *ok)
{
    uint32_t cyl = ((cx >> 8) & 0xFF) | ((cx & 0xC0) << 2);
    uint32_t sec = cx & 0x3F, head = (dx >> 8) & 0xFF;
    *ok = sec >= 1 && sec <= d->spt && head < d->heads && cyl < d->cyls;
    return (cyl * d->heads + head) * d->spt + sec - 1;
}

void int13_emulate(struct disk *d, struct regs *r)
{
    bool was_on = read_eflags() & 0x200;
    cpu_sti();
    uint8_t ah = (uint8_t)(r->eax >> 8), al = (uint8_t)r->eax;
    uint8_t st = 0;
    uint32_t done = 0;
    bool ok;
    switch (ah) {
    case 0x00: case 0x0D: case 0x10: case 0x11:          /* reset, ready, recalibrate */
        break;
    case 0x01:                                          /* last status */
        st = *at(0, BDA_STATUS);
        set8h(&r->eax, st);
        if (st)
            r->eflags |= EFLAGS_CF;
        else
            r->eflags &= ~EFLAGS_CF;
        goto out;
    case 0x02: case 0x03: case 0x04: {                  /* read, write, verify (CHS) */
        uint32_t lba = chs_lba(d, r->ecx, r->edx, &ok);
        if (!ok || !al) {
            st = ok ? 0x01 : 0x04;
            break;
        }
        if (ah == 0x04) {
            done = lba + al <= d->sectors ? al : 0;
            st = done ? 0 : 0x04;
        } else {
            st = (uint8_t)transfer(d, lba, al, at(r->v86_es, r->ebx), ah == 0x03, &done);
        }
        r->eax = (r->eax & 0xFFFFFF00u) | (done & 0xFF);
        break;
    }
    case 0x08:                                          /* parameters */
        set16(&r->ecx, d->bios_cx);
        set16(&r->edx, (uint32_t)d->bios_dh << 8 | *at(0, 0x475));
        set16(&r->eax, 0);
        r->ebx &= 0xFFFFFF00u;
        break;
    case 0x0C: case 0x47:                               /* seek */
        break;
    case 0x15: {                                        /* type: a fixed disk, CX:DX sectors */
        uint32_t n = (uint32_t)d->cyls * d->heads * d->spt;
        set16(&r->ecx, n >> 16);
        set16(&r->edx, n);
        set8h(&r->eax, 0x03);
        r->eflags &= ~EFLAGS_CF;
        *at(0, BDA_STATUS) = 0;
        goto out;
    }
    case 0x41:                                          /* EDD installed? */
        if ((r->ebx & 0xFFFF) != 0x55AA) {
            st = 0x01;
            break;
        }
        set16(&r->ebx, 0xAA55);
        set16(&r->ecx, 0x0001);                         /* the extended read/write calls */
        set8h(&r->eax, 0x21);                           /* EDD 1.1 */
        r->eflags &= ~EFLAGS_CF;
        goto out;
    case 0x42: case 0x43: case 0x44: {                  /* extended read, write, verify */
        uint8_t *pkt = at(r->v86_ds, r->esi);
        uint32_t n = pkt[2] | pkt[3] << 8;
        uint32_t lba = pkt[8] | pkt[9] << 8 | pkt[10] << 16 | (uint32_t)pkt[11] << 24;
        bool high = pkt[12] | pkt[13] | pkt[14] | pkt[15];
        uint16_t off = (uint16_t)(pkt[4] | pkt[5] << 8), seg = (uint16_t)(pkt[6] | pkt[7] << 8);
        if (high || pkt[0] < 0x10 || n > 127) {
            st = high ? 0x04 : 0x01;
            n = 0;
        } else if (ah == 0x44) {
            st = lba + n <= d->sectors ? 0 : 0x04;
            done = st ? 0 : n;
        } else {
            st = (uint8_t)transfer(d, lba, n, at(seg, off), ah == 0x43, &done);
        }
        pkt[2] = (uint8_t)done;
        pkt[3] = (uint8_t)(done >> 8);
        break;
    }
    case 0x48: {                                        /* drive parameters */
        uint8_t *b = at(r->v86_ds, r->esi);
        uint16_t size = (uint16_t)(b[0] | b[1] << 8);
        if (size < 0x1A) {
            st = 0x01;
            break;
        }
        memset(b + 2, 0, 0x18);
        b[0] = 0x1A;
        b[1] = 0;
        b[2] = 0x02;                                    /* the geometry is valid */
        uint32_t vals[3] = { d->cyls, d->heads, d->spt };
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 4; k++)
                b[4 + i * 4 + k] = (uint8_t)(vals[i] >> (8 * k));
        for (int k = 0; k < 4; k++)
            b[16 + k] = (uint8_t)(d->sectors >> (8 * k));
        b[24] = SECTOR & 0xFF;
        b[25] = SECTOR >> 8;
        break;
    }
    default:
        st = 0x01;                                      /* bad command */
    }
    set8h(&r->eax, st);
    if (st)
        r->eflags |= EFLAGS_CF;
    else
        r->eflags &= ~EFLAGS_CF;
    *at(0, BDA_STATUS) = st;
out:
    if (!was_on)
        cpu_cli();
}
