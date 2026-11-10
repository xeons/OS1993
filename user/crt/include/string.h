/*
 * string.h -- strings and memory, with C11 Annex K's bounds-checked
 * versions and _FORTIFY_SOURCE checking (see crtdefs.h).
 */
#ifndef _STRING_H
#define _STRING_H

#include <crtdefs.h>

__CRT_BEGIN
void   *memcpy(void *restrict d, const void *restrict s, size_t n);
void   *memmove(void *d, const void *s, size_t n);
void   *memset(void *d, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);
size_t  strlen(const char *s);
size_t  strnlen(const char *s, size_t max);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
int     strcasecmp(const char *a, const char *b);
int     strncasecmp(const char *a, const char *b, size_t n);
int     strcoll(const char *a, const char *b);
char   *strcpy(char *restrict d, const char *restrict s);
char   *strncpy(char *restrict d, const char *restrict s, size_t n);
char   *strcat(char *restrict d, const char *restrict s);
char   *strncat(char *restrict d, const char *restrict s, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *hay, const char *needle);
size_t  strspn(const char *s, const char *accept);
size_t  strcspn(const char *s, const char *reject);
char   *strpbrk(const char *s, const char *accept);
char   *strtok(char *restrict s, const char *restrict delim);
char   *strtok_r(char *restrict s, const char *restrict delim, char **restrict save);
char   *strdup(const char *s) __crt_malloc;
char   *strndup(const char *s, size_t n) __crt_malloc;
char   *strerror(int e);

/* Truncating copies that always terminate (BSD); they return the length
 * they tried to make, so >= size means it was cut short. */
size_t  strlcpy(char *restrict d, const char *restrict s, size_t size);
size_t  strlcat(char *restrict d, const char *restrict s, size_t size);
char   *strncpy0(char *d, const char *s, size_t size);   /* OS1993's: as strlcpy, returns d */
void    explicit_bzero(void *d, size_t n);               /* never optimized away */
int     timingsafe_memcmp(const void *a, const void *b, size_t n);  /* time doesn't depend on data */

/* C11 Annex K: errno_t 0, or a constraint broken (handler called, d
 * emptied where there is one). */
errno_t memcpy_s(void *restrict d, rsize_t dmax, const void *restrict s, rsize_t n);
errno_t memmove_s(void *d, rsize_t dmax, const void *s, rsize_t n);
errno_t memset_s(void *d, rsize_t dmax, int c, rsize_t n);
errno_t strcpy_s(char *restrict d, rsize_t dmax, const char *restrict s);
errno_t strncpy_s(char *restrict d, rsize_t dmax, const char *restrict s, rsize_t n);
errno_t strcat_s(char *restrict d, rsize_t dmax, const char *restrict s);
errno_t strncat_s(char *restrict d, rsize_t dmax, const char *restrict s, rsize_t n);
char   *strtok_s(char *restrict s, rsize_t *restrict smax, const char *restrict delim,
                 char **restrict save);
size_t  strnlen_s(const char *s, size_t max);
errno_t strerror_s(char *d, rsize_t dmax, errno_t e);
size_t  strerrorlen_s(errno_t e);

/* _FORTIFY_SOURCE: called when a copy would overrun its destination */
void   *__memcpy_chk(void *d, const void *s, size_t n, size_t dsize);
void   *__memmove_chk(void *d, const void *s, size_t n, size_t dsize);
void   *__memset_chk(void *d, int c, size_t n, size_t dsize);
char   *__strcpy_chk(char *d, const char *s, size_t dsize);
char   *__strncpy_chk(char *d, const char *s, size_t n, size_t dsize);
char   *__strcat_chk(char *d, const char *s, size_t dsize);
char   *__strncat_chk(char *d, const char *s, size_t n, size_t dsize);
__CRT_END

#ifdef __CRT_FORTIFY
#define memcpy(d, s, n)   __builtin___memcpy_chk(d, s, n, __crt_bos(d))
#define memmove(d, s, n)  __builtin___memmove_chk(d, s, n, __crt_bos(d))
#define memset(d, c, n)   __builtin___memset_chk(d, c, n, __crt_bos(d))
#define strcpy(d, s)      __builtin___strcpy_chk(d, s, __crt_bos(d))
#define strncpy(d, s, n)  __builtin___strncpy_chk(d, s, n, __crt_bos(d))
#define strcat(d, s)      __builtin___strcat_chk(d, s, __crt_bos(d))
#define strncat(d, s, n)  __builtin___strncat_chk(d, s, n, __crt_bos(d))
#endif

#endif
