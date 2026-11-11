/*
 * disk.h -- native disk access: the IDE/ATA driver, the block cache and
 * partitions (kernel/disk).
 *
 * The VMM drives the IDE channels (all with a disk and no ATAPI device):
 * V86 code can't reach their ports, and INT 13h for their disks is done
 * by the VMM (int13.c) through its own driver. DOS reaches the volumes the
 * VMM mounted through the drive letters the redirector gives it (redir.c),
 * and its own boot drive through INT 13h.
 *
 * Everything here runs under fs_lock() (fs.h).
 */
#ifndef OS1993_DISK_H
#define OS1993_DISK_H

#include <stdbool.h>
#include <stdint.h>

#define SECTOR      512
#define DISK_MAX    4                   /* two channels, master and slave */
#define PART_MAX    16

struct disk {
    bool     present, owned;            /* owned: the VMM drives it */
    uint8_t  channel, slave;
    uint8_t  bios;                      /* its BIOS drive number, 0: not seen */
    uint16_t cyls, heads;               /* ... and the BIOS's geometry for it (0: none) */
    uint8_t  spt;
    uint16_t bios_cx;                   /* ... as INT 13h AH=08h gave it */
    uint8_t  bios_dh;
    bool     lba48;
    uint32_t sectors;                   /* capped at 2^32-1 */
    char     model[41];
};

struct partition {
    struct disk *disk;
    uint8_t  type;                      /* MBR type, 0: the whole disk (no MBR) */
    uint32_t start, count;              /* sectors */
};

extern struct disk disks[DISK_MAX];
extern struct partition parts[PART_MAX];
extern int nparts;

/* ata.c */
void ata_init(void);                    /* probe; picks the channels to claim */
void ata_claim(void);                   /* ... and takes them from DOS and the BIOS */
int  ata_read(struct disk *d, uint32_t lba, unsigned n, void *buf);     /* 0 or -1 */
int  ata_write(struct disk *d, uint32_t lba, unsigned n, const void *buf);
int  ata_flush(struct disk *d);
bool ata_port_owned(uint16_t port);     /* a port V86 code must not touch */
struct disk *ata_bios_disk(uint8_t drive);  /* INT 13h for this drive is ours */
void ata_release(void);                 /* back to the BIOS (shutdown) */
void ata_tick(void);                    /* timer: wake a transfer whose interrupt never came */

/* int13.c */
struct regs;
void int13_emulate(struct disk *d, struct regs *r);

/* cache.c: 512-byte sectors, written behind (two seconds, or cache_sync()) */
struct buf {
    struct disk *disk;
    uint32_t lba;
    uint16_t refs;
    bool     valid, dirty;
    bool     pinned;                    /* in a journal transaction not yet committed: stays put */
    uint32_t used;                      /* LRU stamp */
    uint8_t  data[SECTOR];
};
void cache_init(void);
struct buf *bread(struct disk *d, uint32_t lba);        /* NULL: I/O error */
struct buf *bzero_get(struct disk *d, uint32_t lba);    /* will be overwritten whole */
void bdirty(struct buf *b);
void brelse(struct buf *b);
int  cache_sync(void);                  /* write every dirty sector; 0 or -1 */
void cache_forget(struct disk *d, uint32_t lba, unsigned n);
int  cache_flush_range(struct disk *d, uint32_t lba, unsigned n);  /* write those dirty in it */
int  cache_flush_disk(struct disk *d);  /* a disk's dirty sectors (not pinned ones) */
void cache_unpin(struct disk *d, uint32_t lba, unsigned n);
uint32_t cache_hits(void), cache_misses(void);
void cache_tick(void);                  /* timer: old dirty sectors wake the flusher */

/* part.c */
void part_scan(void);                   /* MBR and extended partitions of owned disks */

#endif
