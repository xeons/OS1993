/*
 * btrfs.c -- btrfs, read-only, one device.
 *
 * Everything is in copy-on-write B-trees of nodes (a header, then keys
 * and child pointers, or keys and items in a leaf), addressed by logical
 * bytes. The chunk tree maps logical ranges to the device (bootstrapped
 * from the superblock's system chunk array; single and DUP profiles, so
 * the first stripe is the data). The root tree holds the other trees'
 * roots; a file system tree (5, or a subvolume's) holds, keyed by inode
 * number, INODE_ITEMs, DIR_ITEMs (by name hash) and DIR_INDEXes (in order),
 * and EXTENT_DATA items: inline data, or a disk extent (with an offset
 * into it), possibly compressed with zlib, LZO or zstd (decomp.c), or
 * preallocated or a hole (no item at all, with no_holes) -- zeros. The
 * checksum tree's CRC32C per sector is checked as data is read, and every
 * node's own checksum as it is read; a mismatch fails the read.
 *
 * Names are UTF-8 (Latin-1 here, as ext2); a lookup tries the exact name
 * through its hash, then any case and DOS's aliases (fs_make_alias, from
 * the name hash) by walking the directory's index. A subvolume shows as a
 * folder. Symbolic links read as their target.
 *
 * Read-only: btrfs's writing (copy-on-write trees, extent and free space
 * accounting, checksums of all of it) isn't done here.
 */
#include "kernel.h"
#include "disk.h"
#include "fsimpl.h"
#include "mm.h"
#include "user.h"
#include "decomp.h"

#define SB_OFFSET    0x10000
#define MAGIC        "_BHRfS_M"
#define T_INODE      1
#define T_INODE_REF  12
#define T_DIR_ITEM   84
#define T_DIR_INDEX  96
#define T_EXTENT     108
#define T_CSUM       128
#define T_ROOT       132
#define T_CHUNK      228
#define ROOT_TREE    1
#define CHUNK_TREE   3
#define FS_TREE      5
#define CSUM_TREE    7
#define CSUM_OBJ     0xFFFFFFFFFFFFFFF6ull
#define INCOMPAT_KNOWN (0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x100 | 0x200 | 0x400)
#define HDR          0x65               /* a node's header */
#define CHUNKS       64
#define NODE_MAX     16
#define BV(x)  ((struct bvol *)(x))
#define BN(x)  ((struct bnode *)(x))
#define CBN(x) ((const struct bnode *)(x))

struct key { uint64_t obj; uint8_t type; uint64_t off; };

struct chunk { uint64_t logical, length, physical; };

struct bvol {
    struct vol h;
    struct disk *disk;
    uint32_t start;
    uint32_t sector, nodesize, sshift;  /* sector = 1 << sshift */
    uint64_t total, used;
    struct chunk chunks[CHUNKS];
    int      nchunks;
    uint64_t root_tree, csum_tree, fs_tree;     /* their roots' logical addresses */
    uint8_t  root_level, csum_level, fs_level;
    uint64_t root_dir;                  /* the file system tree's top directory */
    bool     csum_ok;                   /* CRC32C, so checksums are checked */
};

struct bnode {
    struct fnode h;
    int      refs;
    struct bvol *v;
    uint64_t tree;                      /* its tree's root ... */
    uint8_t  level;
    uint64_t ino;
    uint64_t size;
    uint32_t mode;
    uint16_t date, time;
    uint8_t  fcb[11];
};

struct dirref {                         /* a directory: which tree, which inode */
    uint64_t tree;
    uint8_t  level;
    uint64_t ino;
};

static struct bvol vols[4];
static int nvols;
static struct bnode nodes[NODE_MAX];
static const struct fs_ops btrfs_ops;
static uint8_t *nbuf[8];                /* one node per tree level being walked */
static uint8_t *cbuf, *dbuf;            /* compressed, decompressed extents */
static uint64_t d_extent;               /* the extent in dbuf (disk byte number, 0: none) */
static struct bvol *d_vol;
static int32_t d_len;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static int kcmp(const struct key *a, const struct key *b)
{
    if (a->obj != b->obj) return a->obj < b->obj ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return 0;
}

static void kread(struct key *k, const uint8_t *p) { k->obj = le64(p); k->type = p[8]; k->off = le64(p + 9); }

static int done(struct bvol *v, int rc)
{
    (void)v;
    fs_unlock();
    return rc;
}

/* ---- the device ----------------------------------------------------------------------- */

static int phys_read(struct bvol *v, uint64_t off, uint8_t *buf, uint32_t len)
{
    while (len) {
        uint64_t sec = off / SECTOR;
        if (sec > 0xFFFFFFFFu - v->start)
            return -1;
        uint32_t in = (uint32_t)(off % SECTOR), k = SECTOR - in < len ? SECTOR - in : len;
        struct buf *b = bread(v->disk, v->start + (uint32_t)sec);
        if (!b)
            return -1;
        memcpy(buf, b->data + in, k);
        brelse(b);
        buf += k;
        off += k;
        len -= k;
    }
    return 0;
}

/* logical -> on the device (single or DUP: the first stripe) */
static int map(struct bvol *v, uint64_t logical, uint64_t *phys, uint64_t *left)
{
    for (int i = 0; i < v->nchunks; i++) {
        struct chunk *c = &v->chunks[i];
        if (logical >= c->logical && logical - c->logical < c->length) {
            *phys = c->physical + (logical - c->logical);
            *left = c->length - (logical - c->logical);
            return 0;
        }
    }
    return -1;
}

