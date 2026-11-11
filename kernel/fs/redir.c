/*
 * redir.c -- DOS reaches the VMM's volumes as network drives.
 *
 * Each volume gets a drive letter in DOS's current directory structure
 * (CDS) marked "network", so DOS sends every call for it to the network
 * redirector: INT 2Fh AH=11h, which DOS itself issues, which traps into
 * the V86 monitor (v86.c), and which this file answers from the file
 * system, in 32-bit code, inside whichever VM made the call. That is how
 * MSCDEX and network clients plug into DOS, and what Windows 95's IFS
 * manager did to DOS's own drives.
 *
 * A volume DOS had mounted itself (the same volume serial number) keeps
 * its letter: DOS's buffers are flushed and dropped (INT 21h AH=0Dh), and
 * the CDS entry is switched to network until OS1993 ends, when it is put
 * back and DOS made to rebuild that drive (DPB "accessed" byte FFh).
 * Volumes DOS didn't see get free letters below LASTDRIVE; with none
 * left, only OS1993 programs see them.
 *
 * DOS passes the names as 8.3 names in its swappable data area (SDA),
 * open files as its own file table entries (SFTs) with our file number in
 * them, and searches as the 21-byte block in the caller's DTA, where we
 * keep the directory's cluster and the next entry.
 *
 * Offsets below are DOS 4+'s, as the FreeDOS kernel keeps them.
 */
#include "kernel.h"
#include "fs.h"
#include "interrupts.h"
#include "io.h"
#include "user.h"
#include "v86.h"
#include "vm.h"

/* SDA */
#define SDA_DTA      0x0C
#define SDA_FN1      0x9E
#define SDA_FN2      0x11E
#define SDA_SDB      0x19E
#define SDA_FOUND    0x1B3
#define SDA_ATTR     0x24D
#define SDA_CDS      0x282
#define SDA_XACTION  0x2DD
#define SDA_XATTR    0x2DF
#define SDA_XMODE    0x2E1
/* SFT */
#define SFT_COUNT    0x00
#define SFT_MODE     0x02
#define SFT_ATTR     0x04
#define SFT_FLAGS    0x05
#define SFT_DEV      0x07
#define SFT_ID       0x0B
#define SFT_TIME     0x0D
#define SFT_DATE     0x0F
#define SFT_SIZE     0x11
#define SFT_POS      0x15
#define SFT_NAME     0x20
#define SF_NET       0x8000
#define SF_DATESET   0x4000
#define SF_CLEAN     0x0040
/* CDS */
#define CDS_SIZE     0x58
#define CDS_FLAGS    0x43
#define CDS_DPB      0x45
#define CDS_START    0x49
#define CDS_BSOFF    0x4F
#define CDS_NETWORK  0x8000
#define CDS_PHYSICAL 0x4000

#define RFILE_MAX    64

static struct {
    bool     on;
    uint32_t sda, cds;                  /* linear addresses */
    uint8_t  lastdrive;
    struct { bool used; uint8_t saved[CDS_SIZE]; bool taken; } drive[26];
} rd;

static struct { struct fnode *n; struct vm *vm; } rfiles[RFILE_MAX];

static uint8_t *at(uint32_t lin)                 { return (uint8_t *)(uintptr_t)lin; }
static uint32_t far_lin(uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }
static uint32_t far_ptr(uint32_t lin)            /* a far pointer stored at lin */
{
    uint16_t *p = (uint16_t *)at(lin);
    return far_lin(p[1], p[0]);
}
static uint16_t rd16(uint32_t lin)               { return *(uint16_t *)at(lin); }
static uint32_t rd32(uint32_t lin)               { return *(uint32_t *)at(lin); }
static void wr16(uint32_t lin, uint16_t v)       { *(uint16_t *)at(lin) = v; }
static void wr32(uint32_t lin, uint32_t v)       { *(uint32_t *)at(lin) = v; }

static uint32_t cds_of(int d) { return rd.cds + (uint32_t)d * CDS_SIZE; }

/* ---- setting up ----------------------------------------------------------- */

