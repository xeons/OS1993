/*
 * exfat.c -- exFAT.
 *
 * Free space is the allocation bitmap's (one bit per cluster); the FAT is
 * only for files that aren't in one piece: a file or directory whose
 * stream entry says NoFatChain is contiguous from its first cluster, and
 * stays so while the cluster after its end is free. Growing past one that
 * isn't writes its chain into the FAT first. The root directory always
 * has a FAT chain.
 *
 * Every file is an entry set: a File entry (attributes, times), a Stream
 * Extension (first cluster, valid data length, data length, the name's
 * length and hash) and File Name entries of 15 UTF-16 characters, with a
 * checksum over the set. Names keep their case and compare through the
 * volume's up-case table (its first 256 characters: names here are 8-bit,
 * Latin-1). Bytes past the valid data length read as zeros; what this
 * driver writes is valid to its end.
 *
 * exFAT has no 8.3 names and no "." and ".." entries. For DOS each entry
 * gets an alias: the name itself in upper case if it fits 8.3, otherwise
 * two characters, the name hash in hex and "~1" ("AB12CD~1.TXT", as
 * Windows makes them when the ~N tails run out); searches in a folder
 * show "." and ".." first, as FAT's do. Times are kept as DOS's, with no
 * time zone (the RTC's local time, like FAT).
 *
 * Volumes up to 2^32 clusters, files and directories up to 4 GB - 1.
 */
#include "kernel.h"
#include "disk.h"
#include "fsimpl.h"
#include "mm.h"
#include "user.h"

#define A_RO   0x01
#define A_HID  0x02
#define A_SYS  0x04
#define A_VOL  0x08
#define A_DIR  0x10
#define A_ARC  0x20

#define T_END     0x00
#define T_BITMAP  0x81
#define T_UPCASE  0x82
#define T_LABEL   0x83
#define T_FILE    0x85
#define T_STREAM  0xC0
#define T_NAME    0xC1
#define F_ALLOC   0x01                  /* stream flags */
#define F_NOCHAIN 0x02

#define EOC       0xFFFFFFFFu
#define NODE_MAX  32
#define VOLS      4
#define SET_MAX   19                    /* file + stream + 17 names (255 characters) */

#define XV(x)  ((struct xvol *)(x))
#define XN(x)  ((struct xnode *)(x))
#define CXN(x) ((const struct xnode *)(x))

struct xvol {
    struct vol h;
    struct disk *disk;
    uint32_t start;
    uint32_t fat_lba, heap_lba;         /* absolute */
    uint32_t nclus;                     /* clusters 2 .. nclus + 1 */
    uint32_t spc, cbytes, epc;
    uint32_t root;
    uint32_t *bm;                       /* the bitmap's clusters */
    uint32_t bm_n;
    uint16_t upcase[256];
    uint32_t free, next_free;
    bool     changed;
};

struct dref {                           /* where a directory's entries are */
    uint32_t first;
    bool     contig;                    /* NoFatChain */
    uint32_t nclus;                     /* contig: how many; 0: follow the FAT */
};

struct xdir {                           /* a directory, and where its own entry is */
    struct dref d;
    bool     root;
    struct dref parent;
    uint32_t pidx;
};

struct xent {                           /* an entry set, read */
    uint32_t idx;
    int      count;                     /* entries in the set */
    uint8_t  raw[SET_MAX * 32];
    uint8_t  attr;
    uint8_t  flags;
    uint32_t first, size, vdl;
    char     name[FS_NAME_MAX + 1];
    char     alias[13];
    uint8_t  fcb[11];
};

struct xnode {
    struct fnode h;
    int      refs;
    struct xvol *v;
    struct dref parent;
    uint32_t idx;
    uint32_t first, size, vdl, nalloc;
    bool     contig;
    uint8_t  attr;
    uint16_t date, time;
    uint8_t  fcb[11];
    bool     dirty;
    uint32_t c_idx, c_clus;
};

static struct xvol vols[VOLS];
static int nvols;
static struct xnode nodes[NODE_MAX];
static const struct fs_ops exfat_ops;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint32_t clus_lba(const struct xvol *v, uint32_t c) { return v->heap_lba + (c - 2) * v->spc; }
static bool clus_ok(const struct xvol *v, uint32_t c)      { return c >= 2 && c - 2 < v->nclus; }

static int done(struct xvol *v, int rc)
{
    if (v->changed) {
        v->changed = false;
        if (cache_sync() < 0 && rc >= 0)
            rc = -ERROR_GEN_FAILURE;
    }
    fs_unlock();
    return rc;
}

/* ---- the FAT and the bitmap ------------------------------------------------- */

static int fat_get(struct xvol *v, uint32_t c, uint32_t *next)
{
    struct buf *b = bread(v->disk, v->fat_lba + c / 128);
    if (!b)
        return -ERROR_GEN_FAILURE;
    uint32_t x = le32(b->data + (c % 128) * 4);
    brelse(b);
    *next = clus_ok(v, x) ? x : EOC;
    return 0;
}

static int fat_set(struct xvol *v, uint32_t c, uint32_t val)
{
    struct buf *b = bread(v->disk, v->fat_lba + c / 128);
    if (!b)
        return -ERROR_GEN_FAILURE;
    put32(b->data + (c % 128) * 4, val);
    bdirty(b);
    brelse(b);
    v->changed = true;
    return 0;
}

/* The bitmap byte holding cluster c's bit. */
static struct buf *bm_byte(struct xvol *v, uint32_t c, uint8_t **p, uint8_t *bit)
{
    uint32_t i = c - 2, byte = i / 8;
    uint32_t ci = byte / v->cbytes, in = byte % v->cbytes;
    if (ci >= v->bm_n)
        return NULL;
    struct buf *b = bread(v->disk, clus_lba(v, v->bm[ci]) + in / SECTOR);
    if (b) {
        *p = b->data + in % SECTOR;
        *bit = (uint8_t)(1 << (i % 8));
    }
    return b;
}

static int bm_get(struct xvol *v, uint32_t c, bool *used)
{
    uint8_t *p, bit;
    struct buf *b = bm_byte(v, c, &p, &bit);
    if (!b)
        return -ERROR_GEN_FAILURE;
    *used = *p & bit;
    brelse(b);
    return 0;
}

static int bm_set(struct xvol *v, uint32_t c, bool used)
{
    uint8_t *p, bit;
    struct buf *b = bm_byte(v, c, &p, &bit);
    if (!b)
        return -ERROR_GEN_FAILURE;
    bool was = *p & bit;
    if (used) *p |= bit;
    else      *p &= (uint8_t)~bit;
    bdirty(b);
    brelse(b);
    if (was != used)
        v->free += used ? (uint32_t)-1 : 1;
    v->changed = true;
    return 0;
}

static bool clus_free(struct xvol *v, uint32_t c)
{
    bool used = true;
    return clus_ok(v, c) && bm_get(v, c, &used) == 0 && !used;
}

