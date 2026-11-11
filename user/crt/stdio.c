/*
 * stdio.c -- buffered files over KERNEL.DLL's (CreateFile, ReadFile ...).
 *
 * A FILE has one buffer, used for reading or for writing, whichever it did
 * last: switching flushes what was written, or gives back what was read
 * ahead (a seek). stdout and stderr are the debug log: each line becomes a
 * log line. stdin reads nothing (there is no console).
 */
#include "crt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os1993.h"

enum { K_FILE, K_LOG, K_NONE };
enum { M_READ = 1, M_WRITE = 2, M_APPEND = 4 };
enum { S_EOF = 1, S_ERR = 2 };

struct __crt_file {
    bool     used;
    uint8_t  kind;
    uint8_t  mode;                      /* M_* */
    uint8_t  state;                     /* S_* */
    uint8_t  last;                      /* 0, M_READ or M_WRITE: what the buffer holds */
    uint8_t  buffering;                 /* _IOFBF, _IOLBF, _IONBF */
    HANDLE   h;
    int      unget;                     /* a pushed-back character, or EOF */
    size_t   pos, len;                  /* reading: next byte, bytes read ahead; writing: len unused */
    uint8_t  buf[BUFSIZ];
};

static struct __crt_file files[FOPEN_MAX];

static struct __crt_file *std_file(int fd)
{
    struct __crt_file *f = &files[fd];
    if (!f->used) {
        f->used = true;
        f->kind = fd ? K_LOG : K_NONE;
        f->mode = fd ? M_WRITE : M_READ;
        f->buffering = fd == 2 ? _IONBF : _IOLBF;
        f->h = INVALID_HANDLE_VALUE;
        f->unget = EOF;
    }
    return f;
}

FILE *__crt_stdio(int fd)
{
    return fd >= 0 && fd <= 2 ? std_file(fd) : NULL;
}

static void set_win_errno(void) { errno = __crt_errno_from_win(GetLastError()); }

/* A console window (SHELL.DLL's RunConsoleProgram) also gets stdout. */
static void (*output_hook)(const char *s, size_t n);

void __crt_set_output(void (*fn)(const char *s, size_t n)) { output_hook = fn; }

/* ---- the buffer ---------------------------------------------------------------- */

static void log_out(const uint8_t *s, size_t n)    /* one debug log line per call */
{
    char line[160];
    while (n) {
        size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, s, k);
        line[k] = '\0';
        OutputDebugString(line);
        s += k;
        n -= k;
    }
}

static int flush_write(FILE *f)
{
    if (f->last != M_WRITE)
        return 0;
    size_t n = f->pos;
    f->pos = 0;
    f->last = 0;
    if (!n)
        return 0;
    if (f->kind == K_LOG) {
        if (output_hook)
            output_hook((const char *)f->buf, n);
        if (f->buf[n - 1] == '\n')                  /* the log adds its own line end */
            n--;
        log_out(f->buf, n);
        return 0;
    }
    if (f->mode & M_APPEND)
        SetFilePointer(f->h, 0, FILE_END);
    uint32_t done = 0;
    if (!WriteFile(f->h, f->buf, (uint32_t)n, &done) || done != n) {
        if (!GetLastError())
            errno = ENOSPC;
        else
            set_win_errno();
        f->state |= S_ERR;
        return EOF;
    }
    return 0;
}

static void drop_read(FILE *f)                     /* give back what was read ahead */
{
    if (f->last == M_READ) {
        int32_t ahead = (int32_t)(f->len - f->pos) + (f->unget != EOF);
        if (ahead && f->kind == K_FILE)
            SetFilePointer(f->h, -ahead, FILE_CURRENT);
    }
    f->pos = f->len = 0;
    f->unget = EOF;
    f->last = 0;
}

static bool start(FILE *f, int how)
{
    if (!(f->mode & how)) {
        errno = EBADF;
        f->state |= S_ERR;
        return false;
    }
    if (f->last != how) {
        if (how == M_READ && flush_write(f))
            return false;
        if (how == M_WRITE)
            drop_read(f);
        f->last = (uint8_t)how;
    }
    return true;
}

static bool fill(FILE *f)
{
    if (f->kind != K_FILE) {
        f->state |= S_EOF;
        return false;
    }
    uint32_t got = 0;
    if (!ReadFile(f->h, f->buf, BUFSIZ, &got)) {
        set_win_errno();
        f->state |= S_ERR;
        return false;
    }
    f->pos = 0;
    f->len = got;
    if (!got)
        f->state |= S_EOF;
    return got != 0;
}

