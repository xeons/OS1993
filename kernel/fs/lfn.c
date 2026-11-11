/*
 * lfn.c -- Windows 95's long-name calls for DOS programs (INT 21h
 * AX=71xxh), on the VMM's drives.
 *
 * The V86 monitor sends every INT 21h AH=71h here first. A call about one
 * of the VMM's drives is answered from the file system in the monitor --
 * or, when it must end in a DOS file handle or DOS's current directory
 * (716Ch open, 713Bh chdir), turned into the plain DOS call (6Ch, 3Bh)
 * with the 8.3 path, which DOS then sends to the redirector like any
 * other. A call about any other drive goes on to DOS, which answers
 * AX=7100h ("not supported"), and the program uses 8.3 names there: that
 * is how such programs behave on a DOS without long names.
 *
 * Paths may be relative, to DOS's current drive and its current directory
 * on that drive (8.3, from the CDS); the file system takes 8.3 and long
 * components alike. The rewritten calls get their path in the last 512
 * bytes of the VM's transfer buffer, which only the kernel's own calls in
 * the System VM use otherwise.
 *
 * Supported: 7139 mkdir, 713A rmdir, 713B chdir, 7141 delete (wildcards
 * with SI=1), 7143 attributes and times, 7147 current directory, 714E/
 * 714F/71A1 find, 7156 rename, 7160 full path, 716C open/create, 71A0
 * volume information, 71A6 file information by handle, 71A7 time
 * conversion, 71A8 short name.
 */
#include "kernel.h"
#include "fsimpl.h"
#include "interrupts.h"
#include "io.h"
#include "v86.h"
#include "vm.h"

#define FIND_DATA_SIZE 318

static struct { bool used; struct vm *vm; int slot; uint32_t stamp; uint8_t allow, need; bool dos_time; } handles[16];

#define ERROR_INVALID_FUNCTION 1
#define ERROR_INVALID_DRIVE    15

static bool wild_in(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return true;
    return false;
}