static int alloc_any(struct xvol *v, uint32_t *out)
{
    if (!v->free)
        return -ERROR_DISK_FULL;
    uint32_t c = clus_ok(v, v->next_free) ? v->next_free : 2;
    for (uint32_t n = 0; n < v->nclus; n++, c = c - 2 + 1 >= v->nclus ? 2 : c + 1) {
        bool used;
        if (bm_get(v, c, &used) < 0)
            return -ERROR_GEN_FAILURE;
        if (used)
            continue;
        if (bm_set(v, c, true) < 0)
            return -ERROR_GEN_FAILURE;
        v->next_free = c + 1;
        *out = c;
        return 0;
    }
    return -ERROR_DISK_FULL;
}

static int zero_clus(struct xvol *v, uint32_t c)
{
    for (uint32_t s = 0; s < v->spc; s++) {
        struct buf *b = bzero_get(v->disk, clus_lba(v, c) + s);
        bdirty(b);
        brelse(b);
    }
    v->changed = true;
    return 0;
}

/* Give back n clusters from first (contiguous, or a FAT chain). */
static int free_clusters(struct xvol *v, uint32_t first, bool contig, uint32_t n)
{
    uint32_t c = first;
    for (uint32_t i = 0; i < n && clus_ok(v, c); i++) {
        if (bm_set(v, c, false) < 0)
            return -ERROR_GEN_FAILURE;
        if (contig) {
            c++;
        } else {
            uint32_t next;
            if (fat_get(v, c, &next) < 0)
                return -ERROR_GEN_FAILURE;
            c = next;
        }
    }
    return 0;
}

/* Write the FAT chain of a contiguous run (it is about to stop being one). */
static int chain_run(struct xvol *v, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (fat_set(v, first + i, i + 1 < n ? first + i + 1 : EOC) < 0)
            return -ERROR_GEN_FAILURE;
    return 0;
}

/* One more cluster after a file's last (n so far, last = its cluster n-1).
 * Contiguous if the next one is free; else the file gets a FAT chain. */
static int grow(struct xvol *v, uint32_t *first, bool *contig, uint32_t n, uint32_t last, uint32_t *out)
{
    int rc;
    if (n == 0) {
        if ((rc = alloc_any(v, out)) < 0)
            return rc;
        *first = *out;
        *contig = true;
        return 0;
    }
    if (*contig && clus_free(v, last + 1)) {
        if (bm_set(v, last + 1, true) < 0)
            return -ERROR_GEN_FAILURE;
        *out = last + 1;
        return 0;
    }
    if ((rc = alloc_any(v, out)) < 0)
        return rc;
    if (*contig) {
        if ((rc = chain_run(v, *first, n)) < 0)
            return rc;
        *contig = false;
    }
    if (fat_set(v, last, *out) < 0 || fat_set(v, *out, EOC) < 0)
        return -ERROR_GEN_FAILURE;
    return 0;
}

/* ---- directories ---------------------------------------------------------------- */

struct dcur {
    struct xvol *v;
    struct dref d;
    uint32_t c_idx, c_clus;
};

static void dcur_init(struct dcur *dc, struct xvol *v, const struct dref *d)
{
    *dc = (struct dcur){ v, *d, 0, 0 };
}

/* The sector of entry idx: 0, 1 past the end, <0 error. */
static int dir_sector(struct dcur *dc, uint32_t idx, uint32_t *lba)
{
    struct xvol *v = dc->v;
    uint32_t want = idx / v->epc;
    uint32_t c;
    if (dc->d.contig) {
        if (want >= dc->d.nclus)
            return 1;
        c = dc->d.first + want;
    } else {
        if (!dc->c_clus || dc->c_idx > want) {
            dc->c_idx = 0;
            dc->c_clus = dc->d.first;
        }
        while (dc->c_idx < want) {
            uint32_t next;
            if (fat_get(v, dc->c_clus, &next) < 0)
                return -ERROR_GEN_FAILURE;
            if (next == EOC)
                return 1;
            dc->c_clus = next;
            dc->c_idx++;
        }
        c = dc->c_clus;
    }
    if (!clus_ok(v, c))
        return -ERROR_GEN_FAILURE;
    *lba = clus_lba(v, c) + (idx % v->epc) / 16;
    return 0;
}

static int ent_read(struct dcur *dc, uint32_t idx, uint8_t ent[32])
{
    uint32_t lba;
    int rc = dir_sector(dc, idx, &lba);
    if (rc)
        return rc;
    struct buf *b = bread(dc->v->disk, lba);
    if (!b)
        return -ERROR_GEN_FAILURE;
    memcpy(ent, b->data + (idx % 16) * 32, 32);
    brelse(b);
    return 0;
}

static int ent_write(struct dcur *dc, uint32_t idx, const uint8_t ent[32])
{
    uint32_t lba;
    int rc = dir_sector(dc, idx, &lba);
    if (rc)
        return rc > 0 ? -ERROR_CANNOT_MAKE : rc;
    struct buf *b = bread(dc->v->disk, lba);
    if (!b)
        return -ERROR_GEN_FAILURE;
    memcpy(b->data + (idx % 16) * 32, ent, 32);
    bdirty(b);
    brelse(b);
    dc->v->changed = true;
    return 0;
}

static uint16_t set_sum(const uint8_t *set, int count)
{
    uint16_t s = 0;
    for (int i = 0; i < count * 32; i++)
        if (i != 2 && i != 3)
            s = (uint16_t)(((s & 1) ? 0x8000 : 0) + (s >> 1) + set[i]);
    return s;
}

static uint16_t up16(const struct xvol *v, uint8_t c) { return v->upcase[c]; }

static uint16_t name_hash(const struct xvol *v, const char *name)
{
    uint16_t h = 0;
    for (const uint8_t *p = (const uint8_t *)name; *p; p++) {
        uint16_t u = up16(v, *p);
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (u & 0xFF));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (u >> 8));
    }
    return h;
}

/* Decode a set already read into e->raw. false: not a sound file set. */
static bool decode(struct xent *e)
{
    const uint8_t *f = e->raw, *s = e->raw + 32;
    if (e->count < 3 || s[0] != T_STREAM || set_sum(e->raw, e->count) != le16(f + 2))
        return false;
    e->attr = (uint8_t)le16(f + 4);
    e->flags = s[1];
    int len = s[3];
    e->first = le32(s + 20);
    e->vdl = le32(s + 12) ? 0xFFFFFFFFu : le32(s + 8);
    e->size = le32(s + 28) ? 0xFFFFFFFFu : le32(s + 24);
    int n = 0;
    for (int i = 2; i < e->count && n < len; i++) {
        const uint8_t *nm = e->raw + i * 32;
        if (nm[0] != T_NAME)
            break;
        for (int k = 0; k < 15 && n < len; k++) {
            uint16_t ch = le16(nm + 2 + k * 2);
            e->name[n++] = ch < 0x100 ? (char)ch : '_';
        }
    }
    e->name[n] = '\0';
    if (!n)
        return false;
    fs_make_alias(e->name, le16(s + 4), e->fcb, e->alias);
    return true;
}

/* The next file set from *idx (it moves past it). 1 found, 0 end, <0 error.
 * label, if given, gets the volume label when the entry goes by. */