/* ---- opening and closing ------------------------------------------------------------ */

static bool parse_mode(const char *m, uint8_t *mode, uint32_t *access, uint32_t *creation)
{
    bool plus = strchr(m, '+') != NULL, excl = strchr(m, 'x') != NULL;
    switch (m[0]) {
    case 'r': *mode = M_READ;              *creation = OPEN_EXISTING; break;
    case 'w': *mode = M_WRITE;             *creation = excl ? CREATE_NEW : CREATE_ALWAYS; break;
    case 'a': *mode = M_WRITE | M_APPEND;  *creation = OPEN_ALWAYS; break;
    default:  return false;
    }
    for (const char *p = m + 1; *p; p++)
        if (*p != '+' && *p != 'b' && *p != 't' && !(*p == 'x' && m[0] == 'w'))
            return false;
    if (plus)
        *mode |= M_READ | M_WRITE;
    *access = (*mode & M_READ ? GENERIC_READ : 0) | (*mode & M_WRITE ? GENERIC_WRITE : 0);
    return true;
}

FILE *fopen(const char *restrict path, const char *restrict mode)
{
    uint8_t m;
    uint32_t access, creation;
    if (!path || !mode || !parse_mode(mode, &m, &access, &creation)) {
        errno = EINVAL;
        return NULL;
    }
    __crt_lock(CRT_LOCK_FILES);
    struct __crt_file *f = NULL;
    for (int i = 3; i < FOPEN_MAX && !f; i++)
        if (!files[i].used)
            f = &files[i];
    if (!f) {
        __crt_unlock(CRT_LOCK_FILES);
        errno = EMFILE;
        return NULL;
    }
    f->used = true;
    __crt_unlock(CRT_LOCK_FILES);
    HANDLE h = CreateFile(path, access, creation);
    if (h == INVALID_HANDLE_VALUE) {
        set_win_errno();
        f->used = false;
        return NULL;
    }
    if (m & M_APPEND)
        SetFilePointer(h, 0, FILE_END);
    f->kind = K_FILE;
    f->mode = m;
    f->state = f->last = 0;
    f->buffering = _IOFBF;
    f->h = h;
    f->unget = EOF;
    f->pos = f->len = 0;
    return f;
}

errno_t fopen_s(FILE *restrict *restrict out, const char *restrict path, const char *restrict mode)
{
    if (!out || !path || !mode) {
        if (out) *out = NULL;
        __crt_constraint("fopen_s: a null argument", EINVAL);
        return EINVAL;
    }
    *out = fopen(path, mode);
    return *out ? 0 : errno;
}

static int fflush_unlocked(FILE *f)
{
    if (!f) {                                       /* all of them */
        int r = 0;
        for (int i = 0; i < FOPEN_MAX; i++)
            if (files[i].used && files[i].last == M_WRITE && fflush(&files[i]))
                r = EOF;
        return r;
    }
    if (f->last == M_READ) {
        drop_read(f);
        return 0;
    }
    return flush_write(f);
}

static int fclose_unlocked(FILE *f)
{
    if (!f || !f->used)
        return EOF;
    int r = fflush(f);
    if (f->kind == K_FILE && !CloseHandle(f->h)) {
        set_win_errno();
        r = EOF;
    }
    if (f - files < 3) {                            /* the standard streams stay usable */
        f->state = 0;
        return r;
    }
    memset(f, 0, sizeof *f);
    return r;
}

int setvbuf(FILE *restrict f, char *restrict buf, int mode, size_t size)
{
    (void)buf; (void)size;                          /* the FILE's own buffer is used */
    if (!f || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF))
        return -1;
    fflush(f);
    f->buffering = (uint8_t)mode;
    return 0;
}

/* ---- reading -------------------------------------------------------------------- */

static int fgetc_unlocked(FILE *f)
{
    if (!start(f, M_READ))
        return EOF;
    if (f->unget != EOF) {
        int c = f->unget;
        f->unget = EOF;
        return c;
    }
    if (f->pos == f->len && !fill(f))
        return EOF;
    return f->buf[f->pos++];
}

int getc(FILE *f)     { return fgetc(f); }
int getchar(void)     { return fgetc(stdin); }

static int ungetc_unlocked(int c, FILE *f)
{
    if (c == EOF || !start(f, M_READ) || f->unget != EOF)
        return EOF;
    f->unget = (uint8_t)c;
    f->state &= (uint8_t)~S_EOF;
    return (uint8_t)c;
}