static int log_read(struct bvol *v, uint64_t logical, uint8_t *buf, uint32_t len)
{
    while (len) {
        uint64_t phys, left;
        if (map(v, logical, &phys, &left) < 0)
            return -1;
        uint32_t k = left < len ? (uint32_t)left : len;
        if (phys_read(v, phys, buf, k) < 0)
            return -1;
        buf += k;
        logical += k;
        len -= k;
    }
    return 0;
}

static void add_chunk(struct bvol *v, uint64_t logical, const uint8_t *item)
{
    uint16_t nstripes = le16(item + 44);
    if (!nstripes || v->nchunks == CHUNKS)
        return;
    for (int i = 0; i < v->nchunks; i++)
        if (v->chunks[i].logical == logical)
            return;
    v->chunks[v->nchunks++] = (struct chunk){ logical, le64(item), le64(item + 48 + 8) };
}

/* ---- trees ------------------------------------------------------------------------------ */

static int node_read(struct bvol *v, uint64_t logical, uint8_t *buf)
{
    if (log_read(v, logical, buf, v->nodesize) < 0)
        return -ERROR_GEN_FAILURE;
    if (le64(buf + 0x30) != logical)
        return -ERROR_FILE_CORRUPT;
    if (v->csum_ok && ~fs_crc32c(0xFFFFFFFFu, buf + 0x20, v->nodesize - 0x20) != le32(buf)) {
        static int shown;
        if (shown++ < 4)
            klog("BTRFS: node at %u:%u: its checksum is wrong\n", (uint32_t)(logical >> 32), (uint32_t)logical);
        return -ERROR_FILE_CORRUPT;
    }
    return 0;
}

/* The item with the smallest key >= k (le: the largest <= k) in a tree.
 * -> 1 found (its key, a pointer to its data in a node buffer, its size), 0 none. */
static int tree_find(struct bvol *v, uint64_t root, uint8_t level, const struct key *k, bool le,
                     struct key *found, const uint8_t **data, uint32_t *size)
{
    uint64_t at = root;
    for (int depth = 0; depth < 8; depth++) {
        uint8_t *n = nbuf[depth];
        int rc = node_read(v, at, n);
        if (rc < 0)
            return rc;
        uint32_t items = le32(n + 0x60);
        uint8_t lvl = n[0x64];
        if (depth == 0 && lvl != level)
            return -ERROR_FILE_CORRUPT;
        if (lvl) {                                      /* the last child whose key <= k */
            int pick = 0;
            for (uint32_t i = 0; i < items; i++) {
                struct key ik;
                kread(&ik, n + HDR + i * 33);
                if (kcmp(&ik, k) <= 0)
                    pick = (int)i;
                else
                    break;
            }
            if (!items)
                return 0;
            at = le64(n + HDR + (uint32_t)pick * 33 + 17);
            continue;                                   /* (>=: past this child's end, the next leaf is asked) */
        }
        int pick = -1;
        for (uint32_t i = 0; i < items; i++) {
            struct key ik;
            kread(&ik, n + HDR + i * 25);
            int c = kcmp(&ik, k);
            if (le ? c <= 0 : c >= 0) {
                pick = (int)i;
                if (!le)
                    break;
            } else if (le) {
                break;
            }
        }
        if (pick < 0) {
            if (le || !items)
                return 0;
            /* nothing >= k in this leaf: the next leaf's first item, via the key just past it */
            struct key last;
            kread(&last, n + HDR + (items - 1) * 25);
            struct key next = last;
            if (++next.off == 0 && ++next.type == 0)
                next.obj++;
            if (kcmp(&next, k) <= 0)
                return 0;
            return tree_find(v, root, level, &next, false, found, data, size) ;
        }
        const uint8_t *it = n + HDR + (uint32_t)pick * 25;
        kread(found, it);
        *data = n + HDR + le32(it + 17);
        *size = le32(it + 21);
        if (le32(it + 17) + *size > v->nodesize - HDR)
            return -ERROR_FILE_CORRUPT;
        return 1;
    }
    return -ERROR_FILE_CORRUPT;
}

/* Exactly this key's item. */
static int tree_get(struct bvol *v, uint64_t root, uint8_t level, const struct key *k, const uint8_t **data,
                    uint32_t *size)
{
    struct key f;
    int rc = tree_find(v, root, level, k, false, &f, data, size);
    if (rc <= 0)
        return rc < 0 ? rc : -ERROR_FILE_NOT_FOUND;
    return kcmp(&f, k) ? -ERROR_FILE_NOT_FOUND : 0;
}

/* A tree's root, from the root tree. */
static int tree_root(struct bvol *v, uint64_t id, uint64_t *bytenr, uint8_t *level, uint64_t *root_dir)
{
    struct key k = { id, T_ROOT, 0 }, f;
    const uint8_t *d;
    uint32_t sz;
    int rc = tree_find(v, v->root_tree, v->root_level, &k, false, &f, &d, &sz);
    if (rc <= 0 || f.obj != id || f.type != T_ROOT || sz < 239)
        return rc < 0 ? rc : -ERROR_FILE_NOT_FOUND;
    *bytenr = le64(d + 176);
    *level = d[238];
    if (root_dir)
        *root_dir = le64(d + 168);
    return 0;
}

