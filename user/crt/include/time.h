/*
 * time.h -- calendar time from the real-time clock, and the time since
 * the program started. The RTC keeps local time and OS1993 has no time
 * zones, so local time and UTC are the same.
 */
#ifndef _TIME_H
#define _TIME_H

#include <crtdefs.h>

typedef long time_t;                    /* seconds since 1970-01-01 (until 2038) */
typedef long clock_t;
#define CLOCKS_PER_SEC 1000L

struct tm {
    int tm_sec, tm_min, tm_hour;        /* 0-60, 0-59, 0-23 */
    int tm_mday, tm_mon, tm_year;       /* 1-31, 0-11, years since 1900 */
    int tm_wday, tm_yday, tm_isdst;     /* 0-6 (Sunday 0), 0-365, 0 */
};

__CRT_BEGIN
time_t  time(time_t *t);
clock_t clock(void);
time_t  mktime(struct tm *tm);
struct tm *gmtime_r(const time_t *restrict t, struct tm *restrict tm);
struct tm *localtime_r(const time_t *restrict t, struct tm *restrict tm);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
size_t  strftime(char *restrict s, size_t max, const char *restrict fmt, const struct tm *restrict tm);
__CRT_END

#endif
