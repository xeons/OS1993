/*
 * fat.c -- FAT12, FAT16 and FAT32, with VFAT long names.
 *
 * A volume is a partition with a BPB; the cluster count decides the FAT
 * type, as Microsoft's specification says. Directories are walked entry
 * by entry, long names put back together from their LFN entries (checked
 * by sequence and checksum; an orphan is ignored, as DOS ignores it). A
 * new name that isn't a plain 8.3 one gets LFN entries and a unique
 * "BASIS~N.EXT" alias; one that is 8.3 in a single case per part gets the
 * NT case bits instead, as Windows does.
 *
 * Open files are nodes, one per directory entry however often it is open,
 * so every handle sees one size. A node remembers the last cluster it
 * walked to, so sequential access doesn't walk the chain from the start.
 * Whole clusters move straight between the disk and the caller; partial
 * ones go through the cache.
 *
 * Every public call takes the lock and, if it changed anything, ends with
 * the FAT32 FSInfo sector and cache_sync(): between calls the disk is
 * consistent, and what a call wrote is on the disk when it returns.
 *
 * The root of a volume is directory cluster 0 throughout (as ".." says),
 * whether it is FAT12/16's fixed root or FAT32's root cluster chain.
 */
#include "kernel.h"
#include "disk.h"
#include "fs.h"
#include "user.h"

#define A_RO   0x01
#define A_HID  0x02
#define A_SYS  0x04
#define A_VOL  0x08
#define A_DIR  0x10
#define A_ARC  0x20
#define A_LFN  0x0F

#define EOC      0x0FFFFFFFu
#define NODE_MAX 64
#define LFN_MAX  20                     /* 20 x 13 >= 255 */

struct vol {
    bool     used;
    char     letter;
    struct disk *disk;
    uint32_t start, count;              /* the partition */
    int      type;                      /* 12, 16, 32 */
    uint32_t spc, cbytes, epc;          /* sectors, bytes, entries per cluster */
    uint32_t fat_lba, fat_secs, nfats, active;
    bool     mirror;
    uint32_t root_lba, root_ents, root_clus;
    uint32_t data_lba, nclus;           /* clusters 2 .. nclus + 1 */
    uint32_t eoc_min;
    uint32_t free, next_free;
    bool     free_known;
    uint32_t fsinfo;                    /* absolute LBA, 0: none */
    bool     fsinfo_dirty, changed;
    uint32_t serial;
    char     label[12];
};

struct fnode {
    int      refs;
    struct vol *v;
    uint32_t dir, idx, first;           /* its entries: LFN from first, the 8.3 one at idx */
    uint32_t clus, size;
    uint8_t  attr;
    uint16_t date, time;
    uint8_t  fcb[11];
    bool     dirty;                     /* the directory entry needs writing */
    uint32_t c_idx, c_clus;             /* cluster c_idx of the file is c_clus (0: none) */
};

static struct vol vols[VOL_MAX];
static int nvols;
static struct fnode nodes[NODE_MAX];

/* ---- little helpers ------------------------------------------------------ */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static bool name_eq(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b))
        a++, b++;
    return !*a && !*b;
}

static uint32_t clus_lba(const struct vol *v, uint32_t c) { return v->data_lba + (c - 2) * v->spc; }
static bool clus_ok(const struct vol *v, uint32_t c)      { return c >= 2 && c <= v->nclus + 1; }

static uint32_t ent_clus(const struct vol *v, const uint8_t *e)
{
    return le16(e + 26) | (v->type == 32 ? (uint32_t)le16(e + 20) << 16 : 0);
}

static void set_ent_clus(const struct vol *v, uint8_t *e, uint32_t c)
{
    put16(e + 26, c);
    put16(e + 20, v->type == 32 ? c >> 16 : 0);
}

/* ---- the FAT --------------------------------------------------------------- */

/* One byte of FAT copy `copy` at byte offset off (FAT12 entries straddle). */
static int fat_byte(struct vol *v, uint32_t copy, uint32_t off, uint8_t **p, struct buf **b)
{
    *b = bread(v->disk, v->fat_lba + copy * v->fat_secs + off / SECTOR);
    if (!*b)
        return -ERROR_GEN_FAILURE;
    *p = (*b)->data + off % SECTOR;
    return 0;
}

static int fat_get(struct vol *v, uint32_t c, uint32_t *next)
{
    struct buf *b, *b2;
    uint8_t *p, *p2;
    if (!clus_ok(v, c))
        return -ERROR_GEN_FAILURE;
    uint32_t x;
    if (v->type == 12) {
        uint32_t off = c + c / 2;
        if (fat_byte(v, v->active, off, &p, &b) < 0)
            return -ERROR_GEN_FAILURE;
        uint8_t lo = *p;
        brelse(b);
        if (fat_byte(v, v->active, off + 1, &p2, &b2) < 0)
            return -ERROR_GEN_FAILURE;
        uint16_t w = (uint16_t)(lo | *p2 << 8);
        brelse(b2);
        x = (c & 1) ? w >> 4 : w & 0xFFF;
    } else {
        uint32_t off = c * (v->type == 16 ? 2 : 4);
        if (fat_byte(v, v->active, off, &p, &b) < 0)
            return -ERROR_GEN_FAILURE;
        x = v->type == 16 ? le16(p) : le32(p) & 0x0FFFFFFF;
        brelse(b);
    }
    *next = x >= v->eoc_min - 1 ? EOC : x;              /* bad clusters end a chain too */
    return 0;
}

static int fat_set(struct vol *v, uint32_t c, uint32_t val)
{
    struct buf *b, *b2;
    uint8_t *p, *p2;
    if (!clus_ok(v, c))
        return -ERROR_GEN_FAILURE;
    for (uint32_t copy = 0; copy < v->nfats; copy++) {
        if (!v->mirror && copy != v->active)
            continue;
        if (v->type == 12) {
            uint32_t off = c + c / 2;
            uint16_t x = (uint16_t)(val & 0xFFF);
            if (fat_byte(v, copy, off, &p, &b) < 0)
                return -ERROR_GEN_FAILURE;
            if (c & 1) *p = (uint8_t)((*p & 0x0F) | (x << 4));
            else       *p = (uint8_t)x;
            bdirty(b);
            brelse(b);
            if (fat_byte(v, copy, off + 1, &p2, &b2) < 0)
                return -ERROR_GEN_FAILURE;
            if (c & 1) *p2 = (uint8_t)(x >> 4);
            else       *p2 = (uint8_t)((*p2 & 0xF0) | (x >> 8));
            bdirty(b2);
            brelse(b2);
        } else {
            uint32_t off = c * (v->type == 16 ? 2 : 4);
            if (fat_byte(v, copy, off, &p, &b) < 0)
                return -ERROR_GEN_FAILURE;
            if (v->type == 16)
                put16(p, val);
            else
                put32(p, (le32(p) & 0xF0000000u) | (val & 0x0FFFFFFF));
            bdirty(b);
            brelse(b);
        }
    }
    v->changed = true;
    return 0;
}

