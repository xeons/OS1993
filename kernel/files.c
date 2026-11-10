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
 */
#include "kernel.h"
#include "files.h"
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
    for (int i = 0; i < FILE_MAX; i++)
        pf->dos[i] = -1;
    for (int i = 0; i < FIND_MAX; i++)
        pf->find[i].used = false;
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
            for (int i = 0; i < len; i++) {
                char c = s[i];
                out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
        }
        s = *e ? e + 1 : e;
    }
    out[n] = '\0';
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
    memcpy(xbuf() + off, out, strlen(out) + 1);
    return 0;
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
    return i >= 0 && i < FILE_MAX && pf->dos[i] >= 0 ? i : -1;
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
        if (pf->dos[i] < 0)
            slot = i;
    if (slot < 0)
        return -ERROR_TOO_MANY_OPEN_FILES;
    char path[PATH_MAX];
    int rc = user_path(pf, a->path, 0, path);
    if (rc < 0)
        return rc;
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
    copy_str(fd->name, (const char *)dta + 30, sizeof fd->name);
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

/* ---- the operations ----------------------------------------------------- */

int file_op(struct proc_files *pf, uint32_t op, struct file_args *a)
{
    char path[PATH_MAX], path2[PATH_MAX];
    int rc, s;
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
    case FOP_SEEK: {
        uint32_t pos;
        if ((s = slot_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        if (a->b > FILE_END)
            return -ERROR_INVALID_PARAMETER;
        rc = seek(pf->dos[s], (int32_t)a->a, (int)a->b, &pos);
        return rc < 0 ? rc : (int)pos;
    }
    case FOP_SIZE: {
        uint32_t cur, end;
        if ((s = slot_of(pf, a->handle)) < 0)
            return -ERROR_INVALID_HANDLE;
        if ((rc = seek(pf->dos[s], 0, FILE_CURRENT, &cur)) < 0 ||
            (rc = seek(pf->dos[s], 0, FILE_END, &end)) < 0)
            return rc;
        seek(pf->dos[s], (int32_t)cur, FILE_BEGIN, NULL);
        return (int)end;
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
    }
    return -ERROR_INVALID_PARAMETER;
}
