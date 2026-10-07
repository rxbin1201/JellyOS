#include "core/log.h"
#include "core/export.h"

#include "core/arch.h"
#include "core/format.h"
#include "time/clock.h"

#include <stdarg.h>
#include <stdint.h>

#define LOG_RING_SIZE 65536
#define LOG_LINE_MAX  512

static const char *const level_names[] = {
    [KLOG_DEBUG] = "debug",
    [KLOG_INFO]  = "info",
    [KLOG_WARN]  = "warn",
    [KLOG_ERROR] = "error",
};

static char ring[LOG_RING_SIZE];
static size_t ring_head; /* total bytes ever written; position = head % size */
static klog_level_t console_level = KLOG_INFO;
static void (*console_mirror)(const char *text, size_t length);

void kconsole_set_mirror(void (*write)(const char *text, size_t length))
{
    console_mirror = write;
}

void kconsole_write(const char *text, size_t length)
{
    char chunk[128];
    for (size_t done = 0; done < length;) {
        size_t n = length - done < sizeof(chunk) - 1 ? length - done : sizeof(chunk) - 1;
        for (size_t i = 0; i < n; i++)
            chunk[i] = text[done + i];
        chunk[n] = '\0';
        arch_early_console_write(chunk);
        done += n;
    }
    if (console_mirror)
        console_mirror(text, length);
}

void kconsole_mirror_char(char c)
{
    if (console_mirror)
        console_mirror(&c, 1);
}

void klog_replay(void (*write)(const char *text, size_t length))
{
    uint64_t flags = arch_interrupts_save();
    size_t start = ring_head > LOG_RING_SIZE ? ring_head - LOG_RING_SIZE : 0;
    for (size_t i = start; i < ring_head;) {
        size_t position = i % LOG_RING_SIZE;
        size_t n = LOG_RING_SIZE - position < ring_head - i ? LOG_RING_SIZE - position : ring_head - i;
        write(ring + position, n);
        i += n;
    }
    arch_interrupts_restore(flags);
}

static void ring_append(const char *s, size_t length)
{
    for (size_t i = 0; i < length; i++)
        ring[(ring_head + i) % LOG_RING_SIZE] = s[i];
    ring_head += length;
}

void klog_set_console_level(klog_level_t level)
{
    console_level = level;
}

void klog(klog_level_t level, const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    uint64_t ns = clock_monotonic_ns();
    size_t length;
    va_list args;

    length = format(line, sizeof(line), "[%5u.%06u] %s: ", (unsigned)(ns / 1000000000),
                    (unsigned)(ns / 1000 % 1000000), level_names[level]);

    va_start(args, fmt);
    if (length < sizeof(line))
        length += format_v(line + length, sizeof(line) - length, fmt, args);
    va_end(args);

    if (length >= sizeof(line) - 1)
        length = sizeof(line) - 2;
    line[length++] = '\n';
    line[length] = '\0';

    uint64_t flags = arch_interrupts_save();
    ring_append(line, length);
    if (level >= console_level)
        kconsole_write(line, length);
    arch_interrupts_restore(flags);
}

void klog_raw(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    va_list args;

    va_start(args, fmt);
    size_t length = format_v(line, sizeof(line), fmt, args);
    va_end(args);
    kconsole_write(line, length < sizeof(line) ? length : sizeof(line) - 1);
}

EXPORT_SYMBOL(klog);