/* ---- names -------------------------------------------------------------------------------- */

static void from_utf8(const uint8_t *in, int n, char *out)
{
    int o = 0;
    for (int i = 0; i < n && o < FS_NAME_MAX;) {
        uint8_t c = in[i];
        uint32_t cp;
        int len;
        if (c < 0x80)                { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else                         { cp = '_'; len = 1; }
        for (int k = 1; k < len; k++) {
            if (i + k >= n || (in[i + k] & 0xC0) != 0x80) {
                cp = '_';
                len = k;
                break;
            }
            cp = cp << 6 | (in[i + k] & 0x3F);
        }
        out[o++] = cp < 0x100 && cp ? (char)cp : '_';
        i += len;
    }
    out[o] = '\0';
}

static int to_utf8(const char *in, uint8_t *out)
{
    int o = 0;
    for (const uint8_t *p = (const uint8_t *)in; *p; p++) {
        if (*p < 0x80) {
            if (o + 1 > 255) return -1;
            out[o++] = *p;
        } else {
            if (o + 2 > 255) return -1;
            out[o++] = (uint8_t)(0xC0 | *p >> 6);
            out[o++] = (uint8_t)(0x80 | (*p & 0x3F));
        }
    }
    return o;
}

struct dentry {                         /* a directory entry found */
    struct key loc;                     /* its inode (INODE_ITEM), or a subvolume (ROOT_ITEM) */
    uint8_t  type;
    uint64_t index;
    char     name[FS_NAME_MAX + 1];
    uint8_t  raw[256];
    int      rawlen;
};

static uint16_t raw_hash(const struct dentry *e)
{
    uint16_t h = 0;
    for (int i = 0; i < e->rawlen; i++)
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + e->raw[i]);
    return h;
}

/* The next DIR_INDEX entry of a directory from index *next. 1 found, 0 end. */
static int dir_next(struct bvol *v, const struct dirref *d, uint64_t *next, struct dentry *e)
{
    struct key k = { d->ino, T_DIR_INDEX, *next }, f;
    const uint8_t *p;
    uint32_t sz;
    int rc = tree_find(v, d->tree, d->level, &k, false, &f, &p, &sz);
    if (rc <= 0 || f.obj != d->ino || f.type != T_DIR_INDEX)
        return rc < 0 ? rc : 0;
    if (sz < 30)
        return -ERROR_FILE_CORRUPT;
    kread(&e->loc, p);
    uint16_t nlen = le16(p + 27);
    e->type = p[29];
    if (30u + nlen > sz || nlen > 255)
        return -ERROR_FILE_CORRUPT;
    memcpy(e->raw, p + 30, nlen);
    e->rawlen = nlen;
    from_utf8(e->raw, nlen, e->name);
    e->index = f.off;
    *next = f.off + 1;
    return 1;
}

static int dir_find(struct bvol *v, const struct dirref *d, const char *name, struct dentry *e)
{
    uint8_t want[256];
    int wl = to_utf8(name, want);
    if (wl > 0) {                                       /* the exact name, by its hash */
        uint32_t hash = fs_crc32c(~1u, want, (uint32_t)wl);
        struct key k = { d->ino, T_DIR_ITEM, hash };
        const uint8_t *p;
        uint32_t sz;
        if (tree_get(v, d->tree, d->level, &k, &p, &sz) == 0) {
            for (uint32_t at = 0; at + 30 <= sz;) {     /* items that share the hash */
                uint16_t dl = le16(p + at + 25), nl = le16(p + at + 27);
                if (nl == wl && at + 30 + nl <= sz && !memcmp(p + at + 30, want, nl)) {
                    kread(&e->loc, p + at);
                    e->type = p[at + 29];
                    memcpy(e->raw, want, (size_t)wl);
                    e->rawlen = wl;
                    from_utf8(want, wl, e->name);
                    return 0;
                }
                at += 30 + nl + dl;
            }
        }
    }
    uint8_t base[11];                                   /* then any case, then an alias */
    int alias_n = 0, seen = 0;
    bool numbered = fs_alias_parse(name, base, &alias_n);
    for (int pass = 0; pass < 2; pass++) {
        uint64_t next = 0;
        int rc;
        seen = 0;
        while ((rc = dir_next(v, d, &next, e)) > 0) {
            if (pass == 0 && fs_name_eq(e->name, name))
                return 0;
            if (pass == 1) {
                uint8_t fcb[11];
                char alias[13];
                bool hashed = fs_make_alias(e->name, raw_hash(e), fcb, alias);
                if (!hashed && fs_name_eq(alias, name))
                    return 0;
                if (numbered && hashed && !memcmp(fcb, base, 11) && ++seen == alias_n)
                    return 0;
            }
        }
        if (rc < 0)
            return rc;
    }
    return -ERROR_FILE_NOT_FOUND;
}

/* Into a directory entry: a subvolume's entry leads into its own tree. */
static int enter(struct bvol *v, const struct dirref *in, const struct dentry *e, struct dirref *out)
{
    if (e->loc.type == T_ROOT) {
        uint64_t dir;
        int rc = tree_root(v, e->loc.obj, &out->tree, &out->level, &dir);
        if (rc < 0)
            return rc;
        out->ino = dir;
        return 0;
    }
    out->tree = in->tree;
    out->level = in->level;
    out->ino = e->loc.obj;
    return 0;
}