static int count_free(struct vol *v)
{
    uint32_t n = 0, x;
    for (uint32_t c = 2; c <= v->nclus + 1; c++) {
        if (fat_get(v, c, &x) < 0)
            return -ERROR_GEN_FAILURE;
        n += x == 0;
    }
    v->free = n;
    v->free_known = true;
    return 0;
}

/* A free cluster, marked as a chain's end and linked after prev (if any). */
static int alloc_clus(struct vol *v, uint32_t prev, uint32_t *out)
{
    if (v->free_known && v->free == 0)
        return -ERROR_DISK_FULL;
    uint32_t c = clus_ok(v, v->next_free) ? v->next_free : 2, x;
    for (uint32_t n = 0; n < v->nclus; n++, c = c >= v->nclus + 1 ? 2 : c + 1) {
        if (fat_get(v, c, &x) < 0)
            return -ERROR_GEN_FAILURE;
        if (x != 0)
            continue;
        if (fat_set(v, c, EOC) < 0 || (prev && fat_set(v, prev, c) < 0))
            return -ERROR_GEN_FAILURE;
        v->next_free = c + 1;
        if (v->free_known)
            v->free--;
        v->fsinfo_dirty = true;
        *out = c;
        return 0;
    }
    v->free = 0;
    v->free_known = true;
    return -ERROR_DISK_FULL;
}

static int free_chain(struct vol *v, uint32_t c)
{
    for (uint32_t n = 0; clus_ok(v, c) && n <= v->nclus; n++) {
        uint32_t next;
        if (fat_get(v, c, &next) < 0 || fat_set(v, c, 0) < 0)
            return -ERROR_GEN_FAILURE;
        if (v->free_known)
            v->free++;
        v->fsinfo_dirty = true;
        c = next;
    }
    return 0;
}

static int zero_clus(struct vol *v, uint32_t c)
{
    for (uint32_t s = 0; s < v->spc; s++) {
        struct buf *b = bzero_get(v->disk, clus_lba(v, c) + s);
        bdirty(b);
        brelse(b);
    }
    return 0;
}

/* ---- directories ------------------------------------------------------------ */

struct dcur {                           /* a directory being walked */
    struct vol *v;
    uint32_t dir;                       /* 0: the root */
    uint32_t c_idx, c_clus;             /* its cluster c_idx is c_clus (0: none yet) */
};

static void dcur_init(struct dcur *d, struct vol *v, uint32_t dir)
{
    *d = (struct dcur){ v, dir, 0, 0 };
}

static bool fixed_root(const struct dcur *d) { return d->dir == 0 && d->v->type != 32; }

/* The sector holding entry idx. 0, 1 past the end, or <0. With grow, a
 * cluster chain is extended to reach it (with zeroed clusters). */
static int dir_sector(struct dcur *d, uint32_t idx, bool grow, uint32_t *lba)
{
    struct vol *v = d->v;
    if (fixed_root(d)) {
        if (idx >= v->root_ents)
            return 1;
        *lba = v->root_lba + idx / 16;
        return 0;
    }
    uint32_t want = idx / v->epc;
    if (!d->c_clus || d->c_idx > want) {
        d->c_idx = 0;
        d->c_clus = d->dir ? d->dir : v->root_clus;
    }
    while (d->c_idx < want) {
        uint32_t next;
        if (fat_get(v, d->c_clus, &next) < 0)
            return -ERROR_GEN_FAILURE;
        if (next == EOC) {
            if (!grow)
                return 1;
            int rc = alloc_clus(v, d->c_clus, &next);
            if (rc < 0)
                return rc;
            zero_clus(v, next);
        } else if (!clus_ok(v, next)) {
            return -ERROR_GEN_FAILURE;
        }
        d->c_clus = next;
        d->c_idx++;
    }
    *lba = clus_lba(v, d->c_clus) + (idx % v->epc) / 16;
    return 0;
}

static int dir_read(struct dcur *d, uint32_t idx, uint8_t ent[32])
{
    uint32_t lba;
    int rc = dir_sector(d, idx, false, &lba);
    if (rc)
        return rc;
    struct buf *b = bread(d->v->disk, lba);
    if (!b)
        return -ERROR_GEN_FAILURE;
    memcpy(ent, b->data + (idx % 16) * 32, 32);
    brelse(b);
    return 0;
}

static int dir_write(struct dcur *d, uint32_t idx, const uint8_t ent[32])
{
    uint32_t lba;
    int rc = dir_sector(d, idx, true, &lba);
    if (rc)
        return rc > 0 ? -ERROR_CANNOT_MAKE : rc;
    struct buf *b = bread(d->v->disk, lba);
    if (!b)
        return -ERROR_GEN_FAILURE;
    memcpy(b->data + (idx % 16) * 32, ent, 32);
    bdirty(b);
    brelse(b);
    d->v->changed = true;
    return 0;
}

struct dent {                           /* an entry found */
    uint8_t  raw[32];                   /* the 8.3 entry */
    uint32_t idx, first;                /* where it is; its LFN entries start at first */
    bool     lfn;
    char     lname[FS_NAME_MAX + 1];
};

static uint8_t lfn_sum(const uint8_t *fcb)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + fcb[i]);
    return s;
}

static const uint8_t lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* The next entry from *idx (volume labels too, not deleted ones or LFN
 * pieces); *idx moves past it. 1 found, 0 the end, <0 error. */
static int dir_next(struct dcur *d, uint32_t *idx, struct dent *e)
{
    uint16_t ucs[LFN_MAX * 13 + 1];
    int expect = 0;                     /* the LFN sequence number due next */
    bool whole = false;
    uint8_t sum = 0;
    uint32_t first = 0;
    for (;; (*idx)++) {
        uint8_t ent[32];
        int rc = dir_read(d, *idx, ent);
        if (rc)
            return rc < 0 ? rc : 0;
        if (ent[0] == 0)
            return 0;
        if (ent[0] == 0xE5) {
            expect = 0;
            whole = false;
            continue;
        }
        if ((ent[11] & 0x3F) == A_LFN) {
            int seq = ent[0] & 0x1F;
            if (ent[0] & 0x40) {
                if (seq < 1 || seq > LFN_MAX) {
                    expect = 0;
                    continue;
                }
                memset(ucs, 0, sizeof ucs);
                expect = seq;
                sum = ent[13];
                first = *idx;
                whole = false;
            }
            if (!expect || seq != expect || ent[13] != sum) {
                expect = 0;
                whole = false;
                continue;
            }
            for (int k = 0; k < 13; k++)
                ucs[(seq - 1) * 13 + k] = le16(ent + lfn_pos[k]);
            expect--;
            whole = expect == 0;
            continue;
        }
        memcpy(e->raw, ent, 32);
        e->idx = *idx;
        e->lfn = whole && lfn_sum(ent) == sum;
        e->first = e->lfn ? first : *idx;
        if (e->lfn) {
            int n = 0;
            for (int k = 0; k < LFN_MAX * 13 && ucs[k] && ucs[k] != 0xFFFF && n < FS_NAME_MAX; k++)
                e->lname[n++] = ucs[k] < 0x100 ? (char)ucs[k] : '_';
            e->lname[n] = '\0';
            if (!n)
                e->lfn = false;
        }
        (*idx)++;
        return 1;
    }
}

