/*
 * cache.c -- the block cache: 512-byte sectors of the VMM's disks.
 *
 * 1024 sectors (512 KB), found through a hash table, reused least
 * recently used first; a buffer in use (refs > 0) is never taken for
 * another sector.
 *
 * Write-behind: a changed sector stays in the cache until it is two
 * seconds old (the flusher thread), something needs its buffer, a file is
 * flushed (FlushFileBuffers, DOS's commit, INT 21h AH=0Dh), raw INT 13h
 * reads its range, or OS1993 ends. Writing out sorts the sectors and
 * sends runs of neighbours as one transfer. A pinned sector (an ext3
 * transaction's, until it commits: ext2.c) is never written or reused.
 */
#include "kernel.h"
#include "devices.h"
#include "disk.h"
#include "fs.h"
#include "io.h"
#include "mm.h"
#include "sched.h"

#define NBUF    1024
#define NHASH   512
#define RUN_MAX 64                              /* sectors per write */
#define AGE     (2 * 18)                        /* ticks a dirty sector may wait */

struct entry {
    struct buf b;
    struct entry *hnext;                        /* hash chain */
    struct entry *lprev, *lnext;                /* LRU list (all buffers; head = oldest) */
};

static struct entry *pool;
static struct entry *hash[NHASH];
static struct entry lru;                        /* sentinel */
static uint32_t hits, misses, ndirty, dirty_since;
static uint8_t *runbuf;
static struct thread *flusher;

uint32_t cache_hits(void)   { return hits; }
uint32_t cache_misses(void) { return misses; }

static unsigned hkey(const struct disk *d, uint32_t lba)
{
    return (lba ^ (uint32_t)(uintptr_t)d * 2654435761u) % NHASH;
}

static void lru_unlink(struct entry *e)
{
    e->lprev->lnext = e->lnext;
    e->lnext->lprev = e->lprev;
}

static void lru_append(struct entry *e)          /* the newest */
{
    e->lprev = lru.lprev;
    e->lnext = &lru;
    lru.lprev->lnext = e;
    lru.lprev = e;
}

static void hash_remove(struct entry *e)
{
    unsigned k = hkey(e->b.disk, e->b.lba);
    for (struct entry **p = &hash[k]; *p; p = &(*p)->hnext)
        if (*p == e) {
            *p = e->hnext;
            return;
        }
}

static void mark_clean(struct entry *e)
{
    if (e->b.dirty) {
        e->b.dirty = false;
        ndirty--;
    }
}

static void flusher_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t f = irq_save();
        current->state = T_BLOCKED;
        schedule();
        irq_restore(f);
        fs_lock();
        cache_sync();
        fs_unlock();
    }
}

void cache_init(void)
{
    pool = kzalloc(NBUF * sizeof *pool);
    runbuf = kmalloc(RUN_MAX * SECTOR);
    if (!pool || !runbuf)
        panic("No memory for the disk cache.");
    lru.lprev = lru.lnext = &lru;
    for (int i = 0; i < NBUF; i++)
        lru_append(&pool[i]);
    flusher = thread_create("disk flush", flusher_main, NULL, NULL);
    thread_wake(flusher);                               /* it parks itself */
}

/* Timer: dirty sectors two seconds old wake the flusher. */
void cache_tick(void)
{
    if (ndirty && flusher && flusher->state == T_BLOCKED && (int32_t)(timer_ticks() - dirty_since) >= AGE)
        thread_wake(flusher);
}

static int write_out(struct entry *e)
{
    if (ata_write(e->b.disk, e->b.lba, 1, e->b.data) < 0)
        return -1;
    mark_clean(e);
    return 0;
}

static struct entry *find(struct disk *d, uint32_t lba)
{
    for (struct entry *e = hash[hkey(d, lba)]; e; e = e->hnext)
        if (e->b.valid && e->b.disk == d && e->b.lba == lba)
            return e;
    return NULL;
}