static uint32_t dos_serial(int d)               /* 0: unknown */
{
    uint8_t *buf = v86_ptr(v86_buf_seg(), v86_buf_off());
    memset(buf, 0, 32);
    struct v86_regs r = { .eax = 0x440D, .ebx = (uint32_t)d + 1, .ecx = 0x0866,
                          .edx = v86_buf_off(), .ds = v86_buf_seg() };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF))
        return 0;
    return rd32((uint32_t)(uintptr_t)buf + 2);
}

static void take(int d, struct vol *v, bool was_dos)
{
    uint32_t c = cds_of(d);
    memcpy(rd.drive[d].saved, at(c), CDS_SIZE);
    rd.drive[d].used = true;
    rd.drive[d].taken = was_dos;
    char *path = (char *)at(c);
    memset(path, 0, 67);
    path[0] = (char)('A' + d);
    path[1] = ':';
    path[2] = '\\';
    wr16(c + CDS_FLAGS, CDS_NETWORK | CDS_PHYSICAL);
    wr32(c + CDS_DPB, 0);
    wr32(c + CDS_START, 0xFFFFFFFFu);
    wr16(c + CDS_BSOFF, 2);
    fs_set_letter(v, (char)('A' + d));
}

void redir_init(void)
{
    int nv = fs_volume_count();
    if (!nv)
        return;
    struct v86_regs r = { .eax = 0x5200 };
    if (v86_int(0x21, &r) != 0)
        return;
    uint32_t lol = far_lin(r.es, (uint16_t)r.ebx);
    rd.cds = far_ptr(lol + 0x16);
    rd.lastdrive = *at(lol + 0x21);
    r = (struct v86_regs){ .eax = 0x5D06 };
    if (v86_int(0x21, &r) != 0 || (r.flags & EFLAGS_CF) || rd.cds == far_lin(0xFFFF, 0xFFFF))
        return;
    rd.sda = far_lin(r.ds, (uint16_t)r.esi);
    if (rd.lastdrive > 26)
        rd.lastdrive = 26;

    /* the volumes DOS mounted itself: by serial number */
    bool flushed = false;
    for (int d = 2; d < rd.lastdrive; d++) {
        uint16_t flags = rd16(cds_of(d) + CDS_FLAGS);
        if (!(flags & CDS_PHYSICAL) || (flags & (CDS_NETWORK | 0x3000)))
            continue;
        uint32_t serial = dos_serial(d);
        if (!serial)
            continue;
        for (int i = 0; i < nv; i++) {
            struct vol *v = fs_volume_at(i);
            if (fs_letter(v) || fs_serial(v) != serial)
                continue;
            if (!flushed) {
                struct v86_regs f = { .eax = 0x0D00 };  /* write and drop DOS's buffers */
                v86_int(0x21, &f);
                flushed = true;
            }
            take(d, v, true);
            klog("REDIR: %c: is the VMM's now (%s, serial %04X-%04X)\n", 'A' + d, fs_type_name(v),
                 serial >> 16, serial & 0xFFFF);
            break;
        }
    }
    /* the rest: free letters */
    for (int i = 0; i < nv; i++) {
        struct vol *v = fs_volume_at(i);
        if (fs_letter(v))
            continue;
        int d;
        for (d = 2; d < rd.lastdrive; d++)
            if (!(rd16(cds_of(d) + CDS_FLAGS) & 0xF000) && !rd.drive[d].used)
                break;
        if (d < rd.lastdrive) {
            take(d, v, false);
            klog("REDIR: %c: for the VMM's %s volume\n", 'A' + d, fs_type_name(v));
            continue;
        }
        for (d = rd.lastdrive; d < 26 && (fs_volume((char)('A' + d)) || d < 2); d++)
            ;
        if (d < 26) {
            fs_set_letter(v, (char)('A' + d));
            klog("REDIR: %c: for the VMM's %s volume (past LASTDRIVE: OS1993 programs only)\n",
                 'A' + d, fs_type_name(v));
        }
    }
    rd.on = true;
}

void redir_restore(void)
{
    if (!rd.on)
        return;
    for (int d = 0; d < 26; d++) {
        if (!rd.drive[d].used)
            continue;
        uint32_t c = cds_of(d);
        memcpy(at(c), rd.drive[d].saved, CDS_SIZE);
        uint32_t dpb = far_ptr(c + CDS_DPB);
        if (rd.drive[d].taken && dpb && dpb != far_lin(0xFFFF, 0xFFFF))
            *at(dpb + 0x18) = 0xFF;                     /* rebuild it from the disk */
    }
    rd.on = false;
}