static int inode_get(struct bvol *v, const struct dirref *d, const uint8_t **p)
{
    struct key k = { d->ino, T_INODE, 0 };
    uint32_t sz;
    int rc = tree_get(v, d->tree, d->level, &k, p, &sz);
    if (rc == 0 && sz < 160)
        rc = -ERROR_FILE_CORRUPT;
    return rc;
}

static void top(const struct bvol *v, struct dirref *d) { *d = (struct dirref){ v->fs_tree, v->fs_level, v->root_dir }; }

/* To the directory holding the path's last component. */
static int walk(struct bvol *v, const char *path, struct dirref *dir, const char **last)
{
    top(v, dir);
    const char *s = path;
    for (;;) {
        const char *e = s;
        while (*e && *e != '\\')
            e++;
        if (!*e) {
            *last = s;
            return 0;
        }
        char comp[FS_NAME_MAX + 1];
        uint32_t len = (uint32_t)(e - s);
        if (!len || len > FS_NAME_MAX)
            return -ERROR_PATH_NOT_FOUND;
        memcpy(comp, s, len);
        comp[len] = '\0';
        static struct dentry de;
        int rc = dir_find(v, dir, comp, &de);
        if (rc < 0)
            return -ERROR_PATH_NOT_FOUND;
        if (de.type != 2 && de.loc.type != T_ROOT)      /* BTRFS_FT_DIR */
            return -ERROR_PATH_NOT_FOUND;
        struct dirref nd;
        if ((rc = enter(v, dir, &de, &nd)) < 0)
            return -ERROR_PATH_NOT_FOUND;
        *dir = nd;
        s = e + 1;
    }
}

static uint8_t attr_of(uint32_t mode, const char *name)
{
    uint8_t a;
    switch (mode & 0xF000) {
    case 0x4000: a = 0x10; break;
    case 0x8000: a = 0x20; break;
    default:     a = 0x04 | 0x01; break;                /* links, devices: read-only system */
    }
    a |= 0x01;                                          /* read-only: we don't write btrfs */
    if (name && name[0] == '.' && name[1] && !(name[1] == '.' && !name[2]))
        a |= 0x02;
    return a;
}

static void time_of(const uint8_t *inode, uint16_t *date, uint16_t *time)
{
    uint32_t sec = (uint32_t)le64(inode + 136);         /* mtime, UTC */
    fs_unix_to_dos(sec - (uint32_t)(fs_bias_utc(sec) * 60), date, time);
}

/* ---- file data ------------------------------------------------------------------------------ */

/* The checksums of the sectors at disk byte bytenr (CRC32C each). */
static int csum_check(struct bvol *v, uint64_t bytenr, const uint8_t *buf, uint32_t len)
{
    if (!v->csum_ok || !v->csum_tree)
        return 0;
    for (uint32_t done_b = 0; done_b < len; done_b += v->sector) {
        uint64_t at = bytenr + done_b;
        struct key k = { CSUM_OBJ, T_CSUM, at }, f;
        const uint8_t *p;
        uint32_t sz;
        int rc = tree_find(v, v->csum_tree, v->csum_level, &k, true, &f, &p, &sz);
        if (rc < 0)
            return rc;
        if (rc == 0 || f.obj != CSUM_OBJ || f.type != T_CSUM)
            continue;                                   /* no checksum (nodatasum) */
        uint64_t idx = (at - f.off) >> v->sshift;
        if (idx * 4 + 4 > sz)
            continue;
        uint32_t want = le32(p + idx * 4);
        uint32_t k2 = len - done_b < v->sector ? len - done_b : v->sector;
        if (~fs_crc32c(0xFFFFFFFFu, buf + done_b, k2) != want) {
            static int shown;
            if (shown++ < 4)
                klog("BTRFS: data at %u: its checksum is wrong\n", (uint32_t)at);
            return -ERROR_FILE_CORRUPT;
        }
    }
    return 0;
}

/* btrfs's LZO: a 4-byte total, then segments (a 4-byte length and LZO1X
 * data), each decompressing to at most a sector; a length never straddles
 * a sector boundary of the compressed data (it moves to the next). */
static int btrfs_lzo(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max, uint32_t sector)
{
    if (inlen < 4)
        return -1;
    uint32_t total = le32(in), at = 4, pos = 0;
    if (total > inlen)
        return -1;
    while (at + 4 <= total && pos < max) {
        if (sector - at % sector < 4)
            at += sector - at % sector;                 /* the padding before a sector's end */
        if (at + 4 > total)
            break;
        uint32_t seg = le32(in + at);
        at += 4;
        if (!seg || at + seg > total)
            return -1;
        int n = lzo1x_decompress(in + at, seg, out + pos, max - pos);
        if (n < 0)
            return -1;
        pos += (uint32_t)n;
        at += seg;
    }
    return (int)pos;
}

static int decompress(struct bvol *v, int type, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t max)
{
    switch (type) {
    case 1: return inflate_zlib(in, inlen, out, max);
    case 2: return btrfs_lzo(in, inlen, out, max, v->sector);
    case 3: return zstd_decompress(in, inlen, out, max);
    }
    return -1;
}

