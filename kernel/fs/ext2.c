/*
 * ext2.c -- the second extended file system, as Linux has had it since 1993.
 *
 * Block groups, each with a block bitmap, an inode bitmap and an inode
 * table; files map their blocks through 12 direct pointers and single,
 * double and triple indirect blocks (a pointer of 0 is a hole, read as
 * zeros). New blocks go next to the file's last one, new inodes in their
 * directory's group (a new directory: the group with the most free
 * inodes); the free counts in the group descriptors and the superblock
 * are kept exact, so e2fsck finds nothing to say.
 *
 * Directories are linked lists of records inside blocks; a name is added
 * in the first record with room after it (or a new block) and removed by
 * folding its record into the one before. A directory with a dir_index
 * hash tree loses the tree's flag when changed: the tree is only a lookup
 * aid, and Linux rebuilds it.
 *
 * Names are bytes on disk, UTF-8 by convention, and Latin-1 here: they are
 * converted both ways ('_' for what Latin-1 lacks). ext2 tells case apart,
 * DOS and Win32 don't: a lookup takes the exact name first, then any case,
 * and a new name may not differ from an existing one only in case. DOS gets
 * an 8.3 alias per name (fs_make_alias). Permissions map to the read-only
 * attribute (the owner's write bit), a leading dot to hidden; symbolic
 * links and devices show as read-only system files. Times are Unix times,
 * taken as the RTC's local time.
 *
 * Volumes with features this driver doesn't know: an incompatible one (ext4
 * extents, 64-bit, flex_bg ...) isn't mounted; a read-only-compatible one,
 * or an ext3 journal, mounts read-only. An ext3 volume that wasn't
 * unmounted cleanly first has its journal replayed (below).
 */
#include "kernel.h"
#include "disk.h"
#include "fsimpl.h"
#include "mm.h"
#include "user.h"

#define A_RO   0x01
#define A_HID  0x02
#define A_SYS  0x04
#define A_DIR  0x10
#define A_ARC  0x20

#define ROOT_INO      2
#define S_IFMT        0xF000
#define S_IFDIR       0x4000
#define S_IFREG       0x8000
#define S_IFLNK       0xA000
#define INDEX_FL      0x1000
#define COMPAT_JOURNAL 0x0004
#define INCOMPAT_FILETYPE 0x0002
#define INCOMPAT_KNOWN    0x0002
#define RO_SPARSE     0x0001
#define RO_LARGE      0x0002
#define RO_KNOWN      (RO_SPARSE | RO_LARGE)
#define FT_REG        1
#define FT_DIR        2
#define FT_SYMLINK    7

#define NODE_MAX  32
#define VOLS      4

#define EV(x)  ((struct evol *)(x))
#define EN(x)  ((struct enode *)(x))
#define CEN(x) ((const struct enode *)(x))

struct group {
    uint32_t bbm, ibm, itab;
    uint16_t free_blocks, free_inodes, dirs;
};

struct evol {
    struct vol h;
    struct disk *disk;
    uint32_t start;
    uint32_t bsize, spb, ppb, bshift;   /* block bytes (1 << bshift), sectors, pointers per block */
    uint32_t nblocks, ninodes, first_data, bpg, ipg, ngroups;
    uint32_t inode_size, first_ino;
    uint16_t extra_isize;
    bool     filetype, ro, changed, sb_dirty;
    uint32_t free_blocks, free_inodes;
    struct group *g;
    bool    *g_dirty;
    /* ext3: journalled writes (journal_*) */
    bool     journal;
    uint32_t *j_map;                    /* the journal's blocks */
    uint32_t j_first, j_maxlen, j_head, j_used, j_seq, j_incompat;
    uint8_t  j_uuid[16];
    uint32_t t_blocks[128], t_n;        /* the transaction in progress: its metadata blocks */
    uint32_t t_revoke[128], t_nrev;     /* ... and the blocks it freed */
    uint8_t *j_buf, *j_copy;
    uint32_t j_start, j_start_seq;      /* the oldest transaction not yet home */
};

struct enode {
    struct fnode h;
    int      refs;
    struct evol *v;
    uint32_t ino;
    uint8_t  raw[128];                  /* the inode's first 128 bytes */
    bool     dirty;
    uint32_t goal;                      /* where its next block should go */
    uint8_t  fcb[11];
};

struct dent {                           /* a directory record found */
    uint32_t ino, off, prev;            /* its offset, and the record before it in its block (or ~0) */
    uint16_t rec_len;
    uint8_t  type;
    char     raw[256];                  /* the name's bytes */
    char     name[FS_NAME_MAX + 1];     /* ... in Latin-1 */
};

static struct evol vols[VOLS];
static int nvols;
static struct enode nodes[NODE_MAX];
static const struct fs_ops ext2_ops;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* ext2 keeps UTC; the RTC and DOS keep local time. */
static uint32_t now_unix(void)
{
    uint16_t d, t;
    uint8_t odd;
    rtc_dos_time_odd(&d, &t, &odd);
    return fs_dos_to_unix(d, t) + odd + (uint32_t)(fs_timezone() * 60);
}

static uint32_t local_to_unix(uint16_t d, uint16_t t) { return fs_dos_to_unix(d, t) + (uint32_t)(fs_timezone() * 60); }
static void unix_to_local(uint32_t u, uint16_t *d, uint16_t *t) { fs_unix_to_dos(u - (uint32_t)(fs_timezone() * 60), d, t); }

/* ---- blocks ------------------------------------------------------------------------- */

static uint32_t blk_lba(const struct evol *v, uint32_t b) { return v->start + b * v->spb; }

/* The sector holding byte off of block b. */
static struct buf *bbyte(struct evol *v, uint32_t b, uint32_t off, uint8_t **p)
{
    struct buf *bf = bread(v->disk, blk_lba(v, b) + off / SECTOR);
    if (bf)
        *p = bf->data + off % SECTOR;
    return bf;
}

static void journal_note(struct evol *v, uint32_t block);
static void journal_forget(struct evol *v, uint32_t block);

/* A metadata sector changed: on ext3, part of the transaction (pinned in
 * the cache until it commits). File data uses bdirty. */
static void meta(struct evol *v, struct buf *bf)
{
    bdirty(bf);
    if (v->journal) {
        bf->pinned = true;
        journal_note(v, (bf->lba - v->start) / v->spb);
    }
}

static int rd32(struct evol *v, uint32_t b, uint32_t off, uint32_t *x)
{
    uint8_t *p;
    struct buf *bf = bbyte(v, b, off, &p);
    if (!bf)
        return -ERROR_GEN_FAILURE;
    *x = le32(p);
    brelse(bf);
    return 0;
}

static int wr32(struct evol *v, uint32_t b, uint32_t off, uint32_t x)
{
    uint8_t *p;
    struct buf *bf = bbyte(v, b, off, &p);
    if (!bf)
        return -ERROR_GEN_FAILURE;
    put32(p, x);
    meta(v, bf);
    brelse(bf);
    v->changed = true;
    return 0;
}

static void zero_any(struct evol *v, uint32_t b, bool is_meta)
{
    for (uint32_t s = 0; s < v->spb; s++) {
        struct buf *bf = bzero_get(v->disk, blk_lba(v, b) + s);
        if (is_meta)
            meta(v, bf);
        else
            bdirty(bf);
        brelse(bf);
    }
    v->changed = true;
}

static void zero_block(struct evol *v, uint32_t b) { zero_any(v, b, true); }   /* metadata */
static void zero_data(struct evol *v, uint32_t b)  { zero_any(v, b, false); }

/* ---- the superblock and group descriptors -------------------------------------------- */

static int write_meta(struct evol *v)
{
    if (v->sb_dirty) {
        struct buf *b = bread(v->disk, v->start + 2);
        if (!b)
            return -ERROR_GEN_FAILURE;
        put32(b->data + 12, v->free_blocks);
        put32(b->data + 16, v->free_inodes);
        put32(b->data + 48, now_unix());                /* s_wtime */
        meta(v, b);
        brelse(b);
        v->sb_dirty = false;
        v->changed = true;
    }
    uint32_t gdt = v->first_data + 1;
    for (uint32_t i = 0; i < v->ngroups; i++) {
        if (!v->g_dirty[i])
            continue;
        uint8_t *p;
        struct buf *b = bbyte(v, gdt + (i * 32) / v->bsize, (i * 32) % v->bsize, &p);
        if (!b)
            return -ERROR_GEN_FAILURE;
        put16(p + 12, v->g[i].free_blocks);
        put16(p + 14, v->g[i].free_inodes);
        put16(p + 16, v->g[i].dirs);
        meta(v, b);
        brelse(b);
        v->g_dirty[i] = false;
        v->changed = true;
    }
    return 0;
}

static int journal_commit(struct evol *v);

static int done(struct evol *v, int rc)
{
    int mrc = write_meta(v);
    if (mrc < 0 && rc >= 0)
        rc = mrc;
    if (v->journal && journal_commit(v) < 0 && rc >= 0)
        rc = -ERROR_GEN_FAILURE;
    if (v->changed) {
        v->changed = false;
        /* write-behind: the cache writes it out (cache.c) */
    }
    fs_unlock();
    return rc;
}

static void set_state(struct evol *v, bool clean)
{
    struct buf *b = bread(v->disk, v->start + 2);
    if (!b)
        return;
    uint16_t st = le16(b->data + 58);
    put16(b->data + 58, clean ? (st | 1) : (st & ~1u));
    bdirty(b);
    brelse(b);
    v->changed = true;
}

/* ---- bitmaps --------------------------------------------------------------------------- */

/* Find a clear bit from `from` in a group's bitmap block, set it. -> bit, or -1. */
static int64_t take_bit(struct evol *v, uint32_t bm, uint32_t from, uint32_t nbits)
{
    for (uint32_t i = from; i < nbits; i++) {
        uint8_t *p;
        struct buf *b = bbyte(v, bm, i / 8, &p);
        if (!b)
            return -1;
        uint8_t byte = *p;
        if (byte == 0xFF && !(i % 8)) {
            brelse(b);
            i += 7;
            continue;
        }
        if (!(byte & (1 << (i % 8)))) {
            *p = (uint8_t)(byte | (1 << (i % 8)));
            meta(v, b);
            brelse(b);
            v->changed = true;
            return i;
        }
        brelse(b);
    }
    return -1;
}

static int clear_bit(struct evol *v, uint32_t bm, uint32_t i)
{
    uint8_t *p;
    struct buf *b = bbyte(v, bm, i / 8, &p);
    if (!b)
        return -ERROR_GEN_FAILURE;
    *p &= (uint8_t)~(1 << (i % 8));
    meta(v, b);
    brelse(b);
    v->changed = true;
    return 0;
}

static uint32_t group_blocks(const struct evol *v, uint32_t g)
{
    uint32_t first = v->first_data + g * v->bpg;
    return v->nblocks - first < v->bpg ? v->nblocks - first : v->bpg;
}

/* A free block, as close after goal as there is. */
static int alloc_block(struct evol *v, uint32_t goal, uint32_t *out)
{
    if (!v->free_blocks)
        return -ERROR_DISK_FULL;
    if (goal < v->first_data || goal >= v->nblocks)
        goal = v->first_data;
    uint32_t g0 = (goal - v->first_data) / v->bpg;
    for (uint32_t k = 0; k <= v->ngroups; k++) {
        uint32_t g = (g0 + k) % v->ngroups;
        if (!v->g[g].free_blocks)
            continue;
        uint32_t from = k == 0 ? (goal - v->first_data) % v->bpg : 0;
        int64_t bit = take_bit(v, v->g[g].bbm, from, group_blocks(v, g));
        if (bit < 0 && from)
            bit = take_bit(v, v->g[g].bbm, 0, from);
        if (bit < 0)
            continue;
        v->g[g].free_blocks--;
        v->g_dirty[g] = true;
        v->free_blocks--;
        v->sb_dirty = true;
        *out = v->first_data + g * v->bpg + (uint32_t)bit;
        return 0;
    }
    return -ERROR_DISK_FULL;
}

static int free_block(struct evol *v, uint32_t b)
{
    if (b < v->first_data || b >= v->nblocks)
        return -ERROR_GEN_FAILURE;
    uint32_t g = (b - v->first_data) / v->bpg;
    if (clear_bit(v, v->g[g].bbm, (b - v->first_data) % v->bpg) < 0)
        return -ERROR_GEN_FAILURE;
    v->g[g].free_blocks++;
    v->g_dirty[g] = true;
    v->free_blocks++;
    v->sb_dirty = true;
    journal_forget(v, b);
    cache_forget(v->disk, blk_lba(v, b), v->spb);
    return 0;
}

