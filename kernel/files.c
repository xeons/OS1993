/*
 * files.c -- file access for ring-3 processes.
 *
 * Files are DOS's: the VMM console thread, which owns the System VM, makes
 * the INT 21h calls on a process's behalf (user.c queues the request and
 * installs the process's address space), moving data through the V86
 * transfer buffer. Every process has its own handle table, so its files
 * are closed when it ends however it ends, its own searches (each keeps a
 * copy of DOS's disk transfer area), and its own current directory, which
 * relative paths start from.
 *
 * A drive letter of the VMM's own (fs.h) skips DOS: the call goes to the
 * file system directly, long names and all, with the file pointer kept
 * here. Paths keep their case there; on DOS's drives they are upper case.
 */
#include "kernel.h"
#include "files.h"
#include "fs.h"
#include "mm.h"
#include "user.h"
#include "v86.h"

#define HANDLE_BASE 0x100
#define FIND_BASE   0x200
#define PATH2_OFF   256                 /* a second path in the transfer buffer */

static void copy_str(char *d, const char *s, size_t size)
{
    size_t n = 0;
    while (n + 1 < size && s[n]) { d[n] = s[n]; n++; }
    d[n] = '\0';
}

static uint8_t *xbuf(void) { return v86_ptr(v86_buf_seg(), v86_buf_off()); }

/* INT 21h; -> 0, or -(DOS error code) */
static int dos(struct v86_regs *r)
{
    if (v86_int(0x21, r) != 0)
        return -ERROR_ACCESS_DENIED;
    return (r->flags & EFLAGS_CF) ? -(int)(r->eax & 0xFFFF) : 0;
}

void files_init(struct proc_files *pf, const char *cwd)
{
    for (int i = 0; i < FILE_MAX; i++) {
        pf->dos[i] = -1;
        pf->nf[i] = NULL;
    }
    for (int i = 0; i < FIND_MAX; i++) {
        pf->find[i].used = false;
        pf->find[i].nat = NULL;
    }
    copy_str(pf->cwd, cwd, sizeof pf->cwd);
    static bool more_handles;
    if (!more_handles) {                /* every process's files share one PSP */
        struct v86_regs r = { .eax = 0x6700, .ebx = 40 };
        dos(&r);
        more_handles = true;
    }
}

void files_close_all(struct proc_files *pf)
{
    for (int i = 0; i < FILE_MAX; i++)
        if (pf->nf[i]) {
            fs_share_close(pf->nf[i], (uintptr_t)&pf->nf[i]);
            fs_close(pf->nf[i]);
            pf->nf[i] = NULL;
        }
    for (int i = 0; i < FIND_MAX; i++) {
        kfree(pf->find[i].nat);
        pf->find[i].nat = NULL;
        pf->find[i].used = false;
    }
    for (int i = 0; i < FILE_MAX; i++)
        if (pf->dos[i] >= 0) {
            struct v86_regs r = { .eax = 0x3E00, .ebx = (uint32_t)pf->dos[i] };
            dos(&r);
            pf->dos[i] = -1;
        }
}

/* ---- paths ------------------------------------------------------------- */

/* An absolute path, with "." and ".." taken out: "C:\OS1993\..\X" -> "C:\X" */
static bool normalize(char *p)
{
    if (!((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) || p[1] != ':' || p[2] != '\\')
        return false;
    if (p[0] >= 'a') p[0] = (char)(p[0] - 32);
    char out[PATH_MAX];
    int n = 3;
    memcpy(out, p, 3);
    const char *s = p + 3;
    while (*s) {
        const char *e = s;
        while (*e && *e != '\\') e++;
        int len = (int)(e - s);
        if (len == 0 || (len == 1 && s[0] == '.')) {
            /* nothing */
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 3 && out[n - 1] != '\\') n--;       /* back up a component */
            if (n > 3) n--;
        } else {
            if (n > 3) out[n++] = '\\';
            if (n + len >= PATH_MAX)
                return false;
            for (int i = 0; i < len; i++)
                out[n++] = s[i];
        }
        s = *e ? e + 1 : e;
    }
    out[n] = '\0';
    if (!fs_volume(out[0]))                             /* DOS's: upper case, short */
        for (char *c = out; *c; c++)
            if (*c >= 'a' && *c <= 'z')
                *c = (char)(*c - 32);
    copy_str(p, out, PATH_MAX);
    return true;
}

