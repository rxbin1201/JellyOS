/*
 * libc: time on the monotonic clock (no wall clock before an RTC driver).
 */

#include <time.h>

#include <jelly/os.h>

time_t time(time_t *result)
{
    time_t now = (time_t)(jelly_clock_ns() / 1000000000ULL);
    if (result)
        *result = now;
    return now;
}

clock_t clock(void)
{
    return (clock_t)(jelly_clock_ns() / 1000);
}

int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC)
        return 0;
    uint64_t ns = jelly_clock_ns();
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return base;
}

double difftime(time_t end, time_t start)
{
    return (double)(end - start);
}
