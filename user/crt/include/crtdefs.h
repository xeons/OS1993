/*
 * crtdefs.h -- what the C runtime's headers share.
 *
 * OS1993's C runtime comes two ways, built from the same sources: CRT.DLL,
 * shared by the programs that import it, and libcrt.a, linked into a
 * module of its own (the system DLLs use it so). Either way it is
 * hardened:
 *
 *   - the bounds-checked functions of C11 Annex K (strcpy_s, sprintf_s,
 *     ...) are always declared; a broken constraint calls the
 *     runtime-constraint handler, which by default ends the program;
 *   - with -D_FORTIFY_SOURCE (on for everything in OS1993) copies into
 *     objects of known size are checked, at compile time where possible;
 *   - every module has a stack canary that the loader sets to a random
 *     value per process (-fstack-protector-strong);
 *   - malloc checks its blocks (cookies, a guard after each, double free)
 *     and calloc the multiplication; printf has no %n;
 *   - a detected corruption ends the process at once (SYS_FAIL), as
 *     Win32's __fastfail.
 *
 * Variables can't be imported from a DLL, so errno, stdin, stdout and
 * stderr are macros around functions.
 */
#ifndef _OS1993_CRTDEFS_H
#define _OS1993_CRTDEFS_H

#include <stddef.h>
#include <stdint.h>

#define __STDC_LIB_EXT1__ 201112L

#ifndef _ERRNO_T_DEFINED
#define _ERRNO_T_DEFINED
typedef int errno_t;
#endif
#ifndef _RSIZE_T_DEFINED
#define _RSIZE_T_DEFINED
typedef size_t rsize_t;
#endif
#define RSIZE_MAX (SIZE_MAX >> 1)       /* larger sizes are taken for negative numbers */

#define __crt_printf(f, a)  __attribute__((format(printf, f, a)))
#define __crt_noreturn      __attribute__((noreturn))
#define __crt_malloc        __attribute__((malloc, warn_unused_result))

#ifdef __cplusplus
#define __CRT_BEGIN extern "C" {
#define __CRT_END   }
#else
#define __CRT_BEGIN
#define __CRT_END
#endif

#if defined(_FORTIFY_SOURCE) && _FORTIFY_SOURCE > 0 && defined(__OPTIMIZE__) && !defined(__CRT_BUILD)
#define __CRT_FORTIFY 1
#define __crt_bos(p) __builtin_object_size(p, _FORTIFY_SOURCE > 1)
#endif

#endif