static uint8_t *at(uint32_t seg, uint32_t off) { return (uint8_t *)(uintptr_t)(((seg & 0xFFFF) << 4) + (off & 0xFFFF)); }
static void set16(uint32_t *reg, uint32_t v)   { *reg = (*reg & 0xFFFF0000u) | (v & 0xFFFF); }
static void put32(uint8_t *p, uint32_t v)      { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v)      { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get32(const uint8_t *p)        { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void lfn_vm_end(struct vm *vm)
{
    for (int i = 0; i < 16; i++)
        if (handles[i].used && handles[i].vm == vm) {
            redir_search_free(handles[i].slot);
            handles[i].used = false;
        }
}

/* A path from the caller -> "X:\FULL\PATH" (no "." or ".."). 0, or -error. */
static int resolve(const char *in, char *out)
{
    char tmp[600];
    int n = 0;
    size_t len = strlen(in);
    if (!len || len > 259)
        return -ERROR_PATH_NOT_FOUND;
    int drive;
    const char *rest;
    if (in[1] == ':') {
        drive = fs_up(in[0]) - 'A';
        rest = in + 2;
    } else {
        drive = redir_current_drive();
        rest = in;
    }
    if (drive < 0 || drive >= 26)
        return -ERROR_INVALID_DRIVE;
    if (rest[0] == '\\' || rest[0] == '/') {
        tmp[n++] = (char)('A' + drive);
        tmp[n++] = ':';
    } else {
        char cds[80];
        if (redir_cds_path(drive, cds) < 3 || cds[0] != 'A' + drive)
            ksnprintf(cds, sizeof cds, "%c:\\", 'A' + drive);     /* DOS's own idea, or the root */
        n = ksnprintf(tmp, sizeof tmp, "%s", cds);
        if (tmp[n - 1] != '\\')
            tmp[n++] = '\\';
    }
    for (const char *s = rest; *s && n < (int)sizeof tmp - 1; s++)
        tmp[n++] = *s == '/' ? '\\' : *s;
    tmp[n] = '\0';
    /* fold "." and ".." */
    int o = 3;
    memcpy(out, tmp, 3);
    out[2] = '\\';
    const char *s = tmp + 2;
    while (*s == '\\')
        s++;
    while (*s) {
        const char *e = s;
        while (*e && *e != '\\')
            e++;
        int l = (int)(e - s);
        if (l == 1 && s[0] == '.') {
        } else if (l == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 3 && out[o - 1] != '\\')
                o--;
            if (o > 3)
                o--;
        } else if (l) {
            if (o > 3)
                out[o++] = '\\';
            if (o + l >= 260)
                return -ERROR_FILENAME_EXCED_RANGE;
            memcpy(out + o, s, (size_t)l);
            o += l;
        }
        s = *e ? e + 1 : e;
    }
    out[o] = '\0';
    return 0;
}

/* The caller's path at seg:off, on one of our volumes? */
static struct vol *our_path(uint32_t seg, uint32_t off, char *full, const char **rest)
{
    char in[262];
    const char *p = (const char *)at(seg, off);
    int i = 0;
    while (i < 261 && p[i]) {
        in[i] = p[i];
        i++;
    }
    in[i] = '\0';
    if (resolve(in, full) < 0)
        return NULL;
    struct vol *v = fs_volume(full[0]);
    *rest = full + 3;
    return v;
}

static int dos_error(int rc)
{
    int e = -rc;
    if (e == ERROR_DIR_NOT_EMPTY || e == ERROR_DISK_FULL)
        return ERROR_ACCESS_DENIED;
    return e > 0 && e < 0x100 ? e : ERROR_ACCESS_DENIED;
}

static void finish(struct regs *r, int rc)
{
    if (rc < 0) {
        set16(&r->eax, (uint32_t)dos_error(rc));
        r->eflags |= EFLAGS_CF;
    } else {
        r->eflags &= ~EFLAGS_CF;
    }
}

static void put_time(uint8_t *p, uint16_t date, uint16_t time, bool dos)
{
    if (dos) {
        put32(p, (uint32_t)date << 16 | time);
        put32(p + 4, 0);
    } else {
        put64(p, fs_dos_to_filetime(date, time));
    }
}

static void fill_find(uint8_t *p, const struct fs_entry *e, bool dos_time)
{
    memset(p, 0, FIND_DATA_SIZE);
    put32(p, e->attr);
    put_time(p + 0x04, e->cdate ? e->cdate : e->date, e->cdate ? e->ctime : e->time, dos_time);
    put_time(p + 0x0C, e->adate ? e->adate : e->date, 0, dos_time);
    put_time(p + 0x14, e->date, e->time, dos_time);
    put32(p + 0x1C, (uint32_t)(e->size >> 32));
    put32(p + 0x20, (uint32_t)e->size);
    size_t n = strlen(e->name);
    memcpy(p + 0x2C, e->name, n + 1);
    if (e->lfn)
        memcpy(p + 0x130, e->short_name, strlen(e->short_name) + 1);
}

static bool attr_ok(uint8_t a, uint8_t allow, uint8_t need)
{
    return !(a & ~allow & 0x1E) && (a & need) == need;
}

/* The rewritten call runs as a little V86 routine in the last 512 bytes
 * of the VM's transfer buffer, with its 8.3 path beside it: it points
 * DS:reg at the path, calls DOS, puts DS and reg back and returns with
 * DOS's flags -- so the caller's registers are as a real call leaves them.
 * created: on success, CX says "created" (2), as 716Ch should. */
static uint16_t stub_off;

static void rewrite_path(struct regs *r, const char *path, bool ds_si, bool created)
{
    uint16_t base = (uint16_t)(v86_buf_off() + v86_buf_size() - 512), seg = v86_buf_seg();
    size_t n = strlen(path);
    memcpy(v86_ptr(seg, base), path, n + 1);
    uint8_t *c = v86_ptr(seg, (uint16_t)(base + 300));
    uint8_t reg = ds_si ? 6 : 2;                        /* SI or DX */
    int i = 0;
    c[i++] = 0x1E;                                      /* push ds */
    c[i++] = (uint8_t)(0x50 + reg);                     /* push si/dx */
    c[i++] = (uint8_t)(0xB8 + reg);                     /* mov si/dx, seg */
    c[i++] = (uint8_t)seg; c[i++] = (uint8_t)(seg >> 8);
    c[i++] = 0x8E; c[i++] = (uint8_t)(0xD8 + reg);      /* mov ds, si/dx */
    c[i++] = (uint8_t)(0xB8 + reg);                     /* mov si/dx, path */
    c[i++] = (uint8_t)base; c[i++] = (uint8_t)(base >> 8);
    c[i++] = 0xCD; c[i++] = 0x21;                       /* int 21h */
    if (created) {
        c[i++] = 0x72; c[i++] = 0x03;                   /* jc +3 */
        c[i++] = 0xB9; c[i++] = 0x02; c[i++] = 0x00;    /* mov cx, 2 */
    }
    c[i++] = (uint8_t)(0x58 + reg);                     /* pop si/dx */
    c[i++] = 0x1F;                                      /* pop ds */
    c[i++] = 0xCA; c[i++] = 0x02; c[i++] = 0x00;        /* retf 2: DOS's flags stay */
    stub_off = (uint16_t)(base + 300);
    (void)r;
}

uint16_t lfn_stub(uint16_t *seg)
{
    *seg = v86_buf_seg();
    return stub_off;
}

/* "X:\" + the 8.3 path of rest. */
static int short_full(struct vol *v, const char *full, const char *rest, char *out)
{
    out[0] = full[0];
    out[1] = ':';
    out[2] = '\\';
    out[3] = '\0';
    return *rest ? fs_short_path(v, rest, out + 3, 128 - 3) : 0;
}

static int find_next_match(int h, struct fs_entry *e)
{
    struct fs_find *f = redir_search(handles[h].slot, handles[h].stamp);
    if (!f)
        return -ERROR_NO_MORE_FILES;
    int rc;
    while ((rc = fs_find_next(f, e)) == 0 && !attr_ok(e->attr, handles[h].allow, handles[h].need))
        ;
    return rc;
}

int lfn_int21(struct vm *vm, struct regs *r)
{
    uint8_t al = (uint8_t)r->eax;
    char full[262], other[262], sh[128];
    const char *rest, *rest2;
    struct vol *v;
    int rc = 0;
    bool was_on = read_eflags() & 0x200;
    int result = LFN_DONE;

    /* Calls without a path, or whose drive is known up front */
    switch (al) {
    case 0x4F: case 0xA1: {                             /* find next, find close */
        int h = (int)(r->ebx & 0xFFFF) - 0x400;
        if (h < 0 || h >= 16 || !handles[h].used || handles[h].vm != vm)
            return LFN_PASS;
        cpu_sti();
        if (al == 0xA1) {
            redir_search_free(handles[h].slot);
            handles[h].used = false;
        } else {
            struct fs_entry e;
            if ((rc = find_next_match(h, &e)) == 0) {
                fill_find(at(r->v86_es, r->edi), &e, handles[h].dos_time);
                set16(&r->ecx, 0);
            }
        }
        goto out;
    }
    case 0xA6: {                                        /* information by handle */
        struct fnode *n;
        if (redir_handle_node((uint16_t)r->ebx, &n) < 0)
            return LFN_PASS;
        cpu_sti();
        uint8_t *p = at(r->v86_ds, r->edx);
        uint16_t d, t;
        fs_get_time(n, &d, &t);
        memset(p, 0, 52);
        put32(p, fs_node_attr(n));
        put64(p + 4, fs_dos_to_filetime(d, t));
        put64(p + 12, fs_dos_to_filetime(d, t));
        put64(p + 20, fs_dos_to_filetime(d, t));
        put32(p + 28, fs_serial(n->v));
        put32(p + 32, (uint32_t)(fs_size(n) >> 32));
        put32(p + 36, (uint32_t)fs_size(n));
        put32(p + 40, 1);                               /* links */
        put32(p + 48, (uint32_t)(uintptr_t)n);          /* a file index: the open file */
        goto out;
    }
    case 0xA7: {                                        /* time conversion */
        uint16_t d, t;
        if ((r->ebx & 0xFF) == 0) {                     /* FILETIME at DS:SI -> CX time, DX date */
            const uint8_t *p = at(r->v86_ds, r->esi);
            fs_filetime_to_dos((uint64_t)get32(p + 4) << 32 | get32(p), &d, &t);
            set16(&r->ecx, t);
            set16(&r->edx, d);
            r->ebx &= 0xFFFF00FFu;                      /* BH: 10 ms units */
        } else {                                        /* CX time, DX date -> FILETIME at ES:DI */
            put64(at(r->v86_es, r->edi), fs_dos_to_filetime((uint16_t)r->edx, (uint16_t)r->ecx));
        }
        r->eflags &= ~EFLAGS_CF;
        return LFN_DONE;
    }
    case 0xA8: {                                        /* the 8.3 name for a long one */
        char name[262];
        const char *p = (const char *)at(r->v86_ds, r->esi);
        int i = 0;
        while (i < 261 && p[i]) {
            name[i] = p[i];
            i++;
        }
        name[i] = '\0';
        uint8_t fcb[11];
        char alias[13];
        fs_make_alias(name, fs_hash16(name), fcb, alias);
        uint8_t *o = at(r->v86_es, r->edi);
        if (((r->edx >> 8) & 0xFF) == 0)
            memcpy(o, fcb, 11);
        else
            memcpy(o, alias, strlen(alias) + 1);
        r->eflags &= ~EFLAGS_CF;
        return LFN_DONE;
    }
    case 0x47: {                                        /* current directory, long */
        int d = (r->edx & 0xFF) ? (int)(r->edx & 0xFF) - 1 : redir_current_drive();
        char cds[80];
        if (d < 0 || redir_cds_path(d, cds) < 3 || !(v = fs_volume((char)('A' + d))))
            return LFN_PASS;
        cpu_sti();
        char *o = (char *)at(r->v86_ds, r->esi);
        o[0] = '\0';
        if (cds[3]) {                                   /* each 8.3 component -> its long name */
            char sofar[262] = "";
            const char *s = cds + 3;
            while (*s && rc == 0) {
                const char *e = s;
                while (*e && *e != '\\')
                    e++;
                size_t l = strlen(sofar);
                if (l)
                    sofar[l++] = '\\';
                memcpy(sofar + l, s, (size_t)(e - s));
                sofar[l + (size_t)(e - s)] = '\0';
                struct fs_entry fe;
                if ((rc = fs_stat(v, sofar, &fe)) == 0) {
                    size_t ol = strlen(o);
                    if (ol)
                        o[ol++] = '\\';
                    memcpy(o + ol, fe.name, strlen(fe.name) + 1);
                    sofar[l] = '\0';
                    memcpy(sofar + l, fe.name, strlen(fe.name) + 1);
                }
                s = *e ? e + 1 : e;
            }
        }
        goto out;
    }
    case 0xA0: {                                        /* volume information */
        if (!(v = our_path(r->v86_ds, r->edx, full, &rest)))
            return LFN_PASS;
        const char *type = fs_type_name(v);
        uint32_t size = r->ecx & 0xFFFF;
        if (size > strlen(type))
            memcpy(at(r->v86_es, r->edi), type, strlen(type) + 1);
        set16(&r->ebx, 0x4000 | 0x0002);                /* the LFN calls; case kept */
        set16(&r->ecx, 255);
        set16(&r->edx, 260);
        r->eflags &= ~EFLAGS_CF;
        return LFN_DONE;
    }
    }

    /* Calls with a path at DS:DX (or DS:SI) */
    uint32_t poff = (al == 0x60 || al == 0x6C) ? r->esi : r->edx;
    if (!(v = our_path(r->v86_ds, poff, full, &rest)))
        return LFN_PASS;
    cpu_sti();
    switch (al) {
    case 0x39:
        rc = fs_mkdir(v, rest);
        break;
    case 0x3A:
        rc = fs_rmdir(v, rest);
        break;
    case 0x3B: {                                        /* chdir: DOS keeps the 8.3 path */
        struct fs_entry e;
        if ((rc = fs_stat(v, rest, &e)) < 0 || !(e.attr & 0x10)) {
            rc = rc < 0 ? -ERROR_PATH_NOT_FOUND : -ERROR_PATH_NOT_FOUND;
            break;
        }
        if ((rc = short_full(v, full, rest, sh)) < 0)
            break;
        rewrite_path(r, sh, false, false);
        r->eax = (r->eax & 0xFFFF0000u) | 0x3B00;
        result = LFN_REWRITE;
        break;
    }
    case 0x41: {                                        /* delete; SI=1: wildcards */
        if ((r->esi & 0xFFFF) == 0 || !wild_in(rest)) {
            rc = fs_delete(v, rest);
            break;
        }
        uint8_t allow = (uint8_t)r->ecx, need = (uint8_t)(r->ecx >> 8);
        const char *slash = strrchr(rest, '\\');
        char dir[262];
        size_t dl = slash ? (size_t)(slash - rest) : 0;
        memcpy(dir, rest, dl);
        dir[dl] = '\0';
        static struct fs_find f;
        struct fs_entry e;
        int deleted = 0;
        rc = fs_find_first(v, dir, slash ? slash + 1 : rest, &f, &e);
        while (rc == 0) {
            if (!(e.attr & 0x10) && attr_ok(e.attr, allow, need)) {
                char path[262];
                ksnprintf(path, sizeof path, "%s%s%s", dir, dl ? "\\" : "", e.name);
                if (fs_delete(v, path) == 0)
                    deleted++;
            }
            rc = fs_find_next(&f, &e);
        }
        rc = deleted ? 0 : -ERROR_FILE_NOT_FOUND;
        break;
    }
    case 0x43: {                                        /* attributes and times */
        uint8_t bl = (uint8_t)r->ebx;
        struct fs_entry e;
        if (bl == 1) {
            rc = fs_set_attr(v, rest, (uint8_t)r->ecx);
            break;
        }
        if ((rc = fs_stat(v, rest, &e)) < 0)
            break;
        switch (bl) {
        case 0: set16(&r->ecx, e.attr); break;
        case 2: set16(&r->eax, (uint32_t)e.size); set16(&r->edx, (uint32_t)e.size >> 16); break;
        case 4: set16(&r->ecx, e.time); set16(&r->edi, e.date); break;
        case 6: set16(&r->edi, e.adate ? e.adate : e.date); break;
        case 8: set16(&r->ecx, e.ctime); set16(&r->edi, e.cdate); set16(&r->esi, 0); break;
        case 3: {                                       /* set the last write time: open it */
            struct fnode *n;
            if ((rc = fs_open(v, rest, OPEN_EXISTING, false, 0, &n, NULL)) == 0) {
                rc = fs_set_time(n, (uint16_t)r->edi, (uint16_t)r->ecx);
                fs_close(n);
            }
            break;
        }
        case 5: case 7:
            break;                                      /* kept as they are */
        default:
            rc = -ERROR_INVALID_FUNCTION;
        }
        break;
    }
    case 0x4E: {                                        /* find first */
        int h;
        for (h = 0; h < 16 && handles[h].used; h++)
            ;
        if (h == 16) {
            rc = -ERROR_NO_MORE_FILES;
            break;
        }
        uint32_t stamp;
        int slot = redir_search_new(vm, &stamp);
        if (slot < 0) {
            rc = -ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        handles[h] = (typeof(handles[0])){ true, vm, slot, stamp, (uint8_t)r->ecx, (uint8_t)(r->ecx >> 8),
                                           (r->esi & 0xFFFF) == 1 };
        const char *slash = strrchr(rest, '\\');
        char dir[262];
        size_t dl = slash ? (size_t)(slash - rest) : 0;
        memcpy(dir, rest, dl);
        dir[dl] = '\0';
        struct fs_find *f = redir_search(slot, stamp);
        struct fs_entry e;
        rc = fs_find_first(v, dir, slash ? slash + 1 : rest, f, &e);
        while (rc == 0 && !attr_ok(e.attr, handles[h].allow, handles[h].need))
            rc = fs_find_next(f, &e);
        if (rc < 0) {
            redir_search_free(slot);
            handles[h].used = false;
            if (rc == -ERROR_NO_MORE_FILES)
                rc = -ERROR_FILE_NOT_FOUND;
            break;
        }
        fill_find(at(r->v86_es, r->edi), &e, handles[h].dos_time);
        set16(&r->eax, 0x400 + (uint32_t)h);
        set16(&r->ecx, 0);
        break;
    }
    case 0x56:                                          /* rename DS:DX -> ES:DI */
        if (our_path(r->v86_es, r->edi, other, &rest2) != v) {
            rc = -ERROR_NOT_SAME_DEVICE;
            break;
        }
        rc = fs_rename(v, rest, rest2);
        break;
    case 0x60: {                                        /* full path: CL 0 as given, 1 short, 2 long */
        uint8_t cl = (uint8_t)r->ecx;
        char *o = (char *)at(r->v86_es, r->edi);
        if (cl == 1) {
            if ((rc = short_full(v, full, rest, sh)) == 0)
                memcpy(o, sh, strlen(sh) + 1);
        } else if (cl == 2) {
            char longp[262];
            int n = 3;
            memcpy(longp, full, 3);
            const char *s = rest;
            char sofar[262] = "";
            while (*s && rc == 0) {
                const char *e = s;
                while (*e && *e != '\\')
                    e++;
                size_t l = strlen(sofar);
                if (l)
                    sofar[l++] = '\\';
                memcpy(sofar + l, s, (size_t)(e - s));
                sofar[l + (size_t)(e - s)] = '\0';
                struct fs_entry fe;
                if ((rc = fs_stat(v, sofar, &fe)) == 0) {
                    if (n > 3)
                        longp[n++] = '\\';
                    size_t nl = strlen(fe.name);
                    if (n + (int)nl >= 260) {
                        rc = -ERROR_FILENAME_EXCED_RANGE;
                        break;
                    }
                    memcpy(longp + n, fe.name, nl);
                    n += (int)nl;
                    sofar[l] = '\0';
                    memcpy(sofar + l, fe.name, nl + 1);
                }
                s = *e ? e + 1 : e;
            }
            longp[n] = '\0';
            if (rc == 0)
                memcpy(o, longp, (size_t)n + 1);
        } else {
            memcpy(o, full, strlen(full) + 1);
        }
        break;
    }
    case 0x6C: {                                        /* open or create: DOS makes the handle */
        uint16_t action = (uint16_t)r->edx;
        struct fs_entry e;
        bool exists = fs_stat(v, rest, &e) == 0;
        if (exists && (e.attr & 0x10)) {
            rc = -ERROR_ACCESS_DENIED;
            break;
        }
        bool made = false;
        if (!exists && (action & 0x10)) {               /* make it here, with its long name */
            struct fnode *n;
            if ((rc = fs_open(v, rest, CREATE_NEW, true, (uint8_t)r->ecx, &n, NULL)) < 0)
                break;
            fs_close(n);
            action = 0x01;                              /* DOS opens what is now there */
            made = true;
        }
        if ((rc = short_full(v, full, rest, sh)) < 0) {
            if (!exists && !(action & 0x10))
                rc = -ERROR_FILE_NOT_FOUND;
            break;
        }
        rewrite_path(r, sh, true, made);
        r->eax = (r->eax & 0xFFFF0000u) | 0x6C00;
        set16(&r->edx, action);
        result = LFN_REWRITE;
        break;
    }
    default:
        rc = -ERROR_INVALID_FUNCTION;
    }
out:
    if (!was_on)
        cpu_cli();
    if (result == LFN_REWRITE && rc == 0)
        return LFN_REWRITE;
    finish(r, rc);
    return LFN_DONE;
}
