/*
 * Real-time clock: wall-clock time in UTC.
 */

#ifndef TIME_RTC_H
#define TIME_RTC_H

#include <stdbool.h>
#include <stdint.h>

/* Read the CMOS clock (once, at boot). */
void     rtc_init(void);
bool     rtc_available(void);
/* Nanoseconds since 1970-01-01 00:00 UTC, 0 if unknown. */
uint64_t clock_realtime_ns(void);

#endif