static int dir_next(struct dcur *dc, uint32_t *idx, struct xent *e, char *label)
{
    for (;;) {
        uint8_t ent[32];
        int rc = ent_read(dc, *idx, ent);
        if (rc)
            return rc < 0 ? rc : 0;
        uint8_t t = ent[0];
        if (t == T_END)
            return 0;
        if (t == T_LABEL && label) {
            int n = ent[1] > 11 ? 11 : ent[1];
            for (int k = 0; k < n; k++) {
                uint16_t ch = le16(ent + 2 + k * 2);
                label[k] = ch < 0x100 ? (char)ch : '_';
            }
            label[n] = '\0';
        }
        if (t != T_FILE) {
            (*idx)++;
            if ((t & 0xE0) == 0xA0)                     /* a benign primary: its secondaries too */
                *idx += ent[1];
            continue;
        }
        int count = 1 + ent[1];
        if (count < 3 || count > SET_MAX) {
            (*idx)++;
            continue;
        }
        e->idx = *idx;
        e->count = count;
        memcpy(e->raw, ent, 32);
        bool ok = true;
        for (int i = 1; i < count && ok; i++) {
            rc = ent_read(dc, *idx + (uint32_t)i, e->raw + i * 32);
            if (rc < 0)
                return rc;
            ok = rc == 0 && (e->raw[i * 32] & 0xC0) == 0xC0;
        }
        if (!ok || !decode(e)) {
            (*idx)++;                                   /* damaged: skip its first entry */
            continue;
        }
        *idx += (uint32_t)count;
        return 1;
    }
}

static int dir_find(struct xvol *v, const struct dref *d, const char *name, struct xent *e)
{
    struct dcur dc;
    dcur_init(&dc, v, d);
    uint32_t idx = 0;
    for (;;) {
        int rc = dir_next(&dc, &idx, e, NULL);
        if (rc <= 0)
            return rc < 0 ? rc : -ERROR_FILE_NOT_FOUND;
        if (fs_name_eq(name, e->name) || fs_name_eq(name, e->alias))
            return 0;
    }
}

static struct dref child_dref(const struct xvol *v, const struct xent *e)
{
    struct dref d = { e->first, (e->flags & F_NOCHAIN) != 0, 0 };
    if (d.contig)
        d.nclus = (e->size + v->cbytes - 1) / v->cbytes;
    return d;
}

static void root_dir(const struct xvol *v, struct xdir *x)
{
    *x = (struct xdir){ .d = { v->root, false, 0 }, .root = true };
}

/* To the directory holding the path's last component. */
static int walk(struct xvol *v, const char *path, struct xdir *dir, const char **last)
{
    root_dir(v, dir);
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
        if (len == 0 || len > FS_NAME_MAX)
            return -ERROR_PATH_NOT_FOUND;
        memcpy(comp, s, len);
        comp[len] = '\0';
        static struct xent x;                           /* big: not on the kernel stack */
        int rc = dir_find(v, &dir->d, comp, &x);
        if (rc < 0)
            return rc == -ERROR_FILE_NOT_FOUND ? -ERROR_PATH_NOT_FOUND : rc;
        if (!(x.attr & A_DIR) || !clus_ok(v, x.first))
            return -ERROR_PATH_NOT_FOUND;
        struct xdir child = { .d = child_dref(v, &x), .root = false, .parent = dir->d, .pidx = x.idx };
        *dir = child;
        s = e + 1;
    }
}

/* Rewrite a set (recomputing its checksum). */
static int set_write(struct xvol *v, const struct dref *d, uint32_t idx, uint8_t *raw, int count)
{
    put16(raw + 2, set_sum(raw, count));
    struct dcur dc;
    dcur_init(&dc, v, d);
    for (int i = 0; i < count; i++) {
        int rc = ent_write(&dc, idx + (uint32_t)i, raw + i * 32);
        if (rc < 0)
            return rc;
    }
    return 0;
}

static int set_read(struct xvol *v, const struct dref *d, uint32_t idx, struct xent *e)
{
    struct dcur dc;
    dcur_init(&dc, v, d);
    if (ent_read(&dc, idx, e->raw) != 0 || e->raw[0] != T_FILE)
        return -ERROR_GEN_FAILURE;
    e->idx = idx;
    e->count = 1 + e->raw[1];
    if (e->count < 3 || e->count > SET_MAX)
        return -ERROR_GEN_FAILURE;
    for (int i = 1; i < e->count; i++)
        if (ent_read(&dc, idx + (uint32_t)i, e->raw + i * 32) != 0)
            return -ERROR_GEN_FAILURE;
    return decode(e) ? 0 : -ERROR_GEN_FAILURE;
}

/* A directory is one cluster bigger (zeroed). Its own entry says so. */
static int dir_grow(struct xvol *v, struct xdir *x)
{
    uint32_t n, last;
    if (x->d.contig) {
        n = x->d.nclus;
        last = x->d.first + n - 1;
    } else {
        n = 0;
        last = x->d.first;
        for (;;) {
            uint32_t next;
            if (fat_get(v, last, &next) < 0)
                return -ERROR_GEN_FAILURE;
            n++;
            if (next == EOC)
                break;
            last = next;
            if (n > v->nclus)
                return -ERROR_GEN_FAILURE;
        }
    }
    if (n * v->cbytes >= 256u * 1024 * 1024)
        return -ERROR_CANNOT_MAKE;                      /* exFAT's limit for a directory */
    uint32_t c;
    bool contig = x->d.contig;
    int rc = grow(v, &x->d.first, &contig, n, last, &c);
    if (rc < 0)
        return rc;
    zero_clus(v, c);
    x->d.contig = contig;
    x->d.nclus = contig ? n + 1 : 0;
    if (x->root)
        return 0;
    static struct xent pe;
    if ((rc = set_read(v, &x->parent, x->pidx, &pe)) < 0)
        return rc;
    uint8_t *s = pe.raw + 32;
    s[1] = (uint8_t)(F_ALLOC | (contig ? F_NOCHAIN : 0));
    put32(s + 8, (n + 1) * v->cbytes);
    put32(s + 12, 0);
    put32(s + 24, (n + 1) * v->cbytes);
    put32(s + 28, 0);
    return set_write(v, &x->parent, x->pidx, pe.raw, pe.count);
}

static void stamp_now(uint8_t *f, bool created)
{
    uint16_t date, time;
    rtc_dos_time(&date, &time);
    uint32_t ts = (uint32_t)date << 16 | time;
    put32(f + 12, ts);
    put32(f + 16, ts);
    f[21] = 0;
    f[23] = f[24] = 0;
    if (created) {
        put32(f + 8, ts);
        f[20] = 0;
        f[22] = 0;
    }
}