void redir_vm_end(struct vm *vm)
{
    for (int i = 0; i < RFILE_MAX; i++)
        if (rfiles[i].n && rfiles[i].vm == vm) {
            fs_close(rfiles[i].n);
            rfiles[i].n = NULL;
        }
}

/* ---- the calls -------------------------------------------------------------- */

static int dos_error(int rc)                    /* a DOS error code for -rc */
{
    int e = -rc;
    switch (e) {
    case ERROR_INVALID_NAME:
    case ERROR_FILENAME_EXCED_RANGE:
        return ERROR_PATH_NOT_FOUND;
    case ERROR_DIR_NOT_EMPTY:
    case ERROR_DISK_FULL:
        return ERROR_ACCESS_DENIED;
    }
    return e > 0 && e <= 88 ? e : ERROR_ACCESS_DENIED;
}

static void ok(struct regs *r)
{
    r->eflags &= ~EFLAGS_CF;
}

static void fail(struct regs *r, int rc)
{
    r->eax = (r->eax & 0xFFFF0000u) | (uint16_t)dos_error(rc);
    r->eflags |= EFLAGS_CF;
}

static void set16(uint32_t *reg, uint32_t v) { *reg = (*reg & 0xFFFF0000u) | (v & 0xFFFF); }

/* "X:\DIR\FILE" -> the volume and "DIR\FILE" */
static struct vol *path_vol(uint32_t fn, const char **rest)
{
    const char *p = (const char *)at(fn);
    if (p[1] != ':' || p[2] != '\\')
        return NULL;
    struct vol *v = fs_volume(p[0]);
    *rest = p + 3;
    return v;
}

static int rfile_new(struct fnode *n, struct vm *vm)
{
    for (int i = 0; i < RFILE_MAX; i++)
        if (!rfiles[i].n) {
            rfiles[i].n = n;
            rfiles[i].vm = vm;
            return i;
        }
    return -1;
}

static struct fnode *sft_node(uint32_t sft, int *id)
{
    if (!(rd16(sft + SFT_FLAGS) & SF_NET))
        return NULL;
    int i = rd16(sft + SFT_ID) - 1;
    if (i < 0 || i >= RFILE_MAX || !rfiles[i].n)
        return NULL;
    *id = i;
    return rfiles[i].n;
}

static bool sft_ours(uint32_t sft)
{
    uint16_t f = rd16(sft + SFT_FLAGS);
    return (f & SF_NET) && fs_volume((char)('A' + (f & 0x1F)));
}

static void fill_sft(uint32_t sft, struct fnode *n, int id, char letter)
{
    uint16_t date, time;
    fs_get_time(n, &date, &time);
    *at(sft + SFT_ATTR) = fs_node_attr(n);
    wr16(sft + SFT_FLAGS, (uint16_t)(SF_NET | SF_CLEAN | (letter - 'A')));
    wr32(sft + SFT_DEV, 0);
    wr16(sft + SFT_ID, (uint16_t)(id + 1));
    wr16(sft + SFT_TIME, time);
    wr16(sft + SFT_DATE, date);
    wr32(sft + SFT_SIZE, fs_size(n));
    wr32(sft + SFT_POS, 0);
    wr16(sft + 0x19, 0xFFFF);
    wr16(sft + 0x1B, 0xFFFF);
    wr16(sft + 0x1D, 0);
    *at(sft + 0x1F) = 0xFF;
    fs_name_of(n, at(sft + SFT_NAME));
}

/* The last component of an 8.3 path as an FCB template ('*' fills with '?'). */
static void fcb_template(const char *name, uint8_t fcb[11])
{
    memset(fcb, ' ', 11);
    if (name[0] == '.') {                       /* "." and ".." */
        for (int i = 0; i < 2 && name[i] == '.'; i++)
            fcb[i] = '.';
        return;
    }
    int i = 0, n = 0;
    for (; name[i] && name[i] != '.'; i++) {
        if (name[i] == '*') {
            while (n < 8) fcb[n++] = '?';
        } else if (n < 8) {
            fcb[n++] = (uint8_t)name[i];
        }
    }
    if (name[i] == '.') {
        n = 8;
        for (i++; name[i]; i++) {
            if (name[i] == '*') {
                while (n < 11) fcb[n++] = '?';
            } else if (n < 11) {
                fcb[n++] = (uint8_t)name[i];
            }
        }
    }
}

