#include "time/clock.h"

#include "scheduler/scheduler.h"

static uint64_t period_ns;
static volatile uint64_t ticks;

void clock_init(uint64_t tick_period_ns)
{
    period_ns = tick_period_ns;
    ticks = 0;
}

void clock_tick(void)
{
    ticks++;
    scheduler_tick();
}

uint64_t clock_ticks(void)
{
    return ticks;
}

uint64_t clock_monotonic_ns(void)
{
    return ticks * period_ns;
}
