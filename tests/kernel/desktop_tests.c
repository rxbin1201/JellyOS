/*
 * Kernel tests for Phase 10 (ABI 6): wall-clock time from the RTC and the
 * live process count behind SYS_SYSTEM_INFO.
 *
 * Starting a session with other credentials (SYS_PROCESS_SPAWN_AS) is
 * exercised by the integration test: the desktop runs as uid 1000 and the
 * files it creates belong to that user.
 */

#include "tests/kernel/ktest.h"

#include "process/process.h"
#include "scheduler/thread.h"
#include "time/clock.h"
#include "time/rtc.h"

#define YEAR_2024 1704067200ULL /* 2024-01-01 00:00 UTC */
#define YEAR_2200 7258118400ULL

KTEST(rtc_gives_wall_clock_time)
{
    if (!rtc_available()) {
        KEXPECT(clock_realtime_ns() == 0);
        return;
    }
    uint64_t a = clock_realtime_ns();
    KEXPECT(a / 1000000000ULL > YEAR_2024 && a / 1000000000ULL < YEAR_2200);
    thread_sleep(5000000ULL);
    uint64_t b = clock_realtime_ns();
    /* It advances with the monotonic clock. */
    KEXPECT(b > a && b - a >= 4000000ULL && b - a < 1000000000ULL);
}

KTEST(process_count_follows_processes)
{
    uint32_t before = process_live_count();
    process_t *p;
    KASSERT(process_create("ktest-count", &p) == STATUS_SUCCESS);
    KEXPECT(process_live_count() == before + 1);
    object_release(&p->object);
    KEXPECT(process_live_count() == before);
}
