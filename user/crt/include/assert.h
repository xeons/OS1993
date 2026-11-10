/*
 * assert.h -- a failed assertion is logged and ends the program (SYS_FAIL).
 * Included again with NDEBUG defined, assert does nothing.
 */
#include <crtdefs.h>

#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
__CRT_BEGIN
__crt_noreturn void __assert_fail(const char *expr, const char *file, int line, const char *func);
__CRT_END
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#ifndef __cplusplus
#define static_assert _Static_assert
#endif
