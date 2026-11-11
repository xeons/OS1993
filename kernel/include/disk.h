/*
 * disk.h -- native disk access: the IDE/ATA driver, the block cache and
 * partitions (kernel/disk).
 *
 * The VMM drives the IDE channels that DOS doesn't boot from; a channel
 * holding the BIOS boot disk (drive 80h) stays DOS's, through the BIOS.
 * On a channel the VMM owns, V86 code can't reach the ports and BIOS
 * INT 13h for its disks fails, so DOS reaches them only through the
 * drive letters the redirector gives it (redir.c).
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
bool ata_bios_owned(uint8_t drive);     /* INT 13h for this drive fails */
void ata_release(void);                 /* back to the BIOS (shutdown) */

/* cache.c: 512-byte sectors, write-back until cache_sync() */
struct buf {
    struct disk *disk;
    uint32_t lba;
    uint16_t refs;
    bool     valid, dirty;
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
uint32_t cache_hits(void), cache_misses(void);

/* part.c */
void part_scan(void);                   /* MBR and extended partitions of owned disks */

#endif