static void put_found(const struct fs_entry *e)
{
    uint32_t d = rd.sda + SDA_FOUND;
    memset(at(d), 0, 32);
    memcpy(at(d), e->fcb_name, 11);
    *at(d + 0x0B) = e->attr;
    wr16(d + 0x16, e->time);
    wr16(d + 0x18, e->date);
    wr16(d + 0x1A, (uint16_t)e->cluster);
    wr32(d + 0x1C, e->size);
}

static void save_search(uint32_t blk, const struct fs_find *f, char letter)
{
    *at(blk) = (uint8_t)(0x80 | (letter - 'A'));
    memcpy(at(blk + 1), f->fcb, 11);
    *at(blk + 0x0C) = f->attr;
    wr16(blk + 0x0D, (uint16_t)f->next);
    wr32(blk + 0x0F, f->dir);
    wr16(blk + 0x13, (uint16_t)f->dirlen);
}

static int find_first(uint32_t fn, struct fs_find *f, struct fs_entry *e, struct vol **vp)
{
    const char *rest;
    struct vol *v = path_vol(fn, &rest);
    if (!v)
        return -ERROR_PATH_NOT_FOUND;
    char dir[128];
    const char *slash = strrchr(rest, '\\');
    const char *name = slash ? slash + 1 : rest;
    size_t dl = slash ? (size_t)(slash - rest) : 0;
    if (dl >= sizeof dir)
        return -ERROR_PATH_NOT_FOUND;
    memcpy(dir, rest, dl);
    dir[dl] = '\0';
    uint8_t fcb[11];
    fcb_template(name, fcb);
    *vp = v;
    return fs_find_dos(v, dir, fcb, *at(rd.sda + SDA_ATTR), f, e);
}

static int rw(struct regs *r, uint32_t sft, bool write)
{
    int id;
    struct fnode *n = sft_node(sft, &id);
    if (!n)
        return -ERROR_INVALID_HANDLE;
    uint32_t pos = rd32(sft + SFT_POS), len = r->ecx & 0xFFFF;
    uint8_t *buf = at(far_ptr(rd.sda + SDA_DTA));
    int rc;
    if (write) {
        rc = len ? fs_write(n, pos, buf, len) : fs_set_size(n, pos);
        if (rc == -ERROR_DISK_FULL)
            rc = 0;                                     /* DOS: fewer bytes, no error */
        wr32(sft + SFT_SIZE, fs_size(n));
        wr16(sft + SFT_FLAGS, rd16(sft + SFT_FLAGS) & ~SF_CLEAN);
        if (!len && rc == 0)
            rc = 0;
    } else {
        rc = fs_read(n, pos, buf, len);
    }
    if (rc < 0)
        return rc;
    wr32(sft + SFT_POS, pos + (uint32_t)rc);
    set16(&r->ecx, (uint32_t)rc);
    return 0;
}

static int open_call(struct vm *vm, struct regs *r, uint8_t al, uint16_t arg)
{
    uint32_t sft = far_lin((uint16_t)r->v86_es, (uint16_t)r->edi);
    const char *rest;
    struct vol *v = path_vol(rd.sda + SDA_FN1, &rest);
    if (!v)
        return -ERROR_PATH_NOT_FOUND;
    uint32_t disp;
    bool write;
    uint8_t attr = 0;
    uint16_t status = 0;
    if (al == 0x16) {                                   /* open: the mode */
        disp = OPEN_EXISTING;
        write = (arg & 3) != 0;
    } else if (al == 0x17) {                            /* create: the attributes */
        disp = (arg & 0x100) ? CREATE_NEW : CREATE_ALWAYS;
        write = true;
        attr = (uint8_t)arg;
    } else {                                            /* 2Eh: extended open */
        uint16_t action = rd16(rd.sda + SDA_XACTION);
        attr = *at(rd.sda + SDA_XATTR);
        write = (rd16(rd.sda + SDA_XMODE) & 3) != 0;
        struct fs_entry e;
        bool exists = fs_stat(v, rest, &e) == 0;
        if (exists) {
            if ((action & 0x0F) == 1)      { disp = OPEN_EXISTING; status = 1; }
            else if ((action & 0x0F) == 2) { disp = TRUNCATE_EXISTING; status = 3; write = true; }
            else                           return -ERROR_FILE_EXISTS;
        } else {
            if (!(action & 0x10))
                return -ERROR_FILE_NOT_FOUND;
            disp = CREATE_NEW;
            status = 2;
            write = true;
        }
    }
    struct fnode *n;
    int rc = fs_open(v, rest, disp, write, attr, &n, NULL);
    if (rc < 0)
        return rc;
    int id = rfile_new(n, vm);
    if (id < 0) {
        fs_close(n);
        return -ERROR_TOO_MANY_OPEN_FILES;
    }
    fill_sft(sft, n, id, fs_letter(v));
    set16(&r->ecx, status);
    return 0;
}