/* ---- inodes ----------------------------------------------------------------------------- */

static int inode_loc(struct evol *v, uint32_t ino, uint32_t *blk, uint32_t *off)
{
    if (ino < 1 || ino > v->ninodes)
        return -ERROR_GEN_FAILURE;
    uint32_t g = (ino - 1) / v->ipg, i = (ino - 1) % v->ipg;
    uint32_t byte = i * v->inode_size;
    *blk = v->g[g].itab + byte / v->bsize;
    *off = byte % v->bsize;
    return 0;
}

static int inode_read(struct evol *v, uint32_t ino, uint8_t raw[128])
{
    uint32_t blk, off;
    if (inode_loc(v, ino, &blk, &off) < 0)
        return -ERROR_GEN_FAILURE;
    uint8_t *p;
    struct buf *b = bbyte(v, blk, off, &p);           /* an inode never straddles sectors (128 | 512) */
    if (!b)
        return -ERROR_GEN_FAILURE;
    memcpy(raw, p, 128);
    brelse(b);
    return 0;
}

static int inode_write(struct evol *v, uint32_t ino, const uint8_t raw[128])
{
    uint32_t blk, off;
    if (inode_loc(v, ino, &blk, &off) < 0)
        return -ERROR_GEN_FAILURE;
    uint8_t *p;
    struct buf *b = bbyte(v, blk, off, &p);
    if (!b)
        return -ERROR_GEN_FAILURE;
    memcpy(p, raw, 128);
    meta(v, b);
    brelse(b);
    v->changed = true;
    return 0;
}

static uint16_t i_mode(const uint8_t *r)   { return le16(r); }
static uint64_t i_size(const uint8_t *r)
{
    uint64_t s = le32(r + 4);
    if ((i_mode(r) & S_IFMT) == S_IFREG)
        s |= (uint64_t)le32(r + 108) << 32;             /* large_file: i_size_high */
    return s;
}

static void set_isize(struct evol *v, uint8_t *r, uint64_t s)
{
    put32(r + 4, (uint32_t)s);
    if ((i_mode(r) & S_IFMT) == S_IFREG) {
        put32(r + 108, (uint32_t)(s >> 32));
        if (s >= 0x80000000u) {                         /* a file of 2 GB or more: say so */
            struct buf *b = bread(v->disk, v->start + 2);
            if (b) {
                if (!(le32(b->data + 100) & RO_LARGE)) {
                    put32(b->data + 100, le32(b->data + 100) | RO_LARGE);
                    meta(v, b);
                    v->changed = true;
                }
                brelse(b);
            }
        }
    }
}
static uint16_t i_links(const uint8_t *r)  { return le16(r + 26); }

/* A new inode (zeroed, its whole size) near group g0. */
static int alloc_inode(struct evol *v, uint32_t g0, bool dir, uint32_t *out)
{
    if (!v->free_inodes)
        return -ERROR_CANNOT_MAKE;
    uint32_t best = g0 % v->ngroups;
    if (dir) {                                          /* spread directories out */
        for (uint32_t g = 0; g < v->ngroups; g++)
            if (v->g[g].free_inodes > v->g[best].free_inodes)
                best = g;
    }
    for (uint32_t k = 0; k < v->ngroups; k++) {
        uint32_t g = (best + k) % v->ngroups;
        if (!v->g[g].free_inodes)
            continue;
        uint32_t from = g == 0 ? v->first_ino - 1 : 0;
        int64_t bit = take_bit(v, v->g[g].ibm, from, v->ipg);
        if (bit < 0)
            continue;
        v->g[g].free_inodes--;
        if (dir)
            v->g[g].dirs++;
        v->g_dirty[g] = true;
        v->free_inodes--;
        v->sb_dirty = true;
        uint32_t ino = g * v->ipg + (uint32_t)bit + 1;
        uint32_t blk = 0, off = 0;
        if (inode_loc(v, ino, &blk, &off) < 0)
            return -ERROR_GEN_FAILURE;
        for (uint32_t done_b = 0; done_b < v->inode_size; done_b += 128) {
            uint8_t *p;
            struct buf *b = bbyte(v, blk, off + done_b, &p);
            if (!b)
                return -ERROR_GEN_FAILURE;
            memset(p, 0, 128);
            if (done_b == 128 && v->extra_isize)
                put16(p, v->extra_isize);               /* i_extra_isize */
            meta(v, b);
            brelse(b);
        }
        *out = ino;
        return 0;
    }
    return -ERROR_CANNOT_MAKE;
}

static int free_inode(struct evol *v, uint32_t ino, bool dir)
{
    uint32_t g = (ino - 1) / v->ipg;
    if (clear_bit(v, v->g[g].ibm, (ino - 1) % v->ipg) < 0)
        return -ERROR_GEN_FAILURE;
    v->g[g].free_inodes++;
    if (dir && v->g[g].dirs)
        v->g[g].dirs--;
    v->g_dirty[g] = true;
    v->free_inodes++;
    v->sb_dirty = true;
    return 0;
}

/* ---- the block map ------------------------------------------------------------------------ */

/* Block lb of the file; 0 for a hole. With alloc, holes are filled (zeroed
 * indirect blocks, data blocks left for the caller to fill). */
static int bmap2(struct evol *v, uint8_t *raw, uint32_t lb, bool alloc, uint32_t *goal, uint32_t *out, bool *fresh);

static int bmap(struct evol *v, uint8_t *raw, uint32_t lb, bool alloc, uint32_t *goal, uint32_t *out)
{
    bool fresh;
    return bmap2(v, raw, lb, alloc, goal, out, &fresh);
}

/* fresh: the data block was allocated just now (its old bytes are still there). */
static int bmap2(struct evol *v, uint8_t *raw, uint32_t lb, bool alloc, uint32_t *goal, uint32_t *out, bool *fresh)
{
    *fresh = false;
    uint32_t ppb = v->ppb, depth, idx[4], slot;
    if (lb < 12) {
        depth = 0;
        slot = lb;
    } else if ((lb -= 12) < ppb) {
        depth = 1;
        slot = 12;
        idx[0] = lb;
    } else if ((lb -= ppb) < ppb * ppb) {
        depth = 2;
        slot = 13;
        idx[0] = lb / ppb;
        idx[1] = lb % ppb;
    } else {
        lb -= ppb * ppb;
        if (lb / ppb / ppb >= ppb)
            return -ERROR_DISK_FULL;                    /* past the triple indirect block */
        depth = 3;
        slot = 14;
        idx[0] = lb / ppb / ppb;
        idx[1] = lb / ppb % ppb;
        idx[2] = lb % ppb;
    }
    uint32_t b = le32(raw + 40 + slot * 4);
    int rc;
    if (!b) {
        if (!alloc) {
            *out = 0;
            return 0;
        }
        if ((rc = alloc_block(v, *goal, &b)) < 0)
            return rc;
        *goal = b + 1;
        if (depth)
            zero_block(v, b);
        else
            *fresh = true;
        put32(raw + 40 + slot * 4, b);
        put32(raw + 28, le32(raw + 28) + v->spb);       /* i_blocks, in sectors */
    }
    for (uint32_t d = 0; d < depth; d++) {
        uint32_t next;
        if (rd32(v, b, idx[d] * 4, &next) < 0)
            return -ERROR_GEN_FAILURE;
        if (!next) {
            if (!alloc) {
                *out = 0;
                return 0;
            }
            if ((rc = alloc_block(v, *goal, &next)) < 0)
                return rc;
            *goal = next + 1;
            if (d + 1 < depth)
                zero_block(v, next);
            else
                *fresh = true;
            if (wr32(v, b, idx[d] * 4, next) < 0)
                return -ERROR_GEN_FAILURE;
            put32(raw + 28, le32(raw + 28) + v->spb);
        }
        b = next;
    }
    if (b >= v->nblocks)
        return -ERROR_GEN_FAILURE;
    *out = b;
    return 0;
}

/* Free the blocks under an indirect block from logical block `keep` on
 * (base: the first logical block it maps). -> true if it is now empty. */
static bool trunc_ind(struct evol *v, uint8_t *raw, uint32_t b, int level, uint32_t base, uint32_t keep)
{
    uint32_t span = 1;
    for (int i = 1; i < level; i++)
        span *= v->ppb;
    bool empty = true;
    for (uint32_t i = 0; i < v->ppb; i++) {
        uint32_t lo = base + i * span;
        uint32_t c;
        if (rd32(v, b, i * 4, &c) < 0 || !c)
            continue;
        if (lo + span <= keep || (span > 1 && lo + span < lo)) {    /* all kept */
            empty = false;
            continue;
        }
        bool gone = level == 1 ? lo >= keep : trunc_ind(v, raw, c, level - 1, lo, keep);
        if (gone) {
            free_block(v, c);
            put32(raw + 28, le32(raw + 28) - v->spb);
            wr32(v, b, i * 4, 0);
        } else {
            empty = false;
        }
    }
    return empty;
}

static void truncate_blocks(struct evol *v, uint8_t *raw, uint32_t keep)
{
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t b = le32(raw + 40 + i * 4);
        if (b && i >= keep) {
            free_block(v, b);
            put32(raw + 40 + i * 4, 0);
            put32(raw + 28, le32(raw + 28) - v->spb);
        }
    }
    uint32_t base = 12, span = v->ppb;
    for (int level = 1; level <= 3; level++) {
        uint32_t b = le32(raw + 40 + (11 + level) * 4);
        if (b && (keep < base + span || base + span < base)) {
            if (trunc_ind(v, raw, b, level, base, keep) && keep <= base) {
                free_block(v, b);
                put32(raw + 40 + (11 + level) * 4, 0);
                put32(raw + 28, le32(raw + 28) - v->spb);
            }
        }
        base += span;
        span *= v->ppb;
    }
}

/* ---- file data ----------------------------------------------------------------------------- */

static int data_io(struct evol *v, uint8_t *raw, uint32_t *goal, uint64_t pos, uint8_t *buf, uint32_t len,
                   bool write, bool *raw_changed)
{
    uint32_t done_n = 0;
    while (done_n < len) {
        if ((pos >> v->bshift) > 0xFFFFFFFFu)
            return done_n ? (int)done_n : -ERROR_FILE_TOO_LARGE;
        uint32_t lb = (uint32_t)(pos >> v->bshift), in = (uint32_t)pos & (v->bsize - 1), pb;
        uint32_t blocks_before = le32(raw + 28);
        bool fresh;
        int rc = bmap2(v, raw, lb, write, goal, &pb, &fresh);
        if (le32(raw + 28) != blocks_before)
            *raw_changed = true;
        if (rc < 0)
            return done_n ? (int)done_n : rc;
        uint32_t chunk = v->bsize - in < len - done_n ? v->bsize - in : len - done_n;
        if (fresh && chunk != v->bsize)
            zero_data(v, pb);                           /* no old bytes around a partial write */
        if (!pb) {                                      /* a hole */
            memset(buf + done_n, 0, chunk);
        } else if (in % SECTOR == 0 && chunk % SECTOR == 0 && buf) {
            uint32_t lba = blk_lba(v, pb) + in / SECTOR, n = chunk / SECTOR;
            if (write) {
                if (ata_write(v->disk, lba, n, buf + done_n) < 0)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
                cache_forget(v->disk, lba, n);
                v->changed = true;
            } else {
                cache_flush_range(v->disk, lba, n);
                if (ata_read(v->disk, lba, n, buf + done_n) < 0)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
            }
        } else {
            uint32_t off = in, left = chunk;
            uint8_t *p = buf ? buf + done_n : NULL;
            while (left) {
                uint32_t so = off % SECTOR, k = SECTOR - so < left ? SECTOR - so : left;
                struct buf *b = bread(v->disk, blk_lba(v, pb) + off / SECTOR);
                if (!b)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
                if (write) {
                    if (p) memcpy(b->data + so, p, k);
                    else   memset(b->data + so, 0, k);
                    bdirty(b);
                    v->changed = true;
                } else {
                    memcpy(p, b->data + so, k);
                }
                brelse(b);
                off += k;
                left -= k;
                if (p)
                    p += k;
            }
        }
        done_n += chunk;
        pos += chunk;
    }
    return (int)done_n;
}

