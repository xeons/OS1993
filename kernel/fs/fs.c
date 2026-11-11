/*
 * fs.c -- the file systems' common part: bringing them up (probe the
 * disks, read the partition tables, mount what each driver recognizes,
 * give DOS its drive letters), the lock, the list of volumes, and the
 * public calls of fs.h, handed to each volume's driver (fsimpl.h).
 */
#include "kernel.h"
#include "disk.h"
#include "fsimpl.h"
#include "io.h"
#include "sched.h"

static struct vol *vols[VOL_MAX];
static int nvols;

/* ---- the lock: one for every volume, taken around each call ---------------- */

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

/* ---- volumes ------------------------------------------------------------------ */

void fs_add_volume(struct vol *v)
{
    if (nvols < VOL_MAX)
        vols[nvols++] = v;
}

void fs_init(void)
{
    cache_init();
    ata_init();
    fs_lock();
    part_scan();
    for (int i = 0; i < nparts; i++)
        if (!exfat_mount(&parts[i]) && !ext2_mount(&parts[i]))
            fat_mount(&parts[i]);
    fs_unlock();
    redir_init();
    ata_claim();
}

void fs_shutdown(void)
{
    for (int i = 0; i < nvols; i++)
        vols[i]->ops->sync(vols[i]);
    ata_release();
}

int fs_volume_count(void)                 { return nvols; }
struct vol *fs_volume_at(int i)           { return i >= 0 && i < nvols ? vols[i] : NULL; }
char fs_letter(const struct vol *v)       { return v->letter; }
void fs_set_letter(struct vol *v, char c) { v->letter = c; }
uint32_t fs_serial(const struct vol *v)   { return v->serial; }
const char *fs_label(const struct vol *v) { return v->label; }
const char *fs_type_name(const struct vol *v) { return v->type; }

struct vol *fs_volume(char letter)
{
    letter = fs_up(letter);
    for (int i = 0; i < nvols; i++)
        if (vols[i]->letter && vols[i]->letter == letter)
            return vols[i];
    return NULL;
}

/* ---- the calls ------------------------------------------------------------------ */

int fs_open(struct vol *v, const char *path, uint32_t disp, bool write, uint8_t attr,
            struct fnode **out, bool *created)
{
    return v->ops->open(v, path, disp, write, attr, out, created);
}

int fs_read(struct fnode *n, uint32_t pos, void *buf, uint32_t len)  { return n->v->ops->read(n, pos, buf, len); }
int fs_write(struct fnode *n, uint32_t pos, const void *buf, uint32_t len) { return n->v->ops->write(n, pos, buf, len); }
int fs_set_size(struct fnode *n, uint32_t size) { return n->v->ops->set_size(n, size); }
uint32_t fs_size(const struct fnode *n)         { return n->v->ops->size(n); }
void fs_get_time(const struct fnode *n, uint16_t *date, uint16_t *time) { n->v->ops->get_time(n, date, time); }
int fs_set_time(struct fnode *n, uint16_t date, uint16_t time) { return n->v->ops->set_time(n, date, time); }
uint8_t fs_node_attr(const struct fnode *n)     { return n->v->ops->attr(n); }
void fs_name_of(const struct fnode *n, uint8_t fcb[11]) { n->v->ops->name_of(n, fcb); }
int fs_flush(struct fnode *n)                   { return n->v->ops->flush(n); }
void fs_close(struct fnode *n)                  { n->v->ops->close(n); }

struct fnode *fs_dup(struct fnode *n)
{
    n->v->ops->dup(n);
    return n;
}

int fs_stat(struct vol *v, const char *path, struct fs_entry *e) { return v->ops->stat(v, path, e); }
int fs_set_attr(struct vol *v, const char *path, uint8_t attr)   { return v->ops->set_attr(v, path, attr); }
int fs_delete(struct vol *v, const char *path)  { return v->ops->del(v, path); }
int fs_mkdir(struct vol *v, const char *path)   { return v->ops->mkdir(v, path); }
int fs_rmdir(struct vol *v, const char *path)   { return v->ops->rmdir(v, path); }
int fs_rename(struct vol *v, const char *from, const char *to) { return v->ops->rename(v, from, to); }
int fs_short_path(struct vol *v, const char *path, char *out, uint32_t size)
{
    return v->ops->short_path(v, path, out, size);
}

int fs_find_first(struct vol *v, const char *dir, const char *pattern, struct fs_find *f, struct fs_entry *e)
{
    return v->ops->find_first(v, dir, pattern, f, e);
}

int fs_find_dos(struct vol *v, const char *dir, const uint8_t fcb[11], uint8_t attr, struct fs_find *f,
                struct fs_entry *e)
{
    return v->ops->find_dos(v, dir, fcb, attr, f, e);
}

int fs_find_next(struct fs_find *f, struct fs_entry *e) { return f->v->ops->find_next(f, e); }

int fs_space(struct vol *v, uint32_t *spc, uint32_t *bps, uint32_t *freec, uint32_t *total)
{
    return v->ops->space(v, spc, bps, freec, total);
}