/* Up to len bytes of the file from pos (within its size). */
static int file_read(struct bvol *v, const struct bnode *n, uint64_t pos, uint8_t *buf, uint32_t len)
{
    uint32_t got = 0;
    while (got < len) {
        struct key k = { n->ino, T_EXTENT, pos }, f;
        const uint8_t *p;
        uint32_t sz;
        int rc = tree_find(v, n->tree, n->level, &k, true, &f, &p, &sz);
        if (rc < 0)
            return got ? (int)got : rc;
        uint64_t ext_end = 0, ext_start = 0;
        if (rc && f.obj == n->ino && f.type == T_EXTENT && sz >= 21) {
            ext_start = f.off;
            uint8_t comp = p[16], type = p[20];
            uint64_t ram = le64(p + 8);
            if (type == 0) {                            /* inline */
                ext_end = ext_start + ram;
                if (pos < ext_end) {
                    uint32_t want = (uint32_t)(ext_end - pos) < len - got ? (uint32_t)(ext_end - pos) : len - got;
                    if (comp) {
                        int dn = decompress(v, comp, p + 21, sz - 21, dbuf, 131072);
                        if (dn < 0 || pos - ext_start + want > (uint64_t)dn)
                            return got ? (int)got : -ERROR_FILE_CORRUPT;
                        memcpy(buf + got, dbuf + (pos - ext_start), want);
                        d_extent = 0;
                    } else {
                        if (21 + (pos - ext_start) + want > sz)
                            return got ? (int)got : -ERROR_FILE_CORRUPT;
                        memcpy(buf + got, p + 21 + (pos - ext_start), want);
                    }
                    got += want;
                    pos += want;
                    continue;
                }
            } else if (sz >= 53) {
                uint64_t bytenr = le64(p + 21), disk_len = le64(p + 29), off = le64(p + 37), nbytes = le64(p + 45);
                ext_end = ext_start + nbytes;
                if (pos < ext_end) {
                    uint32_t want = (uint32_t)(ext_end - pos) < len - got ? (uint32_t)(ext_end - pos) : len - got;
                    uint64_t in = off + (pos - ext_start);
                    if (!bytenr || type == 2) {         /* a hole, or preallocated */
                        memset(buf + got, 0, want);
                    } else if (!comp) {
                        if ((rc = log_read(v, bytenr + in, buf + got, want)) < 0)
                            return got ? (int)got : rc;
                        /* sector checksums cover whole sectors: check those wholly read */
                        uint64_t mask = ~(uint64_t)(v->sector - 1);
                        uint64_t s0 = (in + v->sector - 1) & mask, s1 = (in + want) & mask;
                        if (s1 > s0 && (rc = csum_check(v, bytenr + s0, buf + got + (s0 - in), (uint32_t)(s1 - s0))) < 0)
                            return got ? (int)got : rc;
                    } else {
                        if (d_extent != bytenr || d_vol != v) {     /* decompress the whole extent once */
                            if (disk_len > 131072 || ram > 131072)
                                return got ? (int)got : -ERROR_FILE_CORRUPT;
                            if (log_read(v, bytenr, cbuf, (uint32_t)disk_len) < 0)
                                return got ? (int)got : -ERROR_GEN_FAILURE;
                            if ((rc = csum_check(v, bytenr, cbuf, (uint32_t)disk_len)) < 0)
                                return got ? (int)got : rc;
                            d_len = decompress(v, comp, cbuf, (uint32_t)disk_len, dbuf, (uint32_t)ram);
                            d_extent = d_len >= 0 ? bytenr : 0;
                            d_vol = v;
                            if (d_len < 0) {
                                klog("BTRFS: a compressed extent (type %d) won't decompress\n", comp);
                                return got ? (int)got : -ERROR_FILE_CORRUPT;
                            }
                        }
                        if (in + want > (uint64_t)d_len)
                            return got ? (int)got : -ERROR_FILE_CORRUPT;
                        memcpy(buf + got, dbuf + in, want);
                    }
                    got += want;
                    pos += want;
                    continue;
                }
            }
        }
        /* no extent here (no_holes): zeros to the next extent, or the end */
        struct key nk = { n->ino, T_EXTENT, pos + 1 }, nf;
        uint64_t next = n->size;
        if (tree_find(v, n->tree, n->level, &nk, false, &nf, &p, &sz) > 0 && nf.obj == n->ino && nf.type == T_EXTENT &&
            nf.off < next)
            next = nf.off;
        uint32_t want = next - pos < len - got ? (uint32_t)(next - pos) : len - got;
        if (!want)
            break;
        memset(buf + got, 0, want);
        got += want;
        pos += want;
    }
    return (int)got;
}

/* ---- mounting --------------------------------------------------------------------------------- */