/* Build a set for name: from proto's File and Stream entries if given. */
static int build_set(struct xvol *v, const char *in, const uint8_t *proto, uint8_t *raw, int *count)
{
    char name[FS_NAME_MAX + 1];
    int len = fs_clean_name(in, name);
    if (len < 0)
        return len;
    int names = (len + 14) / 15;
    *count = 2 + names;
    memset(raw, 0, (size_t)*count * 32);
    if (proto)
        memcpy(raw, proto, 64);
    raw[0] = T_FILE;
    raw[1] = (uint8_t)(*count - 1);
    raw[32] = T_STREAM;
    raw[32 + 3] = (uint8_t)len;
    put16(raw + 32 + 4, name_hash(v, name));
    for (int i = 0; i < names; i++) {
        uint8_t *nm = raw + (2 + i) * 32;
        nm[0] = T_NAME;
        for (int k = 0; k < 15; k++) {
            int ci = i * 15 + k;
            put16(nm + 2 + k * 2, ci < len ? (uint8_t)name[ci] : 0);
        }
    }
    put16(raw + 2, set_sum(raw, *count));
    return 0;
}

/* Put a set in a directory: the first run of free entries long enough,
 * growing the directory if need be. -> its index. */
static int set_add(struct xvol *v, struct xdir *x, uint8_t *raw, int count, uint32_t *at)
{
    struct dcur dc;
    for (int tries = 0; tries < 2 + count; tries++) {
        dcur_init(&dc, v, &x->d);
        uint32_t run = 0, idx;
        for (idx = 0;; idx++) {
            uint8_t ent[32];
            int rc = ent_read(&dc, idx, ent);
            if (rc < 0)
                return rc;
            if (rc == 1)
                break;                                  /* full: grow it */
            run = (ent[0] & 0x80) ? 0 : run + 1;        /* free: end marker, or deleted */
            if (run == (uint32_t)count) {
                uint32_t start = idx + 1 - run;
                for (int i = 0; i < count; i++)
                    if ((rc = ent_write(&dc, start + (uint32_t)i, raw + i * 32)) < 0)
                        return rc;
                *at = start;
                return 0;
            }
        }
        int rc = dir_grow(v, x);
        if (rc < 0)
            return rc;
    }
    return -ERROR_CANNOT_MAKE;
}

static int set_remove(struct xvol *v, const struct dref *d, const struct xent *e)
{
    struct dcur dc;
    dcur_init(&dc, v, d);
    for (int i = 0; i < e->count; i++) {
        uint8_t ent[32];
        memcpy(ent, e->raw + i * 32, 32);
        ent[0] &= 0x7F;
        int rc = ent_write(&dc, e->idx + (uint32_t)i, ent);
        if (rc < 0)
            return rc;
    }
    return 0;
}

static void fill_entry(const struct xent *x, struct fs_entry *e)
{
    memcpy(e->name, x->name, strlen(x->name) + 1);
    memcpy(e->short_name, x->alias, strlen(x->alias) + 1);
    memcpy(e->fcb_name, x->fcb, 11);
    e->attr = x->attr & 0x37;
    e->size = (x->attr & A_DIR) ? 0 : x->size;
    uint32_t m = le32(x->raw + 12), c = le32(x->raw + 8), a = le32(x->raw + 16);
    e->time = (uint16_t)m;
    e->date = (uint16_t)(m >> 16);
    e->ctime = (uint16_t)c;
    e->cdate = (uint16_t)(c >> 16);
    e->adate = (uint16_t)(a >> 16);
    e->cluster = x->first;
    e->lfn = !fs_name_eq(x->name, x->alias) || strcmp(x->name, x->alias) != 0;
}

/* ---- mounting --------------------------------------------------------------------- */

static void load_upcase(struct xvol *v, uint32_t first, uint32_t len)
{
    for (int i = 0; i < 256; i++)
        v->upcase[i] = (uint16_t)i;
    uint32_t pos = 0, ch = 0;
    struct dref d = { first, true, (len + v->cbytes - 1) / v->cbytes };
    while (pos + 1 < len && ch < 256) {
        uint32_t c = d.first + pos / v->cbytes;        /* the table is read as one run */
        struct buf *b = bread(v->disk, clus_lba(v, c) + (pos % v->cbytes) / SECTOR);
        if (!b)
            return;
        uint16_t u = le16(b->data + pos % SECTOR);
        brelse(b);
        pos += 2;
        if (u == 0xFFFF && pos + 1 < len) {             /* a run of identity mappings */
            b = bread(v->disk, clus_lba(v, d.first + pos / v->cbytes) + (pos % v->cbytes) / SECTOR);
            if (!b)
                return;
            ch += le16(b->data + pos % SECTOR);
            brelse(b);
            pos += 2;
        } else {
            v->upcase[ch++] = u;
        }
    }
}

struct vol *exfat_mount(struct partition *p)
{
    if (nvols == VOLS)
        return NULL;
    struct buf *b = bread(p->disk, p->start);
    if (!b)
        return NULL;
    uint8_t bs[SECTOR];
    memcpy(bs, b->data, SECTOR);
    brelse(b);
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0 || bs[510] != 0x55 || bs[511] != 0xAA)
        return NULL;
    if (bs[108] != 9 || bs[109] > 25 || bs[110] != 1) {
        klog("FS: exFAT at %u: %u-byte sectors or %d FATs: not supported\n", p->start, 1u << bs[108], bs[110]);
        return NULL;
    }
    struct xvol *v = &vols[nvols];
    *v = (struct xvol){ .disk = p->disk, .start = p->start };
    v->fat_lba = p->start + le32(bs + 80);
    v->heap_lba = p->start + le32(bs + 88);
    v->nclus = le32(bs + 92);
    v->root = le32(bs + 96);
    v->h.serial = le32(bs + 100);
    v->spc = 1u << bs[109];
    v->cbytes = v->spc * SECTOR;
    v->epc = v->cbytes / 32;
    if (v->heap_lba - p->start + (uint64_t)v->nclus * v->spc > p->count || !clus_ok(v, v->root))
        return NULL;
    for (int i = 0; i < 256; i++)
        v->upcase[i] = (uint16_t)(i >= 'a' && i <= 'z' ? i - 32 : i);

    /* the root's critical entries: bitmap, up-case table, label */
    struct dcur dc;
    struct dref rd = { v->root, false, 0 };
    dcur_init(&dc, v, &rd);
    uint32_t bm_first = 0, bm_len = 0, up_first = 0, up_len = 0;
    for (uint32_t idx = 0; idx < 4096; idx++) {
        uint8_t ent[32];
        if (ent_read(&dc, idx, ent) != 0 || ent[0] == T_END)
            break;
        if (ent[0] == T_BITMAP && !(ent[1] & 1)) {
            bm_first = le32(ent + 20);
            bm_len = le32(ent + 24);
        } else if (ent[0] == T_UPCASE) {
            up_first = le32(ent + 20);
            up_len = le32(ent + 24);
        } else if (ent[0] == T_LABEL) {
            int n = ent[1] > 11 ? 11 : ent[1];
            for (int k = 0; k < n; k++) {
                uint16_t ch = le16(ent + 2 + k * 2);
                v->h.label[k] = ch < 0x100 ? (char)ch : '_';
            }
            v->h.label[n] = '\0';
        }
    }
    if (!clus_ok(v, bm_first) || bm_len * 8 < v->nclus) {
        klog("FS: exFAT at %u: no allocation bitmap\n", p->start);
        return NULL;
    }
    v->bm_n = (bm_len + v->cbytes - 1) / v->cbytes;
    v->bm = kmalloc(v->bm_n * sizeof *v->bm);
    if (!v->bm)
        return NULL;
    uint32_t c = bm_first;
    for (uint32_t i = 0; i < v->bm_n; i++) {           /* its clusters, from the FAT */
        v->bm[i] = c;
        if (i + 1 < v->bm_n) {
            uint32_t next;
            if (fat_get(v, c, &next) < 0)
                return NULL;
            c = next == EOC ? c + 1 : next;             /* no chain written: contiguous */
        }
    }
    if (clus_ok(v, up_first) && up_len)
        load_upcase(v, up_first, up_len);
    /* count the free clusters */
    uint32_t used = 0;
    for (uint32_t i = 0; i < v->bm_n; i++)
        for (uint32_t s = 0; s < v->spc; s++) {
            uint32_t base = (i * v->cbytes + s * SECTOR) * 8;
            if (base >= v->nclus)
                break;
            struct buf *bb = bread(v->disk, clus_lba(v, v->bm[i]) + s);
            if (!bb)
                return NULL;
            for (uint32_t k = 0; k < SECTOR && base + k * 8 < v->nclus; k++) {
                uint8_t x = bb->data[k];
                for (int bit = 0; bit < 8 && base + k * 8 + (uint32_t)bit < v->nclus; bit++)
                    used += (x >> bit) & 1;
            }
            brelse(bb);
        }
    v->free = v->nclus - used;
    v->next_free = 2;
    v->h.ops = &exfat_ops;
    v->h.type = "exFAT";
    nvols++;
    klog("FS: exFAT on disk %d at %u: %u clusters of %u bytes, %u free, serial %04X-%04X, label \"%s\"\n",
         (int)(p->disk - disks), p->start, v->nclus, v->cbytes, v->free, v->h.serial >> 16,
         v->h.serial & 0xFFFF, v->h.label);
    fs_add_volume(&v->h);
    return &v->h;
}