static struct buf *get(struct disk *d, uint32_t lba, bool read)
{
    struct entry *e = find(d, lba);
    if (e) {
        hits++;
    } else {
        misses++;
        for (e = lru.lnext; e != &lru && (e->b.refs || e->b.pinned); e = e->lnext)
            ;
        if (e == &lru)
            panic("Disk cache: every buffer is in use.");
        if (e->b.valid) {
            if (e->b.dirty && write_out(e) < 0) {
                klog("CACHE: lost a write to LBA %u\n", e->b.lba);
                mark_clean(e);
            }
            hash_remove(e);
        }
        e->b.valid = false;
        e->b.pinned = false;
        e->b.disk = d;
        e->b.lba = lba;
        if (read && ata_read(d, lba, 1, e->b.data) < 0)
            return NULL;
        if (!read)
            memset(e->b.data, 0, SECTOR);
        e->b.valid = true;
        unsigned k = hkey(d, lba);
        e->hnext = hash[k];
        hash[k] = e;
    }
    lru_unlink(e);
    lru_append(e);
    e->b.refs++;
    return &e->b;
}

struct buf *bread(struct disk *d, uint32_t lba)     { return get(d, lba, true); }
struct buf *bzero_get(struct disk *d, uint32_t lba)
{
    struct buf *b = get(d, lba, false);
    memset(b->data, 0, SECTOR);
    return b;
}

void bdirty(struct buf *b)
{
    if (!b->dirty) {
        b->dirty = true;
        if (!ndirty++)
            dirty_since = timer_ticks();
    }
}

void brelse(struct buf *b)
{
    if (b && b->refs)
        b->refs--;
}

/* Write the dirty sectors (of one disk and range, or all): sorted, in runs. */
static int flush(struct disk *only, uint32_t lo, uint32_t n)
{
    static struct entry *list[NBUF];
    int count = 0, rc = 0;
    for (int i = 0; i < NBUF; i++) {
        struct entry *e = &pool[i];
        if (e->b.valid && e->b.dirty && !e->b.pinned && (!only || (e->b.disk == only && e->b.lba - lo < n)))
            list[count++] = e;
    }
    for (int i = 1; i < count; i++) {                   /* insertion sort: disk, then sector */
        struct entry *x = list[i];
        int j = i - 1;
        while (j >= 0 && (list[j]->b.disk > x->b.disk ||
                          (list[j]->b.disk == x->b.disk && list[j]->b.lba > x->b.lba))) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = x;
    }
    struct disk *touched[DISK_MAX] = { 0 };
    for (int i = 0; i < count;) {
        int k = 1;
        while (i + k < count && k < RUN_MAX && list[i + k]->b.disk == list[i]->b.disk &&
               list[i + k]->b.lba == list[i]->b.lba + (uint32_t)k)
            k++;
        for (int j = 0; j < k; j++)
            memcpy(runbuf + j * SECTOR, list[i + j]->b.data, SECTOR);
        if (ata_write(list[i]->b.disk, list[i]->b.lba, (unsigned)k, runbuf) < 0) {
            klog("CACHE: write error, LBA %u (+%d)\n", list[i]->b.lba, k);
            rc = -1;
        }
        for (int j = 0; j < k; j++)
            mark_clean(list[i + j]);
        for (int t = 0; t < DISK_MAX; t++)
            if (touched[t] == list[i]->b.disk || !touched[t]) {
                touched[t] = list[i]->b.disk;
                break;
            }
        i += k;
    }
    for (int t = 0; t < DISK_MAX && touched[t]; t++)
        ata_flush(touched[t]);
    if (ndirty)
        dirty_since = timer_ticks();
    return rc;
}

int cache_sync(void)                            { return flush(NULL, 0, 0); }
int cache_flush_disk(struct disk *d)            { return flush(d, 0, 0xFFFFFFFFu); }

void cache_unpin(struct disk *d, uint32_t lba, unsigned n)
{
    for (uint32_t s = 0; s < n; s++) {
        struct entry *e = find(d, lba + s);
        if (e)
            e->b.pinned = false;
    }
}
int cache_flush_range(struct disk *d, uint32_t lba, unsigned n) { return flush(d, lba, n); }

void cache_forget(struct disk *d, uint32_t lba, unsigned n)
{
    for (uint32_t s = 0; s < n; s++) {
        struct entry *e = find(d, lba + s);
        if (e && !e->b.refs) {
            e->b.pinned = false;
            mark_clean(e);
            hash_remove(e);
            e->b.valid = false;
        }
    }
}