struct vol *btrfs_mount(struct partition *p)
{
    if (nvols == 4 || p->count < SB_OFFSET / SECTOR + 8)
        return NULL;
    static uint8_t sb[4096];
    struct bvol tmp = { .disk = p->disk, .start = p->start };
    if (phys_read(&tmp, SB_OFFSET, sb, 4096) < 0 || memcmp(sb + 0x40, MAGIC, 8))
        return NULL;
    struct bvol *v = &vols[nvols];
    *v = tmp;
    uint64_t incompat = le64(sb + 0xBC);
    v->csum_ok = le16(sb + 0xC4) == 0;                  /* CRC32C (xxhash, SHA-256, BLAKE2: not checked) */
    if (v->csum_ok && ~fs_crc32c(0xFFFFFFFFu, sb + 0x20, 4096 - 0x20) != le32(sb)) {
        klog("FS: btrfs at %u: the superblock's checksum is wrong: not mounted\n", p->start);
        return NULL;
    }
    if (incompat & ~(uint64_t)INCOMPAT_KNOWN) {
        klog("FS: btrfs at %u: features %X%08X: not mounted\n", p->start, (uint32_t)(incompat >> 32), (uint32_t)incompat);
        return NULL;
    }
    if (le64(sb + 0x88) != 1) {
        klog("FS: btrfs at %u: more than one device: not mounted\n", p->start);
        return NULL;
    }
    v->sector = le32(sb + 0x90);
    v->nodesize = le32(sb + 0x94);
    v->total = le64(sb + 0x70);
    v->used = le64(sb + 0x78);
    if (v->sector < 512 || v->sector > 65536 || (v->sector & (v->sector - 1)) || v->nodesize > 65536 ||
        v->nodesize < 4096 || (v->total >> 9) > p->count)
        return NULL;
    while ((1u << v->sshift) < v->sector)
        v->sshift++;
    for (int i = 0; i < 8; i++)
        if (!nbuf[i] && !(nbuf[i] = kmalloc(65536)))
            return NULL;
    if ((!cbuf && !(cbuf = kmalloc(131072))) || (!dbuf && !(dbuf = kmalloc(131072 + 64))))
        return NULL;
    /* the system chunks, then the chunk tree */
    uint32_t arr = le32(sb + 0xA0);
    for (uint32_t at = 0; at + 17 + 80 <= arr && arr <= 2048;) {
        const uint8_t *q = sb + 0x32B + at;
        struct key k;
        kread(&k, q);
        if (k.type != T_CHUNK)
            break;
        uint16_t ns = le16(q + 17 + 44);
        add_chunk(v, k.off, q + 17);
        at += 17 + 48 + 32u * ns;
    }
    uint64_t chunk_root = le64(sb + 0x58);
    uint8_t chunk_level = sb[0xC7];
    struct key k = { 256, T_CHUNK, 0 }, f;
    const uint8_t *d;
    uint32_t sz;
    for (int guard = 0; guard < 1024; guard++) {
        int rc = tree_find(v, chunk_root, chunk_level, &k, false, &f, &d, &sz);
        if (rc <= 0 || f.type != T_CHUNK)
            break;
        if (sz >= 80)
            add_chunk(v, f.off, d);
        k = f;
        k.off++;
    }
    v->root_tree = le64(sb + 0x50);
    v->root_level = sb[0xC6];
    if (tree_root(v, FS_TREE, &v->fs_tree, &v->fs_level, &v->root_dir) < 0) {
        klog("FS: btrfs at %u: no file system tree\n", p->start);
        return NULL;
    }
    if (tree_root(v, CSUM_TREE, &v->csum_tree, &v->csum_level, NULL) < 0)
        v->csum_tree = 0;
    v->h.serial = le32(sb + 0x20) ^ le32(sb + 0x24);
    int ln = 0;
    for (; ln < 11 && sb[0x12B + ln]; ln++)
        v->h.label[ln] = (char)sb[0x12B + ln];
    v->h.label[ln] = '\0';
    v->h.ops = &btrfs_ops;
    v->h.type = "btrfs";
    v->h.disk = p->disk;
    v->h.start = p->start;
    nvols++;
    klog("FS: btrfs on disk %d at %u: %u MB, %d chunks, sectors of %u, nodes of %u, read-only, label \"%s\"\n",
         (int)(p->disk - disks), p->start, (uint32_t)(v->total >> 20), v->nchunks, v->sector, v->nodesize, v->h.label);
    fs_add_volume(&v->h);
    return &v->h;
}

/* ---- the calls -------------------------------------------------------------------------------- */

static int bt_open(struct vol *vv, const char *path, uint32_t disp, bool write, uint8_t attr,
                   struct fnode **out, bool *created)
{
    struct bvol *v = BV(vv);
    (void)attr;
    fs_lock();
    struct dirref dir;
    const char *name;
    static struct dentry e;
    int rc = walk(v, path, &dir, &name);
    if (rc < 0)
        return done(v, rc);
    if (!*name)
        return done(v, -ERROR_ACCESS_DENIED);
    rc = dir_find(v, &dir, name, &e);
    if (rc == -ERROR_FILE_NOT_FOUND && (disp == CREATE_NEW || disp == CREATE_ALWAYS || disp == OPEN_ALWAYS))
        return done(v, -ERROR_WRITE_PROTECT);
    if (rc < 0)
        return done(v, rc);
    if (write || disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING || disp == CREATE_NEW)
        return done(v, disp == CREATE_NEW ? -ERROR_FILE_EXISTS : -ERROR_WRITE_PROTECT);
    struct dirref fd;
    if ((rc = enter(v, &dir, &e, &fd)) < 0)
        return done(v, rc);
    const uint8_t *ip;
    if ((rc = inode_get(v, &fd, &ip)) < 0)
        return done(v, rc);
    uint32_t mode = le32(ip + 52);
    if ((mode & 0xF000) == 0x4000)
        return done(v, -ERROR_ACCESS_DENIED);
    struct bnode *n = NULL;
    for (int i = 0; i < NODE_MAX && !n; i++)
        if (nodes[i].refs && nodes[i].v == v && nodes[i].tree == fd.tree && nodes[i].ino == fd.ino) {
            n = &nodes[i];
            n->refs++;
        }
    for (int i = 0; i < NODE_MAX && !n; i++)
        if (!nodes[i].refs) {
            n = &nodes[i];
            *n = (struct bnode){ .h.v = &v->h, .refs = 1, .v = v, .tree = fd.tree, .level = fd.level, .ino = fd.ino,
                                 .size = le64(ip + 16), .mode = mode };
            time_of(ip, &n->date, &n->time);
            char alias[13];
            fs_make_alias(e.name, raw_hash(&e), n->fcb, alias);
        }
    if (!n)
        return done(v, -ERROR_TOO_MANY_OPEN_FILES);
    *out = &n->h;
    if (created)
        *created = false;
    return done(v, 0);
}