/* ---- names --------------------------------------------------------------------------------- */

/* UTF-8 bytes -> Latin-1 ('_' for anything past it, or broken). */
static void from_utf8(const char *in, int n, char *out)
{
    int o = 0;
    for (int i = 0; i < n && o < FS_NAME_MAX;) {
        uint8_t c = (uint8_t)in[i];
        uint32_t cp;
        int len;
        if (c < 0x80)              { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else                       { cp = '_'; len = 1; }
        for (int k = 1; k < len; k++) {
            if (i + k >= n || ((uint8_t)in[i + k] & 0xC0) != 0x80) {
                cp = '_';
                len = k;
                break;
            }
            cp = cp << 6 | ((uint8_t)in[i + k] & 0x3F);
        }
        out[o++] = cp < 0x100 && cp ? (char)cp : '_';
        i += len;
    }
    out[o] = '\0';
}

/* Latin-1 -> UTF-8. -> its length, or -1 if it is too long. */
static int to_utf8(const char *in, char *out)
{
    int o = 0;
    for (const uint8_t *p = (const uint8_t *)in; *p; p++) {
        if (*p < 0x80) {
            if (o + 1 > 255) return -1;
            out[o++] = (char)*p;
        } else {
            if (o + 2 > 255) return -1;
            out[o++] = (char)(0xC0 | *p >> 6);
            out[o++] = (char)(0x80 | (*p & 0x3F));
        }
    }
    out[o] = '\0';
    return o;
}

static uint32_t rec_size(uint32_t name_len) { return (8 + name_len + 3) & ~3u; }

/* The next record with an inode from *off in a directory. 1 found, 0 end. */
static int dir_next(struct evol *v, uint8_t *draw, uint32_t *off, struct dent *e)
{
    uint32_t size = le32(draw + 4);
    uint32_t goal = 0;
    while (*off < size) {
        uint32_t lb = *off / v->bsize, in = *off % v->bsize, pb;
        if (bmap(v, draw, lb, false, &goal, &pb) < 0)
            return -ERROR_GEN_FAILURE;
        if (!pb) {                                      /* a hole in a directory: skip the block */
            *off = (lb + 1) * v->bsize;
            continue;
        }
        /* walk the block from its start, to know the record before */
        uint32_t prev = ~0u, pos = 0;
        while (pos < v->bsize) {
            uint8_t hdr[8];
            uint8_t *p;
            struct buf *b = bbyte(v, pb, pos, &p);
            if (!b)
                return -ERROR_GEN_FAILURE;
            memcpy(hdr, p, 8);
            brelse(b);
            uint16_t rl = le16(hdr + 4);
            if (rl < 8 || rl % 4 || pos + rl > v->bsize) {
                klog("EXT2: bad directory record in block %u at %u\n", pb, pos);
                *off = (lb + 1) * v->bsize;             /* the rest of the block is lost */
                break;
            }
            uint32_t ino = le32(hdr);
            uint32_t nlen = v->filetype ? hdr[6] : le16(hdr + 6);
            if (pos >= in && ino && nlen && nlen <= 255 && 8 + nlen <= rl) {
                e->ino = ino;
                e->off = lb * v->bsize + pos;
                e->prev = prev == ~0u ? ~0u : lb * v->bsize + prev;
                e->rec_len = rl;
                e->type = v->filetype ? hdr[7] : 0;
                for (uint32_t k = 0; k < nlen; k++) {   /* a name may cross a sector */
                    struct buf *nb = bbyte(v, pb, pos + 8 + k, &p);
                    if (!nb)
                        return -ERROR_GEN_FAILURE;
                    e->raw[k] = (char)*p;
                    brelse(nb);
                }
                e->raw[nlen] = '\0';
                from_utf8(e->raw, (int)nlen, e->name);
                *off = e->off + rl;
                return 1;
            }
            prev = pos;
            pos += rl;
        }
        if (pos >= v->bsize)
            *off = (lb + 1) * v->bsize;
    }
    return 0;
}

/* DOS's alias for a record: numbered past the earlier records sharing it. */
static void alias_of(struct evol *v, uint8_t *draw, const struct dent *e, uint8_t fcb[11], char *alias)
{
    if (!fs_make_alias(e->name, fs_hash16(e->raw), fcb, alias))
        return;
    static struct dent o;
    uint32_t off = 0;
    int n = 1;
    while (dir_next(v, draw, &off, &o) > 0 && o.off < e->off) {
        uint8_t f2[11];
        char a2[13];
        n += fs_make_alias(o.name, fs_hash16(o.raw), f2, a2) && !memcmp(f2, fcb, 11);
    }
    if (n > 1 && n <= 9)
        fs_alias_number(fcb, alias, n);
}

static int dir_find(struct evol *v, uint32_t dino, const char *name, struct dent *e)
{
    uint8_t draw[128];
    if (inode_read(v, dino, draw) < 0)
        return -ERROR_GEN_FAILURE;
    if ((i_mode(draw) & S_IFMT) != S_IFDIR)
        return -ERROR_PATH_NOT_FOUND;
    char want[256];
    if (to_utf8(name, want) < 0)
        return -ERROR_FILE_NOT_FOUND;
    uint8_t base[11];
    int want_n = 0;
    bool numbered = fs_alias_parse(name, base, &want_n);
    /* the exact bytes first, then any case (in Latin-1), then an alias */
    for (int pass = 0; pass < 3; pass++) {
        uint32_t off = 0;
        int rc, seen = 0;
        while ((rc = dir_next(v, draw, &off, e)) > 0) {
            if (pass == 0 && !strcmp(e->raw, want))
                return 0;
            if (pass == 1 && fs_name_eq(e->name, name))
                return 0;
            if (pass == 2) {
                uint8_t fcb[11];
                char alias[13];
                bool hashed = fs_make_alias(e->name, fs_hash16(e->raw), fcb, alias);
                if (!hashed && fs_name_eq(alias, name))
                    return 0;
                if (numbered && hashed && !memcmp(fcb, base, 11) && ++seen == want_n)
                    return 0;
            }
        }
        if (rc < 0)
            return rc;
    }
    return -ERROR_FILE_NOT_FOUND;
}

static bool is_dots(const char *n) { return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])); }

/* To the directory holding the path's last component. */
static int walk(struct evol *v, const char *path, uint32_t *dino, const char **last)
{
    uint32_t d = ROOT_INO;
    const char *s = path;
    for (;;) {
        const char *e = s;
        while (*e && *e != '\\')
            e++;
        if (!*e) {
            *dino = d;
            *last = s;
            return 0;
        }
        char comp[FS_NAME_MAX + 1];
        uint32_t len = (uint32_t)(e - s);
        if (len == 0 || len > FS_NAME_MAX)
            return -ERROR_PATH_NOT_FOUND;
        memcpy(comp, s, len);
        comp[len] = '\0';
        static struct dent de;
        int rc = dir_find(v, d, comp, &de);
        if (rc < 0)
            return -ERROR_PATH_NOT_FOUND;
        uint8_t raw[128];
        if (inode_read(v, de.ino, raw) < 0 || (i_mode(raw) & S_IFMT) != S_IFDIR)
            return -ERROR_PATH_NOT_FOUND;
        d = de.ino;
        s = e + 1;
    }
}

/* Write a record at offset off of directory draw (its block is mapped). */
static int put_record(struct evol *v, uint8_t *draw, uint32_t off, uint32_t ino, uint16_t rl,
                      const char *raw, uint8_t type)
{
    uint32_t goal = 0, pb;
    if (bmap(v, draw, off / v->bsize, false, &goal, &pb) < 0 || !pb)
        return -ERROR_GEN_FAILURE;
    uint32_t pos = off % v->bsize, n = (uint32_t)strlen(raw);
    uint8_t rec[8 + 256];
    put32(rec, ino);
    put16(rec + 4, rl);
    rec[6] = (uint8_t)n;
    rec[7] = v->filetype ? type : 0;
    memcpy(rec + 8, raw, n);
    uint32_t total = 8 + n;
    for (uint32_t k = 0; k < total; k++) {
        uint8_t *p;
        struct buf *b = bbyte(v, pb, pos + k, &p);
        if (!b)
            return -ERROR_GEN_FAILURE;
        *p = rec[k];
        meta(v, b);
        brelse(b);
    }
    v->changed = true;
    return 0;
}

static int set_rec_len(struct evol *v, uint8_t *draw, uint32_t off, uint16_t rl)
{
    uint32_t goal = 0, pb;
    if (bmap(v, draw, off / v->bsize, false, &goal, &pb) < 0 || !pb)
        return -ERROR_GEN_FAILURE;
    uint8_t *p;
    struct buf *b = bbyte(v, pb, off % v->bsize + 4, &p);
    if (!b)
        return -ERROR_GEN_FAILURE;
    put16(p, rl);
    meta(v, b);
    brelse(b);
    v->changed = true;
    return 0;
}

static void touch_dir(struct evol *v, uint8_t *draw)
{
    uint32_t t = now_unix();
    put32(draw + 16, t);                                /* mtime */
    put32(draw + 12, t);                                /* ctime */
    put32(draw + 32, le32(draw + 32) & ~(uint32_t)INDEX_FL);   /* the hash tree is out of date */
    (void)v;
}

/* Add name -> ino to directory dino. */
static int dir_add(struct evol *v, uint32_t dino, const char *name, uint32_t ino, uint8_t type)
{
    char raw[256];
    int n = to_utf8(name, raw);
    if (n < 0)
        return -ERROR_FILENAME_EXCED_RANGE;
    uint32_t need = rec_size((uint32_t)n);
    uint8_t draw[128];
    if (inode_read(v, dino, draw) < 0)
        return -ERROR_GEN_FAILURE;
    uint32_t size = le32(draw + 4);
    /* room after a record (or an empty first record) */
    for (uint32_t blk = 0; blk * v->bsize < size; blk++) {
        uint32_t goal = 0, pb;
        if (bmap(v, draw, blk, false, &goal, &pb) < 0)
            return -ERROR_GEN_FAILURE;
        if (!pb)
            continue;
        uint32_t pos = 0;
        while (pos < v->bsize) {
            uint8_t hdr[8];
            uint8_t *p;
            struct buf *b = bbyte(v, pb, pos, &p);
            if (!b)
                return -ERROR_GEN_FAILURE;
            memcpy(hdr, p, 8);
            brelse(b);
            uint16_t rl = le16(hdr + 4);
            if (rl < 8 || rl % 4 || pos + rl > v->bsize)
                break;
            uint32_t used = le32(hdr) ? rec_size(v->filetype ? hdr[6] : le16(hdr + 6)) : 0;
            if (rl - used >= need) {
                uint32_t at = blk * v->bsize + pos;
                int rc;
                if (used) {
                    if ((rc = set_rec_len(v, draw, at, (uint16_t)used)) < 0)
                        return rc;
                    at += used;
                }
                if ((rc = put_record(v, draw, at, ino, (uint16_t)(rl - used), raw, type)) < 0)
                    return rc;
                touch_dir(v, draw);
                return inode_write(v, dino, draw);
            }
            pos += rl;
        }
    }
    /* a new block */
    uint32_t goal = 0, pb;
    for (uint32_t i = 0; i < 12; i++)
        if (le32(draw + 40 + i * 4))
            goal = le32(draw + 40 + i * 4) + 1;
    int rc = bmap(v, draw, size / v->bsize, true, &goal, &pb);
    if (rc < 0)
        return rc;
    zero_block(v, pb);
    put32(draw + 4, size + v->bsize);
    if ((rc = put_record(v, draw, size, ino, (uint16_t)v->bsize, raw, type)) < 0)
        return rc;
    touch_dir(v, draw);
    return inode_write(v, dino, draw);
}

static int dir_remove(struct evol *v, uint32_t dino, const struct dent *e)
{
    uint8_t draw[128];
    if (inode_read(v, dino, draw) < 0)
        return -ERROR_GEN_FAILURE;
    int rc;
    if (e->prev == ~0u) {                               /* first in its block: just unused */
        uint32_t goal = 0, pb;
        if (bmap(v, draw, e->off / v->bsize, false, &goal, &pb) < 0 || !pb)
            return -ERROR_GEN_FAILURE;
        rc = wr32(v, pb, e->off % v->bsize, 0);
    } else {
        uint32_t goal = 0, pb;
        if (bmap(v, draw, e->prev / v->bsize, false, &goal, &pb) < 0 || !pb)
            return -ERROR_GEN_FAILURE;
        uint8_t *p;
        struct buf *b = bbyte(v, pb, e->prev % v->bsize + 4, &p);
        if (!b)
            return -ERROR_GEN_FAILURE;
        put16(p, le16(p) + e->rec_len);
        meta(v, b);
        brelse(b);
        v->changed = true;
        rc = 0;
    }
    if (rc < 0)
        return rc;
    touch_dir(v, draw);
    return inode_write(v, dino, draw);
}