/* ---- files ---------------------------------------------------------------------- */

static struct xnode *node_get(struct xvol *v, const struct dref *parent, const struct xent *e)
{
    struct xnode *fr = NULL;
    for (int i = 0; i < NODE_MAX; i++) {
        struct xnode *n = &nodes[i];
        if (n->refs && n->v == v && n->parent.first == parent->first && n->idx == e->idx) {
            n->refs++;
            return n;
        }
        if (!n->refs && !fr)
            fr = n;
    }
    if (!fr)
        return NULL;
    uint32_t m = le32(e->raw + 12);
    *fr = (struct xnode){ .h.v = &v->h, .refs = 1, .v = v, .parent = *parent, .idx = e->idx,
                          .first = e->first, .size = e->size, .vdl = e->vdl,
                          .contig = (e->flags & F_NOCHAIN) != 0, .attr = e->attr,
                          .date = (uint16_t)(m >> 16), .time = (uint16_t)m };
    fr->nalloc = clus_ok(v, fr->first) ? (fr->size + v->cbytes - 1) / v->cbytes : 0;
    if (!fr->nalloc)
        fr->first = 0;
    memcpy(fr->fcb, e->fcb, 11);
    return fr;
}

static bool node_open(struct xvol *v, const struct dref *parent, uint32_t idx)
{
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v && nodes[i].parent.first == parent->first && nodes[i].idx == idx)
            return true;
    return false;
}

static int node_update(struct xnode *n)
{
    if (!n->dirty)
        return 0;
    static struct xent e;
    int rc = set_read(n->v, &n->parent, n->idx, &e);
    if (rc < 0)
        return rc;
    uint8_t *f = e.raw, *s = e.raw + 32;
    put16(f + 4, n->attr);
    uint32_t ts = (uint32_t)n->date << 16 | n->time;
    put32(f + 12, ts);
    put32(f + 16, ts);
    f[21] = 0;
    s[1] = (uint8_t)(F_ALLOC | (n->contig && n->first ? F_NOCHAIN : 0));
    put32(s + 8, n->vdl);
    put32(s + 12, 0);
    put32(s + 20, n->first);
    put32(s + 24, n->size);
    put32(s + 28, 0);
    n->dirty = false;
    return set_write(n->v, &n->parent, n->idx, e.raw, e.count);
}

/* The file's cluster ci (with alloc: one past its end is added). */
static int node_clus(struct xnode *n, uint32_t ci, bool alloc, uint32_t *out)
{
    struct xvol *v = n->v;
    if (ci >= n->nalloc) {
        if (!alloc || ci > n->nalloc)
            return 1;
        uint32_t last = 0;
        if (n->nalloc) {
            int rc = node_clus(n, n->nalloc - 1, false, &last);
            if (rc)
                return rc < 0 ? rc : -ERROR_GEN_FAILURE;
        }
        uint32_t c;
        int rc = grow(v, &n->first, &n->contig, n->nalloc, last, &c);
        if (rc < 0)
            return rc;
        n->nalloc++;
        n->dirty = true;
        n->c_idx = ci;
        n->c_clus = c;
        *out = c;
        return 0;
    }
    if (n->contig) {
        *out = n->first + ci;
        return 0;
    }
    if (!n->c_clus || n->c_idx > ci) {
        n->c_idx = 0;
        n->c_clus = n->first;
    }
    while (n->c_idx < ci) {
        uint32_t next;
        if (fat_get(v, n->c_clus, &next) < 0 || next == EOC)
            return -ERROR_GEN_FAILURE;
        n->c_clus = next;
        n->c_idx++;
    }
    *out = n->c_clus;
    return 0;
}

/* Bytes between the file and buf (NULL when writing: zeros). */
static int node_io(struct xnode *n, uint32_t pos, uint8_t *buf, uint32_t len, bool write)
{
    struct xvol *v = n->v;
    uint32_t done_n = 0;
    while (done_n < len) {
        uint32_t c;
        int rc = node_clus(n, pos / v->cbytes, write, &c);
        if (rc) {
            if (done_n)
                break;
            return rc < 0 ? rc : 0;
        }
        uint32_t in = pos % v->cbytes;
        uint32_t chunk = v->cbytes - in < len - done_n ? v->cbytes - in : len - done_n;
        if (in % SECTOR == 0 && chunk >= SECTOR && buf) {      /* whole sectors: no cache */
            uint32_t secs = chunk / SECTOR, lba = clus_lba(v, c) + in / SECTOR;
            if (secs > 256)
                secs = 256;
            chunk = secs * SECTOR;
            if (write) {
                if (ata_write(v->disk, lba, secs, buf + done_n) < 0)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
                cache_forget(v->disk, lba, secs);
            } else {
                cache_sync();
                if (ata_read(v->disk, lba, secs, buf + done_n) < 0)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
            }
        } else {
            uint32_t lba = clus_lba(v, c) + in / SECTOR, so = in % SECTOR;
            if (chunk > SECTOR - so)
                chunk = SECTOR - so;
            struct buf *b = bread(v->disk, lba);
            if (!b)
                return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
            if (write) {
                if (buf) memcpy(b->data + so, buf + done_n, chunk);
                else     memset(b->data + so, 0, chunk);
                bdirty(b);
            } else {
                memcpy(buf + done_n, b->data + so, chunk);
            }
            brelse(b);
        }
        if (write)
            v->changed = true;
        done_n += chunk;
        pos += chunk;
    }
    return (int)done_n;
}