/* ---- names, for the drivers ---------------------------------------------------------- */

char fs_up(char c)
{
    uint8_t u = (uint8_t)c;
    if ((u >= 'a' && u <= 'z') || (u >= 0xE0 && u <= 0xFE && u != 0xF7))
        return (char)(u - 32);
    return c;
}

bool fs_name_eq(const char *a, const char *b)
{
    while (*a && fs_up(*a) == fs_up(*b))
        a++, b++;
    return !*a && !*b;
}

bool fs_wild(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return true;
            for (; *s; s++)
                if (fs_wild(p, s))
                    return true;
            return fs_wild(p, s);
        }
        if (!*s)
            return *p == '.' && p[1] == '*' && !p[2];   /* "NAME" matches "NAME.*" */
        if (*p != '?' && fs_up(*p) != fs_up(*s))
            return false;
        p++, s++;
    }
    return !*s;
}

bool fs_fcb_match(const uint8_t *tmpl, const uint8_t *name)
{
    for (int i = 0; i < 11; i++)
        if (tmpl[i] != '?' && tmpl[i] != name[i])
            return false;
    return true;
}

bool fs_short_char(uint8_t c)
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

/* A name as it will be stored: trailing dots and spaces go (as in Win32). */
int fs_clean_name(const char *in, char *out)
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

/* DOS's name for an entry: the name in upper case if it is 8.3, else
 * "XXhhhh~1.EXT" from the name hash. */
void fs_make_alias(const char *name, uint16_t hash, uint8_t fcb[11], char *alias)
{
    memset(fcb, ' ', 11);
    const char *dot = NULL;
    int dots = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.') {
            dot = p;
            dots++;
        }
    bool fits = name[0] != '.' && dots <= 1;
    int n = 0;
    uint8_t basis[8];
    int bn = 0;
    for (const char *p = name; *p && (!dot || p < dot); p++) {
        uint8_t c = (uint8_t)fs_up(*p);
        if (c == ' ' || !fs_short_char(c)) {
            fits = false;
            if (c == ' ' || c == '.')
                continue;
            c = '_';
        }
        if (n < 8)
            fcb[n++] = c;
        else
            fits = false;
        if (bn < 8)
            basis[bn++] = c;
    }
    if (!n)
        fits = false;
    if (dot) {
        int m = 0;
        for (const char *p = dot + 1; *p; p++) {
            uint8_t c = (uint8_t)fs_up(*p);
            if (c == ' ' || !fs_short_char(c)) {
                fits = false;
                if (c == ' ')
                    continue;
                c = '_';
            }
            if (m < 3)
                fcb[8 + m++] = c;
            else
                fits = false;
        }
    }
    if (!fits) {
        static const char hex[] = "0123456789ABCDEF";
        memset(fcb, ' ', 8);
        int k = 0;
        for (int i = 0; i < bn && k < 2; i++)
            fcb[k++] = basis[i];
        for (int s = 12; s >= 0; s -= 4)
            fcb[k++] = (uint8_t)hex[(hash >> s) & 15];
        fcb[k++] = '~';
        fcb[k++] = '1';
    }
    if (fcb[0] == 0xE5)
        fcb[0] = 0x05;
    int a = 0;
    for (int i = 0; i < 8 && fcb[i] != ' '; i++)
        alias[a++] = (char)(i == 0 && fcb[0] == 0x05 ? 0xE5 : fcb[i]);
    if (fcb[8] != ' ') {
        alias[a++] = '.';
        for (int i = 8; i < 11 && fcb[i] != ' '; i++)
            alias[a++] = (char)fcb[i];
    }
    alias[a] = '\0';
    if (fcb[0] == 0x05)
        fcb[0] = 0xE5;
}


/* A 16-bit hash of a name's bytes (exFAT's rotate-and-add), for aliases. */
uint16_t fs_hash16(const char *name)
{
    uint16_t h = 0;
    for (const uint8_t *p = (const uint8_t *)name; *p; p++)
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + *p);
    return h;
}

/* ---- times: DOS's and Unix's (seconds since 1970, taken as local time) ---------- */

void fs_unix_to_dos(uint32_t t, uint16_t *date, uint16_t *time)
{
    uint32_t days = t / 86400, secs = t % 86400;
    uint32_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    if (y < 1980) {
        *date = (1 << 5) | 1;                           /* 1980-01-01 */
        *time = 0;
        return;
    }
    if (y > 2107)
        y = 2107;
    *date = (uint16_t)((y - 1980) << 9 | m << 5 | d);
    *time = (uint16_t)((secs / 3600) << 11 | (secs / 60 % 60) << 5 | (secs % 60) / 2);
}

uint32_t fs_dos_to_unix(uint16_t date, uint16_t time)
{
    uint32_t y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12)
        m = 1;
    if (d < 1)
        d = 1;
    y -= m <= 2;
    uint32_t era = y / 400, yoe = y - era * 400;
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = era * 146097 + doe - 719468;
    return days * 86400 + (uint32_t)(time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}
