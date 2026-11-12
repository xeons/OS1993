#ifndef OS1993_KERNEL_H
#define OS1993_KERNEL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "bootinfo.h"

#define OS1993_VERSION "0.1"

extern const struct bootinfo *g_bootinfo;

/* kprintf.c -- output goes to the screen and to COM1.
 * No __attribute__((format)): on i686-elf uint32_t is `unsigned long`, and
 * this printf treats %d/%u/%x as 32-bit regardless, so GCC's check is noise. */
int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...);
void kprintf(const char *fmt, ...);
void klog(const char *fmt, ...);   /* COM1 only */

/* panic.c */
__attribute__((noreturn))
void panic(const char *fmt, ...);

/* vmm.c */
struct vm;
struct vm_settings;
struct vm *dos_new_session(const struct vm_settings *set);  /* a DOS session VM, focused;
                                           set NULL: OS1993.INI's settings */

/* vmm.c */
__attribute__((noreturn))
void kernel_shutdown(uint8_t exit_code);

/* string.c (the compiler may emit calls to these) */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strrchr(const char *s, int c);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
