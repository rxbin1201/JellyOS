/*
 * Monotonic system clock (README section 40), driven by the architecture's
 * periodic timer. Higher resolution sources (TSC, HPET) can replace the tick
 * count later without changing this interface.
 */

#ifndef TIME_CLOCK_H
#define TIME_CLOCK_H

#include <stdint.h>

/* Called by the timer driver once it knows its period. */
void clock_init(uint64_t tick_period_ns);

/* Called from the timer interrupt. */
void clock_tick(void);

/* Nanoseconds since the clock started; 0 before clock_init(). */
uint64_t clock_monotonic_ns(void);

uint64_t clock_ticks(void);

#endif