/* What DOS and Win32 see of an inode. */
static uint8_t attr_of(const uint8_t *raw, const char *name)
{
    uint16_t m = i_mode(raw);
    uint8_t a;
    switch (m & S_IFMT) {
    case S_IFDIR: a = A_DIR; break;
    case S_IFREG: a = A_ARC; break;
    default:      a = A_SYS | A_RO; break;
    }
    if (!(m & 0200))
        a |= A_RO;
    if (name && name[0] == '.' && !is_dots(name))
        a |= A_HID;
    return a;
}

/* draw: the directory, to number the alias (NULL: as it comes) */
static void fill_entry(struct evol *v, uint8_t *draw, const struct dent *d, const uint8_t *raw, struct fs_entry *e)
{
    memcpy(e->name, d->name, strlen(d->name) + 1);
    if (draw)
        alias_of(v, draw, d, e->fcb_name, e->short_name);
    else
        fs_make_alias(d->name, fs_hash16(d->raw), e->fcb_name, e->short_name);
    if (is_dots(d->name)) {
        memset(e->fcb_name, ' ', 11);
        memcpy(e->fcb_name, d->name, strlen(d->name));
        memcpy(e->short_name, d->name, strlen(d->name) + 1);
    }
    e->attr = attr_of(raw, d->name);
    e->size = (e->attr & A_DIR) ? 0 : i_size(raw);
    unix_to_local(le32(raw + 16), &e->date, &e->time);
    unix_to_local(le32(raw + 12), &e->cdate, &e->ctime);
    uint16_t t;
    unix_to_local(le32(raw + 8), &e->adate, &t);
    e->cluster = d->ino;
    e->lfn = strcmp(e->name, e->short_name) != 0;
}

/* ---- ext3: replaying the journal -------------------------------------------------- */

/*
 * ext3's journal (JBD, and JBD2's compatible layout) is a file -- usually
 * inode 8 -- of big-endian records: a superblock, then transactions, each
 * descriptor blocks (which name the blocks that follow, copies of
 * metadata blocks), revoke blocks (blocks not to replay from this
 * transaction or earlier ones) and a commit block. A volume that wasn't
 * unmounted cleanly says needs_recovery; its journal starts at s_start.
 *
 * Replay is the kernel's and e2fsck's three passes: find the last
 * committed transaction (a transaction without its commit block didn't
 * happen), gather the revoke records, then write every logged block whose
 * number no later revoke covers, restoring a first word the journal had to
 * escape. Then the journal is empty (s_start 0, s_sequence the next one)
 * and needs_recovery goes.
 *
 * Checksums (journal_csum_v2/v3) are read past, not checked; a 64-bit
 * journal's blocks past 2^32 are skipped (this driver's block numbers are
 * 32-bit); an external journal device can't be replayed (not mounted).
 */
#define JBD_MAGIC      0xC03B3998u
#define JBD_DESC       1
#define JBD_COMMIT     2
#define JBD_SB_V1      3
#define JBD_SB_V2      4
#define JBD_REVOKE     5
#define JF_ESCAPE      1
#define JF_SAME_UUID   2
#define JF_LAST        8
#define JI_REVOKE      0x01
#define JI_64BIT       0x02
#define JI_ASYNC       0x04
#define JI_CSUM2       0x08
#define JI_CSUM3       0x10
#define JI_KNOWN       0x1F
#define INCOMPAT_RECOVER  0x0004
#define INCOMPAT_JOURNAL_DEV 0x0008

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void putbe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

struct journal {
    struct evol *v;
    uint8_t  raw[128];                  /* the journal's inode */
    uint32_t first, maxlen, incompat;
    uint8_t *blk;                       /* one block */
    struct { uint32_t block, seq; } *revoked;
    uint32_t nrevoked, cap;
};

static int block_io(struct evol *v, uint32_t b, uint8_t *buf, bool write)
{
    for (uint32_t s = 0; s < v->spb; s++) {
        struct buf *bf = write ? bzero_get(v->disk, blk_lba(v, b) + s) : bread(v->disk, blk_lba(v, b) + s);
        if (!bf)
            return -1;
        if (write) {
            memcpy(bf->data, buf + s * SECTOR, SECTOR);
            bdirty(bf);
        } else {
            memcpy(buf + s * SECTOR, bf->data, SECTOR);
        }
        brelse(bf);
    }
    return 0;
}

/* Journal block j (wrapping past the end to s_first) into j->blk. */
static uint32_t jwrap(const struct journal *j, uint32_t b)
{
    if (!j->maxlen || b < j->maxlen)
        return b;                                       /* (block 0, before the bounds are known) */
    return j->first + (b - j->maxlen) % (j->maxlen - j->first);
}

static int jread(struct journal *j, uint32_t lb)
{
    uint32_t goal = 0, pb;
    if (bmap(j->v, j->raw, jwrap(j, lb), false, &goal, &pb) < 0 || !pb)
        return -1;
    return block_io(j->v, pb, j->blk, false);
}

static uint32_t tag_bytes(const struct journal *j)
{
    if (j->incompat & JI_CSUM3)
        return 16;
    return 8 + ((j->incompat & JI_64BIT) ? 4 : 0);
}

/* The tags of the descriptor block in j->blk: calls fn for each (or just
 * counts them). -> how many. */
static uint32_t each_tag(struct journal *j, void (*fn)(struct journal *, uint32_t blocknr, uint32_t high,
                                                           uint32_t flags, uint32_t index, void *ctx), void *ctx)
{
    uint32_t end = j->v->bsize - ((j->incompat & (JI_CSUM2 | JI_CSUM3)) ? 4 : 0);
    uint32_t tsz = tag_bytes(j), n = 0;
    for (uint32_t off = 12; off + tsz <= end;) {
        const uint8_t *t = j->blk + off;
        uint32_t flags = (j->incompat & JI_CSUM3) ? be32(t + 4) : be16(t + 6);
        uint32_t high = (j->incompat & JI_64BIT) ? be32(t + 8) : 0;
        if (fn)
            fn(j, be32(t), high, flags, n, ctx);
        n++;
        off += tsz + ((flags & JF_SAME_UUID) ? 0 : 16);
        if (flags & JF_LAST)
            break;
    }
    return n;
}

static void add_revoke(struct journal *j, uint32_t block, uint32_t seq)
{
    for (uint32_t i = 0; i < j->nrevoked; i++)
        if (j->revoked[i].block == block) {
            if ((int32_t)(seq - j->revoked[i].seq) > 0)
                j->revoked[i].seq = seq;
            return;
        }
    if (j->nrevoked == j->cap) {
        uint32_t cap = j->cap ? j->cap * 2 : 64;
        void *n = kmalloc(cap * sizeof *j->revoked);
        if (!n)
            return;
        if (j->revoked) {
            memcpy(n, j->revoked, j->nrevoked * sizeof *j->revoked);
            kfree(j->revoked);
        }
        j->revoked = n;
        j->cap = cap;
    }
    j->revoked[j->nrevoked].block = block;
    j->revoked[j->nrevoked++].seq = seq;
}

static bool is_revoked(const struct journal *j, uint32_t block, uint32_t seq)
{
    for (uint32_t i = 0; i < j->nrevoked; i++)
        if (j->revoked[i].block == block)
            return (int32_t)(j->revoked[i].seq - seq) >= 0;
    return false;
}

struct replay_ctx { uint32_t data_at, seq, written; uint8_t *copy; };

static void replay_tag(struct journal *j, uint32_t blocknr, uint32_t high, uint32_t flags, uint32_t index, void *c)
{
    struct replay_ctx *r = c;
    if (high || blocknr >= j->v->nblocks || is_revoked(j, blocknr, r->seq))
        return;
    uint32_t goal = 0, pb;
    if (bmap(j->v, j->raw, jwrap(j, r->data_at + index), false, &goal, &pb) < 0 || !pb ||
        block_io(j->v, pb, r->copy, false) < 0)
        return;
    if (flags & JF_ESCAPE)
        putbe32(r->copy, JBD_MAGIC);                    /* the journal had to hide it */
    if (block_io(j->v, blocknr, r->copy, true) == 0)
        r->written++;
}

/* -> 0 replayed (or nothing to do), <0 it can't be. */
static int journal_replay(struct evol *v, uint32_t inum)
{
    struct journal j = { .v = v };
    int rc = -1;
    if (!inum || inode_read(v, inum, j.raw) < 0 || !(j.blk = kmalloc(v->bsize)))
        return -1;
    uint8_t *copy = kmalloc(v->bsize);
    if (!copy || jread(&j, 0) < 0)
        goto out;
    uint8_t *sb = j.blk;
    uint32_t type = be32(sb + 4);
    if (be32(sb) != JBD_MAGIC || (type != JBD_SB_V1 && type != JBD_SB_V2) || be32(sb + 12) != v->bsize) {
        klog("EXT2: the journal's superblock is not one\n");
        goto out;
    }
    j.maxlen = be32(sb + 16);
    j.first = be32(sb + 20);
    uint32_t seq0 = be32(sb + 24), start = be32(sb + 28);
    j.incompat = type == JBD_SB_V2 ? be32(sb + 40) : 0;
    if (j.incompat & ~(uint32_t)JI_KNOWN) {
        klog("EXT2: journal features %X: can't replay\n", j.incompat);
        goto out;
    }
    if (!j.first || j.first >= j.maxlen) {
        klog("EXT2: the journal's bounds are wrong\n");
        goto out;
    }
    uint32_t end_seq = seq0, transactions = 0;
    if (start) {
        /* pass 1: to the last commit */
        uint32_t b = start, seq = seq0;
        for (uint32_t steps = 0; steps < j.maxlen; steps++) {
            if (jread(&j, b) < 0 || be32(j.blk) != JBD_MAGIC || be32(j.blk + 8) != seq)
                break;
            uint32_t t = be32(j.blk + 4);
            if (t == JBD_DESC) {
                b += 1 + each_tag(&j, NULL, NULL);
            } else if (t == JBD_COMMIT) {
                seq++;
                b++;
                end_seq = seq;
                transactions++;
            } else if (t == JBD_REVOKE) {
                b++;
            } else {
                break;
            }
            b = jwrap(&j, b);
        }
        /* pass 2: revoke records; pass 3: the blocks */
        for (int pass = 2; pass <= 3; pass++) {
            struct replay_ctx r = { .copy = copy };
            b = start;
            seq = seq0;
            while ((int32_t)(end_seq - seq) > 0) {
                if (jread(&j, b) < 0)
                    break;
                uint32_t t = be32(j.blk + 4);
                if (t == JBD_DESC) {
                    r.data_at = b + 1;
                    r.seq = seq;
                    uint32_t n = pass == 3 ? each_tag(&j, replay_tag, &r) : each_tag(&j, NULL, NULL);
                    b += 1 + n;
                } else if (t == JBD_COMMIT) {
                    seq++;
                    b++;
                } else if (t == JBD_REVOKE) {
                    if (pass == 2) {
                        uint32_t used = be32(j.blk + 12), rsz = (j.incompat & JI_64BIT) ? 8 : 4;
                        if (used > v->bsize)
                            used = v->bsize;
                        for (uint32_t off = 16; off + rsz <= used; off += rsz)
                            if (rsz == 4 || !be32(j.blk + off))
                                add_revoke(&j, be32(j.blk + off + rsz - 4), seq);
                    }
                    b++;
                } else {
                    break;
                }
                b = jwrap(&j, b);
            }
            if (pass == 3 && transactions)
                klog("EXT2: replayed %u transaction(s) from the journal: %u block(s), %u revoked\n",
                     transactions, r.written, j.nrevoked);
        }
    }
    /* the journal is empty now */
    if (jread(&j, 0) < 0)
        goto out;
    putbe32(j.blk + 24, end_seq);
    putbe32(j.blk + 28, 0);
    uint32_t goal = 0, pb;
    if (bmap(v, j.raw, 0, false, &goal, &pb) < 0 || !pb || block_io(v, pb, j.blk, true) < 0)
        goto out;
    struct buf *b0 = bread(v->disk, v->start + 2);
    if (!b0)
        goto out;
    put32(b0->data + 96, le32(b0->data + 96) & ~(uint32_t)INCOMPAT_RECOVER);
    bdirty(b0);
    brelse(b0);
    rc = cache_sync() < 0 ? -1 : 0;
out:
    kfree(j.blk);
    kfree(copy);
    kfree(j.revoked);
    return rc;
}

