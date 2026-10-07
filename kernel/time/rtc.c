/*
 * Real-time clock (README section 40: time system).
 *
 * The CMOS RTC is read once at boot; wall-clock time is then that moment
 * plus the monotonic clock. The RTC is assumed to run in UTC (QEMU's
 * default). Time zones are a userspace matter.
 */

#include "time/rtc.h"
#include "time/clock.h"

#include "core/arch.h"
#include "core/log.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static uint64_t boot_epoch_ns;   /* wall-clock time at monotonic time 0 */
static bool valid;

static uint8_t cmos(uint8_t reg)
{
    arch_io_write8(CMOS_ADDRESS, reg);
    return arch_io_read8(CMOS_DATA);
}

static bool update_in_progress(void)
{
    return cmos(0x0A) & 0x80;
}

typedef struct {
    uint8_t second, minute, hour, day, month, year, century;
} rtc_time_t;

static void read_raw(rtc_time_t *t)
{
    /* An update takes about 2 ms; without an RTC the flag may read as set forever. */
    for (uint32_t spins = 0; spins < 100000 && update_in_progress(); spins++)
        ;
    t->second = cmos(0x00);
    t->minute = cmos(0x02);
    t->hour = cmos(0x04);
    t->day = cmos(0x07);
    t->month = cmos(0x08);
    t->year = cmos(0x09);
    t->century = cmos(0x32);
}

static uint8_t from_bcd(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

/* Days since 1970-01-01 (Howard Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void rtc_init(void)
{
    rtc_time_t a, b;
    /* Read until two reads agree, so no update happened in between. */
    int tries = 0;
    read_raw(&a);
    do {
        b = a;
        read_raw(&a);
    } while (++tries < 10 && (a.second != b.second || a.minute != b.minute || a.hour != b.hour || a.day != b.day ||
                              a.month != b.month || a.year != b.year));

    uint8_t status_b = cmos(0x0B);
    bool pm = a.hour & 0x80;
    a.hour &= 0x7F;
    if (!(status_b & 0x04)) { /* BCD */
        a.second = from_bcd(a.second);
        a.minute = from_bcd(a.minute);
        a.hour = from_bcd(a.hour);
        a.day = from_bcd(a.day);
        a.month = from_bcd(a.month);
        a.year = from_bcd(a.year);
        a.century = from_bcd(a.century);
    }
    if (!(status_b & 0x02) && pm) /* 12-hour clock */
        a.hour = (uint8_t)((a.hour % 12) + 12);
    unsigned year = (a.century >= 19 && a.century <= 30 ? a.century : 20) * 100u + a.year;

    if (a.month < 1 || a.month > 12 || a.day < 1 || a.day > 31 || a.hour > 23 || a.minute > 59 || a.second > 60) {
        klog_warn("rtc: invalid time, wall clock unavailable");
        return;
    }
    int64_t seconds = days_from_civil(year, a.month, a.day) * 86400 + a.hour * 3600 + a.minute * 60 + a.second;
    boot_epoch_ns = (uint64_t)seconds * 1000000000ULL - clock_monotonic_ns();
    valid = true;
    klog_info("rtc: %04u-%02u-%02u %02u:%02u:%02u UTC", year, a.month, a.day, a.hour, a.minute, a.second);
}

bool rtc_available(void)
{
    return valid;
}

uint64_t clock_realtime_ns(void)
{
    return valid ? boot_epoch_ns + clock_monotonic_ns() : 0;
}
