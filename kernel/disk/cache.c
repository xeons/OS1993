/*
 * cache.c -- the block cache: 512-byte sectors of the VMM's disks.
 *
 * A fixed pool, least recently used first out. Writes stay in the cache
 * until cache_sync(), which the file systems call at the end of every
 * operation that changed something (fs.c), so the disk is consistent
 * whenever no operation is running. A buffer in use (refs > 0) is never
 * taken for another sector.
 */
#include "kernel.h"
#include "disk.h"
#include "mm.h"

#define NBUF 160                                /* 80 KB */

static struct buf *pool;
static uint32_t stamp, hits, misses;

void cache_init(void)
{
    pool = kzalloc(NBUF * sizeof *pool);
    if (!pool)
        panic("No memory for the disk cache.");
}

uint32_t cache_hits(void)   { return hits; }
uint32_t cache_misses(void) { return misses; }

static int write_out(struct buf *b)
{
    if (ata_write(b->disk, b->lba, 1, b->data) < 0)
        return -1;
    b->dirty = false;
    return 0;
}

static struct buf *lookup(struct disk *d, uint32_t lba, bool *found)
{
    struct buf *victim = NULL;
    for (int i = 0; i < NBUF; i++) {
        struct buf *b = &pool[i];
        if (b->valid && b->disk == d && b->lba == lba) {
            *found = true;
            return b;
        }
        if (b->refs)
            continue;
        if (!b->valid) {                        /* an empty one beats any */
            if (!victim || victim->valid)
                victim = b;
        } else if (!victim || (victim->valid && b->used < victim->used)) {
            victim = b;
        }
    }
    *found = false;
    if (!victim)
        panic("Disk cache: every buffer is in use.");
    if (victim->valid && victim->dirty && write_out(victim) < 0)
        klog("CACHE: lost a write to LBA %u\n", victim->lba);
    victim->valid = false;
    victim->dirty = false;
    victim->disk = d;
    victim->lba = lba;
    return victim;
}

static struct buf *get(struct disk *d, uint32_t lba, bool read)
{
    bool found;
    struct buf *b = lookup(d, lba, &found);
    if (found) {
        hits++;
    } else {
        misses++;
        if (read && ata_read(d, lba, 1, b->data) < 0)
            return NULL;
        if (!read)
            memset(b->data, 0, SECTOR);
        b->valid = true;
    }
    b->refs++;
    b->used = ++stamp;
    return b;
}

struct buf *bread(struct disk *d, uint32_t lba)     { return get(d, lba, true); }
struct buf *bzero_get(struct disk *d, uint32_t lba)
{
    struct buf *b = get(d, lba, false);
    memset(b->data, 0, SECTOR);
    return b;
}

void bdirty(struct buf *b) { b->dirty = true; }

void brelse(struct buf *b)
{
    if (b && b->refs)
        b->refs--;
}

int cache_sync(void)
{
    int rc = 0;
    struct disk *touched[DISK_MAX] = { 0 };
    /* in LBA order per disk: one sweep of the heads */
    for (;;) {
        struct buf *next = NULL;
        for (int i = 0; i < NBUF; i++) {
            struct buf *b = &pool[i];
            if (b->valid && b->dirty &&
                (!next || b->disk < next->disk || (b->disk == next->disk && b->lba < next->lba)))
                next = b;
        }
        if (!next)
            break;
        if (write_out(next) < 0) {
            klog("CACHE: write error, LBA %u\n", next->lba);
            next->dirty = false;
            rc = -1;
        }
        for (int i = 0; i < DISK_MAX; i++)
            if (touched[i] == next->disk || !touched[i]) {
                touched[i] = next->disk;
                break;
            }
    }
    for (int i = 0; i < DISK_MAX && touched[i]; i++)
        ata_flush(touched[i]);
    return rc;
}

void cache_forget(struct disk *d, uint32_t lba, unsigned n)
{
    for (int i = 0; i < NBUF; i++) {
        struct buf *b = &pool[i];
        if (b->valid && b->disk == d && b->lba - lba < n && !b->refs)
            b->valid = b->dirty = false;
    }
}