/* ---- ext3: journalled writes ------------------------------------------------------ */

/*
 * Ordered mode, as ext3 defaults to. Every metadata block a call changes
 * (superblock, group descriptors, bitmaps, inode tables, directory and
 * indirect blocks) joins the transaction and stays pinned in the cache;
 * blocks it frees are revoked. At the end of the call (done()): the file
 * data still in the cache goes to the disk first, then the transaction --
 * descriptor blocks naming the blocks, their copies (a first word that
 * looks like the journal's magic number escaped), revoke records -- then,
 * once those are on the disk, the commit block. Only then may the blocks
 * go home, written behind like any other. A call too big for one
 * transaction (it fills the pinned limit) is committed in parts.
 *
 * When the journal is full, everything is written home and the journal
 * starts over (a checkpoint); the journal superblock points at the oldest
 * transaction not yet home. While mounted the volume says needs_recovery;
 * a clean shutdown writes everything home, empties the journal and clears
 * it. A journal with checksums (csum v2/v3) or async commit, an external
 * one, or a JBD v1 superblock keeps the volume read-only.
 */
static int journal_commit(struct evol *v);

static uint32_t jnext(const struct evol *v, uint32_t b) { return b + 1 >= v->j_maxlen ? v->j_first : b + 1; }

static uint32_t pin_limit(const struct evol *v)
{
    uint32_t n = 256 / v->spb;                          /* at most a quarter of the cache pinned */
    return n < 2 ? 2 : n > 128 ? 128 : n;
}

static void unpin_block(struct evol *v, uint32_t b)
{
    cache_unpin(v->disk, blk_lba(v, b), v->spb);
}

static void journal_note(struct evol *v, uint32_t block)
{
    for (uint32_t i = 0; i < v->t_n; i++)
        if (v->t_blocks[i] == block)
            return;
    if (v->t_n >= pin_limit(v))
        journal_commit(v);                              /* a big call: this part first */
    v->t_blocks[v->t_n++] = block;
}

/* A block freed: not to be replayed from the journal any more. */
static void journal_forget(struct evol *v, uint32_t block)
{
    if (!v->journal)
        return;
    for (uint32_t i = 0; i < v->t_n; i++)
        if (v->t_blocks[i] == block) {
            unpin_block(v, block);
            v->t_blocks[i] = v->t_blocks[--v->t_n];
            break;
        }
    for (uint32_t i = 0; i < v->t_nrev; i++)
        if (v->t_revoke[i] == block)
            return;
    if (v->t_nrev == 128)
        journal_commit(v);
    v->t_revoke[v->t_nrev++] = block;
}

static int jwrite(struct evol *v, uint32_t jb, const uint8_t *buf)
{
    uint32_t pb = v->j_map[jb];
    cache_forget(v->disk, blk_lba(v, pb), v->spb);
    return ata_write(v->disk, blk_lba(v, pb), v->spb, buf);
}

/* The journal superblock: where the log starts (0: empty), its sequence. */
static int jsb_write(struct evol *v, uint32_t start, uint32_t seq)
{
    if (ata_read(v->disk, blk_lba(v, v->j_map[0]), v->spb, v->j_buf) < 0)
        return -1;
    putbe32(v->j_buf + 24, seq);
    putbe32(v->j_buf + 28, start);
    if (v->t_nrev) {                                    /* revoke records need the feature */
        v->j_incompat |= JI_REVOKE;
        putbe32(v->j_buf + 40, v->j_incompat);
    }
    if (jwrite(v, 0, v->j_buf) < 0)
        return -1;
    return ata_flush(v->disk);
}

/* Everything home; the journal starts over. */
static int journal_checkpoint(struct evol *v)
{
    if (v->j_used)
        klog("EXT2: journal full at transaction %u: everything home, it starts over\n", v->j_seq);
    if (cache_flush_disk(v->disk) < 0)
        return -1;
    v->j_used = 0;
    return jsb_write(v, 0, v->j_seq);
}

static void jheader(uint8_t *b, uint32_t type, uint32_t seq, uint32_t bsize)
{
    memset(b, 0, bsize);
    putbe32(b, JBD_MAGIC);
    putbe32(b + 4, type);
    putbe32(b + 8, seq);
}

static int journal_commit(struct evol *v)
{
    if (!v->journal || (!v->t_n && !v->t_nrev))
        return 0;
    uint32_t bs = v->bsize, tsz = (v->j_incompat & JI_64BIT) ? 12 : 8;
    uint32_t per_desc = (bs - 12 - 16) / tsz;
    uint32_t rsz = (v->j_incompat & JI_64BIT) ? 8 : 4, per_rev = (bs - 16) / rsz;
    uint32_t ndesc = (v->t_n + per_desc - 1) / per_desc, nrevb = (v->t_nrev + per_rev - 1) / per_rev;
    uint32_t need = ndesc + v->t_n + nrevb + 1, cap = v->j_maxlen - v->j_first;
    int rc = 0;
    if (need > cap) {                                   /* (a journal smaller than a transaction) */
        for (uint32_t i = 0; i < v->t_n; i++)
            unpin_block(v, v->t_blocks[i]);
        v->t_n = v->t_nrev = 0;
        return cache_flush_disk(v->disk);
    }
    if (cache_flush_disk(v->disk) < 0)                  /* ordered: the file data first */
        rc = -1;
    if (v->j_used + need > cap && journal_checkpoint(v) < 0)
        rc = -1;
    bool fresh_log = !v->j_used;
    if ((fresh_log || (v->t_nrev && !(v->j_incompat & JI_REVOKE))) &&
        jsb_write(v, fresh_log ? v->j_head : v->j_start, fresh_log ? v->j_seq : v->j_start_seq) < 0)
        rc = -1;                                        /* a new log: say where it starts */
    if (fresh_log) {
        v->j_start = v->j_head;
        v->j_start_seq = v->j_seq;
    }
    uint32_t pos = v->j_head, seq = v->j_seq;
    for (uint32_t i = 0; i < v->t_n;) {                 /* descriptors and copies */
        uint32_t k = v->t_n - i < per_desc ? v->t_n - i : per_desc;
        jheader(v->j_buf, JBD_DESC, seq, bs);
        uint32_t off = 12;
        for (uint32_t j = 0; j < k; j++) {
            uint32_t blk = v->t_blocks[i + j];
            uint8_t *p;
            struct buf *b0 = bbyte(v, blk, 0, &p);
            uint32_t first = b0 ? be32(p) : 0;
            brelse(b0);
            uint32_t flags = (j ? JF_SAME_UUID : 0) | (first == JBD_MAGIC ? JF_ESCAPE : 0) | (j + 1 == k ? JF_LAST : 0);
            putbe32(v->j_buf + off, blk);
            v->j_buf[off + 6] = (uint8_t)(flags >> 8);
            v->j_buf[off + 7] = (uint8_t)flags;
            off += tsz;
            if (!j) {
                memcpy(v->j_buf + off, v->j_uuid, 16);
                off += 16;
            }
        }
        if (jwrite(v, pos, v->j_buf) < 0)
            rc = -1;
        pos = jnext(v, pos);
        for (uint32_t j = 0; j < k; j++) {
            uint32_t blk = v->t_blocks[i + j];
            if (block_io(v, blk, v->j_copy, false) < 0)
                rc = -1;
            if (be32(v->j_copy) == JBD_MAGIC)
                putbe32(v->j_copy, 0);                  /* escaped: replay puts it back */
            if (jwrite(v, pos, v->j_copy) < 0)
                rc = -1;
            pos = jnext(v, pos);
        }
        i += k;
    }
    for (uint32_t i = 0; i < v->t_nrev;) {              /* revoke records */
        uint32_t k = v->t_nrev - i < per_rev ? v->t_nrev - i : per_rev;
        jheader(v->j_buf, JBD_REVOKE, seq, bs);
        putbe32(v->j_buf + 12, 16 + k * rsz);
        for (uint32_t j = 0; j < k; j++)
            putbe32(v->j_buf + 16 + j * rsz + rsz - 4, v->t_revoke[i + j]);
        if (jwrite(v, pos, v->j_buf) < 0)
            rc = -1;
        pos = jnext(v, pos);
        i += k;
    }
    if (ata_flush(v->disk) < 0)                         /* all of it on the disk ... */
        rc = -1;
    jheader(v->j_buf, JBD_COMMIT, seq, bs);             /* ... before the commit block */
    if (jwrite(v, pos, v->j_buf) < 0 || ata_flush(v->disk) < 0)
        rc = -1;
    pos = jnext(v, pos);
    v->j_head = pos;
    v->j_used += need;
    v->j_seq++;
    for (uint32_t i = 0; i < v->t_n; i++)               /* now they may go home */
        unpin_block(v, v->t_blocks[i]);
    v->t_n = v->t_nrev = 0;
    if (rc < 0)
        klog("EXT2: a journal write failed\n");
    return rc;
}

/* Journalled writes on: the journal empty (replayed if it wasn't). */
static bool journal_open(struct evol *v, uint32_t inum)
{
    uint8_t raw[128];
    if (!inum || inode_read(v, inum, raw) < 0 || !(v->j_buf = kmalloc(v->bsize)) || !(v->j_copy = kmalloc(v->bsize)))
        return false;
    uint32_t goal = 0, pb;
    if (bmap(v, raw, 0, false, &goal, &pb) < 0 || !pb || block_io(v, pb, v->j_buf, false) < 0)
        return false;
    const uint8_t *sb = v->j_buf;
    uint32_t incompat = be32(sb + 40);
    if (be32(sb) != JBD_MAGIC || be32(sb + 4) != JBD_SB_V2 || be32(sb + 12) != v->bsize || be32(sb + 28) ||
        (incompat & ~(uint32_t)(JI_REVOKE | JI_64BIT))) {
        klog("EXT2: this journal (v%u, features %X) isn't written here: read-only\n",
             be32(sb + 4) == JBD_SB_V2 ? 2 : 1, incompat);
        return false;
    }
    v->j_maxlen = be32(sb + 16);
    v->j_first = be32(sb + 20);
    v->j_seq = be32(sb + 24);
    v->j_incompat = incompat;
    memcpy(v->j_uuid, sb + 48, 16);
    if (!v->j_first || v->j_first >= v->j_maxlen || !(v->j_map = kmalloc(v->j_maxlen * 4)))
        return false;
    for (uint32_t lb = 0; lb < v->j_maxlen; lb++) {     /* the journal's blocks, once */
        goal = 0;
        if (bmap(v, raw, lb, false, &goal, &v->j_map[lb]) < 0 || !v->j_map[lb]) {
            kfree(v->j_map);
            v->j_map = NULL;
            return false;
        }
    }
    v->j_head = v->j_first;
    v->j_used = 0;
    v->journal = true;
    return true;
}

/* Mounted (needs_recovery, not clean) or not. Written straight away. */
static void journal_mounted(struct evol *v, bool mounted)
{
    struct buf *b = bread(v->disk, v->start + 2);
    if (!b)
        return;
    uint32_t inc = le32(b->data + 96);
    put32(b->data + 96, mounted ? (inc | INCOMPAT_RECOVER) : (inc & ~(uint32_t)INCOMPAT_RECOVER));
    uint16_t st = le16(b->data + 58);
    put16(b->data + 58, mounted ? (st & ~1u) : (st | 1));
    bdirty(b);
    brelse(b);
    cache_flush_disk(v->disk);
    ata_flush(v->disk);
}

static void journal_close(struct evol *v)
{
    if (!v->journal)
        return;
    journal_commit(v);
    journal_checkpoint(v);
    journal_mounted(v, false);
}

/* ---- mounting --------------------------------------------------------------------------- */