static bool resolve(const struct proc_files *pf, const char *in, char *out)
{
    size_t n = strlen(in);
    if (!n || n >= PATH_MAX - 4)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)in[i] < 32 || in[i] == '"' || in[i] == '|' || in[i] == '<' || in[i] == '>')
            return false;
    char tmp[PATH_MAX * 2];
    if (n >= 2 && in[1] == ':') {
        copy_str(tmp, in, sizeof tmp);
        if (in[2] != '\\') {                            /* "C:FILE": from the root */
            tmp[0] = in[0]; tmp[1] = ':'; tmp[2] = '\\';
            copy_str(tmp + 3, in + 2, sizeof tmp - 3);
        }
    } else if (in[0] == '\\') {
        tmp[0] = pf->cwd[0]; tmp[1] = ':';
        copy_str(tmp + 2, in, sizeof tmp - 2);
    } else {
        copy_str(tmp, pf->cwd, sizeof tmp);
        size_t c = strlen(tmp);
        if (tmp[c - 1] != '\\') tmp[c++] = '\\';
        copy_str(tmp + c, in, sizeof tmp - c);
    }
    if (strlen(tmp) >= PATH_MAX)
        return false;
    for (char *c = tmp; *c; c++)                        /* "/" works too */
        if (*c == '/')
            *c = '\\';
    copy_str(out, tmp, PATH_MAX);
    return normalize(out);
}

/* A user path, resolved and put in the transfer buffer at off. */
static int user_path(const struct proc_files *pf, const char *up, uint32_t off, char *out)
{
    char in[PATH_MAX];
    if (!up || !user_string((uint32_t)(uintptr_t)up, in, sizeof in))
        return -ERROR_NOACCESS;
    if (!resolve(pf, in, out))
        return -ERROR_INVALID_NAME;
    if (!fs_volume(out[0])) {
        if (strlen(out) >= DOS_PATH_MAX)
            return -ERROR_FILENAME_EXCED_RANGE;
        memcpy(xbuf() + off, out, strlen(out) + 1);
    }
    return 0;
}

/* The VMM's volume for a resolved path, and the path on it. */
static struct vol *native(const char *path, const char **rest)
{
    struct vol *v = fs_volume(path[0]);
    if (v)
        *rest = path + 3;
    return v;
}

static int attributes(uint32_t off)             /* of the path at off */
{
    struct v86_regs r = { .eax = 0x4300, .edx = v86_buf_off() + off, .ds = v86_buf_seg() };
    int rc = dos(&r);
    return rc < 0 ? rc : (int)(r.ecx & 0xFF);
}

/* ---- handles ------------------------------------------------------------- */

static int slot_of(const struct proc_files *pf, uint32_t h)
{
    int i = (int)h - HANDLE_BASE;
    return i >= 0 && i < FILE_MAX && (pf->dos[i] >= 0 || pf->nf[i]) ? i : -1;
}

static int find_of(const struct proc_files *pf, uint32_t h)
{
    int i = (int)h - FIND_BASE;
    return i >= 0 && i < FIND_MAX && pf->find[i].used ? i : -1;
}

static int seek(int dh, int32_t off, int method, uint32_t *pos)
{
    struct v86_regs r = { .eax = 0x4200u | (uint8_t)method, .ebx = (uint32_t)dh,
                          .ecx = ((uint32_t)off >> 16) & 0xFFFF, .edx = (uint32_t)off & 0xFFFF };
    int rc = dos(&r);
    if (rc == 0 && pos)
        *pos = ((r.edx & 0xFFFF) << 16) | (r.eax & 0xFFFF);
    return rc;
}

