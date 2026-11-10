/*
 * stdlib.h -- memory, numbers, sorting, ending the program.
 *
 * No floating point: the baseline is a 486SX, which has no FPU.
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <crtdefs.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFF
#define MB_CUR_MAX   1

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef void (*constraint_handler_t)(const char *restrict msg, void *restrict ptr, errno_t error);

__CRT_BEGIN
void   *malloc(size_t n) __crt_malloc;
void   *calloc(size_t count, size_t size) __crt_malloc;    /* fails if count * size overflows */
void   *realloc(void *p, size_t n) __attribute__((warn_unused_result));
void   *reallocarray(void *p, size_t count, size_t size) __attribute__((warn_unused_result));
void    free(void *p);
size_t  malloc_usable_size(void *p);

__crt_noreturn void abort(void);
__crt_noreturn void exit(int code);
__crt_noreturn void _Exit(int code);
int     atexit(void (*fn)(void));

int     atoi(const char *s);
long    atol(const char *s);
long long atoll(const char *s);
long    strtol(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
long long strtoll(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end, int base);

int     abs(int v);
long    labs(long v);
long long llabs(long long v);
div_t   div(int a, int b);
ldiv_t  ldiv(long a, long b);

int     rand(void);                     /* the C library's sequence; not for secrets */
void    srand(unsigned seed);
int     rand_s(unsigned *out);          /* the kernel's random numbers: 0, or an errno */
int     getrandom_bytes(void *buf, size_t n);   /* the same, any length: 0, or an errno */

void    qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void   *bsearch(const void *key, const void *base, size_t n, size_t size,
                int (*cmp)(const void *, const void *));
errno_t qsort_s(void *base, rsize_t n, rsize_t size,
                int (*cmp)(const void *, const void *, void *), void *ctx);
void   *bsearch_s(const void *key, const void *base, rsize_t n, rsize_t size,
                  int (*cmp)(const void *, const void *, void *), void *ctx);

char   *getenv(const char *name);       /* OS1993 has no environment: NULL */
errno_t getenv_s(size_t *restrict len, char *restrict d, rsize_t dmax, const char *restrict name);

/* Annex K: what a broken constraint does (abort_handler_s by default). */
constraint_handler_t set_constraint_handler_s(constraint_handler_t handler);
void    abort_handler_s(const char *restrict msg, void *restrict ptr, errno_t error);
void    ignore_handler_s(const char *restrict msg, void *restrict ptr, errno_t error);
__CRT_END

#endif
