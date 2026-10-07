/*
 * libc: time. Wall-clock time comes from the kernel's RTC clock (UTC).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <jelly/os.h>

static uint64_t now_ns(void)
{
    uint64_t ns;
    if (STATUS_IS_ERROR(jelly_clock_realtime(&ns)))
        ns = jelly_clock_ns(); /* no RTC: time since boot */
    return ns;
}

time_t time(time_t *result)
{
    time_t now = (time_t)(now_ns() / 1000000000ULL);
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
    uint64_t ns = now_ns();
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return base;
}

double difftime(time_t end, time_t start)
{
    return (double)(end - start);
}

/* --- Calendar (Howard Hinnant's civil date algorithms) ---------------------- */

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int is_leap(int64_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

struct tm *gmtime_r(const time_t *t, struct tm *tm)
{
    int64_t seconds = *t, days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        days--;
    }
    tm->tm_hour = (int)(rest / 3600);
    tm->tm_min = (int)(rest % 3600 / 60);
    tm->tm_sec = (int)(rest % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;

    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm->tm_isdst = 0;
    (void)is_leap;
    return tm;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm result;
    return gmtime_r(t, &result);
}

struct tm *localtime(const time_t *t)
{
    return gmtime(t);
}

time_t timegm(const struct tm *tm)
{
    int64_t year = tm->tm_year + 1900 + tm->tm_mon / 12;
    int month = tm->tm_mon % 12;
    if (month < 0) {
        month += 12;
        year--;
    }
    return (time_t)(days_from_civil(year, month + 1, 1) + tm->tm_mday - 1) * 86400 + tm->tm_hour * 3600 +
           tm->tm_min * 60 + tm->tm_sec;
}

time_t mktime(struct tm *tm)
{
    time_t t = timegm(tm);
    gmtime_r(&t, tm); /* normalize */
    return t;
}

static const char *const weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                        "Saturday" };
static const char *const months[] = { "January", "February", "March",     "April",   "May",      "June",
                                      "July",    "August",   "September", "October", "November", "December" };

size_t strftime(char *buffer, size_t size, const char *format, const struct tm *tm)
{
    size_t used = 0;
    char piece[64];

    for (const char *f = format; *f; f++) {
        if (*f != '%') {
            piece[0] = *f;
            piece[1] = '\0';
        } else {
            f++;
            switch (*f) {
            case 'Y': snprintf(piece, sizeof(piece), "%d", tm->tm_year + 1900); break;
            case 'y': snprintf(piece, sizeof(piece), "%02d", (tm->tm_year + 1900) % 100); break;
            case 'm': snprintf(piece, sizeof(piece), "%02d", tm->tm_mon + 1); break;
            case 'd': snprintf(piece, sizeof(piece), "%02d", tm->tm_mday); break;
            case 'e': snprintf(piece, sizeof(piece), "%2d", tm->tm_mday); break;
            case 'j': snprintf(piece, sizeof(piece), "%03d", tm->tm_yday + 1); break;
            case 'H': snprintf(piece, sizeof(piece), "%02d", tm->tm_hour); break;
            case 'I': snprintf(piece, sizeof(piece), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'M': snprintf(piece, sizeof(piece), "%02d", tm->tm_min); break;
            case 'S': snprintf(piece, sizeof(piece), "%02d", tm->tm_sec); break;
            case 'p': snprintf(piece, sizeof(piece), "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'a': snprintf(piece, sizeof(piece), "%.3s", weekdays[tm->tm_wday % 7]); break;
            case 'A': snprintf(piece, sizeof(piece), "%s", weekdays[tm->tm_wday % 7]); break;
            case 'b': snprintf(piece, sizeof(piece), "%.3s", months[tm->tm_mon % 12]); break;
            case 'B': snprintf(piece, sizeof(piece), "%s", months[tm->tm_mon % 12]); break;
            case 'F':
                snprintf(piece, sizeof(piece), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
                break;
            case 'T': snprintf(piece, sizeof(piece), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': snprintf(piece, sizeof(piece), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'D':
                snprintf(piece, sizeof(piece), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday,
                         (tm->tm_year + 1900) % 100);
                break;
            case '%': strcpy(piece, "%"); break;
            case '\0': f--; piece[0] = '\0'; break;
            default: snprintf(piece, sizeof(piece), "%%%c", *f); break;
            }
        }
        size_t n = strlen(piece);
        if (used + n >= size)
            return 0;
        memcpy(buffer + used, piece, n);
        used += n;
    }
    if (size)
        buffer[used] = '\0';
    return used;
}