/* Make the file valid (written) up to `to`: zeros from the valid length. */
static int validate(struct xnode *n, uint32_t to)
{
    if (to <= n->vdl)
        return 0;
    uint32_t from = n->vdl;
    int rc = node_io(n, from, NULL, to - from, true);
    if (rc < 0)
        return rc;
    n->vdl = from + (uint32_t)rc;
    if (n->vdl > n->size)
        n->size = n->vdl;
    n->dirty = true;
    return n->vdl == to ? 0 : -ERROR_DISK_FULL;
}

static int set_size(struct xnode *n, uint32_t size)
{
    struct xvol *v = n->v;
    if (size > n->size)
        return validate(n, size);
    if (size == n->size)
        return 0;
    uint32_t keep = (size + v->cbytes - 1) / v->cbytes;
    if (keep < n->nalloc) {
        if (n->contig) {
            if (free_clusters(v, n->first + keep, true, n->nalloc - keep) < 0)
                return -ERROR_GEN_FAILURE;
        } else {
            uint32_t from;
            if (keep == 0) {
                from = n->first;
            } else {
                uint32_t last;
                if (node_clus(n, keep - 1, false, &last) != 0 || fat_get(v, last, &from) < 0 ||
                    fat_set(v, last, EOC) < 0)
                    return -ERROR_GEN_FAILURE;
            }
            if (free_clusters(v, from, false, n->nalloc - keep) < 0)
                return -ERROR_GEN_FAILURE;
        }
        n->nalloc = keep;
        if (!keep) {
            n->first = 0;
            n->contig = false;
        }
    }
    n->c_clus = 0;
    n->size = size;
    if (n->vdl > size)
        n->vdl = size;
    n->dirty = true;
    return 0;
}

static int ex_open(struct vol *vv, const char *path, uint32_t disp, bool write, uint8_t attr,
                   struct fnode **out, bool *created)
{
    struct xvol *v = XV(vv);
    fs_lock();
    struct xdir dir;
    const char *name;
    static struct xent e;
    int rc = walk(v, path, &dir, &name);
    if (rc < 0)
        return done(v, rc);
    if (!*name)
        return done(v, -ERROR_ACCESS_DENIED);
    rc = dir_find(v, &dir.d, name, &e);
    bool exists = rc == 0;
    if (rc < 0 && rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc);
    if (exists && (e.attr & A_DIR))
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
    if (exists && (write || truncate) && (e.attr & A_RO))
        return done(v, -ERROR_ACCESS_DENIED);
    if (create) {
        uint8_t proto[64];
        memset(proto, 0, sizeof proto);
        put16(proto + 4, A_ARC | (attr & (A_RO | A_HID | A_SYS)));
        stamp_now(proto, true);
        proto[32 + 1] = F_ALLOC;
        int count;
        if ((rc = build_set(v, name, proto, e.raw, &count)) < 0)
            return done(v, rc);
        uint32_t at;
        if ((rc = set_add(v, &dir, e.raw, count, &at)) < 0)
            return done(v, rc);
        e.idx = at;
        e.count = count;
        decode(&e);
    }
    struct xnode *n = node_get(v, &dir.d, &e);
    if (!n)
        return done(v, -ERROR_TOO_MANY_OPEN_FILES);
    if (truncate) {
        if ((rc = set_size(n, 0)) < 0) {
            n->refs--;
            return done(v, rc);
        }
        n->attr = (uint8_t)((n->attr & ~(A_RO | A_HID | A_SYS)) | A_ARC | (attr & (A_HID | A_SYS)));
        rtc_dos_time(&n->date, &n->time);
        n->dirty = true;
        node_update(n);
    }
    *out = &n->h;
    if (created)
        *created = create;
    return done(v, 0);
}

static int ex_read(struct fnode *nn, uint32_t pos, void *buf, uint32_t len)
{
    struct xnode *n = XN(nn);
    fs_lock();
    if (pos >= n->size)
        return done(n->v, 0);
    if (len > n->size - pos)
        len = n->size - pos;
    uint32_t real = pos < n->vdl ? (n->vdl - pos < len ? n->vdl - pos : len) : 0;
    int rc = real ? node_io(n, pos, buf, real, false) : 0;
    if (rc < 0)
        return done(n->v, rc);
    if ((uint32_t)rc == real && real < len)                  /* past the valid length: zeros */
        memset((uint8_t *)buf + real, 0, len - real), rc = (int)len;
    return done(n->v, rc);
}

static int ex_write(struct fnode *nn, uint32_t pos, const void *buf, uint32_t len)
{
    struct xnode *n = XN(nn);
    struct xvol *v = n->v;
    fs_lock();
    if (len == 0)
        return done(v, 0);
    int rc = validate(n, pos);
    if (rc < 0) {
        node_update(n);
        return done(v, rc);
    }
    if ((uint64_t)pos + len > 0xFFFFFFFFu)
        len = 0xFFFFFFFFu - pos;
    rc = node_io(n, pos, (uint8_t *)buf, len, true);
    if (rc > 0) {
        uint32_t end = pos + (uint32_t)rc;
        if (end > n->size)
            n->size = end;
        if (end > n->vdl)
            n->vdl = end;
        rtc_dos_time(&n->date, &n->time);
        n->attr |= A_ARC;
        n->dirty = true;
    }
    if (rc == 0)
        rc = -ERROR_DISK_FULL;
    int urc = node_update(n);
    return done(v, rc > 0 && urc < 0 ? urc : rc);
}

static int ex_set_size(struct fnode *nn, uint32_t size)
{
    struct xnode *n = XN(nn);
    fs_lock();
    int rc = set_size(n, size);
    if (rc == 0) {
        rtc_dos_time(&n->date, &n->time);
        n->attr |= A_ARC;
    }
    int urc = node_update(n);
    return done(n->v, rc < 0 ? rc : urc);
}

static uint32_t ex_size(const struct fnode *n)  { return CXN(n)->size; }
static uint8_t ex_attr(const struct fnode *n)   { return CXN(n)->attr & 0x37; }
static void ex_name_of(const struct fnode *n, uint8_t fcb[11]) { memcpy(fcb, CXN(n)->fcb, 11); }

static void ex_get_time(const struct fnode *nn, uint16_t *date, uint16_t *time)
{
    *date = CXN(nn)->date;
    *time = CXN(nn)->time;
}

static int ex_set_time(struct fnode *nn, uint16_t date, uint16_t time)
{
    struct xnode *n = XN(nn);
    fs_lock();
    n->date = date;
    n->time = time;
    n->dirty = true;
    return done(n->v, node_update(n));
}

static int ex_flush(struct fnode *nn)
{
    struct xnode *n = XN(nn);
    fs_lock();
    n->v->changed = true;
    return done(n->v, node_update(n));
}

static void ex_dup(struct fnode *n)
{
    fs_lock();
    XN(n)->refs++;
    fs_unlock();
}

