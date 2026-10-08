/*
 * Monotonic system clock (README section 40), driven by the architecture's
 * periodic timer. Higher resolution sources (TSC, HPET) can replace the tick
 * count later without changing this interface.
 */

#ifndef TIME_CLOCK_H
#define TIME_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Called by the timer driver once it knows its period. */
void clock_init(uint64_t tick_period_ns);

/* Called from the timer interrupt. */
void clock_tick(void);

/* Nanoseconds since the clock started; 0 before clock_init(). */
uint64_t clock_monotonic_ns(void);

uint64_t clock_ticks(void);

/*
 * From now on nobody delivers the timer interrupt (a panic: interrupts are off for good). The clock then moves
 * by looking at the timer itself whenever it is read, which is enough for code that waits by reading it, and
 * thread_sleep() waits that way, too. So a driver can still do things that take time to get a report onto a
 * screen. There is no way back.
 */
void clock_poll_from_now(void);
bool clock_is_polled(void);

#endif
