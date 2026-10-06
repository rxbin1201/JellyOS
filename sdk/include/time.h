/*
 * JellyOS libc: time.
 *
 * There is no wall clock yet (no RTC driver): time() counts seconds since
 * boot. clock() and timespec_get() use the monotonic clock.
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

time_t  time(time_t *result);
clock_t clock(void);
int     timespec_get(struct timespec *ts, int base);
double  difftime(time_t end, time_t start);

#endif
