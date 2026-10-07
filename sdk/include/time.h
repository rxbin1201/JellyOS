/*
 * JellyOS libc: time.
 *
 * time() and timespec_get() return wall-clock time from the RTC (UTC). There
 * are no time zones yet: localtime() is gmtime(). Without an RTC, time()
 * counts seconds since boot. clock() measures time since boot.
 */

#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>
#include <stdint.h>

#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC       1

typedef int64_t time_t;
typedef int64_t clock_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec;    /* 0-60 */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0-6, Sunday = 0 */
    int tm_yday;   /* 0-365 */
    int tm_isdst;
};

time_t     time(time_t *result);
clock_t    clock(void);
int        timespec_get(struct timespec *ts, int base);
double     difftime(time_t end, time_t start);

struct tm *gmtime_r(const time_t *t, struct tm *result);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);   /* UTC */
time_t     timegm(const struct tm *tm);
time_t     mktime(struct tm *tm);        /* UTC */
/* %Y %m %d %H %M %S %y %e %j %a %A %b %B %p %I %F %T %R %D %%; returns 0 if it does not fit. */
size_t     strftime(char *buffer, size_t size, const char *format, const struct tm *tm);

#endif