struct vol *ext2_mount(struct partition *p)
{
    if (nvols == VOLS)
        return NULL;
    uint8_t sb[1024];
    for (int s = 0; s < 2; s++) {
        struct buf *b = bread(p->disk, p->start + 2 + (uint32_t)s);
        if (!b)
            return NULL;
        memcpy(sb + s * SECTOR, b->data, SECTOR);
        brelse(b);
    }
    if (le16(sb + 56) != 0xEF53)
        return NULL;
    uint32_t rev = le32(sb + 76);
    uint32_t incompat = rev ? le32(sb + 96) : 0, ro_compat = rev ? le32(sb + 100) : 0;
    uint32_t compat = rev ? le32(sb + 92) : 0;
    uint32_t known = INCOMPAT_KNOWN | ((compat & COMPAT_JOURNAL) ? INCOMPAT_RECOVER : 0);
    if (incompat & ~known) {
        klog("FS: ext2 at %u: incompatible features %X (ext4, or a journal device): not mounted\n",
             p->start, incompat);
        return NULL;
    }
    uint32_t log = le32(sb + 24);
    if (log > 6) {
        klog("FS: ext2 at %u: %u-byte blocks: not supported\n", p->start, 1024u << log);
        return NULL;
    }
    struct evol *v = &vols[nvols];
    *v = (struct evol){ .disk = p->disk, .start = p->start };
    v->bsize = 1024u << log;
    v->bshift = 10 + log;
    v->spb = v->bsize / SECTOR;
    v->ppb = v->bsize / 4;
    v->ninodes = le32(sb + 0);
    v->nblocks = le32(sb + 4);
    v->free_blocks = le32(sb + 12);
    v->free_inodes = le32(sb + 16);
    v->first_data = le32(sb + 20);
    v->bpg = le32(sb + 32);
    v->ipg = le32(sb + 40);
    v->inode_size = rev ? le16(sb + 88) : 128;
    v->first_ino = rev ? le32(sb + 84) : 11;
    v->filetype = incompat & INCOMPAT_FILETYPE;
    if (v->inode_size >= 160)
        v->extra_isize = le16(sb + 0x15C) ? le16(sb + 0x15C) : 32;
    if (!v->bpg || !v->ipg || v->inode_size < 128 || (v->inode_size & (v->inode_size - 1)) ||
        v->inode_size > v->bsize || (uint64_t)v->nblocks * v->spb > p->count)
        return NULL;
    v->ngroups = (v->nblocks - v->first_data + v->bpg - 1) / v->bpg;
    v->ro = (ro_compat & ~(uint32_t)RO_KNOWN) || (compat & COMPAT_JOURNAL);
    v->h.serial = le32(sb + 104) ^ le32(sb + 108) ^ le32(sb + 112) ^ le32(sb + 116);   /* from the UUID */
    int ln = 0;
    for (; ln < 11 && sb[120 + ln]; ln++)
        v->h.label[ln] = (char)sb[120 + ln];
    v->h.label[ln] = '\0';

    v->g = kmalloc(v->ngroups * sizeof *v->g);
    v->g_dirty = kzalloc(v->ngroups * sizeof *v->g_dirty);
    if (!v->g || !v->g_dirty)
        return NULL;
    uint32_t gdt = v->first_data + 1;
    for (uint32_t i = 0; i < v->ngroups; i++) {
        uint8_t *q;
        struct buf *b = bbyte(v, gdt + (i * 32) / v->bsize, (i * 32) % v->bsize, &q);
        if (!b)
            return NULL;
        v->g[i] = (struct group){ le32(q), le32(q + 4), le32(q + 8), le16(q + 12), le16(q + 14), le16(q + 16) };
        brelse(b);
    }
    if (incompat & INCOMPAT_RECOVER) {                  /* ext3, not unmounted: replay its journal */
        int rc = journal_replay(v, le32(sb + 0xE0));
        kfree(v->g);
        kfree(v->g_dirty);
        if (rc < 0) {
            klog("FS: ext3 at %u needs its journal replayed and it can't be: not mounted\n", p->start);
            return NULL;
        }
        return ext2_mount(p);                           /* again, from what is on the disk now */
    }
    uint8_t root[128];
    if (inode_read(v, ROOT_INO, root) < 0 || (i_mode(root) & S_IFMT) != S_IFDIR)
        return NULL;
    v->h.ops = &ext2_ops;
    v->h.disk = p->disk;
    v->h.start = p->start;
    v->h.type = (compat & COMPAT_JOURNAL) ? "ext3" : "ext2";
    if ((compat & COMPAT_JOURNAL) && !(ro_compat & ~(uint32_t)RO_KNOWN) && journal_open(v, le32(sb + 0xE0))) {
        v->ro = false;
        journal_mounted(v, true);                       /* needs_recovery until a clean shutdown */
    } else if (!v->ro) {
        set_state(v, false);                            /* "not clean" while it is mounted */
        cache_sync();
    }
    nvols++;
    klog("FS: %s on disk %d at %u: %u blocks of %u bytes, %u free, %u groups%s, label \"%s\"\n",
         v->h.type, (int)(p->disk - disks), p->start, v->nblocks, v->bsize, v->free_blocks, v->ngroups,
         v->ro ? ", read-only" : "", v->h.label);
    fs_add_volume(&v->h);
    return &v->h;
}

/* ---- files ------------------------------------------------------------------------------- */

static struct enode *node_get(struct evol *v, uint32_t ino, const char *name, const char *raw)
{
    struct enode *fr = NULL;
    for (int i = 0; i < NODE_MAX; i++) {
        struct enode *n = &nodes[i];
        if (n->refs && n->v == v && n->ino == ino) {
            n->refs++;
            return n;
        }
        if (!n->refs && !fr)
            fr = n;
    }
    if (!fr)
        return NULL;
    *fr = (struct enode){ .h.v = &v->h, .refs = 1, .v = v, .ino = ino };
    if (inode_read(v, ino, fr->raw) < 0) {
        fr->refs = 0;
        return NULL;
    }
    char alias[13];
    fs_make_alias(name, fs_hash16(raw), fr->fcb, alias);
    fr->goal = v->first_data + ((ino - 1) / v->ipg) * v->bpg;
    for (int i = 11; i >= 0; i--)
        if (le32(fr->raw + 40 + i * 4)) {
            fr->goal = le32(fr->raw + 40 + i * 4) + 1;
            break;
        }
    return fr;
}

static bool node_open(struct evol *v, uint32_t ino)
{
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v && nodes[i].ino == ino)
            return true;
    return false;
}

static int node_update(struct enode *n)
{
    if (!n->dirty)
        return 0;
    n->dirty = false;
    return inode_write(n->v, n->ino, n->raw);
}

static int set_size(struct enode *n, uint64_t size)
{
    struct evol *v = n->v;
    uint64_t cur = i_size(n->raw);
    if (size > cur) {                                   /* longer: a hole (the last block's tail is zero) */
        set_isize(v, n->raw, size);
        n->dirty = true;
        return 0;
    }
    if (size == cur)
        return 0;
    uint64_t keep = (size + v->bsize - 1) >> v->bshift;
    truncate_blocks(v, n->raw, keep > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)keep);
    uint32_t tail = (uint32_t)size & (v->bsize - 1);
    if (tail) {                                         /* zero the rest of the last block */
        uint32_t goal = 0, pb;
        if (bmap(v, n->raw, (uint32_t)(size >> v->bshift), false, &goal, &pb) == 0 && pb) {
            bool ch = false;
            data_io(v, n->raw, &goal, size, NULL, v->bsize - tail, true, &ch);
        }
    }
    set_isize(v, n->raw, size);
    n->dirty = true;
    return 0;
}

static void stamp(uint8_t *raw)
{
    uint32_t t = now_unix();
    put32(raw + 16, t);
    put32(raw + 12, t);
}

static int ex2_open(struct vol *vv, const char *path, uint32_t disp, bool write, uint8_t attr,
                    struct fnode **out, bool *created)
{
    struct evol *v = EV(vv);
    fs_lock();
    uint32_t dino;
    const char *name;
    static struct dent e;
    int rc = walk(v, path, &dino, &name);
    if (rc < 0)
        return done(v, rc);
    if (!*name)
        return done(v, -ERROR_ACCESS_DENIED);
    rc = dir_find(v, dino, name, &e);
    bool exists = rc == 0;
    if (rc < 0 && rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc);
    uint8_t raw[128];
    if (exists && inode_read(v, e.ino, raw) < 0)
        return done(v, -ERROR_GEN_FAILURE);
    if (exists && (i_mode(raw) & S_IFMT) == S_IFDIR)
        return done(v, -ERROR_ACCESS_DENIED);
    bool truncate = false, create = false;
    switch (disp) {
    case CREATE_NEW:        if (exists) return done(v, -ERROR_FILE_EXISTS); create = true; break;
    case CREATE_ALWAYS:     truncate = exists; create = !exists; break;
    case OPEN_EXISTING:     if (!exists) return done(v, -ERROR_FILE_NOT_FOUND); break;
    case OPEN_ALWAYS:       create = !exists; break;
    case TRUNCATE_EXISTING: if (!exists) return done(v, -ERROR_FILE_NOT_FOUND); truncate = true; break;
    default:                return done(v, -ERROR_INVALID_PARAMETER);
    }
    if ((write || truncate || create) && v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    if (exists && (write || truncate) && (attr_of(raw, e.name) & A_RO))
        return done(v, -ERROR_ACCESS_DENIED);
    if (create) {
        char clean[FS_NAME_MAX + 1];
        if ((rc = fs_clean_name(name, clean)) < 0)
            return done(v, rc);
        uint32_t ino;
        if ((rc = alloc_inode(v, (dino - 1) / v->ipg, false, &ino)) < 0)
            return done(v, rc);
        memset(raw, 0, sizeof raw);
        put16(raw, S_IFREG | ((attr & A_RO) ? 0444 : 0644));
        put16(raw + 26, 1);
        uint32_t t = now_unix();
        put32(raw + 8, t);
        put32(raw + 12, t);
        put32(raw + 16, t);
        if ((rc = inode_write(v, ino, raw)) < 0 || (rc = dir_add(v, dino, clean, ino, FT_REG)) < 0) {
            free_inode(v, ino, false);
            return done(v, rc);
        }
        e.ino = ino;
        to_utf8(clean, e.raw);
        memcpy(e.name, clean, strlen(clean) + 1);
    }
    struct enode *n = node_get(v, e.ino, e.name, e.raw);
    if (!n)
        return done(v, -ERROR_TOO_MANY_OPEN_FILES);
    if (truncate) {
        if ((rc = set_size(n, 0)) < 0) {
            n->refs--;
            return done(v, rc);
        }
        stamp(n->raw);
        node_update(n);
    }
    *out = &n->h;
    if (created)
        *created = create;
    return done(v, 0);
}

/* A symbolic link short enough to live in the block pointers. */
static bool symlink_fast(const struct evol *v, const uint8_t *raw)
{
    uint32_t blocks = le32(raw + 28);
    return (i_mode(raw) & S_IFMT) == S_IFLNK && i_size(raw) < 60 &&
           (blocks == 0 || (le32(raw + 104) && blocks == v->spb));
}

static int ex2_read(struct fnode *nn, uint64_t pos, void *buf, uint32_t len)
{
    struct enode *n = EN(nn);
    struct evol *v = n->v;
    fs_lock();
    uint64_t size = i_size(n->raw);
    uint16_t type = i_mode(n->raw) & S_IFMT;
    if (pos >= size || (type != S_IFREG && type != S_IFLNK))
        return done(v, 0);
    if (len > size - pos)
        len = (uint32_t)(size - pos);
    if (symlink_fast(v, n->raw)) {                         /* the target is in the block pointers */
        memcpy(buf, n->raw + 40 + pos, len);
        return done(v, (int)len);
    }
    bool ch = false;
    return done(v, data_io(v, n->raw, &n->goal, pos, buf, len, false, &ch));
}

static int ex2_write(struct fnode *nn, uint64_t pos, const void *buf, uint32_t len)
{
    struct enode *n = EN(nn);
    struct evol *v = n->v;
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    if ((i_mode(n->raw) & S_IFMT) != S_IFREG)
        return done(v, -ERROR_ACCESS_DENIED);
    if (len == 0)
        return done(v, 0);
    if (pos > i_size(n->raw)) {
        int rc = set_size(n, pos);
        if (rc < 0) {
            node_update(n);
            return done(v, rc);
        }
    }
    bool ch = false;
    int rc = data_io(v, n->raw, &n->goal, pos, (uint8_t *)buf, len, true, &ch);
    if (ch)
        n->dirty = true;
    if (rc > 0) {
        if (pos + (uint32_t)rc > i_size(n->raw))
            set_isize(v, n->raw, pos + (uint32_t)rc);
        stamp(n->raw);
        n->dirty = true;
    }
    if (rc == 0)
        rc = -ERROR_DISK_FULL;
    int urc = node_update(n);
    return done(v, rc > 0 && urc < 0 ? urc : rc);
}

static int ex2_set_size(struct fnode *nn, uint64_t size)
{
    struct enode *n = EN(nn);
    fs_lock();
    if (n->v->ro)
        return done(n->v, -ERROR_WRITE_PROTECT);
    int rc = set_size(n, size);
    if (rc == 0)
        stamp(n->raw);
    int urc = node_update(n);
    return done(n->v, rc < 0 ? rc : urc);
}

static uint64_t ex2_size(const struct fnode *n) { return i_size(CEN(n)->raw); }
static uint8_t ex2_attr(const struct fnode *n)  { return attr_of(CEN(n)->raw, NULL); }
static void ex2_name_of(const struct fnode *n, uint8_t fcb[11]) { memcpy(fcb, CEN(n)->fcb, 11); }

static void ex2_get_time(const struct fnode *n, uint16_t *date, uint16_t *time)
{
    unix_to_local(le32(CEN(n)->raw + 16), date, time);
}

static int ex2_set_time(struct fnode *nn, uint16_t date, uint16_t time)
{
    struct enode *n = EN(nn);
    fs_lock();
    if (n->v->ro)
        return done(n->v, -ERROR_WRITE_PROTECT);
    put32(n->raw + 16, local_to_unix(date, time));
    n->dirty = true;
    return done(n->v, node_update(n));
}

static int ex2_flush(struct fnode *nn)
{
    struct enode *n = EN(nn);
    fs_lock();
    fs_lock();                                          /* done() lets go of one */
    int rc = done(n->v, node_update(n));
    if (cache_sync() < 0 && rc >= 0)                    /* to the disk, now */
        rc = -ERROR_GEN_FAILURE;
    fs_unlock();
    return rc;
}

static void ex2_dup(struct fnode *n)
{
    fs_lock();
    EN(n)->refs++;
    fs_unlock();
}

static void ex2_close(struct fnode *nn)
{
    struct enode *n = EN(nn);
    fs_lock();
    node_update(n);
    if (n->refs > 0)
        n->refs--;
    done(n->v, 0);
}

/* ---- names ------------------------------------------------------------------------------- */

static int lookup(struct evol *v, const char *path, uint32_t *dino, struct dent *e)
{
    const char *name;
    int rc = walk(v, path, dino, &name);
    if (rc == 0 && (!*name || is_dots(name)))
        rc = -ERROR_ACCESS_DENIED;
    if (rc == 0)
        rc = dir_find(v, *dino, name, e);
    return rc;
}

static int ex2_stat(struct vol *vv, const char *path, struct fs_entry *fe)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (!*path) {
        memset(fe, 0, sizeof *fe);
        fe->attr = A_DIR;
        return done(v, 0);
    }
    uint32_t dino;
    static struct dent e;
    int rc = lookup(v, path, &dino, &e);
    uint8_t raw[128], draw[128];
    if (rc == 0 && (rc = inode_read(v, e.ino, raw)) == 0 && (rc = inode_read(v, dino, draw)) == 0)
        fill_entry(v, draw, &e, raw, fe);
    return done(v, rc);
}

