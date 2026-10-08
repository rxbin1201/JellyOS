#include "time/clock.h"
#include "core/export.h"

#include "core/arch.h"
#include "scheduler/scheduler.h"

static uint64_t period_ns;
static volatile uint64_t ticks;
static bool polled;

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
    if (polled && arch_timer_poll())
        ticks++;
    return ticks * period_ns;
}

void clock_poll_from_now(void)
{
    polled = true;
}

bool clock_is_polled(void)
{
    return polled;
}

EXPORT_SYMBOL(clock_monotonic_ns);
