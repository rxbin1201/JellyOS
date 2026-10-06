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
        arch_early_console_write(line);
    arch_interrupts_restore(flags);
}

void klog_raw(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    va_list args;

    va_start(args, fmt);
    format_v(line, sizeof(line), fmt, args);
    va_end(args);
    arch_early_console_write(line);
}

EXPORT_SYMBOL(klog);