static void ex_close(struct fnode *nn)
{
    struct xnode *n = XN(nn);
    fs_lock();
    node_update(n);
    if (n->refs > 0)
        n->refs--;
    done(n->v, 0);
}

/* ---- names ------------------------------------------------------------------------ */

/* The entry for path, with the directory it is in. */
static int lookup(struct xvol *v, const char *path, struct xdir *dir, struct xent *e)
{
    const char *name;
    int rc = walk(v, path, dir, &name);
    if (rc == 0 && !*name)
        rc = -ERROR_ACCESS_DENIED;
    if (rc == 0)
        rc = dir_find(v, &dir->d, name, e);
    return rc;
}

static int ex_stat(struct vol *vv, const char *path, struct fs_entry *fe)
{
    struct xvol *v = XV(vv);
    fs_lock();
    if (!*path) {
        memset(fe, 0, sizeof *fe);
        fe->attr = A_DIR;
        return done(v, 0);
    }
    struct xdir dir;
    static struct xent e;
    int rc = lookup(v, path, &dir, &e);
    if (rc == 0)
        fill_entry(&e, fe);
    return done(v, rc);
}

static int ex_set_attr(struct vol *vv, const char *path, uint8_t attr)
{
    struct xvol *v = XV(vv);
    fs_lock();
    struct xdir dir;
    static struct xent e;
    int rc = lookup(v, path, &dir, &e);
    if (rc < 0)
        return done(v, rc);
    uint8_t a = (uint8_t)((e.attr & A_DIR) | (attr & (A_RO | A_HID | A_SYS | A_ARC)));
    put16(e.raw + 4, a);
    rc = set_write(v, &dir.d, e.idx, e.raw, e.count);
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v && nodes[i].parent.first == dir.d.first && nodes[i].idx == e.idx)
            nodes[i].attr = a;
    return done(v, rc);
}

static int ex_delete(struct vol *vv, const char *path)
{
    struct xvol *v = XV(vv);
    fs_lock();
    struct xdir dir;
    static struct xent e;
    int rc = lookup(v, path, &dir, &e);
    if (rc < 0)
        return done(v, rc);
    if (e.attr & (A_DIR | A_RO))
        return done(v, -ERROR_ACCESS_DENIED);
    if (node_open(v, &dir.d, e.idx))
        return done(v, -ERROR_SHARING_VIOLATION);
    uint32_t n = clus_ok(v, e.first) ? (e.size + v->cbytes - 1) / v->cbytes : 0;
    if ((rc = set_remove(v, &dir.d, &e)) == 0 && n)
        rc = free_clusters(v, e.first, e.flags & F_NOCHAIN, n);
    return done(v, rc);
}

static int ex_mkdir(struct vol *vv, const char *path)
{
    struct xvol *v = XV(vv);
    fs_lock();
    struct xdir dir;
    const char *name;
    static struct xent e;
    int rc = walk(v, path, &dir, &name);
    if (rc == 0 && !*name)
        rc = -ERROR_ACCESS_DENIED;
    if (rc < 0)
        return done(v, rc);
    rc = dir_find(v, &dir.d, name, &e);
    if (rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc < 0 ? rc : -ERROR_ACCESS_DENIED);
    uint32_t c;
    if ((rc = alloc_any(v, &c)) < 0)
        return done(v, rc);
    zero_clus(v, c);
    uint8_t proto[64];
    memset(proto, 0, sizeof proto);
    put16(proto + 4, A_DIR);
    stamp_now(proto, true);
    proto[32 + 1] = F_ALLOC | F_NOCHAIN;
    put32(proto + 32 + 8, v->cbytes);
    put32(proto + 32 + 20, c);
    put32(proto + 32 + 24, v->cbytes);
    int count;
    uint32_t at;
    if ((rc = build_set(v, name, proto, e.raw, &count)) < 0 || (rc = set_add(v, &dir, e.raw, count, &at)) < 0)
        bm_set(v, c, false);
    return done(v, rc);
}

static bool dir_empty(struct xvol *v, const struct dref *d)
{
    struct dcur dc;
    dcur_init(&dc, v, d);
    static struct xent e;
    uint32_t idx = 0;
    return dir_next(&dc, &idx, &e, NULL) == 0;
}

static int ex_rmdir(struct vol *vv, const char *path)
{
    struct xvol *v = XV(vv);
    fs_lock();
    struct xdir dir;
    static struct xent e;
    int rc = lookup(v, path, &dir, &e);
    if (rc < 0)
        return done(v, rc == -ERROR_FILE_NOT_FOUND ? -ERROR_PATH_NOT_FOUND : rc);
    if (!(e.attr & A_DIR))
        return done(v, -ERROR_PATH_NOT_FOUND);
    struct dref d = child_dref(v, &e);
    if (!dir_empty(v, &d))
        return done(v, -ERROR_DIR_NOT_EMPTY);
    uint32_t n = (e.size + v->cbytes - 1) / v->cbytes;
    if ((rc = set_remove(v, &dir.d, &e)) == 0 && clus_ok(v, e.first))
        rc = free_clusters(v, e.first, d.contig, n);
    return done(v, rc);
}

static int ex_rename(struct vol *vv, const char *from, const char *to)
{
    struct xvol *v = XV(vv);
    fs_lock();
    struct xdir fdir, tdir;
    const char *tname;
    static struct xent fe, te;
    int rc = lookup(v, from, &fdir, &fe);
    if (rc < 0)
        return done(v, rc);
    if ((rc = walk(v, to, &tdir, &tname)) < 0)
        return done(v, rc);
    if (!*tname)
        return done(v, -ERROR_ACCESS_DENIED);
    rc = dir_find(v, &tdir.d, tname, &te);
    bool self = rc == 0 && tdir.d.first == fdir.d.first && te.idx == fe.idx;
    if (rc == 0 && !self)
        return done(v, -ERROR_ACCESS_DENIED);
    if (rc < 0 && rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc);
    if (fe.attr & A_DIR) {                              /* not into itself: walk up from the target */
        struct xdir x;
        root_dir(v, &x);
        const char *s = to;
        while (*s) {
            const char *e = s;
            while (*e && *e != '\\')
                e++;
            if (!*e)
                break;
            char comp[FS_NAME_MAX + 1];
            uint32_t len = (uint32_t)(e - s);
            memcpy(comp, s, len);
            comp[len] = '\0';
            if (dir_find(v, &x.d, comp, &te) < 0)
                break;
            if (te.first == fe.first)
                return done(v, -ERROR_ACCESS_DENIED);
            x.d = child_dref(v, &te);
            s = e + 1;
        }
    }
    uint8_t raw[SET_MAX * 32];
    int count;
    if ((rc = build_set(v, tname, fe.raw, raw, &count)) < 0)
        return done(v, rc);
    if (self && (rc = set_remove(v, &fdir.d, &fe)) < 0)
        return done(v, rc);
    uint32_t at;
    if ((rc = set_add(v, &tdir, raw, count, &at)) < 0)
        return done(v, rc);
    if (!self && (rc = set_remove(v, &fdir.d, &fe)) < 0)
        return done(v, rc);
    uint8_t fcb[11];
    char alias[13];
    char clean[FS_NAME_MAX + 1];
    fs_clean_name(tname, clean);
    fs_make_alias(clean, le16(raw + 32 + 4), fcb, alias);
    for (int i = 0; i < NODE_MAX; i++) {
        struct xnode *n = &nodes[i];
        if (n->refs && n->v == v && n->parent.first == fdir.d.first && n->idx == fe.idx) {
            n->parent = tdir.d;
            n->idx = at;
            memcpy(n->fcb, fcb, 11);
        }
    }
    return done(v, 0);
}