static size_t fread_unlocked(void *restrict p, size_t size, size_t n, FILE *restrict f)
{
    if (!size || !n)
        return 0;
    if (n > RSIZE_MAX / size) {
        errno = EINVAL;
        return 0;
    }
    uint8_t *out = p;
    size_t want = size * n, got = 0;
    if (!start(f, M_READ))
        return 0;
    if (f->unget != EOF) {
        out[got++] = (uint8_t)f->unget;
        f->unget = EOF;
    }
    while (got < want) {
        if (f->pos == f->len) {
            if (f->kind == K_FILE && want - got >= BUFSIZ) {    /* big: straight in */
                uint32_t k = 0;
                if (!ReadFile(f->h, out + got, (uint32_t)(want - got), &k)) {
                    set_win_errno();
                    f->state |= S_ERR;
                    break;
                }
                if (!k) {
                    f->state |= S_EOF;
                    break;
                }
                got += k;
                continue;
            }
            if (!fill(f))
                break;
        }
        size_t k = f->len - f->pos < want - got ? f->len - f->pos : want - got;
        memcpy(out + got, f->buf + f->pos, k);
        f->pos += k;
        got += k;
    }
    return got / size;
}

static char *fgets_unlocked(char *restrict s, int n, FILE *restrict f)
{
    if (!s || n <= 0)
        return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (!i || (f->state & S_ERR))
        return NULL;
    s[i] = '\0';
    return s;
}

char *gets_s(char *s, rsize_t n)
{
    if (!s || !n || n > RSIZE_MAX) {
        __crt_constraint("gets_s: s is null or n is 0 or too big", EINVAL);
        return NULL;
    }
    s[0] = '\0';
    return NULL;                                    /* stdin is always at end of file */
}

/* ---- writing ---------------------------------------------------------------------- */

static size_t put_bytes(FILE *f, const uint8_t *p, size_t n)
{
    if (!start(f, M_WRITE))
        return 0;
    size_t done = 0;
    while (done < n) {
        if (f->pos == BUFSIZ && flush_write(f))
            return done;
        f->last = M_WRITE;
        size_t k = BUFSIZ - f->pos < n - done ? BUFSIZ - f->pos : n - done;
        if (f->buffering == _IOLBF || f->kind == K_LOG) {     /* stop after a line end */
            const uint8_t *nl = memchr(p + done, '\n', k);
            if (nl)
                k = (size_t)(nl - (p + done)) + 1;
        }
        memcpy(f->buf + f->pos, p + done, k);
        f->pos += k;
        done += k;
        if ((f->buffering != _IOFBF && f->buf[f->pos - 1] == '\n') || f->buffering == _IONBF)
            if (flush_write(f))
                return done;
    }
    return done;
}

static size_t fwrite_unlocked(const void *restrict p, size_t size, size_t n, FILE *restrict f)
{
    if (!size || !n)
        return 0;
    if (n > RSIZE_MAX / size) {
        errno = EINVAL;
        return 0;
    }
    return put_bytes(f, p, size * n) / size;
}