static int ex2_set_attr(struct vol *vv, const char *path, uint8_t attr)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    uint32_t dino;
    static struct dent e;
    int rc = lookup(v, path, &dino, &e);
    if (rc < 0)
        return done(v, rc);
    struct enode *open_n = NULL;
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v && nodes[i].ino == e.ino)
            open_n = &nodes[i];
    uint8_t raw[128];
    uint8_t *r = open_n ? open_n->raw : raw;
    if (!open_n && inode_read(v, e.ino, raw) < 0)
        return done(v, -ERROR_GEN_FAILURE);
    uint16_t m = i_mode(r);
    m = (attr & A_RO) ? (uint16_t)(m & ~0222) : (uint16_t)(m | 0200);
    put16(r, m);
    put32(r + 12, now_unix());
    if (open_n) {
        open_n->dirty = true;
        rc = node_update(open_n);
    } else {
        rc = inode_write(v, e.ino, raw);
    }
    return done(v, rc);
}

/* One link fewer; the inode and its blocks go with the last. */
static int unlink_inode(struct evol *v, uint32_t ino, bool dir)
{
    uint8_t raw[128];
    if (inode_read(v, ino, raw) < 0)
        return -ERROR_GEN_FAILURE;
    uint16_t links = i_links(raw);
    links = dir ? 0 : (links ? links - 1 : 0);
    put16(raw + 26, links);
    put32(raw + 12, now_unix());
    if (links == 0) {
        if (!symlink_fast(v, raw))
            truncate_blocks(v, raw, 0);
        put32(raw + 4, 0);
        put32(raw + 20, now_unix());                    /* i_dtime */
        if (inode_write(v, ino, raw) < 0)
            return -ERROR_GEN_FAILURE;
        return free_inode(v, ino, dir);
    }
    return inode_write(v, ino, raw);
}

static int ex2_delete(struct vol *vv, const char *path)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    uint32_t dino;
    static struct dent e;
    int rc = lookup(v, path, &dino, &e);
    if (rc < 0)
        return done(v, rc);
    uint8_t raw[128];
    if (inode_read(v, e.ino, raw) < 0)
        return done(v, -ERROR_GEN_FAILURE);
    if ((i_mode(raw) & S_IFMT) == S_IFDIR || (attr_of(raw, e.name) & A_RO && (i_mode(raw) & S_IFMT) == S_IFREG))
        return done(v, -ERROR_ACCESS_DENIED);
    if (node_open(v, e.ino))
        return done(v, -ERROR_SHARING_VIOLATION);
    if ((rc = dir_remove(v, dino, &e)) == 0)
        rc = unlink_inode(v, e.ino, false);
    return done(v, rc);
}

static int link_count(struct evol *v, uint32_t ino, int delta)
{
    uint8_t raw[128];
    if (inode_read(v, ino, raw) < 0)
        return -ERROR_GEN_FAILURE;
    put16(raw + 26, (uint16_t)(i_links(raw) + delta));
    return inode_write(v, ino, raw);
}

static int ex2_mkdir(struct vol *vv, const char *path)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    uint32_t dino;
    const char *name;
    static struct dent e;
    int rc = walk(v, path, &dino, &name);
    if (rc == 0 && (!*name || is_dots(name)))
        rc = -ERROR_ACCESS_DENIED;
    if (rc < 0)
        return done(v, rc);
    rc = dir_find(v, dino, name, &e);
    if (rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc < 0 ? rc : -ERROR_ACCESS_DENIED);
    char clean[FS_NAME_MAX + 1];
    if ((rc = fs_clean_name(name, clean)) < 0)
        return done(v, rc);
    uint32_t ino, blk;
    if ((rc = alloc_inode(v, (dino - 1) / v->ipg, true, &ino)) < 0)
        return done(v, rc);
    uint32_t goal = v->first_data + ((ino - 1) / v->ipg) * v->bpg;
    if ((rc = alloc_block(v, goal, &blk)) < 0) {
        free_inode(v, ino, true);
        return done(v, rc);
    }
    zero_block(v, blk);
    uint8_t raw[128];
    memset(raw, 0, sizeof raw);
    put16(raw, S_IFDIR | 0755);
    put32(raw + 4, v->bsize);
    uint32_t t = now_unix();
    put32(raw + 8, t);
    put32(raw + 12, t);
    put32(raw + 16, t);
    put16(raw + 26, 2);
    put32(raw + 28, v->spb);
    put32(raw + 40, blk);
    if ((rc = inode_write(v, ino, raw)) < 0)
        return done(v, rc);
    put_record(v, raw, 0, ino, 12, ".", FT_DIR);
    put_record(v, raw, 12, dino, (uint16_t)(v->bsize - 12), "..", FT_DIR);
    if ((rc = dir_add(v, dino, clean, ino, FT_DIR)) < 0) {
        free_block(v, blk);
        free_inode(v, ino, true);
        return done(v, rc);
    }
    return done(v, link_count(v, dino, +1));
}

static bool dir_empty(struct evol *v, uint32_t ino)
{
    uint8_t raw[128];
    if (inode_read(v, ino, raw) < 0)
        return false;
    static struct dent e;
    uint32_t off = 0;
    while (dir_next(v, raw, &off, &e) > 0)
        if (!is_dots(e.name))
            return false;
    return true;
}

static int ex2_rmdir(struct vol *vv, const char *path)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    uint32_t dino;
    static struct dent e;
    int rc = lookup(v, path, &dino, &e);
    if (rc < 0)
        return done(v, rc == -ERROR_FILE_NOT_FOUND ? -ERROR_PATH_NOT_FOUND : rc);
    uint8_t raw[128];
    if (inode_read(v, e.ino, raw) < 0 || (i_mode(raw) & S_IFMT) != S_IFDIR)
        return done(v, -ERROR_PATH_NOT_FOUND);
    if (!dir_empty(v, e.ino))
        return done(v, -ERROR_DIR_NOT_EMPTY);
    if ((rc = dir_remove(v, dino, &e)) == 0 && (rc = unlink_inode(v, e.ino, true)) == 0)
        rc = link_count(v, dino, -1);
    return done(v, rc);
}

static int ex2_rename(struct vol *vv, const char *from, const char *to)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    uint32_t fdir, tdir;
    const char *tname;
    static struct dent fe, te;
    int rc = lookup(v, from, &fdir, &fe);
    if (rc < 0)
        return done(v, rc);
    if ((rc = walk(v, to, &tdir, &tname)) < 0)
        return done(v, rc);
    if (!*tname || is_dots(tname))
        return done(v, -ERROR_ACCESS_DENIED);
    rc = dir_find(v, tdir, tname, &te);
    bool self = rc == 0 && te.ino == fe.ino && tdir == fdir;
    if (rc == 0 && !self)
        return done(v, -ERROR_ACCESS_DENIED);
    if (rc < 0 && rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc);
    uint8_t raw[128];
    if (inode_read(v, fe.ino, raw) < 0)
        return done(v, -ERROR_GEN_FAILURE);
    bool is_dir = (i_mode(raw) & S_IFMT) == S_IFDIR;
    if (is_dir) {                                       /* not into itself: up through ".." */
        uint32_t d = tdir;
        for (int n = 0; d != ROOT_INO && n < 1024; n++) {
            if (d == fe.ino)
                return done(v, -ERROR_ACCESS_DENIED);
            static struct dent up;
            if (dir_find(v, d, "..", &up) < 0)
                break;
            d = up.ino;
        }
        if (tdir == fe.ino)
            return done(v, -ERROR_ACCESS_DENIED);
    }
    char clean[FS_NAME_MAX + 1];
    if ((rc = fs_clean_name(tname, clean)) < 0)
        return done(v, rc);
    if (self) {
        if ((rc = dir_remove(v, fdir, &fe)) < 0)
            return done(v, rc);
        rc = dir_add(v, tdir, clean, fe.ino, is_dir ? FT_DIR : FT_REG);
        return done(v, rc);
    }
    uint8_t type = (i_mode(raw) & S_IFMT) == S_IFDIR ? FT_DIR : (i_mode(raw) & S_IFMT) == S_IFREG ? FT_REG : fe.type;
    if ((rc = dir_add(v, tdir, clean, fe.ino, type)) < 0)
        return done(v, rc);
    if (fdir == tdir) {                                 /* the new record may have moved the old one's neighbour */
        static struct dent again;
        if ((rc = dir_find(v, fdir, fe.name, &again)) == 0 && again.ino == fe.ino)
            fe = again;
    }
    if ((rc = dir_remove(v, fdir, &fe)) < 0)
        return done(v, rc);
    if (is_dir && fdir != tdir) {                       /* its ".." and the parents' link counts */
        static struct dent dd;
        if (dir_find(v, fe.ino, "..", &dd) == 0) {
            uint32_t goal = 0, pb;
            if (bmap(v, raw, dd.off / v->bsize, false, &goal, &pb) == 0 && pb)
                wr32(v, pb, dd.off % v->bsize, tdir);
        }
        link_count(v, fdir, -1);
        link_count(v, tdir, +1);
    }
    return done(v, 0);
}