static int bt_read(struct fnode *nn, uint64_t pos, void *buf, uint32_t len)
{
    struct bnode *n = BN(nn);
    fs_lock();
    if (pos >= n->size)
        return done(n->v, 0);
    if (len > n->size - pos)
        len = (uint32_t)(n->size - pos);
    return done(n->v, file_read(n->v, n, pos, buf, len));
}

static int bt_write(struct fnode *n, uint64_t pos, const void *buf, uint32_t len)
{
    (void)n; (void)pos; (void)buf; (void)len;
    return -ERROR_WRITE_PROTECT;
}

static int bt_set_size(struct fnode *n, uint64_t size) { (void)n; (void)size; return -ERROR_WRITE_PROTECT; }
static uint64_t bt_size(const struct fnode *n)        { return CBN(n)->size; }
static uint8_t bt_attr(const struct fnode *n)         { return attr_of(CBN(n)->mode, NULL); }
static void bt_name_of(const struct fnode *n, uint8_t fcb[11]) { memcpy(fcb, CBN(n)->fcb, 11); }
static void bt_get_time(const struct fnode *n, uint16_t *d, uint16_t *t) { *d = CBN(n)->date; *t = CBN(n)->time; }
static int bt_set_time(struct fnode *n, uint16_t d, uint16_t t) { (void)n; (void)d; (void)t; return -ERROR_WRITE_PROTECT; }
static int bt_flush(struct fnode *n) { (void)n; return 0; }

static void bt_dup(struct fnode *n)
{
    fs_lock();
    BN(n)->refs++;
    fs_unlock();
}

static void bt_close(struct fnode *n)
{
    fs_lock();
    if (BN(n)->refs > 0)
        BN(n)->refs--;
    fs_unlock();
}

static void fill_entry(struct bvol *v, const struct dentry *e, const uint8_t *ip, struct fs_entry *fe)
{
    (void)v;
    memset(fe, 0, sizeof *fe);
    memcpy(fe->name, e->name, strlen(e->name) + 1);
    fs_make_alias(e->name, raw_hash(e), fe->fcb_name, fe->short_name);
    uint32_t mode = ip ? le32(ip + 52) : 0x4000;
    fe->attr = attr_of(mode, e->name);
    fe->size = (fe->attr & 0x10) || !ip ? 0 : le64(ip + 16);
    if (ip) {
        time_of(ip, &fe->date, &fe->time);
        fe->cdate = fe->adate = fe->date;
        fe->ctime = fe->time;
    }
    fe->cluster = (uint32_t)e->loc.obj;
    fe->lfn = strcmp(fe->name, fe->short_name) != 0;
}

static int bt_stat(struct vol *vv, const char *path, struct fs_entry *fe)
{
    struct bvol *v = BV(vv);
    fs_lock();
    if (!*path) {
        memset(fe, 0, sizeof *fe);
        fe->attr = 0x10;
        return done(v, 0);
    }
    struct dirref dir, fd;
    const char *name;
    static struct dentry e;
    int rc = walk(v, path, &dir, &name);
    if (rc == 0)
        rc = *name ? dir_find(v, &dir, name, &e) : -ERROR_ACCESS_DENIED;
    const uint8_t *ip = NULL;
    if (rc == 0 && (rc = enter(v, &dir, &e, &fd)) == 0)
        rc = inode_get(v, &fd, &ip);
    if (rc == 0)
        fill_entry(v, &e, ip, fe);
    return done(v, rc);
}

static int bt_ro_path(struct vol *v, const char *p) { (void)v; (void)p; return -ERROR_WRITE_PROTECT; }
static int bt_ro_attr(struct vol *v, const char *p, uint8_t a) { (void)v; (void)p; (void)a; return -ERROR_WRITE_PROTECT; }
static int bt_ro_rename(struct vol *v, const char *a, const char *b) { (void)v; (void)a; (void)b; return -ERROR_WRITE_PROTECT; }

static int bt_short_path(struct vol *vv, const char *path, char *out, uint32_t size)
{
    struct bvol *v = BV(vv);
    fs_lock();
    struct dirref d;
    top(v, &d);
    uint32_t n = 0;
    out[0] = '\0';
    const char *s = path;
    int rc = 0;
    static struct dentry e;
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
        if ((rc = dir_find(v, &d, comp, &e)) < 0)
            break;
        uint8_t fcb[11];
        char alias[13];
        fs_make_alias(e.name, raw_hash(&e), fcb, alias);
        uint32_t al = (uint32_t)strlen(alias);
        if (n + al + 2 > size) {
            rc = -ERROR_FILENAME_EXCED_RANGE;
            break;
        }
        if (n)
            out[n++] = '\\';
        memcpy(out + n, alias, al + 1);
        n += al;
        struct dirref nd;
        if (enter(v, &d, &e, &nd) == 0)
            d = nd;
        s = *end ? end + 1 : end;
    }
    return done(v, rc);
}

