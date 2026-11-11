/*
 * part.c -- partitions: the MBR's four entries and the chain of logical
 * drives in an extended partition. A disk with no partition table but a
 * boot sector at LBA 0 (a "superfloppy") is one partition.
 */
#include "kernel.h"
#include "disk.h"

struct partition parts[PART_MAX];
int nparts;

static bool is_extended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static void add(struct disk *d, uint8_t type, uint32_t start, uint32_t count)
{
    if (nparts == PART_MAX || !count || start >= d->sectors)
        return;
    if (d->sectors - start < count)
        count = d->sectors - start;
    parts[nparts++] = (struct partition){ d, type, start, count };
    klog("DISK: disk %d partition %d: type %02X, %u MB at sector %u\n",
         (int)(d - disks), nparts - 1, type, count / 2048, start);
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* A boot sector with a plausible BPB (no partition table there). */
static bool boot_sector(const uint8_t *s)
{
    uint16_t bps = (uint16_t)(s[11] | s[12] << 8);
    uint8_t spc = s[13];
    return (s[0] == 0xEB || s[0] == 0xE9) && bps == 512 && spc && !(spc & (spc - 1)) && s[16];
}

static void logical(struct disk *d, uint32_t ext_start)
{
    uint32_t ebr = ext_start;
    for (int n = 0; n < PART_MAX; n++) {
        struct buf *b = bread(d, ebr);
        if (!b)
            return;
        const uint8_t *e = b->data + 446;
        uint8_t ok = b->data[510] == 0x55 && b->data[511] == 0xAA;
        uint8_t t0 = e[4], t1 = e[16 + 4];
        uint32_t s0 = le32(e + 8), c0 = le32(e + 12), s1 = le32(e + 16 + 8);
        brelse(b);
        if (!ok)
            return;
        if (t0 && !is_extended(t0))
            add(d, t0, ebr + s0, c0);
        if (!is_extended(t1) || !s1)
            return;
        ebr = ext_start + s1;                           /* the next link, from the extended start */
    }
}

void part_scan(void)
{
    for (int i = 0; i < DISK_MAX; i++) {
        struct disk *d = &disks[i];
        if (!d->owned)
            continue;
        struct buf *b = bread(d, 0);
        if (!b)
            continue;
        uint8_t mbr[SECTOR];
        memcpy(mbr, b->data, SECTOR);
        brelse(b);
        if (mbr[510] != 0x55 || mbr[511] != 0xAA)
            continue;
        if (boot_sector(mbr)) {
            /* A real table's boot flags are 00 or 80 and some type is set;
             * in a boot sector those bytes are code. */
            bool table = false;
            for (int p = 0; p < 4; p++) {
                if (mbr[446 + p * 16] & 0x7F) {
                    table = false;
                    break;
                }
                table |= mbr[446 + p * 16 + 4] != 0;
            }
            if (!table) {
                add(d, 0, 0, d->sectors);
                continue;
            }
        }
        for (int p = 0; p < 4; p++) {
            const uint8_t *e = mbr + 446 + p * 16;
            uint8_t t = e[4];
            if (!t)
                continue;
            if (is_extended(t))
                logical(d, le32(e + 8));
            else
                add(d, t, le32(e + 8), le32(e + 12));
        }
    }
}
