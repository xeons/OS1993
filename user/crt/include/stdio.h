/*
 * stdio.h -- buffered files over KERNEL.DLL's, and formatted output.
 *
 * stdout and stderr go to the debug log (COM1), a line at a time; stdin
 * is always at end of file. Files are binary: no CR LF translation.
 * printf has no %n (it writes through a pointer) and no floating point.
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <crtdefs.h>
#include <stdarg.h>

typedef struct __crt_file FILE;
typedef long fpos_t;

#define EOF          (-1)
#define BUFSIZ       512
#define FOPEN_MAX    16
#define FILENAME_MAX 128
#define L_tmpnam     16
#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2
#define _IOFBF       0
#define _IOLBF       1
#define _IONBF       2

__CRT_BEGIN
FILE   *__crt_stdio(int fd);
#define stdin  (__crt_stdio(0))
#define stdout (__crt_stdio(1))
#define stderr (__crt_stdio(2))

FILE   *fopen(const char *restrict path, const char *restrict mode);
errno_t fopen_s(FILE *restrict *restrict f, const char *restrict path, const char *restrict mode);
int     fclose(FILE *f);
int     fflush(FILE *f);
int     setvbuf(FILE *restrict f, char *restrict buf, int mode, size_t size);
size_t  fread(void *restrict p, size_t size, size_t n, FILE *restrict f);
size_t  fwrite(const void *restrict p, size_t size, size_t n, FILE *restrict f);
int     fgetc(FILE *f);
int     getc(FILE *f);
int     getchar(void);
int     ungetc(int c, FILE *f);
char   *fgets(char *restrict s, int n, FILE *restrict f);
char   *gets_s(char *s, rsize_t n);
int     fputc(int c, FILE *f);
int     putc(int c, FILE *f);
int     putchar(int c);
int     fputs(const char *restrict s, FILE *restrict f);
int     puts(const char *s);
int     fseek(FILE *f, long off, int whence);
long    ftell(FILE *f);
void    rewind(FILE *f);
int     fgetpos(FILE *restrict f, fpos_t *restrict pos);
int     fsetpos(FILE *f, const fpos_t *pos);
int     feof(FILE *f);
int     ferror(FILE *f);
void    clearerr(FILE *f);
int     remove(const char *path);
int     rename(const char *from, const char *to);
void    perror(const char *s);

int     printf(const char *restrict fmt, ...) __crt_printf(1, 2);
int     fprintf(FILE *restrict f, const char *restrict fmt, ...) __crt_printf(2, 3);
int     sprintf(char *restrict s, const char *restrict fmt, ...) __crt_printf(2, 3);
int     snprintf(char *restrict s, size_t n, const char *restrict fmt, ...) __crt_printf(3, 4);
int     vprintf(const char *restrict fmt, va_list ap) __crt_printf(1, 0);
int     vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap) __crt_printf(2, 0);
int     vsprintf(char *restrict s, const char *restrict fmt, va_list ap) __crt_printf(2, 0);
int     vsnprintf(char *restrict s, size_t n, const char *restrict fmt, va_list ap) __crt_printf(3, 0);

/* Annex K: a null argument, %n, or (sprintf_s) output that doesn't fit
 * breaks a constraint; snprintf_s truncates instead. */
int     printf_s(const char *restrict fmt, ...) __crt_printf(1, 2);
int     fprintf_s(FILE *restrict f, const char *restrict fmt, ...) __crt_printf(2, 3);
int     sprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, ...) __crt_printf(3, 4);
int     snprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, ...) __crt_printf(3, 4);
int     vsprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, va_list ap) __crt_printf(3, 0);
int     vsnprintf_s(char *restrict s, rsize_t n, const char *restrict fmt, va_list ap) __crt_printf(3, 0);

/* _FORTIFY_SOURCE */
int     __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...);
int     __snprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, ...);
int     __vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list ap);
int     __vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, va_list ap);
__CRT_END

#ifdef __CRT_FORTIFY
#define sprintf(s, ...)          __builtin___sprintf_chk(s, 0, __crt_bos(s), __VA_ARGS__)
#define snprintf(s, n, ...)      __builtin___snprintf_chk(s, n, 0, __crt_bos(s), __VA_ARGS__)
#define vsprintf(s, f, ap)       __builtin___vsprintf_chk(s, 0, __crt_bos(s), f, ap)
#define vsnprintf(s, n, f, ap)   __builtin___vsnprintf_chk(s, n, 0, __crt_bos(s), f, ap)
#endif

#endif