static int open_file(struct proc_files *pf, struct file_args *a)
{
    int slot = -1;
    for (int i = 0; i < FILE_MAX && slot < 0; i++)
        if (pf->dos[i] < 0 && !pf->nf[i])
            slot = i;
    if (slot < 0)
        return -ERROR_TOO_MANY_OPEN_FILES;
    char path[PATH_MAX];
    int rc = user_path(pf, a->path, 0, path);
    if (rc < 0)
        return rc;
    const char *rest;
    struct vol *v = native(path, &rest);
    if (v) {
        bool wr = a->a & GENERIC_WRITE;
        if ((rc = fs_open(v, rest, a->b, wr, 0, &pf->nf[slot], NULL)) < 0) {
            pf->nf[slot] = NULL;
            return rc;
        }
        if ((rc = fs_share_open(pf->nf[slot], (uintptr_t)&pf->nf[slot], (a->a & GENERIC_READ) || !wr, wr, 4)) < 0) {
            fs_close(pf->nf[slot]);                     /* a DOS program said deny */
            pf->nf[slot] = NULL;
            return rc;
        }
        pf->pos[slot] = 0;
        pf->wr[slot] = wr;
        return HANDLE_BASE + slot;
    }
    int attr = attributes(0);
    bool exists = attr >= 0;
    if (exists && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return -ERROR_ACCESS_DENIED;
    bool create;
    switch (a->b) {
    case CREATE_NEW:        if (exists) return -ERROR_FILE_EXISTS; create = true; break;
    case CREATE_ALWAYS:     create = true; break;
    case OPEN_EXISTING:     if (!exists) return -ERROR_FILE_NOT_FOUND; create = false; break;
    case OPEN_ALWAYS:       create = !exists; break;
    case TRUNCATE_EXISTING: if (!exists) return -ERROR_FILE_NOT_FOUND; create = true; break;
    default:                return -ERROR_INVALID_PARAMETER;
    }
    uint32_t mode = (a->a & GENERIC_WRITE) ? ((a->a & GENERIC_READ) ? 2 : 1) : 0;
    struct v86_regs r = { .eax = create ? 0x3C00 : 0x3D00 | mode, .ecx = 0,
                          .edx = v86_buf_off(), .ds = v86_buf_seg() };
    if ((rc = dos(&r)) < 0)
        return rc;
    pf->dos[slot] = (int16_t)(r.eax & 0xFFFF);
    return HANDLE_BASE + slot;
}

/* Read or write through the transfer buffer, a chunk at a time. */
static int transfer(struct proc_files *pf, struct file_args *a, bool write)
{
    int s = slot_of(pf, a->handle);
    if (s < 0)
        return -ERROR_INVALID_HANDLE;
    uint32_t ubuf = (uint32_t)(uintptr_t)a->buf;
    if (a->len && !(write ? user_range_ok(ubuf, a->len) : user_writable(ubuf, a->len)))
        return -ERROR_NOACCESS;
    if (pf->nf[s]) {
        if (write && !pf->wr[s])
            return -ERROR_ACCESS_DENIED;
        if (!a->len)
            return 0;
        int rc = fs_lock_check(pf->nf[s], (uintptr_t)&pf->nf[s], pf->pos[s], a->len);
        if (rc < 0)
            return rc;
        rc = write ? fs_write(pf->nf[s], pf->pos[s], (const void *)(uintptr_t)ubuf, a->len)
                       : fs_read(pf->nf[s], pf->pos[s], (void *)(uintptr_t)ubuf, a->len);
        if (rc > 0)
            pf->pos[s] += (uint32_t)rc;
        return rc;
    }
    uint32_t chunk = v86_buf_size(), done = 0;
    while (done < a->len) {
        uint32_t n = a->len - done < chunk ? a->len - done : chunk;
        if (write)
            memcpy(xbuf(), (const void *)(uintptr_t)(ubuf + done), n);
        struct v86_regs r = { .eax = write ? 0x4000 : 0x3F00, .ebx = (uint32_t)pf->dos[s],
                              .ecx = n, .edx = v86_buf_off(), .ds = v86_buf_seg() };
        int rc = dos(&r);
        if (rc < 0)
            return done ? (int)done : rc;
        uint32_t got = r.eax & 0xFFFF;
        if (!write)
            memcpy((void *)(uintptr_t)(ubuf + done), xbuf(), got);
        done += got;
        if (got < n)                                    /* end of file, disk full */
            break;
    }
    return (int)done;
}

static void put_find(const uint8_t *dta, struct find_data *fd)
{
    fd->attrib = dta[21];
    fd->time = (uint16_t)(dta[22] | dta[23] << 8);
    fd->date = (uint16_t)(dta[24] | dta[25] << 8);
    fd->size = (uint32_t)dta[26] | (uint32_t)dta[27] << 8 | (uint32_t)dta[28] << 16 |
               (uint32_t)dta[29] << 24;
    copy_str(fd->name, (const char *)dta + 30, 13);
    fd->short_name[0] = '\0';
}

/* FindFirst (pattern at PATH2_OFF) or FindNext, with DOS's DTA in the
 * transfer buffer for the call and the caller's DTA put back after. */
static int find(struct proc_files *pf, int slot, bool first, struct file_args *a)
{
    if (!user_writable((uint32_t)(uintptr_t)a->buf, sizeof(struct find_data)))
        return -ERROR_NOACCESS;
    struct v86_regs g = { .eax = 0x2F00 };              /* the DTA now */
    dos(&g);
    struct v86_regs set = { .eax = 0x1A00, .edx = v86_buf_off(), .ds = v86_buf_seg() };
    if (!first)
        memcpy(xbuf(), pf->find[slot].dta, 43);
    dos(&set);
    struct v86_regs r = { .eax = first ? 0x4E00 : 0x4F00, .ecx = FILE_ATTRIBUTE_DIRECTORY,
                          .edx = v86_buf_off() + PATH2_OFF, .ds = v86_buf_seg() };
    int rc = dos(&r);
    if (rc == 0)
        memcpy(pf->find[slot].dta, xbuf(), 43);
    struct v86_regs back = { .eax = 0x1A00, .edx = g.ebx & 0xFFFF, .ds = g.es };
    dos(&back);
    if (rc < 0)
        return rc;
    struct find_data fd;
    put_find(pf->find[slot].dta, &fd);
    memcpy(a->buf, &fd, sizeof fd);
    return 0;
}

/* ---- the VMM's own drives ----------------------------------------------- */

static int put_entry(const struct fs_entry *e, struct file_args *a)
{
    if (!user_writable((uint32_t)(uintptr_t)a->buf, sizeof(struct find_data)))
        return -ERROR_NOACCESS;
    struct find_data fd;
    memset(&fd, 0, sizeof fd);
    fd.attrib = e->attr;
    fd.size = (uint32_t)e->size;
    fd.size_high = (uint32_t)(e->size >> 32);
    fd.time = e->time;
    fd.date = e->date;
    copy_str(fd.name, e->name, sizeof fd.name);
    if (e->lfn)
        copy_str(fd.short_name, e->short_name, sizeof fd.short_name);
    memcpy(a->buf, &fd, sizeof fd);
    return 0;
}

/* *handled: the call was for one of the VMM's drives. */
static int native_op(struct proc_files *pf, uint32_t op, struct file_args *a, bool *handled)
{
    char path[PATH_MAX], path2[PATH_MAX];
    const char *rest, *rest2;
    struct vol *v = NULL;
    int rc, s;
    *handled = true;
    switch (op) {
    case FOP_CLOSE: case FOP_SEEK: case FOP_SIZE: case FOP_TRUNCATE: case FOP_FLUSH:
    case FOP_GETTIME: case FOP_SETTIME: case FOP_LOCK: case FOP_UNLOCK: case FOP_SEEK64:
    case FOP_SIZE64: {
        if ((s = slot_of(pf, a->handle)) < 0 || !pf->nf[s]) {
            *handled = false;
            return 0;
        }
        struct fnode *n = pf->nf[s];
        switch (op) {
        case FOP_CLOSE:
            fs_share_close(n, (uintptr_t)&pf->nf[s]);
            fs_close(n);
            pf->nf[s] = NULL;
            return 0;
        case FOP_LOCK:
        case FOP_UNLOCK:
            return fs_lock_range(n, (uintptr_t)&pf->nf[s], (uint64_t)a->c << 32 | a->a,
                                 (uint64_t)a->d << 32 | a->b, op == FOP_UNLOCK);
        case FOP_SEEK:
        case FOP_SEEK64: {
            int64_t base = a->b == FILE_BEGIN ? 0 : a->b == FILE_CURRENT ? (int64_t)pf->pos[s]
                         : a->b == FILE_END ? (int64_t)fs_size(n) : -1;
            if (base < 0)
                return -ERROR_INVALID_PARAMETER;
            int64_t dist = op == FOP_SEEK64 ? (int64_t)((uint64_t)a->c << 32 | a->a) : (int32_t)a->a;
            int64_t to = base + dist;
            if (to < 0)
                return -ERROR_NEGATIVE_SEEK;
            if (op == FOP_SEEK) {
                if (to > 0x7FFFFFFF)
                    return -ERROR_INVALID_PARAMETER;    /* SetFilePointerEx for that */
                pf->pos[s] = (uint64_t)to;
                return (int)to;
            }
            if (!user_writable((uint32_t)(uintptr_t)a->buf, 8))
                return -ERROR_NOACCESS;
            pf->pos[s] = (uint64_t)to;
            memcpy(a->buf, &pf->pos[s], 8);
            return 0;
        }
        case FOP_SIZE:
            return fs_size(n) > 0x7FFFFFFF ? -ERROR_INVALID_PARAMETER : (int)fs_size(n);
        case FOP_SIZE64: {
            uint64_t sz = fs_size(n);
            if (!user_writable((uint32_t)(uintptr_t)a->buf, 8))
                return -ERROR_NOACCESS;
            memcpy(a->buf, &sz, 8);
            return 0;
        }
        case FOP_TRUNCATE:
            return pf->wr[s] ? fs_set_size(n, pf->pos[s]) : -ERROR_ACCESS_DENIED;
        case FOP_FLUSH:
            return fs_flush(n);
        case FOP_GETTIME: {
            uint16_t d, t;
            fs_get_time(n, &d, &t);
            uint32_t when = (uint32_t)t << 16 | d;
            if (!user_writable((uint32_t)(uintptr_t)a->buf, 4))
                return -ERROR_NOACCESS;
            memcpy(a->buf, &when, 4);
            return 0;
        }
        default:
            return fs_set_time(n, (uint16_t)a->a, (uint16_t)(a->a >> 16));
        }
    }
    case FOP_FIND_NEXT:
    case FOP_FIND_CLOSE:
        if ((s = find_of(pf, a->handle)) < 0 || !pf->find[s].nat) {
            *handled = false;
            return 0;
        }
        if (op == FOP_FIND_CLOSE) {
            kfree(pf->find[s].nat);
            pf->find[s].nat = NULL;
            pf->find[s].used = false;
            return 0;
        } else {
            struct fs_entry e;
            if ((rc = fs_find_next(pf->find[s].nat, &e)) < 0)
                return rc;
            return put_entry(&e, a);
        }
    case FOP_DISKFREE:
        if (a->path) {
            if ((rc = user_path(pf, a->path, 0, path)) < 0)
                return rc;
        } else {
            copy_str(path, pf->cwd, sizeof path);
        }
        if (!(v = native(path, &rest))) {
            *handled = false;
            return 0;
        } else {
            struct disk_free df;
            if ((rc = fs_space(v, &df.sectors_per_cluster, &df.bytes_per_sector, &df.free_clusters,
                               &df.total_clusters)) < 0)
                return rc;
            if (!user_writable((uint32_t)(uintptr_t)a->buf, sizeof df))
                return -ERROR_NOACCESS;
            memcpy(a->buf, &df, sizeof df);
            return 0;
        }
    case FOP_DELETE: case FOP_MKDIR: case FOP_RMDIR: case FOP_RENAME: case FOP_ATTRIB:
    case FOP_SETCWD: case FOP_FIND_FIRST: case FOP_SETATTR: case FOP_LINK: case FOP_SYMLINK:
        if (!a->path || !user_string((uint32_t)(uintptr_t)a->path, path, sizeof path) ||
            !resolve(pf, path, path)) {
            *handled = false;                           /* DOS's path reports it */
            return 0;
        }
        if (!(v = native(path, &rest))) {
            if (op == FOP_LINK || op == FOP_SYMLINK)
                return -ERROR_NOT_SUPPORTED;            /* DOS's drives (FAT) have no links */
            if (op != FOP_RENAME) {
                *handled = false;
                return 0;
            }
        }
        break;
    default:
        *handled = false;
        return 0;
    }

    struct fs_entry e;
    switch (op) {
    case FOP_DELETE:
        return fs_delete(v, rest);
    case FOP_MKDIR:
        return fs_mkdir(v, rest);
    case FOP_RMDIR:
        if (strncmp(pf->cwd, path, strlen(path)) == 0 &&
            (pf->cwd[strlen(path)] == '\\' || !pf->cwd[strlen(path)]))
            return -ERROR_CURRENT_DIRECTORY;
        return fs_rmdir(v, rest);
    case FOP_RENAME:
        if (!a->path2 || !user_string((uint32_t)(uintptr_t)a->path2, path2, sizeof path2) ||
            !resolve(pf, path2, path2))
            return -ERROR_INVALID_NAME;
        if (!v) {
            if (!fs_volume(path2[0])) {
                *handled = false;                       /* DOS to DOS */
                return 0;
            }
            return -ERROR_NOT_SAME_DEVICE;
        }
        if (native(path2, &rest2) != v)
            return -ERROR_NOT_SAME_DEVICE;
        return fs_rename(v, rest, rest2);
    case FOP_LINK:
        if (!a->path2 || !user_string((uint32_t)(uintptr_t)a->path2, path2, sizeof path2) ||
            !resolve(pf, path2, path2))
            return -ERROR_INVALID_NAME;
        if (native(path2, &rest2) != v)
            return -ERROR_NOT_SAME_DEVICE;
        return fs_link(v, rest, rest2);
    case FOP_SYMLINK:                                   /* the target is kept as given */
        if (!a->path2 || !user_string((uint32_t)(uintptr_t)a->path2, path2, sizeof path2))
            return -ERROR_INVALID_NAME;
        return fs_symlink(v, rest, path2);
    case FOP_ATTRIB:
        if ((rc = fs_stat(v, rest, &e)) < 0)
            return rc;
        return e.attr;
    case FOP_SETATTR:
        return fs_set_attr(v, rest, (uint8_t)a->a);
    case FOP_SETCWD:
        if ((rc = fs_stat(v, rest, &e)) < 0)
            return rc == -ERROR_FILE_NOT_FOUND ? -ERROR_PATH_NOT_FOUND : rc;
        if (!(e.attr & FILE_ATTRIBUTE_DIRECTORY))
            return -ERROR_PATH_NOT_FOUND;
        copy_str(pf->cwd, path, sizeof pf->cwd);
        return 0;
    case FOP_FIND_FIRST: {
        for (s = 0; s < FIND_MAX && pf->find[s].used; s++)
            ;
        if (s == FIND_MAX)
            return -ERROR_TOO_MANY_OPEN_FILES;
        struct fs_find *f = kmalloc(sizeof *f);
        if (!f)
            return -ERROR_NOT_ENOUGH_MEMORY;
        char dir[PATH_MAX];
        const char *slash = strrchr(rest, '\\');
        const char *pattern = slash ? slash + 1 : rest;
        size_t dl = slash ? (size_t)(slash - rest) : 0;
        memcpy(dir, rest, dl);
        dir[dl] = '\0';
        if ((rc = fs_find_first(v, dir, *pattern ? pattern : "*", f, &e)) < 0 ||
            (rc = put_entry(&e, a)) < 0) {
            kfree(f);
            return rc;
        }
        pf->find[s].used = true;
        pf->find[s].nat = f;
        return FIND_BASE + s;
    }
    }
    *handled = false;
    return 0;
}

/* ---- the operations ----------------------------------------------------- */

int file_op(struct proc_files *pf, uint32_t op, struct file_args *a)
{
    char path[PATH_MAX], path2[PATH_MAX];
    int rc, s;
    bool handled;
    rc = native_op(pf, op, a, &handled);
    if (handled)
        return rc;
    struct v86_regs r = { .edx = v86_buf_off(), .ds = v86_buf_seg() };
    switch (op) {
    case FOP_OPEN:
        return open_file(pf, a);
    case FOP_READ:
        return transfer(pf, a, false);
    case FOP_WRITE:
        return transfer(pf, a, true);
    case FOP_CLOSE:
        if ((s = slot_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        r.eax = 0x3E00;
        r.ebx = (uint32_t)pf->dos[s];
        pf->dos[s] = -1;
        return dos(&r);
    case FOP_SEEK:
    case FOP_SEEK64: {
        uint32_t pos;
        if ((s = slot_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        if (a->b > FILE_END)
            return -ERROR_INVALID_PARAMETER;
        if (op == FOP_SEEK64 && (int32_t)a->c != ((int32_t)a->a >> 31))
            return -ERROR_INVALID_PARAMETER;            /* DOS's files stop at 4 GB */
        rc = seek(pf->dos[s], (int32_t)a->a, (int)a->b, &pos);
        if (rc < 0 || op == FOP_SEEK)
            return rc < 0 ? rc : (int)pos;
        uint64_t p64 = pos;
        if (!user_writable((uint32_t)(uintptr_t)a->buf, 8))
            return -ERROR_NOACCESS;
        memcpy(a->buf, &p64, 8);
        return 0;
    }
    case FOP_SIZE:
    case FOP_SIZE64: {
        uint32_t cur, end;
        if ((s = slot_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        if ((rc = seek(pf->dos[s], 0, FILE_CURRENT, &cur)) < 0 ||
            (rc = seek(pf->dos[s], 0, FILE_END, &end)) < 0)
            return rc;
        seek(pf->dos[s], (int32_t)cur, FILE_BEGIN, NULL);
        if (op == FOP_SIZE)
            return (int)end;
        uint64_t e64 = end;
        if (!user_writable((uint32_t)(uintptr_t)a->buf, 8))
            return -ERROR_NOACCESS;
        memcpy(a->buf, &e64, 8);
        return 0;
    }
    case FOP_DELETE:
    case FOP_MKDIR:
    case FOP_RMDIR:
        if ((rc = user_path(pf, a->path, 0, path)) < 0)
            return rc;
        r.eax = op == FOP_DELETE ? 0x4100 : op == FOP_MKDIR ? 0x3900 : 0x3A00;
        return dos(&r);
    case FOP_RENAME:
        if ((rc = user_path(pf, a->path, 0, path)) < 0 ||
            (rc = user_path(pf, a->path2, PATH2_OFF, path2)) < 0)
            return rc;
        r.eax = 0x5600;
        r.es = v86_buf_seg();
        r.edi = v86_buf_off() + PATH2_OFF;
        return dos(&r);
    case FOP_ATTRIB:
        if ((rc = user_path(pf, a->path, 0, path)) < 0)
            return rc;
        if (strlen(path) == 3)                          /* a drive's root */
            return FILE_ATTRIBUTE_DIRECTORY;
        return attributes(0);
    case FOP_GETCWD: {
        size_t n = strlen(pf->cwd) + 1;
        if (a->len < n || !user_writable((uint32_t)(uintptr_t)a->buf, (uint32_t)n))
            return -ERROR_NOACCESS;
        memcpy(a->buf, pf->cwd, n);
        return (int)n - 1;
    }
    case FOP_SETCWD:
        if ((rc = user_path(pf, a->path, 0, path)) < 0)
            return rc;
        if (strlen(path) > 3) {
            int attr = attributes(0);
            if (attr < 0)
                return attr;
            if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
                return -ERROR_PATH_NOT_FOUND;
        }
        copy_str(pf->cwd, path, sizeof pf->cwd);
        return 0;
    case FOP_FIND_FIRST:
        for (s = 0; s < FIND_MAX && pf->find[s].used; s++)
            ;
        if (s == FIND_MAX)
            return -ERROR_TOO_MANY_OPEN_FILES;
        if ((rc = user_path(pf, a->path, PATH2_OFF, path)) < 0)
            return rc;
        if ((rc = find(pf, s, true, a)) < 0)
            return rc;
        pf->find[s].used = true;
        return FIND_BASE + s;
    case FOP_FIND_NEXT:
        if ((s = find_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        return find(pf, s, false, a);
    case FOP_FIND_CLOSE:
        if ((s = find_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        pf->find[s].used = false;
        return 0;
    case FOP_TRUNCATE:                                  /* a write of 0 bytes */
    case FOP_FLUSH:
    case FOP_GETTIME:
    case FOP_SETTIME: {
        if ((s = slot_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        r.ebx = (uint32_t)pf->dos[s];
        if (op == FOP_TRUNCATE) {
            r.eax = 0x4000;
            r.ecx = 0;
        } else if (op == FOP_FLUSH) {
            r.eax = 0x6800;                             /* commit file (DOS 3.3) */
        } else {
            r.eax = op == FOP_GETTIME ? 0x5700 : 0x5701;
            r.ecx = a->a >> 16;
            r.edx = a->a & 0xFFFF;
        }
        if ((rc = dos(&r)) < 0)
            return rc;
        if (op == FOP_GETTIME) {
            uint32_t when = ((r.ecx & 0xFFFF) << 16) | (r.edx & 0xFFFF);
            if (!user_writable((uint32_t)(uintptr_t)a->buf, 4))
                return -ERROR_NOACCESS;
            memcpy(a->buf, &when, 4);
        }
        return 0;
    }
    case FOP_LOCK:
    case FOP_UNLOCK:                                    /* INT 21h AH=5Ch (SHARE, if loaded) */
        if ((s = slot_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        r = (struct v86_regs){ .eax = 0x5C00u | (op == FOP_UNLOCK), .ebx = (uint32_t)pf->dos[s],
                               .ecx = a->a >> 16, .edx = a->a & 0xFFFF, .esi = a->b >> 16, .edi = a->b & 0xFFFF };
        return dos(&r);
    case FOP_SETATTR:
        if ((rc = user_path(pf, a->path, 0, path)) < 0)
            return rc;
        r.eax = 0x4301;
        r.ecx = a->a & 0x27;                            /* read-only, hidden, system, archive */
        return dos(&r);
    case FOP_FULLPATH: {
        if ((rc = user_path(pf, a->path, 0, path)) < 0)
            return rc;
        size_t n = strlen(path) + 1;
        if (a->len < n || !user_writable((uint32_t)(uintptr_t)a->buf, (uint32_t)n))
            return a->len < n ? (int)n : -ERROR_NOACCESS;   /* too small: the size needed */
        memcpy(a->buf, path, n);
        return (int)n - 1;
    }
    case FOP_DISKFREE: {
        struct disk_free df;
        uint8_t drive = 0;                              /* the current one */
        if (a->path) {
            if ((rc = user_path(pf, a->path, 0, path)) < 0)
                return rc;
            drive = (uint8_t)(path[0] - 'A' + 1);
        } else {
            drive = (uint8_t)(pf->cwd[0] - 'A' + 1);
        }
        r = (struct v86_regs){ .eax = 0x3600, .edx = drive };
        if (v86_int(0x21, &r) != 0 || (r.eax & 0xFFFF) == 0xFFFF)
            return -ERROR_INVALID_PARAMETER;
        df = (struct disk_free){ r.eax & 0xFFFF, r.ecx & 0xFFFF, r.ebx & 0xFFFF, r.edx & 0xFFFF };
        if (!user_writable((uint32_t)(uintptr_t)a->buf, sizeof df))
            return -ERROR_NOACCESS;
        memcpy(a->buf, &df, sizeof df);
        return 0;
    }
    }
    return -ERROR_INVALID_PARAMETER;
}