/* "NAME.EXT" from an 8.3 entry; with case, the NT lower-case bits applied. */
static void short_display(const uint8_t *raw, char *out, bool use_case)
{
    int n = 0;
    bool lb = use_case && (raw[12] & 0x08), le = use_case && (raw[12] & 0x10);
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)(i == 0 && raw[0] == 0x05 ? 0xE5 : raw[i]);
        out[n++] = lb && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = le && raw[i] >= 'A' && raw[i] <= 'Z' ? (char)(raw[i] + 32) : (char)raw[i];
    }
    out[n] = '\0';
}

static void fill_entry(const struct vol *v, const struct dent *d, struct fs_entry *e)
{
    const uint8_t *r = d->raw;
    short_display(r, e->short_name, false);
    if (d->lfn)
        memcpy(e->name, d->lname, strlen(d->lname) + 1);
    else
        short_display(r, e->name, true);
    memcpy(e->fcb_name, r, 11);
    if (e->fcb_name[0] == 0x05)
        e->fcb_name[0] = 0xE5;
    e->attr = r[11];
    e->size = (r[11] & A_DIR) ? 0 : le32(r + 28);
    e->time = le16(r + 22);
    e->date = le16(r + 24);
    e->ctime = le16(r + 14);
    e->cdate = le16(r + 16);
    e->adate = le16(r + 18);
    e->cluster = ent_clus(v, r);
    e->lfn = d->lfn;
}

/* The entry called name (long or short, any case) in a directory. */
static int dir_find(struct vol *v, uint32_t dir, const char *name, struct dent *e)
{
    struct dcur d;
    dcur_init(&d, v, dir);
    uint32_t idx = 0;
    char sname[13];
    for (;;) {
        int rc = dir_next(&d, &idx, e);
        if (rc <= 0)
            return rc < 0 ? rc : -ERROR_FILE_NOT_FOUND;
        if (e->raw[11] & A_VOL)
            continue;
        short_display(e->raw, sname, false);
        if ((e->lfn && name_eq(name, e->lname)) || name_eq(name, sname))
            return 0;
    }
}

/* Walk to the directory holding the path's last component. */
static int walk(struct vol *v, const char *path, uint32_t *dir, const char **last)
{
    uint32_t c = 0;
    const char *s = path;
    for (;;) {
        const char *e = s;
        while (*e && *e != '\\')
            e++;
        if (!*e) {
            *dir = c;
            *last = s;
            return 0;
        }
        char comp[FS_NAME_MAX + 1];
        uint32_t len = (uint32_t)(e - s);
        if (len == 0 || len > FS_NAME_MAX)
            return -ERROR_PATH_NOT_FOUND;
        memcpy(comp, s, len);
        comp[len] = '\0';
        struct dent de;
        int rc = dir_find(v, c, comp, &de);
        if (rc < 0)
            return rc == -ERROR_FILE_NOT_FOUND ? -ERROR_PATH_NOT_FOUND : rc;
        if (!(de.raw[11] & A_DIR))
            return -ERROR_PATH_NOT_FOUND;
        c = ent_clus(v, de.raw);                        /* ".." to the root: 0 */
        s = e + 1;
    }
}

/* ---- names ------------------------------------------------------------------ */