/* f->dir: the directory's inode; f->dirlen: its tree (an index into seen
 * trees); f->next: 0 ".", 1 "..", then 2 + the DIR_INDEX index. */
static struct { uint64_t tree; uint8_t level; } trees[32];
static int ntrees;

static int tree_slot(uint64_t tree, uint8_t level)
{
    for (int i = 0; i < ntrees; i++)
        if (trees[i].tree == tree)
            return i;
    if (ntrees == 32)
        return 0;
    trees[ntrees].tree = tree;
    trees[ntrees].level = level;
    return ntrees++;
}

static int find_more(struct fs_find *f, struct fs_entry *out)
{
    struct bvol *v = BV(f->v);
    struct dirref d = { trees[f->dirlen].tree, trees[f->dirlen].level, f->dir };
    bool root = d.tree == v->fs_tree && d.ino == v->root_dir;
    while (f->next < 2) {
        const char *dots = f->next == 0 ? "." : "..";
        f->next++;
        if (root)
            continue;
        uint8_t fcb[11];
        memset(fcb, ' ', 11);
        memcpy(fcb, dots, strlen(dots));
        bool ok = f->dos ? (f->attr & 0x10) && fs_fcb_match(f->fcb, fcb)
                         : fs_wild(f->pattern, dots) || (f->pattern[0] == '*' && !f->pattern[1]);
        if (!ok)
            continue;
        memset(out, 0, sizeof *out);
        memcpy(out->name, dots, strlen(dots) + 1);
        memcpy(out->short_name, dots, strlen(dots) + 1);
        memcpy(out->fcb_name, fcb, 11);
        out->attr = 0x10;
        return 0;
    }
    static struct dentry e;
    for (;;) {
        uint64_t next = f->next - 2;
        int rc = dir_next(v, &d, &next, &e);
        f->next = (uint32_t)next + 2;
        if (rc <= 0)
            return rc < 0 ? rc : -ERROR_NO_MORE_FILES;
        struct dirref fd;
        const uint8_t *ip = NULL;
        if (enter(v, &d, &e, &fd) < 0 || inode_get(v, &fd, &ip) < 0)
            continue;
        struct fs_entry fe;
        fill_entry(v, &e, ip, &fe);
        if (f->dos) {
            if (f->attr == 0x08 || (fe.attr & (0x02 | 0x04 | 0x10) & ~f->attr) || !fs_fcb_match(f->fcb, fe.fcb_name))
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

static int find_dir(struct bvol *v, const char *dir, struct fs_find *f)
{
    struct dirref d;
    if (!*dir) {
        top(v, &d);
    } else {
        struct dirref parent;
        const char *name;
        static struct dentry e;
        int rc = walk(v, dir, &parent, &name);
        if (rc == 0)
            rc = dir_find(v, &parent, name, &e);
        if (rc < 0 || (e.type != 2 && e.loc.type != T_ROOT) || enter(v, &parent, &e, &d) < 0)
            return -ERROR_PATH_NOT_FOUND;
    }
    f->dir = (uint32_t)d.ino;
    f->dirlen = (uint32_t)tree_slot(d.tree, d.level);
    f->next = 0;
    return 0;
}

static int bt_find_first(struct vol *vv, const char *dir, const char *pattern, struct fs_find *f, struct fs_entry *e)
{
    struct bvol *v = BV(vv);
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

static int bt_find_dos(struct vol *vv, const char *dir, const uint8_t fcb[11], uint8_t attr, struct fs_find *f,
                       struct fs_entry *e)
{
    struct bvol *v = BV(vv);
    fs_lock();
    memset(f, 0, sizeof *f);
    f->v = vv;
    f->dos = true;
    f->attr = attr;
    memcpy(f->fcb, fcb, 11);
    int rc = find_dir(v, dir, f);
    if (rc == 0 && attr == 0x08) {
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

static int bt_find_next(struct fs_find *f, struct fs_entry *e)
{
    struct bvol *v = BV(f->v);
    fs_lock();
    if (f->dos && f->attr == 0x08)
        return done(v, -ERROR_NO_MORE_FILES);
    return done(v, find_more(f, e));
}

static int bt_space(struct vol *vv, uint32_t *spc, uint32_t *bps, uint32_t *freec, uint32_t *total)
{
    struct bvol *v = BV(vv);
    *spc = v->sector / SECTOR;
    *bps = SECTOR;
    *total = (uint32_t)(v->total >> v->sshift);
    *freec = (uint32_t)((v->total - v->used) >> v->sshift);
    return 0;
}

static void bt_sync(struct vol *v) { (void)v; }

static const struct fs_ops btrfs_ops = {
    .open = bt_open, .read = bt_read, .write = bt_write, .set_size = bt_set_size,
    .size = bt_size, .get_time = bt_get_time, .set_time = bt_set_time, .attr = bt_attr,
    .name_of = bt_name_of, .flush = bt_flush, .close = bt_close, .dup = bt_dup,
    .stat = bt_stat, .set_attr = bt_ro_attr, .del = bt_ro_path, .mkdir = bt_ro_path,
    .rmdir = bt_ro_path, .rename = bt_ro_rename, .short_path = bt_short_path,
    .find_first = bt_find_first, .find_dos = bt_find_dos, .find_next = bt_find_next,
    .space = bt_space, .sync = bt_sync,
};
