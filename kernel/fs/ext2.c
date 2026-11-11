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
 * or an ext3 journal, mounts read-only.
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
    uint32_t bsize, spb, ppb;           /* block bytes, sectors per block, pointers per block */
    uint32_t nblocks, ninodes, first_data, bpg, ipg, ngroups;
    uint32_t inode_size, first_ino;
    uint16_t extra_isize;
    bool     filetype, ro, changed, sb_dirty;
    uint32_t free_blocks, free_inodes;
    struct group *g;
    bool    *g_dirty;
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

static uint32_t now_unix(void)
{
    uint16_t d, t;
    rtc_dos_time(&d, &t);
    return fs_dos_to_unix(d, t);
}

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
    bdirty(bf);
    brelse(bf);
    v->changed = true;
    return 0;
}

static void zero_block(struct evol *v, uint32_t b)
{
    for (uint32_t s = 0; s < v->spb; s++) {
        struct buf *bf = bzero_get(v->disk, blk_lba(v, b) + s);
        bdirty(bf);
        brelse(bf);
    }
    v->changed = true;
}

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
        bdirty(b);
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
        bdirty(b);
        brelse(b);
        v->g_dirty[i] = false;
        v->changed = true;
    }
    return 0;
}

static int done(struct evol *v, int rc)
{
    int mrc = write_meta(v);
    if (mrc < 0 && rc >= 0)
        rc = mrc;
    if (v->changed) {
        v->changed = false;
        if (cache_sync() < 0 && rc >= 0)
            rc = -ERROR_GEN_FAILURE;
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
            bdirty(b);
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
    bdirty(b);
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
    bdirty(b);
    brelse(b);
    v->changed = true;
    return 0;
}

static uint16_t i_mode(const uint8_t *r)   { return le16(r); }
static uint32_t i_size(const uint8_t *r)   { return le32(r + 108) && (i_mode(r) & S_IFMT) == S_IFREG ? 0xFFFFFFFFu : le32(r + 4); }
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
            bdirty(b);
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
static int bmap(struct evol *v, uint8_t *raw, uint32_t lb, bool alloc, uint32_t *goal, uint32_t *out)
{
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

static int data_io(struct evol *v, uint8_t *raw, uint32_t *goal, uint32_t pos, uint8_t *buf, uint32_t len,
                   bool write, bool *raw_changed)
{
    uint32_t done_n = 0;
    while (done_n < len) {
        uint32_t lb = pos / v->bsize, in = pos % v->bsize, pb;
        uint32_t blocks_before = le32(raw + 28);
        int rc = bmap(v, raw, lb, write, goal, &pb);
        if (le32(raw + 28) != blocks_before)
            *raw_changed = true;
        if (rc < 0)
            return done_n ? (int)done_n : rc;
        uint32_t chunk = v->bsize - in < len - done_n ? v->bsize - in : len - done_n;
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
                cache_sync();
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
    /* the exact bytes first, then any case (in Latin-1), then an alias */
    for (int pass = 0; pass < 3; pass++) {
        uint32_t off = 0;
        int rc;
        while ((rc = dir_next(v, draw, &off, e)) > 0) {
            if (pass == 0 && !strcmp(e->raw, want))
                return 0;
            if (pass == 1 && fs_name_eq(e->name, name))
                return 0;
            if (pass == 2) {
                uint8_t fcb[11];
                char alias[13];
                fs_make_alias(e->name, fs_hash16(e->raw), fcb, alias);
                if (fs_name_eq(alias, name))
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
        bdirty(b);
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
    bdirty(b);
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
        bdirty(b);
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

static void fill_entry(const struct dent *d, const uint8_t *raw, struct fs_entry *e)
{
    memcpy(e->name, d->name, strlen(d->name) + 1);
    fs_make_alias(d->name, fs_hash16(d->raw), e->fcb_name, e->short_name);
    if (is_dots(d->name)) {
        memset(e->fcb_name, ' ', 11);
        memcpy(e->fcb_name, d->name, strlen(d->name));
        memcpy(e->short_name, d->name, strlen(d->name) + 1);
    }
    e->attr = attr_of(raw, d->name);
    e->size = (e->attr & A_DIR) ? 0 : i_size(raw);
    fs_unix_to_dos(le32(raw + 16), &e->date, &e->time);
    fs_unix_to_dos(le32(raw + 12), &e->cdate, &e->ctime);
    uint16_t t;
    fs_unix_to_dos(le32(raw + 8), &e->adate, &t);
    e->cluster = d->ino;
    e->lfn = strcmp(e->name, e->short_name) != 0;
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
    if (incompat & ~(uint32_t)INCOMPAT_KNOWN) {
        klog("FS: ext2 at %u: incompatible features %X (ext4?): not mounted\n", p->start, incompat);
        return NULL;
    }
    uint32_t log = le32(sb + 24);
    if (log > 2) {
        klog("FS: ext2 at %u: %u-byte blocks: not supported\n", p->start, 1024u << log);
        return NULL;
    }
    struct evol *v = &vols[nvols];
    *v = (struct evol){ .disk = p->disk, .start = p->start };
    v->bsize = 1024u << log;
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
    uint8_t root[128];
    if (inode_read(v, ROOT_INO, root) < 0 || (i_mode(root) & S_IFMT) != S_IFDIR)
        return NULL;
    v->h.ops = &ext2_ops;
    v->h.type = (compat & COMPAT_JOURNAL) ? "ext3" : "ext2";
    if (!v->ro) {
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

static int set_size(struct enode *n, uint32_t size)
{
    struct evol *v = n->v;
    uint32_t cur = le32(n->raw + 4);
    if (size > cur) {                                   /* zeros to the new end */
        bool ch = false;
        int rc = data_io(v, n->raw, &n->goal, cur, NULL, size - cur, true, &ch);
        if (rc < 0)
            return rc;
        put32(n->raw + 4, cur + (uint32_t)rc);
        n->dirty = true;
        return cur + (uint32_t)rc == size ? 0 : -ERROR_DISK_FULL;
    }
    if (size == cur)
        return 0;
    truncate_blocks(v, n->raw, (size + v->bsize - 1) / v->bsize);
    if (size % v->bsize) {                              /* zero the rest of the last block */
        uint32_t goal = 0, pb;
        if (bmap(v, n->raw, size / v->bsize, false, &goal, &pb) == 0 && pb) {
            bool ch = false;
            data_io(v, n->raw, &goal, size, NULL, v->bsize - size % v->bsize, true, &ch);
        }
    }
    put32(n->raw + 4, size);
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
    return (i_mode(raw) & S_IFMT) == S_IFLNK && le32(raw + 4) < 60 &&
           (blocks == 0 || (le32(raw + 104) && blocks == v->spb));
}

static int ex2_read(struct fnode *nn, uint32_t pos, void *buf, uint32_t len)
{
    struct enode *n = EN(nn);
    struct evol *v = n->v;
    fs_lock();
    uint32_t size = i_size(n->raw);
    uint16_t type = i_mode(n->raw) & S_IFMT;
    if (pos >= size || (type != S_IFREG && type != S_IFLNK))
        return done(v, 0);
    if (len > size - pos)
        len = size - pos;
    if (symlink_fast(v, n->raw)) {                         /* the target is in the block pointers */
        memcpy(buf, n->raw + 40 + pos, len);
        return done(v, (int)len);
    }
    bool ch = false;
    return done(v, data_io(v, n->raw, &n->goal, pos, buf, len, false, &ch));
}

static int ex2_write(struct fnode *nn, uint32_t pos, const void *buf, uint32_t len)
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
    if (pos > le32(n->raw + 4)) {
        int rc = set_size(n, pos);
        if (rc < 0) {
            node_update(n);
            return done(v, rc);
        }
    }
    if ((uint64_t)pos + len > 0xFFFFFFFFu)
        len = 0xFFFFFFFFu - pos;
    bool ch = false;
    int rc = data_io(v, n->raw, &n->goal, pos, (uint8_t *)buf, len, true, &ch);
    if (ch)
        n->dirty = true;
    if (rc > 0) {
        if (pos + (uint32_t)rc > le32(n->raw + 4))
            put32(n->raw + 4, pos + (uint32_t)rc);
        stamp(n->raw);
        n->dirty = true;
    }
    if (rc == 0)
        rc = -ERROR_DISK_FULL;
    int urc = node_update(n);
    return done(v, rc > 0 && urc < 0 ? urc : rc);
}

static int ex2_set_size(struct fnode *nn, uint32_t size)
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

static uint32_t ex2_size(const struct fnode *n) { return i_size(CEN(n)->raw); }
static uint8_t ex2_attr(const struct fnode *n)  { return attr_of(CEN(n)->raw, NULL); }
static void ex2_name_of(const struct fnode *n, uint8_t fcb[11]) { memcpy(fcb, CEN(n)->fcb, 11); }

static void ex2_get_time(const struct fnode *n, uint16_t *date, uint16_t *time)
{
    fs_unix_to_dos(le32(CEN(n)->raw + 16), date, time);
}

static int ex2_set_time(struct fnode *nn, uint16_t date, uint16_t time)
{
    struct enode *n = EN(nn);
    fs_lock();
    if (n->v->ro)
        return done(n->v, -ERROR_WRITE_PROTECT);
    put32(n->raw + 16, fs_dos_to_unix(date, time));
    n->dirty = true;
    return done(n->v, node_update(n));
}

static int ex2_flush(struct fnode *nn)
{
    struct enode *n = EN(nn);
    fs_lock();
    n->v->changed = true;
    return done(n->v, node_update(n));
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
    uint8_t raw[128];
    if (rc == 0 && (rc = inode_read(v, e.ino, raw)) == 0)
        fill_entry(&e, raw, fe);
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
        if ((rc = dir_find(v, d, comp, &e)) < 0)
            break;
        uint8_t fcb[11];
        char alias[13];
        fs_make_alias(e.name, fs_hash16(e.raw), fcb, alias);
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
        struct fs_entry fe;
        fill_entry(&e, raw, &fe);
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
    if (!v->ro)
        set_state(v, true);                             /* clean: everything is on the disk */
    v->sb_dirty = true;
    done(v, 0);
}

static const struct fs_ops ext2_ops = {
    .open = ex2_open, .read = ex2_read, .write = ex2_write, .set_size = ex2_set_size,
    .size = ex2_size, .get_time = ex2_get_time, .set_time = ex2_set_time, .attr = ex2_attr,
    .name_of = ex2_name_of, .flush = ex2_flush, .close = ex2_close, .dup = ex2_dup,
    .stat = ex2_stat, .set_attr = ex2_set_attr, .del = ex2_delete, .mkdir = ex2_mkdir,
    .rmdir = ex2_rmdir, .rename = ex2_rename, .short_path = ex2_short_path,
    .find_first = ex2_find_first, .find_dos = ex2_find_dos, .find_next = ex2_find_next,
    .space = ex2_space, .sync = ex2_sync,
};