static bool short_char(uint8_t c)
{
    if (c >= 0x80 || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    for (const char *p = "$%'-_@~`!(){}^#&"; *p; p++)
        if (c == (uint8_t)*p)
            return true;
    return false;
}

static bool long_char(uint8_t c)
{
    if (c < 32)
        return false;
    for (const char *p = "\"*/:<>?\\|"; *p; p++)
        if (c == (uint8_t)*p)
            return false;
    return true;
}

/* A name as it will be stored: trailing dots and spaces go (as in Win32).
 * -> its length, or <0 if it can't be a name. */
static int clean_name(const char *in, char *out)
{
    size_t n = strlen(in);
    while (n > 0 && (in[n - 1] == '.' || in[n - 1] == ' '))
        n--;
    if (n == 0 || n > FS_NAME_MAX)
        return n ? -ERROR_FILENAME_EXCED_RANGE : -ERROR_INVALID_NAME;
    for (size_t i = 0; i < n; i++)
        if (!long_char((uint8_t)in[i]))
            return -ERROR_INVALID_NAME;
    memcpy(out, in, n);
    out[n] = '\0';
    return (int)n;
}

/* Does the name fit 8.3 as it is (with *ntcase for an all-lower-case part)?
 * Otherwise fcb gets the basis of an alias and *lossy says whether
 * anything was dropped or replaced. */
static bool fits_83(const char *name, uint8_t fcb[11], uint8_t *ntcase, bool *lossy)
{
    memset(fcb, ' ', 11);
    *ntcase = 0;
    *lossy = false;
    const char *dot = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            dot = p;
    bool fits = name[0] != '.';
    int dots = 0;
    for (const char *p = name; *p; p++)
        dots += *p == '.';
    if (dots > 1)
        fits = false;
    /* the basis: upper case, no spaces or extra dots, others replaced */
    int n = 0;
    bool lower[2] = { false, false }, upper[2] = { false, false };
    for (const char *p = name; *p && (!dot || p < dot); p++) {
        uint8_t c = (uint8_t)*p;
        if (c == ' ' || c == '.') {
            *lossy = true;
            fits = false;
            continue;
        }
        lower[0] |= c >= 'a' && c <= 'z';
        upper[0] |= c >= 'A' && c <= 'Z';
        c = (uint8_t)up((char)c);
        if (!short_char(c)) {
            c = '_';
            *lossy = true;
            fits = false;
        }
        if (n < 8)
            fcb[n++] = c;
        else {
            *lossy = true;
            fits = false;
        }
    }
    if (n == 0)
        fits = false;
    if (dot) {
        int m = 0;
        for (const char *p = dot + 1; *p; p++) {
            uint8_t c = (uint8_t)*p;
            if (c == ' ') {
                *lossy = true;
                fits = false;
                continue;
            }
            lower[1] |= c >= 'a' && c <= 'z';
            upper[1] |= c >= 'A' && c <= 'Z';
            c = (uint8_t)up((char)c);
            if (!short_char(c)) {
                c = '_';
                *lossy = true;
                fits = false;
            }
            if (m < 3)
                fcb[8 + m++] = c;
            else {
                *lossy = true;
                fits = false;
            }
        }
    }
    if (fcb[0] == 0xE5)
        fcb[0] = 0x05;
    for (int i = 0; i < 2; i++)
        if (lower[i] && upper[i])
            fits = false;                               /* mixed case: keep it in a long name */
    if (fits)
        *ntcase = (uint8_t)((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
    return fits;
}

static bool alias_taken(struct vol *v, uint32_t dir, const uint8_t fcb[11])
{
    struct dcur d;
    dcur_init(&d, v, dir);
    struct dent e;
    uint32_t idx = 0;
    while (dir_next(&d, &idx, &e) > 0)
        if (memcmp(e.raw, fcb, 11) == 0)
            return true;
    return false;
}

/* Add entries for name in dir, from proto (attributes, cluster, size,
 * times); the 8.3 name and case bits are made here. */
static int dir_add(struct vol *v, uint32_t dir, const char *in, const uint8_t proto[32], struct dent *out)
{
    char name[FS_NAME_MAX + 1];
    int len = clean_name(in, name);
    if (len < 0)
        return len;
    uint8_t fcb[11], ntcase;
    bool lossy;
    bool plain = fits_83(name, fcb, &ntcase, &lossy);
    int nlfn = 0;
    if (!plain) {
        nlfn = (len + 12) / 13;
        if (lossy || alias_taken(v, dir, fcb)) {
            uint8_t basis[11];
            memcpy(basis, fcb, 11);
            int blen = 0;
            while (blen < 8 && basis[blen] != ' ')
                blen++;
            bool found = false;
            for (uint32_t k = 1; k < 100000 && !found; k++) {
                char tail[8];
                int tl = ksnprintf(tail, sizeof tail, "~%u", k);
                int keep = blen < 8 - tl ? blen : 8 - tl;
                memcpy(fcb, basis, 11);
                memset(fcb + keep, ' ', (size_t)(8 - keep));
                memcpy(fcb + keep, tail, (size_t)tl);
                found = !alias_taken(v, dir, fcb);
            }
            if (!found)
                return -ERROR_CANNOT_MAKE;
        }
    } else if (alias_taken(v, dir, fcb)) {
        return -ERROR_FILE_EXISTS;
    }

    /* room: nlfn + 1 free entries in a row */
    struct dcur d;
    dcur_init(&d, v, dir);
    uint32_t need = (uint32_t)nlfn + 1, run = 0, idx;
    bool at_end = false;
    for (idx = 0;; idx++) {
        uint8_t ent[32];
        int rc = at_end ? 1 : dir_read(&d, idx, ent);
        if (rc < 0)
            return rc;
        if (rc == 1) {                                  /* after the end marker, or past the last cluster */
            if (fixed_root(&d) && idx >= v->root_ents)
                return -ERROR_CANNOT_MAKE;
            at_end = true;
            run++;                                      /* dir_write grows a chain */
        } else if (ent[0] == 0x00) {
            at_end = true;                              /* everything after is free */
            run++;
        } else {
            run = ent[0] == 0xE5 ? run + 1 : 0;
        }
        if (run == need)
            break;
        if (idx > 65535)
            return -ERROR_CANNOT_MAKE;
    }
    uint32_t start = idx + 1 - need;
    if (fixed_root(&d) && idx >= v->root_ents)
        return -ERROR_CANNOT_MAKE;

    uint8_t sum = lfn_sum(fcb);
    for (int k = nlfn; k >= 1; k--) {
        uint8_t ent[32];
        memset(ent, 0, 32);
        ent[0] = (uint8_t)(k | (k == nlfn ? 0x40 : 0));
        ent[11] = A_LFN;
        ent[13] = sum;
        for (int j = 0; j < 13; j++) {
            int ci = (k - 1) * 13 + j;
            uint16_t ch = ci < len ? (uint8_t)name[ci] : ci == len ? 0x0000 : 0xFFFF;
            put16(ent + lfn_pos[j], ch);
        }
        int rc = dir_write(&d, start + (uint32_t)(nlfn - k), ent);
        if (rc < 0)
            return rc;
    }
    uint8_t ent[32];
    memcpy(ent, proto, 32);
    memcpy(ent, fcb, 11);
    ent[12] = ntcase;
    int rc = dir_write(&d, start + (uint32_t)nlfn, ent);
    if (rc < 0)
        return rc;
    if (out) {
        memcpy(out->raw, ent, 32);
        out->idx = start + (uint32_t)nlfn;
        out->first = start;
        out->lfn = nlfn > 0;
        memcpy(out->lname, name, (size_t)len + 1);
    }
    return 0;
}

static int dir_remove(struct vol *v, uint32_t dir, const struct dent *e)
{
    struct dcur d;
    dcur_init(&d, v, dir);
    for (uint32_t i = e->first; i <= e->idx; i++) {
        uint8_t ent[32];
        int rc = dir_read(&d, i, ent);
        if (rc)
            return rc < 0 ? rc : -ERROR_GEN_FAILURE;
        ent[0] = 0xE5;
        if ((rc = dir_write(&d, i, ent)) < 0)
            return rc;
    }
    return 0;
}

static void stamp(uint8_t *ent, bool created)
{
    uint16_t date, time;
    rtc_dos_time(&date, &time);
    put16(ent + 22, time);
    put16(ent + 24, date);
    put16(ent + 18, date);
    if (created) {
        ent[13] = 0;
        put16(ent + 14, time);
        put16(ent + 16, date);
    }
}

/* ---- the lock ------------------------------------------------------------------ */

#include "io.h"
#include "sched.h"

static struct thread *owner;
static int depth;

void fs_lock(void)
{
    for (;;) {
        uint32_t f = irq_save();
        if (!owner || owner == current) {
            owner = current;
            depth++;
            irq_restore(f);
            return;
        }
        irq_restore(f);
        sched_yield();
    }
}

void fs_unlock(void)
{
    uint32_t f = irq_save();
    if (owner == current && --depth == 0)
        owner = NULL;
    irq_restore(f);
}

/* The end of a public call: what changed goes to the disk. */
static int done(struct vol *v, int rc)
{
    if (v && v->fsinfo && v->fsinfo_dirty) {
        struct buf *b = bread(v->disk, v->fsinfo);
        if (b) {
            if (le32(b->data) == 0x41615252 && le32(b->data + 484) == 0x61417272) {
                put32(b->data + 488, v->free_known ? v->free : 0xFFFFFFFF);
                put32(b->data + 492, v->next_free);
                bdirty(b);
            }
            brelse(b);
        }
        v->fsinfo_dirty = false;
    }
    if (v && v->changed) {
        v->changed = false;
        if (cache_sync() < 0 && rc >= 0)
            rc = -ERROR_GEN_FAILURE;
    }
    fs_unlock();
    return rc;
}

/* ---- mounting --------------------------------------------------------------------- */

struct vol *fat_mount(struct partition *p)
{
    if (nvols == VOL_MAX)
        return NULL;
    struct buf *b = bread(p->disk, p->start);
    if (!b)
        return NULL;
    uint8_t bs[SECTOR];
    memcpy(bs, b->data, SECTOR);
    brelse(b);
    uint32_t bps = le16(bs + 11), spc = bs[13], rsvd = le16(bs + 14), nfats = bs[16];
    uint32_t root_ents = le16(bs + 17), tot = le16(bs + 19), fatsz = le16(bs + 22);
    if (!tot)
        tot = le32(bs + 32);
    if (!fatsz)
        fatsz = le32(bs + 36);
    if (bps != SECTOR || !spc || (spc & (spc - 1)) || !rsvd || !nfats || nfats > 4 || !fatsz ||
        tot > p->count || bs[510] != 0x55 || bs[511] != 0xAA)
        return NULL;
    uint32_t root_secs = (root_ents * 32 + SECTOR - 1) / SECTOR;
    uint32_t meta = rsvd + nfats * fatsz + root_secs;
    if (meta >= tot)
        return NULL;
    uint32_t nclus = (tot - meta) / spc;

    struct vol *v = &vols[nvols];
    *v = (struct vol){ .used = true, .disk = p->disk, .start = p->start, .count = tot };
    v->type = nclus < 4085 ? 12 : nclus < 65525 ? 16 : 32;
    v->spc = spc;
    v->cbytes = spc * SECTOR;
    v->epc = v->cbytes / 32;
    v->fat_lba = p->start + rsvd;
    v->fat_secs = fatsz;
    v->nfats = nfats;
    v->mirror = true;
    v->root_lba = v->fat_lba + nfats * fatsz;
    v->root_ents = root_ents;
    v->data_lba = v->root_lba + root_secs;
    v->nclus = nclus;
    v->eoc_min = v->type == 12 ? 0xFF8 : v->type == 16 ? 0xFFF8 : 0x0FFFFFF8;
    v->next_free = 2;
    /* the FAT must hold an entry for every cluster */
    uint32_t need = v->type == 12 ? (nclus + 2) * 3 / 2 : (nclus + 2) * (uint32_t)(v->type / 8);
    if (fatsz * SECTOR < need)
        return NULL;
    const uint8_t *ext = bs + 36;                       /* FAT12/16's extended BPB */
    if (v->type == 32) {
        if (root_ents)
            return NULL;
        uint16_t flags = le16(bs + 40);
        v->mirror = !(flags & 0x80);
        v->active = v->mirror ? 0 : (flags & 0x0F) % nfats;
        v->root_clus = le32(bs + 44);
        if (!clus_ok(v, v->root_clus))
            return NULL;
        uint16_t fsi = le16(bs + 48);
        if (fsi && fsi < rsvd) {
            v->fsinfo = p->start + fsi;
            struct buf *f = bread(v->disk, v->fsinfo);
            if (f && le32(f->data) == 0x41615252 && le32(f->data + 484) == 0x61417272) {
                uint32_t fc = le32(f->data + 488), nf = le32(f->data + 492);
                if (fc <= nclus) {
                    v->free = fc;
                    v->free_known = true;
                }
                if (clus_ok(v, nf))
                    v->next_free = nf;
            } else {
                v->fsinfo = 0;
            }
            brelse(f);
        }
        ext = bs + 64;
    } else if (!root_ents) {
        return NULL;
    }
    if (ext[2] == 0x29) {
        v->serial = le32(ext + 3);
        memcpy(v->label, ext + 7, 11);
    } else {
        memcpy(v->label, "NO NAME    ", 11);
    }
    /* the root's volume label entry wins over the boot sector's */
    struct dcur d;
    dcur_init(&d, v, 0);
    struct dent e;
    uint32_t idx = 0;
    while (dir_next(&d, &idx, &e) > 0)
        if ((e.raw[11] & (A_VOL | A_DIR)) == A_VOL && e.raw[11] != A_LFN) {
            memcpy(v->label, e.raw, 11);
            break;
        }
    int n = 11;
    while (n > 0 && v->label[n - 1] == ' ')
        n--;
    v->label[n] = '\0';
    nvols++;
    klog("FS: FAT%d on disk %d at %u: %u clusters of %u bytes, serial %04X-%04X, label \"%s\"\n",
         v->type, (int)(p->disk - disks), p->start, nclus, v->cbytes, v->serial >> 16,
         v->serial & 0xFFFF, v->label);
    return v;
}

int fs_volume_count(void)         { return nvols; }
struct vol *fs_volume_at(int i)   { return i >= 0 && i < nvols ? &vols[i] : NULL; }
char fs_letter(const struct vol *v)      { return v->letter; }
void fs_set_letter(struct vol *v, char c) { v->letter = c; }
uint32_t fs_serial(const struct vol *v)  { return v->serial; }
const char *fs_label(const struct vol *v) { return v->label; }
const char *fs_type_name(const struct vol *v)
{
    return v->type == 12 ? "FAT12" : v->type == 16 ? "FAT16" : "FAT32";
}

struct vol *fs_volume(char letter)
{
    letter = up(letter);
    for (int i = 0; i < nvols; i++)
        if (vols[i].letter && vols[i].letter == letter)
            return &vols[i];
    return NULL;
}

/* ---- files ----------------------------------------------------------------------- */

static struct fnode *node_get(struct vol *v, uint32_t dir, const struct dent *e)
{
    struct fnode *freen = NULL;
    for (int i = 0; i < NODE_MAX; i++) {
        struct fnode *n = &nodes[i];
        if (n->refs && n->v == v && n->dir == dir && n->idx == e->idx) {
            n->refs++;
            return n;
        }
        if (!n->refs && !freen)
            freen = n;
    }
    if (!freen)
        return NULL;
    const uint8_t *r = e->raw;
    *freen = (struct fnode){ .refs = 1, .v = v, .dir = dir, .idx = e->idx, .first = e->first,
                             .clus = ent_clus(v, r), .size = le32(r + 28), .attr = r[11],
                             .time = le16(r + 22), .date = le16(r + 24) };
    memcpy(freen->fcb, r, 11);
    return freen;
}

static bool node_open(struct vol *v, uint32_t dir, uint32_t idx)
{
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v && nodes[i].dir == dir && nodes[i].idx == idx)
            return true;
    return false;
}

static int node_update(struct fnode *n)
{
    if (!n->dirty)
        return 0;
    struct dcur d;
    dcur_init(&d, n->v, n->dir);
    uint8_t ent[32];
    int rc = dir_read(&d, n->idx, ent);
    if (rc)
        return rc < 0 ? rc : -ERROR_GEN_FAILURE;
    set_ent_clus(n->v, ent, n->clus);
    put32(ent + 28, n->size);
    put16(ent + 22, n->time);
    put16(ent + 24, n->date);
    put16(ent + 18, n->date);
    ent[11] = n->attr;
    n->dirty = false;
    return dir_write(&d, n->idx, ent);
}

/* The file's cluster number ci; with alloc, the chain grows to reach it. */
static int node_clus(struct fnode *n, uint32_t ci, bool alloc, uint32_t *out)
{
    struct vol *v = n->v;
    int rc;
    if (!n->clus) {
        if (!alloc)
            return 1;
        if ((rc = alloc_clus(v, 0, &n->clus)) < 0)
            return rc;
        n->dirty = true;
        n->c_clus = 0;
    }
    if (!n->c_clus || n->c_idx > ci) {
        n->c_idx = 0;
        n->c_clus = n->clus;
    }
    while (n->c_idx < ci) {
        uint32_t next;
        if (fat_get(v, n->c_clus, &next) < 0)
            return -ERROR_GEN_FAILURE;
        if (next == EOC) {
            if (!alloc)
                return 1;
            if ((rc = alloc_clus(v, n->c_clus, &next)) < 0)
                return rc;
        } else if (!clus_ok(v, next)) {
            return -ERROR_GEN_FAILURE;
        }
        n->c_clus = next;
        n->c_idx++;
    }
    *out = n->c_clus;
    return 0;
}

/* Move bytes between the file and buf (NULL when writing: zeros). */
static int node_io(struct fnode *n, uint32_t pos, uint8_t *buf, uint32_t len, bool write)
{
    struct vol *v = n->v;
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
        if (in == 0 && chunk == v->cbytes && buf) {     /* a whole cluster: no cache */
            uint32_t lba = clus_lba(v, c);
            if (write) {
                if (ata_write(v->disk, lba, v->spc, buf + done_n) < 0)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
                cache_forget(v->disk, lba, v->spc);
            } else {
                cache_sync();                           /* nothing of it may be newer in the cache */
                if (ata_read(v->disk, lba, v->spc, buf + done_n) < 0)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
            }
        } else {
            uint32_t off = in;
            uint32_t left = chunk;
            uint8_t *p = buf ? buf + done_n : NULL;
            while (left) {
                uint32_t lba = clus_lba(v, c) + off / SECTOR, so = off % SECTOR;
                uint32_t k = SECTOR - so < left ? SECTOR - so : left;
                struct buf *b = write && so == 0 && k == SECTOR ? bzero_get(v->disk, lba) : bread(v->disk, lba);
                if (!b)
                    return done_n ? (int)done_n : -ERROR_GEN_FAILURE;
                if (write) {
                    if (p) memcpy(b->data + so, p, k);
                    else   memset(b->data + so, 0, k);
                    bdirty(b);
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
        if (write)
            v->changed = true;
        done_n += chunk;
        pos += chunk;
    }
    return (int)done_n;
}

static int set_size(struct fnode *n, uint32_t size)
{
    struct vol *v = n->v;
    if (size > n->size) {
        int rc = node_io(n, n->size, NULL, size - n->size, true);
        if (rc < 0)
            return rc;
        n->size += (uint32_t)rc;
        n->dirty = true;
        return n->size == size ? 0 : -ERROR_DISK_FULL;
    }
    if (size == n->size)
        return 0;
    uint32_t keep = (size + v->cbytes - 1) / v->cbytes;
    if (keep == 0) {
        if (n->clus && free_chain(v, n->clus) < 0)
            return -ERROR_GEN_FAILURE;
        n->clus = 0;
    } else if (n->clus) {
        uint32_t last;
        int rc = node_clus(n, keep - 1, false, &last);
        if (rc < 0)
            return rc;
        if (rc == 0) {
            uint32_t next;
            if (fat_get(v, last, &next) < 0)
                return -ERROR_GEN_FAILURE;
            if (next != EOC) {
                if (fat_set(v, last, EOC) < 0 || free_chain(v, next) < 0)
                    return -ERROR_GEN_FAILURE;
            }
        }
    }
    n->c_clus = 0;
    n->size = size;
    n->dirty = true;
    return 0;
}

int fs_open(struct vol *v, const char *path, uint32_t disp, bool write, uint8_t attr,
            struct fnode **out, bool *created)
{
    fs_lock();
    uint32_t dir;
    const char *name;
    struct dent e;
    int rc = walk(v, path, &dir, &name);
    if (rc < 0)
        return done(v, rc);
    if (!*name)
        return done(v, -ERROR_ACCESS_DENIED);           /* the root */
    rc = dir_find(v, dir, name, &e);
    bool exists = rc == 0;
    if (rc < 0 && rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc);
    if (exists && (e.raw[11] & A_DIR))
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
    if (exists && (write || truncate) && (e.raw[11] & A_RO))
        return done(v, -ERROR_ACCESS_DENIED);
    if (create) {
        uint8_t proto[32];
        memset(proto, 0, 32);
        proto[11] = (uint8_t)(A_ARC | (attr & (A_RO | A_HID | A_SYS)));
        stamp(proto, true);
        if ((rc = dir_add(v, dir, name, proto, &e)) < 0)
            return done(v, rc);
    }
    struct fnode *n = node_get(v, dir, &e);
    if (!n)
        return done(v, -ERROR_TOO_MANY_OPEN_FILES);
    if (truncate) {
        if ((rc = set_size(n, 0)) < 0 || (rc = node_update(n)) < 0) {
            n->refs--;
            return done(v, rc);
        }
        n->attr = (uint8_t)((n->attr & ~(A_RO | A_HID | A_SYS)) | A_ARC | (attr & (A_HID | A_SYS)));
        rtc_dos_time(&n->date, &n->time);
        n->dirty = true;
        node_update(n);
    }
    *out = n;
    if (created)
        *created = create;
    return done(v, 0);
}

int fs_read(struct fnode *n, uint32_t pos, void *buf, uint32_t len)
{
    fs_lock();
    if (pos >= n->size)
        return done(n->v, 0);
    if (len > n->size - pos)
        len = n->size - pos;
    return done(n->v, node_io(n, pos, buf, len, false));
}

int fs_write(struct fnode *n, uint32_t pos, const void *buf, uint32_t len)
{
    fs_lock();
    struct vol *v = n->v;
    if (len == 0)
        return done(v, 0);
    if (pos > n->size) {                                /* the gap reads as zeros */
        int rc = set_size(n, pos);
        if (rc < 0) {
            node_update(n);
            return done(v, rc);
        }
    }
    if ((uint64_t)pos + len > 0xFFFFFFFFu)
        len = 0xFFFFFFFFu - pos;
    int rc = node_io(n, pos, (uint8_t *)buf, len, true);
    if (rc > 0) {
        if (pos + (uint32_t)rc > n->size)
            n->size = pos + (uint32_t)rc;
        rtc_dos_time(&n->date, &n->time);
        n->attr |= A_ARC;
        n->dirty = true;
    }
    if (rc == 0)
        rc = -ERROR_DISK_FULL;
    int urc = node_update(n);
    return done(v, rc > 0 && urc < 0 ? urc : rc);
}

int fs_set_size(struct fnode *n, uint32_t size)
{
    fs_lock();
    int rc = set_size(n, size);
    if (rc == 0) {
        rtc_dos_time(&n->date, &n->time);
        n->attr |= A_ARC;
    }
    int urc = node_update(n);
    return done(n->v, rc < 0 ? rc : urc);
}

uint32_t fs_size(const struct fnode *n)    { return n->size; }
uint8_t fs_node_attr(const struct fnode *n) { return n->attr; }
void fs_name_of(const struct fnode *n, uint8_t fcb[11])
{
    memcpy(fcb, n->fcb, 11);
    if (fcb[0] == 0x05)
        fcb[0] = 0xE5;
}

void fs_get_time(const struct fnode *n, uint16_t *date, uint16_t *time)
{
    *date = n->date;
    *time = n->time;
}

int fs_set_time(struct fnode *n, uint16_t date, uint16_t time)
{
    fs_lock();
    n->date = date;
    n->time = time;
    n->dirty = true;
    return done(n->v, node_update(n));
}

int fs_flush(struct fnode *n)
{
    fs_lock();
    n->v->changed = true;
    return done(n->v, node_update(n));
}

struct fnode *fs_dup(struct fnode *n)
{
    fs_lock();
    n->refs++;
    fs_unlock();
    return n;
}

void fs_close(struct fnode *n)
{
    fs_lock();
    struct vol *v = n->v;
    node_update(n);
    if (n->refs > 0)
        n->refs--;
    done(v, 0);
}

/* ---- names ------------------------------------------------------------------------ */

int fs_stat(struct vol *v, const char *path, struct fs_entry *e)
{
    fs_lock();
    if (!*path) {
        memset(e, 0, sizeof *e);
        e->attr = A_DIR;
        return done(v, 0);
    }
    uint32_t dir;
    const char *name;
    struct dent d;
    int rc = walk(v, path, &dir, &name);
    if (rc == 0)
        rc = dir_find(v, dir, name, &d);
    if (rc == 0)
        fill_entry(v, &d, e);
    return done(v, rc);
}

int fs_set_attr(struct vol *v, const char *path, uint8_t attr)
{
    fs_lock();
    uint32_t dir;
    const char *name;
    struct dent e;
    int rc = walk(v, path, &dir, &name);
    if (rc == 0 && !*name)
        rc = -ERROR_ACCESS_DENIED;
    if (rc == 0)
        rc = dir_find(v, dir, name, &e);
    if (rc < 0)
        return done(v, rc);
    struct dcur d;
    dcur_init(&d, v, dir);
    e.raw[11] = (uint8_t)((e.raw[11] & (A_DIR | A_VOL)) | (attr & (A_RO | A_HID | A_SYS | A_ARC)));
    rc = dir_write(&d, e.idx, e.raw);
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs && nodes[i].v == v && nodes[i].dir == dir && nodes[i].idx == e.idx)
            nodes[i].attr = e.raw[11];
    return done(v, rc);
}

int fs_delete(struct vol *v, const char *path)
{
    fs_lock();
    uint32_t dir;
    const char *name;
    struct dent e;
    int rc = walk(v, path, &dir, &name);
    if (rc == 0 && !*name)
        rc = -ERROR_ACCESS_DENIED;
    if (rc == 0)
        rc = dir_find(v, dir, name, &e);
    if (rc < 0)
        return done(v, rc);
    if (e.raw[11] & (A_DIR | A_RO))
        return done(v, -ERROR_ACCESS_DENIED);
    if (node_open(v, dir, e.idx))
        return done(v, -ERROR_SHARING_VIOLATION);
    uint32_t c = ent_clus(v, e.raw);
    if ((rc = dir_remove(v, dir, &e)) == 0 && c)
        rc = free_chain(v, c);
    return done(v, rc);
}

int fs_mkdir(struct vol *v, const char *path)
{
    fs_lock();
    uint32_t dir;
    const char *name;
    struct dent e;
    int rc = walk(v, path, &dir, &name);
    if (rc == 0 && !*name)
        rc = -ERROR_ACCESS_DENIED;
    if (rc < 0)
        return done(v, rc);
    rc = dir_find(v, dir, name, &e);
    if (rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc < 0 ? rc : -ERROR_ACCESS_DENIED);
    uint32_t c;
    if ((rc = alloc_clus(v, 0, &c)) < 0)
        return done(v, rc);
    zero_clus(v, c);
    uint8_t proto[32];
    memset(proto, 0, 32);
    proto[11] = A_DIR;
    stamp(proto, true);
    struct buf *b = bread(v->disk, clus_lba(v, c));
    if (!b)
        return done(v, -ERROR_GEN_FAILURE);
    memcpy(b->data, proto, 32);                         /* "." and ".." */
    memset(b->data, ' ', 11);
    b->data[0] = '.';
    set_ent_clus(v, b->data, c);
    memcpy(b->data + 32, proto, 32);
    memset(b->data + 32, ' ', 11);
    b->data[32] = b->data[33] = '.';
    set_ent_clus(v, b->data + 32, dir);
    bdirty(b);
    brelse(b);
    set_ent_clus(v, proto, c);
    rc = dir_add(v, dir, name, proto, NULL);
    if (rc < 0)
        free_chain(v, c);
    return done(v, rc);
}

static bool dir_empty(struct vol *v, uint32_t c)
{
    struct dcur d;
    dcur_init(&d, v, c);
    struct dent e;
    uint32_t idx = 0;
    while (dir_next(&d, &idx, &e) > 0) {
        if (e.raw[0] == '.' && (e.raw[1] == ' ' || (e.raw[1] == '.' && e.raw[2] == ' ')))
            continue;
        return false;
    }
    return true;
}

int fs_rmdir(struct vol *v, const char *path)
{
    fs_lock();
    uint32_t dir;
    const char *name;
    struct dent e;
    int rc = walk(v, path, &dir, &name);
    if (rc == 0 && !*name)
        rc = -ERROR_ACCESS_DENIED;
    if (rc == 0)
        rc = dir_find(v, dir, name, &e);
    if (rc < 0)
        return done(v, rc == -ERROR_FILE_NOT_FOUND ? -ERROR_PATH_NOT_FOUND : rc);
    if (!(e.raw[11] & A_DIR))
        return done(v, -ERROR_PATH_NOT_FOUND);
    uint32_t c = ent_clus(v, e.raw);
    if (!c || (e.raw[0] == '.'))
        return done(v, -ERROR_ACCESS_DENIED);
    if (!dir_empty(v, c))
        return done(v, -ERROR_DIR_NOT_EMPTY);
    if ((rc = dir_remove(v, dir, &e)) == 0)
        rc = free_chain(v, c);
    return done(v, rc);
}

/* Is directory c inside (or) directory anc? Up through the ".." entries. */
static bool inside(struct vol *v, uint32_t c, uint32_t anc)
{
    for (int n = 0; c && n < 256; n++) {
        if (c == anc)
            return true;
        struct dcur d;
        dcur_init(&d, v, c);
        uint8_t ent[32];
        if (dir_read(&d, 1, ent) != 0 || ent[0] != '.' || ent[1] != '.')
            return false;
        c = ent_clus(v, ent);
    }
    return false;
}

int fs_rename(struct vol *v, const char *from, const char *to)
{
    fs_lock();
    uint32_t fdir, tdir;
    const char *fname, *tname;
    struct dent fe, te;
    int rc = walk(v, from, &fdir, &fname);
    if (rc == 0 && !*fname)
        rc = -ERROR_ACCESS_DENIED;
    if (rc == 0)
        rc = dir_find(v, fdir, fname, &fe);
    if (rc < 0)
        return done(v, rc);
    if ((rc = walk(v, to, &tdir, &tname)) < 0)
        return done(v, rc);
    if (!*tname)
        return done(v, -ERROR_ACCESS_DENIED);
    rc = dir_find(v, tdir, tname, &te);
    bool self = rc == 0 && tdir == fdir && te.idx == fe.idx;     /* only the case changes */
    if (rc == 0 && !self)
        return done(v, -ERROR_ACCESS_DENIED);
    if (rc < 0 && rc != -ERROR_FILE_NOT_FOUND)
        return done(v, rc);
    bool is_dir = fe.raw[11] & A_DIR;
    uint32_t c = ent_clus(v, fe.raw);
    if (is_dir && (fe.raw[0] == '.' || inside(v, tdir, c)))
        return done(v, -ERROR_ACCESS_DENIED);
    if (self && (rc = dir_remove(v, fdir, &fe)) < 0)    /* its own slots may be reused */
        return done(v, rc);
    struct dent ne;
    if ((rc = dir_add(v, tdir, tname, fe.raw, &ne)) < 0)
        return done(v, rc);
    if (!self && (rc = dir_remove(v, fdir, &fe)) < 0)
        return done(v, rc);
    if (is_dir && tdir != fdir) {                       /* its ".." */
        struct dcur d;
        dcur_init(&d, v, c);
        uint8_t ent[32];
        if (dir_read(&d, 1, ent) == 0 && ent[0] == '.' && ent[1] == '.') {
            set_ent_clus(v, ent, tdir);
            dir_write(&d, 1, ent);
        }
    }
    for (int i = 0; i < NODE_MAX; i++) {
        struct fnode *n = &nodes[i];
        if (n->refs && n->v == v && n->dir == fdir && n->idx == fe.idx) {
            n->dir = tdir;
            n->idx = ne.idx;
            n->first = ne.first;
            memcpy(n->fcb, ne.raw, 11);
        }
    }
    return done(v, 0);
}

/* ---- searches ---------------------------------------------------------------------- */

static bool wild(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return true;
            for (; *s; s++)
                if (wild(p, s))
                    return true;
            return wild(p, s);
        }
        if (!*s)
            return *p == '.' && p[1] == '*' && !p[2] ? true : false;
        if (*p != '?' && up(*p) != up(*s))
            return false;
        p++, s++;
    }
    return !*s;
}

static bool fcb_match(const uint8_t *tmpl, const uint8_t *name)
{
    for (int i = 0; i < 11; i++)
        if (tmpl[i] != '?' && tmpl[i] != name[i])
            return false;
    return true;
}

static int find_dir(struct vol *v, const char *dir, uint32_t *c)
{
    if (!*dir) {
        *c = 0;
        return 0;
    }
    uint32_t parent;
    const char *name;
    struct dent e;
    int rc = walk(v, dir, &parent, &name);
    if (rc == 0)
        rc = dir_find(v, parent, name, &e);
    if (rc < 0)
        return -ERROR_PATH_NOT_FOUND;
    if (!(e.raw[11] & A_DIR))
        return -ERROR_PATH_NOT_FOUND;
    *c = ent_clus(v, e.raw);
    return 0;
}

static int find_more(struct fs_find *f, struct fs_entry *out)
{
    struct dcur d;
    dcur_init(&d, f->v, f->dir);
    struct dent e;
    for (;;) {
        int rc = dir_next(&d, &f->next, &e);
        if (rc <= 0)
            return rc < 0 ? rc : -ERROR_NO_MORE_FILES;
        uint8_t a = e.raw[11];
        if (f->dos) {
            uint8_t name[11];
            memcpy(name, e.raw, 11);
            if (name[0] == 0x05)
                name[0] = 0xE5;
            if (f->attr == A_VOL) {
                if (!(a & A_VOL) || (a & A_DIR))
                    continue;
            } else if ((a & A_VOL) || (a & (A_HID | A_SYS | A_DIR) & ~f->attr)) {
                continue;
            }
            if (!fcb_match(f->fcb, name))
                continue;
        } else {
            if (a & A_VOL)
                continue;
            char sname[13];
            short_display(e.raw, sname, false);
            const char *p = f->pattern;
            bool all = (p[0] == '*' && !p[1]) || (p[0] == '*' && p[1] == '.' && p[2] == '*' && !p[3]);
            if (!all && !(e.lfn && wild(p, e.lname)) && !wild(p, sname))
                continue;
        }
        fill_entry(f->v, &e, out);
        return 0;
    }
}

int fs_find_first(struct vol *v, const char *dir, const char *pattern, struct fs_find *f,
                  struct fs_entry *e)
{
    fs_lock();
    memset(f, 0, sizeof *f);
    f->v = v;
    int rc = find_dir(v, dir, &f->dir);
    if (rc < 0)
        return done(v, rc);
    size_t n = strlen(pattern);
    if (n > FS_NAME_MAX)
        return done(v, -ERROR_INVALID_NAME);
    memcpy(f->pattern, pattern, n + 1);
    rc = find_more(f, e);
    return done(v, rc == -ERROR_NO_MORE_FILES ? -ERROR_FILE_NOT_FOUND : rc);
}

int fs_find_dos(struct vol *v, const char *dir, const uint8_t fcb[11], uint8_t attr,
                struct fs_find *f, struct fs_entry *e)
{
    fs_lock();
    memset(f, 0, sizeof *f);
    f->v = v;
    f->dos = true;
    f->attr = attr;
    memcpy(f->fcb, fcb, 11);
    int rc = find_dir(v, dir, &f->dir);
    if (rc < 0)
        return done(v, rc);
    rc = find_more(f, e);
    return done(v, rc == -ERROR_NO_MORE_FILES ? -ERROR_FILE_NOT_FOUND : rc);
}

int fs_find_next(struct fs_find *f, struct fs_entry *e)
{
    fs_lock();
    return done(f->v, find_more(f, e));
}

int fs_space(struct vol *v, uint32_t *spc, uint32_t *bps, uint32_t *freec, uint32_t *total)
{
    fs_lock();
    int rc = 0;
    if (!v->free_known && (rc = count_free(v)) == 0)
        v->fsinfo_dirty = true;
    *spc = v->spc;
    *bps = SECTOR;
    *freec = v->free;
    *total = v->nclus;
    return done(v, rc);
}

int fs_short_path(struct vol *v, const char *path, char *out, uint32_t size)
{
    fs_lock();
    uint32_t c = 0, n = 0;
    const char *s = path;
    int rc = 0;
    out[0] = '\0';
    while (*s && rc == 0) {
        const char *e = s;
        while (*e && *e != '\\')
            e++;
        char comp[FS_NAME_MAX + 1];
        uint32_t len = (uint32_t)(e - s);
        if (len > FS_NAME_MAX) {
            rc = -ERROR_INVALID_NAME;
            break;
        }
        memcpy(comp, s, len);
        comp[len] = '\0';
        struct dent de;
        if ((rc = dir_find(v, c, comp, &de)) < 0)
            break;
        char sname[13];
        short_display(de.raw, sname, false);
        uint32_t sl = (uint32_t)strlen(sname);
        if (n + sl + 2 > size) {
            rc = -ERROR_FILENAME_EXCED_RANGE;
            break;
        }
        if (n)
            out[n++] = '\\';
        memcpy(out + n, sname, sl + 1);
        n += sl;
        c = ent_clus(v, de.raw);
        s = *e ? e + 1 : e;
    }
    return done(v, rc);
}

void fat_sync_all(void)
{
    for (int i = 0; i < NODE_MAX; i++)
        if (nodes[i].refs)
            node_update(&nodes[i]);
    for (int i = 0; i < nvols; i++)
        if (vols[i].fsinfo_dirty) {
            fs_lock();
            done(&vols[i], 0);
        }
    cache_sync();
}