static int fputc_unlocked(int c, FILE *f)
{
    uint8_t b = (uint8_t)c;
    return put_bytes(f, &b, 1) == 1 ? b : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

static int fputs_unlocked(const char *restrict s, FILE *restrict f)
{
    size_t n = strlen(s);
    return put_bytes(f, (const uint8_t *)s, n) == n ? 0 : EOF;
}

int puts(const char *s)
{
    return fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF ? EOF : 0;
}

/* ---- positions ------------------------------------------------------------------- */

static long ftell_unlocked(FILE *f)
{
    if (!f || f->kind != K_FILE) {
        errno = ESPIPE;
        return -1;
    }
    uint32_t at = SetFilePointer(f->h, 0, FILE_CURRENT);
    if (at == ~0u) {
        set_win_errno();
        return -1;
    }
    if (f->last == M_READ)
        return (long)(at - (f->len - f->pos) - (f->unget != EOF));
    if (f->last == M_WRITE && !(f->mode & M_APPEND))
        return (long)(at + f->pos);
    if (f->last == M_WRITE)
        return (long)(GetFileSize(f->h) + f->pos);
    return (long)at;
}

static int fseek_unlocked(FILE *f, long off, int whence)
{
    if (!f || f->kind != K_FILE) {
        errno = ESPIPE;
        return -1;
    }
    if (whence == SEEK_CUR) {                       /* relative to what the caller has seen */
        long at = ftell(f);
        if (at < 0)
            return -1;
        off += at;
        whence = SEEK_SET;
    }
    if (fflush(f))
        return -1;
    if (whence != SEEK_SET && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    if (SetFilePointer(f->h, (int32_t)off, whence == SEEK_SET ? FILE_BEGIN : FILE_END) == ~0u) {
        set_win_errno();
        return -1;
    }
    f->state &= (uint8_t)~S_EOF;
    return 0;
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    f->state = 0;
}

int fgetpos(FILE *restrict f, fpos_t *restrict pos)
{
    long at = ftell(f);
    if (at < 0)
        return -1;
    *pos = at;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos) { return fseek(f, *pos, SEEK_SET); }

int  feof(FILE *f)     { return (f->state & S_EOF) != 0; }
int  ferror(FILE *f)   { return (f->state & S_ERR) != 0; }
void clearerr(FILE *f) { f->state = 0; }

/* ---- files by name ------------------------------------------------------------------ */

int remove(const char *path)
{
    if (DeleteFile(path) || RemoveDirectory(path))
        return 0;
    set_win_errno();
    return -1;
}

int rename(const char *from, const char *to)
{
    if (MoveFile(from, to))
        return 0;
    set_win_errno();
    return -1;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

/* ---- the locked entry points (a process's threads share the FILEs) ---- */

int fflush(FILE *f)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = fflush_unlocked(f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

int fclose(FILE *f)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = fclose_unlocked(f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

int fgetc(FILE *f)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = fgetc_unlocked(f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

int ungetc(int c, FILE *f)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = ungetc_unlocked(c, f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

size_t fread(void *restrict p, size_t size, size_t n, FILE *restrict f)
{
    __crt_lock(CRT_LOCK_FILES);
    size_t r = fread_unlocked(p, size, n, f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

char *fgets(char *restrict s, int n, FILE *restrict f)
{
    __crt_lock(CRT_LOCK_FILES);
    char * r = fgets_unlocked(s, n, f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

size_t fwrite(const void *restrict p, size_t size, size_t n, FILE *restrict f)
{
    __crt_lock(CRT_LOCK_FILES);
    size_t r = fwrite_unlocked(p, size, n, f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

int fputc(int c, FILE *f)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = fputc_unlocked(c, f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

int fputs(const char *restrict s, FILE *restrict f)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = fputs_unlocked(s, f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

long ftell(FILE *f)
{
    __crt_lock(CRT_LOCK_FILES);
    long r = ftell_unlocked(f);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

int fseek(FILE *f, long off, int whence)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = fseek_unlocked(f, off, whence);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

/* ---- formatted output --------------------------------------------------------------- */

struct file_out {
    struct fmt_out o;
    FILE *f;
    bool  failed;
};

static void file_put(struct fmt_out *o, const char *s, size_t n)
{
    struct file_out *fo = (struct file_out *)o;
    if (!fo->failed && put_bytes(fo->f, (const uint8_t *)s, n) != n)
        fo->failed = true;
    o->count += n;
}

static int to_file_unlocked(FILE *f, const char *fmt, va_list ap, unsigned mode)
{
    struct file_out fo = { { file_put, 0 }, f, false };
    int r = __crt_format(&fo.o, fmt, ap, mode);
    return fo.failed ? -1 : r;
}

static int to_file(FILE *f, const char *fmt, va_list ap, unsigned mode)
{
    __crt_lock(CRT_LOCK_FILES);
    int r = to_file_unlocked(f, fmt, ap, mode);
    __crt_unlock(CRT_LOCK_FILES);
    return r;
}

int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap) { return to_file(f, fmt, ap, 0); }
int vprintf(const char *restrict fmt, va_list ap)                    { return to_file(stdout, fmt, ap, 0); }

int fprintf(FILE *restrict f, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = to_file(f, fmt, ap, 0);
    va_end(ap);
    return r;
}

int printf(const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = to_file(stdout, fmt, ap, 0);
    va_end(ap);
    return r;
}

int fprintf_s(FILE *restrict f, const char *restrict fmt, ...)
{
    if (!f || !fmt) {
        __crt_constraint("fprintf_s: a null argument", EINVAL);
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = to_file(f, fmt, ap, FMT_SECURE);
    va_end(ap);
    return r;
}

int printf_s(const char *restrict fmt, ...)
{
    if (!fmt) {
        __crt_constraint("printf_s: the format is null", EINVAL);
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = to_file(stdout, fmt, ap, FMT_SECURE);
    va_end(ap);
    return r;
}
