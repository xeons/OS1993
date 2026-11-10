/*
 * time.c -- calendar time (the RTC, through GetLocalTime) and the time
 * since the program started. Local time and UTC are the same.
 */
#include "crt.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "os1993.h"

static uint32_t start_ms;
static bool started;

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static const int before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

/* Days since 1970-01-01 (y the full year, m 0-11, d 1-31). */
static long days_from_civil(int y, int m, int d)
{
    long days = 0;
    for (int yy = 1970; yy < y; yy++)
        days += leap(yy) ? 366 : 365;
    for (int yy = y; yy < 1970; yy++)
        days -= leap(yy) ? 366 : 365;
    return days + before_month[m] + (m > 1 && leap(y)) + d - 1;
}

time_t mktime(struct tm *tm)
{
    int y = tm->tm_year + 1900 + tm->tm_mon / 12, m = tm->tm_mon % 12;
    if (m < 0) { m += 12; y--; }
    long t = days_from_civil(y, m, 1) * 86400L + (long)(tm->tm_mday - 1) * 86400L +
             tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    gmtime_r(&t, tm);                               /* normalized, wday and yday filled */
    return t;
}

struct tm *gmtime_r(const time_t *restrict tp, struct tm *restrict tm)
{
    long t = *tp, days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    tm->tm_hour = (int)(secs / 3600);
    tm->tm_min = (int)(secs / 60 % 60);
    tm->tm_sec = (int)(secs % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);       /* 1970-01-01 was a Thursday */
    int y = 1970;
    while (days < 0) { y--; days += leap(y) ? 366 : 365; }
    while (days >= (leap(y) ? 366 : 365)) { days -= leap(y) ? 366 : 365; y++; }
    tm->tm_year = y - 1900;
    tm->tm_yday = (int)days;
    int m = 11;
    while (m > 0 && days < before_month[m] + (m > 1 && leap(y)))
        m--;
    tm->tm_mon = m;
    tm->tm_mday = (int)(days - before_month[m] - (m > 1 && leap(y))) + 1;
    tm->tm_isdst = 0;
    return tm;
}

struct tm *localtime_r(const time_t *restrict t, struct tm *restrict tm) { return gmtime_r(t, tm); }

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return gmtime_r(t, &tm);
}

struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t time(time_t *out)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    struct tm tm = { st.second, st.minute, st.hour, st.day, st.month - 1, st.year - 1900, 0, 0, 0 };
    time_t t = mktime(&tm);
    if (out)
        *out = t;
    return t;
}

clock_t clock(void)
{
    if (!started) {                                 /* the first call is time 0 */
        start_ms = GetTickCount();
        started = true;
    }
    return (clock_t)(GetTickCount() - start_ms);
}

static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                          "Thursday", "Friday", "Saturday" };
static const char *const month_names[12] = { "January", "February", "March", "April", "May", "June",
                                             "July", "August", "September", "October", "November",
                                             "December" };

/* %a %A %b %B %d %e %H %I %j %m %M %p %S %y %Y %F %T %D %R %c %x %X %% */
size_t strftime(char *restrict s, size_t max, const char *restrict fmt, const struct tm *restrict tm)
{
    size_t n = 0;
    char part[64];
    for (; *fmt; fmt++) {
        const char *add = part;
        part[0] = '\0';
        if (*fmt != '%') {
            part[0] = *fmt;
            part[1] = '\0';
        } else {
            unsigned wday = (unsigned)tm->tm_wday % 7, mon = (unsigned)tm->tm_mon % 12;
            int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
            switch (*++fmt) {
            case 'a': snprintf(part, sizeof part, "%.3s", day_names[wday]); break;
            case 'A': add = day_names[wday]; break;
            case 'b': case 'h': snprintf(part, sizeof part, "%.3s", month_names[mon]); break;
            case 'B': add = month_names[mon]; break;
            case 'd': snprintf(part, sizeof part, "%02d", tm->tm_mday); break;
            case 'e': snprintf(part, sizeof part, "%2d", tm->tm_mday); break;
            case 'H': snprintf(part, sizeof part, "%02d", tm->tm_hour); break;
            case 'I': snprintf(part, sizeof part, "%02d", h12); break;
            case 'j': snprintf(part, sizeof part, "%03d", tm->tm_yday + 1); break;
            case 'm': snprintf(part, sizeof part, "%02d", tm->tm_mon + 1); break;
            case 'M': snprintf(part, sizeof part, "%02d", tm->tm_min); break;
            case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'S': snprintf(part, sizeof part, "%02d", tm->tm_sec); break;
            case 'y': snprintf(part, sizeof part, "%02d", (tm->tm_year + 1900) % 100); break;
            case 'Y': snprintf(part, sizeof part, "%d", tm->tm_year + 1900); break;
            case 'F': snprintf(part, sizeof part, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'T': case 'X': snprintf(part, sizeof part, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': snprintf(part, sizeof part, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'D': case 'x': snprintf(part, sizeof part, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
            case 'c': snprintf(part, sizeof part, "%.3s %.3s %2d %02d:%02d:%02d %d", day_names[wday], month_names[mon],
                               tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
            case '%': add = "%"; break;
            case '\0': fmt--; break;
            default: break;
            }
        }
        size_t k = strlen(add);
        if (n + k >= max) {                         /* doesn't fit: 0, as the standard says */
            if (max)
                s[0] = '\0';
            return 0;
        }
        memcpy(s + n, add, k);
        n += k;
    }
    s[n] = '\0';
    return n;
}