static int ex_short_path(struct vol *vv, const char *path, char *out, uint32_t size)
{
    struct xvol *v = XV(vv);
    fs_lock();
    struct xdir x;
    root_dir(v, &x);
    uint32_t n = 0;
    out[0] = '\0';
    const char *s = path;
    int rc = 0;
    static struct xent e;
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
        if ((rc = dir_find(v, &x.d, comp, &e)) < 0)
            break;
        uint32_t al = (uint32_t)strlen(e.alias);
        if (n + al + 2 > size) {
            rc = -ERROR_FILENAME_EXCED_RANGE;
            break;
        }
        if (n)
            out[n++] = '\\';
        memcpy(out + n, e.alias, al + 1);
        n += al;
        x.d = child_dref(v, &e);
        s = *end ? end + 1 : end;
    }
    return done(v, rc);
}

/* ---- searches ----------------------------------------------------------------------- */

/* f->next: 0 ".", 1 "..", then 2 + the entry index. In the root, from 2. */
static int find_more(struct fs_find *f, struct fs_entry *out)
{
    struct xvol *v = XV(f->v);
    struct dref d = { f->dir & 0x7FFFFFFF, (f->dir & 0x80000000u) != 0, f->dirlen };
    while (f->next < 2) {
        const char *dots = f->next == 0 ? "." : "..";
        f->next++;
        uint8_t fcb[11];
        memset(fcb, ' ', 11);
        memcpy(fcb, dots, strlen(dots));
        bool ok = f->dos ? (f->attr & A_DIR) && fs_fcb_match(f->fcb, fcb)
                         : fs_wild(f->pattern, dots) || (f->pattern[0] == '*' && !f->pattern[1]);
        if (!ok)
            continue;
        memset(out, 0, sizeof *out);
        memcpy(out->name, dots, strlen(dots) + 1);
        memcpy(out->short_name, dots, strlen(dots) + 1);
        memcpy(out->fcb_name, fcb, 11);
        out->attr = A_DIR;
        return 0;
    }
    struct dcur dc;
    dcur_init(&dc, v, &d);
    static struct xent e;
    for (;;) {
        uint32_t idx = f->next - 2;
        int rc = dir_next(&dc, &idx, &e, NULL);
        f->next = idx + 2;
        if (rc <= 0)
            return rc < 0 ? rc : -ERROR_NO_MORE_FILES;
        uint8_t a = e.attr & 0x37;
        if (f->dos) {
            if (f->attr == A_VOL || (a & (A_HID | A_SYS | A_DIR) & ~f->attr) || !fs_fcb_match(f->fcb, e.fcb))
                continue;
        } else {
            const char *p = f->pattern;
            bool all = (p[0] == '*' && !p[1]) || (p[0] == '*' && p[1] == '.' && p[2] == '*' && !p[3]);
            if (!all && !fs_wild(p, e.name) && !fs_wild(p, e.alias))
                continue;
        }
        fill_entry(&e, out);
        return 0;
    }
}

static int find_dir(struct xvol *v, const char *dir, struct fs_find *f)
{
    if (!*dir) {
        f->dir = v->root;
        f->dirlen = 0;
        f->next = 2;
        return 0;
    }
    struct xdir x;
    static struct xent e;
    int rc = lookup(v, dir, &x, &e);
    if (rc < 0 || !(e.attr & A_DIR))
        return -ERROR_PATH_NOT_FOUND;
    struct dref d = child_dref(v, &e);
    f->dir = d.first | (d.contig ? 0x80000000u : 0);
    f->dirlen = d.nclus;
    f->next = 0;
    return 0;
}

static int ex_find_first(struct vol *vv, const char *dir, const char *pattern, struct fs_find *f,
                         struct fs_entry *e)
{
    struct xvol *v = XV(vv);
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

static int ex_find_dos(struct vol *vv, const char *dir, const uint8_t fcb[11], uint8_t attr,
                       struct fs_find *f, struct fs_entry *e)
{
    struct xvol *v = XV(vv);
    fs_lock();
    memset(f, 0, sizeof *f);
    f->v = vv;
    f->dos = true;
    f->attr = attr;
    memcpy(f->fcb, fcb, 11);
    int rc = find_dir(v, dir, f);
    if (rc == 0 && attr == A_VOL) {                     /* the label, from the root */
        if (*dir || !v->h.label[0])
            return done(v, -ERROR_FILE_NOT_FOUND);
        memset(e, 0, sizeof *e);
        memset(e->fcb_name, ' ', 11);
        for (int i = 0; i < 11 && v->h.label[i]; i++)
            e->fcb_name[i] = (uint8_t)fs_up(v->h.label[i]);
        memcpy(e->name, v->h.label, strlen(v->h.label) + 1);
        e->attr = A_VOL;
        f->next = 0xFFFF;                               /* next: the end */
        f->dir = 0;
        return done(v, 0);
    }
    if (rc == 0)
        rc = find_more(f, e);
    return done(v, rc == -ERROR_NO_MORE_FILES ? -ERROR_FILE_NOT_FOUND : rc);
}

static int ex_find_next(struct fs_find *f, struct fs_entry *e)
{
    struct xvol *v = XV(f->v);
    fs_lock();
    if (f->dos && f->attr == A_VOL)
        return done(v, -ERROR_NO_MORE_FILES);
    return done(v, find_more(f, e));
}

static int ex_space(struct vol *vv, uint32_t *spc, uint32_t *bps, uint32_t *freec, uint32_t *total)
{
    struct xvol *v = XV(vv);
    fs_lock();
    *spc = v->spc;
    *bps = SECTOR;
    *freec = v->free;
    *total = v->nclus;
    return done(v, 0);
}

static void ex_sync(struct vol *vv)
{
    struct xvol *v = XV(vv);
    fs_lock();
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v)
            node_update(&nodes[i]);
    v->changed = true;
    done(v, 0);
}

static const struct fs_ops exfat_ops = {
    .open = ex_open, .read = ex_read, .write = ex_write, .set_size = ex_set_size,
    .size = ex_size, .get_time = ex_get_time, .set_time = ex_set_time, .attr = ex_attr,
    .name_of = ex_name_of, .flush = ex_flush, .close = ex_close, .dup = ex_dup,
    .stat = ex_stat, .set_attr = ex_set_attr, .del = ex_delete, .mkdir = ex_mkdir,
    .rmdir = ex_rmdir, .rename = ex_rename, .short_path = ex_short_path,
    .find_first = ex_find_first, .find_dos = ex_find_dos, .find_next = ex_find_next,
    .space = ex_space, .sync = ex_sync,
};