static int ex2_short_path(struct vol *vv, const char *path, char *out, uint32_t size)
{
    struct evol *v = EV(vv);
    fs_lock();
    uint32_t d = ROOT_INO, n = 0;
    out[0] = '\0';
    const char *s = path;
    int rc = 0;
    static struct dent e;
    while (*s && rc == 0) {
        const char *end = s;
        while (*end && *end != '\\')
            end++;
        char comp[FS_NAME_MAX + 1];
        uint32_t len = (uint32_t)(end - s);
        if (len > FS_NAME_MAX) {
            rc = -ERROR_INVALID_NAME;
            break;
        }
        memcpy(comp, s, len);
        comp[len] = '\0';
        uint8_t ddraw[128];
        if ((rc = dir_find(v, d, comp, &e)) < 0 || (rc = inode_read(v, d, ddraw)) < 0)
            break;
        uint8_t fcb[11];
        char alias[13];
        alias_of(v, ddraw, &e, fcb, alias);
        uint32_t al = (uint32_t)strlen(alias);
        if (n + al + 2 > size) {
            rc = -ERROR_FILENAME_EXCED_RANGE;
            break;
        }
        if (n)
            out[n++] = '\\';
        memcpy(out + n, alias, al + 1);
        n += al;
        d = e.ino;
        s = *end ? end + 1 : end;
    }
    return done(v, rc);
}

/* ---- searches -------------------------------------------------------------------------------- */

/* f->dir: the directory's inode; the offset: f->next | f->dirlen << 16. */
static int find_more(struct fs_find *f, struct fs_entry *out)
{
    struct evol *v = EV(f->v);
    uint8_t draw[128];
    if (inode_read(v, f->dir, draw) < 0)
        return -ERROR_GEN_FAILURE;
    uint32_t off = (f->next & 0xFFFF) | f->dirlen << 16;
    static struct dent e;
    for (;;) {
        int rc = dir_next(v, draw, &off, &e);
        f->next = off & 0xFFFF;
        f->dirlen = off >> 16;
        if (rc <= 0)
            return rc < 0 ? rc : -ERROR_NO_MORE_FILES;
        if (f->dir == ROOT_INO && is_dots(e.name))
            continue;                                   /* the root has no . and .. on DOS's drives */
        uint8_t raw[128];
        if (inode_read(v, e.ino, raw) < 0)
            continue;
        uint8_t fcb[11];
        char alias[13];
        bool again = false;                             /* an earlier record may share its alias */
        if (fs_make_alias(e.name, fs_hash16(e.raw), fcb, alias)) {
            uint16_t h = fs_hash16(e.raw) & 1023;
            again = f->seen[h / 32] & (1u << (h % 32));
            f->seen[h / 32] |= 1u << (h % 32);
        }
        struct fs_entry fe;
        uint32_t keep = off;                            /* alias_of walks the directory too */
        fill_entry(v, again ? draw : NULL, &e, raw, &fe);
        off = keep;
        if (f->dos) {
            if (f->attr == 0x08 || (fe.attr & (A_HID | A_SYS | A_DIR) & ~f->attr) || !fs_fcb_match(f->fcb, fe.fcb_name))
                continue;
        } else {
            const char *p = f->pattern;
            bool all = (p[0] == '*' && !p[1]) || (p[0] == '*' && p[1] == '.' && p[2] == '*' && !p[3]);
            if (!all && !fs_wild(p, fe.name) && !fs_wild(p, fe.short_name))
                continue;
        }
        *out = fe;
        return 0;
    }
}

static int find_dir(struct evol *v, const char *dir, struct fs_find *f)
{
    f->next = 0;
    f->dirlen = 0;
    if (!*dir) {
        f->dir = ROOT_INO;
        return 0;
    }
    uint32_t dino;
    static struct dent e;
    if (lookup(v, dir, &dino, &e) < 0)
        return -ERROR_PATH_NOT_FOUND;
    uint8_t raw[128];
    if (inode_read(v, e.ino, raw) < 0 || (i_mode(raw) & S_IFMT) != S_IFDIR)
        return -ERROR_PATH_NOT_FOUND;
    f->dir = e.ino;
    return 0;
}

static int ex2_find_first(struct vol *vv, const char *dir, const char *pattern, struct fs_find *f,
                          struct fs_entry *e)
{
    struct evol *v = EV(vv);
    fs_lock();
    memset(f, 0, sizeof *f);
    f->v = vv;
    size_t n = strlen(pattern);
    if (n > FS_NAME_MAX)
        return done(v, -ERROR_INVALID_NAME);
    memcpy(f->pattern, pattern, n + 1);
    int rc = find_dir(v, dir, f);
    if (rc == 0)
        rc = find_more(f, e);
    return done(v, rc == -ERROR_NO_MORE_FILES ? -ERROR_FILE_NOT_FOUND : rc);
}

static int ex2_find_dos(struct vol *vv, const char *dir, const uint8_t fcb[11], uint8_t attr,
                        struct fs_find *f, struct fs_entry *e)
{
    struct evol *v = EV(vv);
    fs_lock();
    memset(f, 0, sizeof *f);
    f->v = vv;
    f->dos = true;
    f->attr = attr;
    memcpy(f->fcb, fcb, 11);
    int rc = find_dir(v, dir, f);
    if (rc == 0 && attr == 0x08) {                      /* the label */
        if (*dir || !v->h.label[0])
            return done(v, -ERROR_FILE_NOT_FOUND);
        memset(e, 0, sizeof *e);
        memset(e->fcb_name, ' ', 11);
        for (int i = 0; i < 11 && v->h.label[i]; i++)
            e->fcb_name[i] = (uint8_t)fs_up(v->h.label[i]);
        memcpy(e->name, v->h.label, strlen(v->h.label) + 1);
        e->attr = 0x08;
        return done(v, 0);
    }
    if (rc == 0)
        rc = find_more(f, e);
    return done(v, rc == -ERROR_NO_MORE_FILES ? -ERROR_FILE_NOT_FOUND : rc);
}

static int ex2_find_next(struct fs_find *f, struct fs_entry *e)
{
    struct evol *v = EV(f->v);
    fs_lock();
    if (f->dos && f->attr == 0x08)
        return done(v, -ERROR_NO_MORE_FILES);
    return done(v, find_more(f, e));
}

static int ex2_space(struct vol *vv, uint32_t *spc, uint32_t *bps, uint32_t *freec, uint32_t *total)
{
    struct evol *v = EV(vv);
    fs_lock();
    *spc = v->spb;
    *bps = SECTOR;
    *freec = v->free_blocks;
    *total = v->nblocks - v->first_data;
    return done(v, 0);
}

static void ex2_sync(struct vol *vv)
{
    struct evol *v = EV(vv);
    fs_lock();
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v)
            node_update(&nodes[i]);
    v->sb_dirty = true;
    if (v->journal) {
        done(v, 0);                                     /* the last transaction */
        fs_lock();
        journal_close(v);                               /* home, the journal empty, clean */
        fs_unlock();
        return;
    }
    if (!v->ro)
        set_state(v, true);                             /* clean: everything is on the disk */
    done(v, 0);
}

static int ex2_link(struct vol *vv, const char *existing, const char *path)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    uint32_t sdir, tdir;
    const char *tname;
    static struct dent se, te;
    int rc = lookup(v, existing, &sdir, &se);
    if (rc < 0)
        return done(v, rc);
    uint8_t raw[128];
    if (inode_read(v, se.ino, raw) < 0)
        return done(v, -ERROR_GEN_FAILURE);
    if ((i_mode(raw) & S_IFMT) == S_IFDIR)
        return done(v, -ERROR_ACCESS_DENIED);           /* directories have one name */
    if (i_links(raw) >= 65000)
        return done(v, -ERROR_TOO_MANY_LINKS);
    if ((rc = walk(v, path, &tdir, &tname)) < 0)
        return done(v, rc);
    if (!*tname || is_dots(tname))
        return done(v, -ERROR_ACCESS_DENIED);
    if (dir_find(v, tdir, tname, &te) == 0)
        return done(v, -ERROR_ALREADY_EXISTS);
    char clean[FS_NAME_MAX + 1];
    if ((rc = fs_clean_name(tname, clean)) < 0)
        return done(v, rc);
    uint8_t type = (i_mode(raw) & S_IFMT) == S_IFLNK ? FT_SYMLINK : FT_REG;
    if ((rc = dir_add(v, tdir, clean, se.ino, type)) < 0)
        return done(v, rc);
    for (int i = 0; i < NODE_MAX; i++)                  /* an open file's copy of the inode too */
        if (nodes[i].refs && nodes[i].v == v && nodes[i].ino == se.ino) {
            put16(nodes[i].raw + 26, i_links(nodes[i].raw) + 1);
            nodes[i].dirty = true;
            node_update(&nodes[i]);
            return done(v, 0);
        }
    put16(raw + 26, i_links(raw) + 1);
    put32(raw + 12, now_unix());
    return done(v, inode_write(v, se.ino, raw));
}

static int ex2_symlink(struct vol *vv, const char *path, const char *target)
{
    struct evol *v = EV(vv);
    fs_lock();
    if (v->ro)
        return done(v, -ERROR_WRITE_PROTECT);
    uint32_t dino;
    const char *name;
    static struct dent e;
    int rc = walk(v, path, &dino, &name);
    if (rc == 0 && (!*name || is_dots(name)))
        rc = -ERROR_ACCESS_DENIED;
    if (rc < 0)
        return done(v, rc);
    if (dir_find(v, dino, name, &e) == 0)
        return done(v, -ERROR_ALREADY_EXISTS);
    char clean[FS_NAME_MAX + 1], utarget[256];
    int tl;
    if ((rc = fs_clean_name(name, clean)) < 0)
        return done(v, rc);
    char unix_target[FS_NAME_MAX + 1];                  /* backslashes are Linux's slashes */
    size_t k;
    for (k = 0; target[k] && k < FS_NAME_MAX; k++)
        unix_target[k] = target[k] == '\\' ? '/' : target[k];
    unix_target[k] = '\0';
    if (!k || (tl = to_utf8(unix_target, utarget)) < 0 || (uint32_t)tl >= v->bsize)
        return done(v, -ERROR_INVALID_PARAMETER);
    uint32_t ino;
    if ((rc = alloc_inode(v, (dino - 1) / v->ipg, false, &ino)) < 0)
        return done(v, rc);
    uint8_t raw[128];
    memset(raw, 0, sizeof raw);
    put16(raw, S_IFLNK | 0777);
    put16(raw + 26, 1);
    uint32_t now = now_unix();
    put32(raw + 8, now);
    put32(raw + 12, now);
    put32(raw + 16, now);
    put32(raw + 4, (uint32_t)tl);
    if (tl < 60) {                                      /* fast: in the block pointers */
        memcpy(raw + 40, utarget, (size_t)tl);
    } else {                                            /* slow: a block of its own */
        uint32_t goal = v->first_data + ((ino - 1) / v->ipg) * v->bpg, pb;
        if ((rc = bmap(v, raw, 0, true, &goal, &pb)) < 0) {
            free_inode(v, ino, false);
            return done(v, rc);
        }
        zero_block(v, pb);
        bool ch = false;
        data_io(v, raw, &goal, 0, (uint8_t *)utarget, (uint32_t)tl, true, &ch);
    }
    if ((rc = inode_write(v, ino, raw)) < 0 || (rc = dir_add(v, dino, clean, ino, FT_SYMLINK)) < 0) {
        if (tl >= 60)
            truncate_blocks(v, raw, 0);
        free_inode(v, ino, false);
        return done(v, rc);
    }
    return done(v, 0);
}

static const struct fs_ops ext2_ops = {
    .open = ex2_open, .read = ex2_read, .write = ex2_write, .set_size = ex2_set_size,
    .size = ex2_size, .get_time = ex2_get_time, .set_time = ex2_set_time, .attr = ex2_attr,
    .name_of = ex2_name_of, .flush = ex2_flush, .close = ex2_close, .dup = ex2_dup,
    .stat = ex2_stat, .set_attr = ex2_set_attr, .del = ex2_delete, .mkdir = ex2_mkdir,
    .rmdir = ex2_rmdir, .rename = ex2_rename, .short_path = ex2_short_path,
    .find_first = ex2_find_first, .find_dos = ex2_find_dos, .find_next = ex2_find_next,
    .space = ex2_space, .sync = ex2_sync, .link = ex2_link, .symlink = ex2_symlink,
};