static void close_sft(uint32_t sft, struct fnode *n, int id, bool last)
{
    if (rd16(sft + SFT_FLAGS) & SF_DATESET) {
        fs_set_time(n, rd16(sft + SFT_DATE), rd16(sft + SFT_TIME));
        wr16(sft + SFT_FLAGS, rd16(sft + SFT_FLAGS) & ~SF_DATESET);
    }
    if (last) {
        fs_close(n);
        rfiles[id].n = NULL;
    } else {
        fs_flush(n);
    }
}

bool redir_int2f(struct vm *vm, struct regs *r)
{
    if (!rd.on)
        return false;
    uint8_t al = (uint8_t)r->eax;
    uint32_t es_di = far_lin((uint16_t)r->v86_es, (uint16_t)r->edi);
    uint16_t arg = *(uint16_t *)at(far_lin((uint16_t)r->user_ss, (uint16_t)r->user_esp));
    const char *rest = "", *rest2 = "";
    struct vol *v;
    int rc = 0;

    /* whose call is it? */
    switch (al) {
    case 0x01: case 0x03: case 0x05: case 0x0E: case 0x0F: case 0x11: case 0x13:
    case 0x16: case 0x17: case 0x2E: case 0x1B:
        if (!path_vol(rd.sda + SDA_FN1, &rest))
            return false;
        break;
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x21:
        if (!sft_ours(es_di))
            return false;
        break;
    case 0x1C:
        if (!(*at(es_di) & 0x80) || !fs_volume((char)('A' + (*at(es_di) & 0x1F))))
            return false;
        break;
    case 0x0C:
        if (!fs_volume(*(char *)at(es_di)) || !(rd16(es_di + CDS_FLAGS) & CDS_NETWORK))
            return false;
        break;
    default:
        return false;                                   /* not ours: on down the chain */
    }

    /* Traps come in with interrupts off; disk transfers take a while. */
    bool was_on = read_eflags() & 0x200;
    cpu_sti();
    switch (al) {
    case 0x01:                                          /* rmdir */
    case 0x03:                                          /* mkdir */
    case 0x05: {                                        /* chdir */
        v = path_vol(rd.sda + SDA_FN1, &rest);
        if (al == 0x05) {
            struct fs_entry e;
            rc = fs_stat(v, rest, &e);
            if (rc == 0 && !(e.attr & 0x10))
                rc = -ERROR_PATH_NOT_FOUND;
            if (rc == -ERROR_FILE_NOT_FOUND)
                rc = -ERROR_PATH_NOT_FOUND;
        } else if (al == 0x01) {
            uint32_t cds = far_ptr(rd.sda + SDA_CDS);
            const char *fn = (const char *)at(rd.sda + SDA_FN1);
            if (cds && strcmp((const char *)at(cds), fn) == 0)
                rc = -ERROR_CURRENT_DIRECTORY;
            else
                rc = fs_rmdir(v, rest);
        } else {
            rc = fs_mkdir(v, rest);
        }
        set16(&r->ecx, 0);
        break;
    }
    case 0x06:                                          /* close */
    case 0x07: {                                        /* commit */
        int id;
        struct fnode *n = sft_node(es_di, &id);
        if (!n) {
            rc = -ERROR_INVALID_HANDLE;
            break;
        }
        bool last = false;
        if (al == 0x06) {
            uint16_t count = rd16(es_di + SFT_COUNT);
            if (count)
                wr16(es_di + SFT_COUNT, --count);
            last = count == 0;
        }
        close_sft(es_di, n, id, last);
        set16(&r->ecx, 0);
        break;
    }
    case 0x08:
    case 0x09:
        rc = rw(r, es_di, al == 0x09);
        break;
    case 0x0A:                                          /* lock/unlock: no sharing here */
        break;
    case 0x0C: {                                        /* disk space */
        uint32_t spc, bps, freec, total;
        v = fs_volume(*(char *)at(es_di));
        if ((rc = fs_space(v, &spc, &bps, &freec, &total)) < 0)
            break;
        while (total > 0xFFFF && spc < 0x80) {         /* 16-bit fields: bigger clusters */
            spc *= 2;
            total /= 2;
            freec /= 2;
        }
        set16(&r->eax, spc);
        set16(&r->ebx, total > 0xFFFF ? 0xFFFF : total);
        set16(&r->ecx, bps);
        set16(&r->edx, freec > 0xFFFF ? 0xFFFF : freec);
        break;
    }
    case 0x0E:                                          /* set attributes */
        v = path_vol(rd.sda + SDA_FN1, &rest);
        rc = fs_set_attr(v, rest, (uint8_t)arg);
        set16(&r->ecx, 0);
        break;
    case 0x0F: {                                        /* get attributes */
        struct fs_entry e;
        v = path_vol(rd.sda + SDA_FN1, &rest);
        if ((rc = fs_stat(v, rest, &e)) < 0)
            break;
        set16(&r->eax, e.attr);
        set16(&r->ebx, e.size >> 16);
        set16(&r->edi, e.size);
        set16(&r->ecx, e.time);
        set16(&r->edx, e.date);
        break;
    }
    case 0x11:                                          /* rename */
        v = path_vol(rd.sda + SDA_FN1, &rest);
        if (path_vol(rd.sda + SDA_FN2, &rest2) != v)
            rc = -ERROR_NOT_SAME_DEVICE;
        else
            rc = fs_rename(v, rest, rest2);
        set16(&r->ecx, 0);
        break;
    case 0x13:                                          /* delete */
        v = path_vol(rd.sda + SDA_FN1, &rest);
        rc = fs_delete(v, rest);
        set16(&r->ecx, 0);
        break;
    case 0x16:                                          /* open */
    case 0x17:                                          /* create */
    case 0x2E:                                          /* extended open */
        rc = open_call(vm, r, al, arg);
        break;
    case 0x1B: {                                        /* find first */
        struct fs_find f;
        struct fs_entry e;
        uint32_t blk = far_ptr(rd.sda + SDA_DTA);
        rc = find_first(rd.sda + SDA_FN1, &f, &e, &v);
        if (rc == 0) {
            save_search(blk, &f, fs_letter(v));
            put_found(&e);
        }
        set16(&r->ecx, 0);
        break;
    }
    case 0x1C: {                                        /* find next */
        struct fs_find f;
        struct fs_entry e;
        memset(&f, 0, sizeof f);
        f.v = fs_volume((char)('A' + (*at(es_di) & 0x1F)));
        f.dos = true;
        memcpy(f.fcb, at(es_di + 1), 11);
        f.attr = *at(es_di + 0x0C);
        f.next = rd16(es_di + 0x0D);
        f.dir = rd32(es_di + 0x0F);
        f.dirlen = rd16(es_di + 0x13);
        rc = fs_find_next(&f, &e);
        if (rc == 0) {
            save_search(es_di, &f, fs_letter(f.v));
            put_found(&e);
        }
        set16(&r->ecx, 0);
        break;
    }
    case 0x21: {                                        /* seek from the end */
        int id;
        struct fnode *n = sft_node(es_di, &id);
        if (!n) {
            rc = -ERROR_INVALID_HANDLE;
            break;
        }
        int32_t off = (int32_t)((r->ecx & 0xFFFF) << 16 | (r->edx & 0xFFFF));
        int64_t pos = (int64_t)fs_size(n) + off;
        if (pos < 0)
            pos = 0;
        wr32(es_di + SFT_POS, (uint32_t)pos);
        set16(&r->eax, (uint32_t)pos);
        set16(&r->edx, (uint32_t)pos >> 16);
        break;
    }
    }
    if (!was_on)
        cpu_cli();
    if (rc < 0)
        fail(r, rc);
    else
        ok(r);
    return true;
}
