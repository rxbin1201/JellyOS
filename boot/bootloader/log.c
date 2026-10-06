#include "log.h"

#include <efilib.h>
#include <stdarg.h>

#define LOG_BUFFER_SIZE  BOOT_LOG_CAPACITY
#define LOG_LINE_MAX     256

static const char *const level_prefix[] = {
    [BOOT_LOG_DEBUG] = "[debug] ",
    [BOOT_LOG_INFO]  = "[info ] ",
    [BOOT_LOG_WARN]  = "[warn ] ",
    [BOOT_LOG_ERROR] = "[error] ",
};

static char  log_buffer[LOG_BUFFER_SIZE];
static UINTN log_length;
static bool  verbose;

static void append(const char *s)
{
    while (*s && log_length + 1 < LOG_BUFFER_SIZE)
        log_buffer[log_length++] = *s++;
    log_buffer[log_length] = '\0';
}

void boot_log(boot_log_level_t level, const CHAR16 *fmt, ...)
{
    CHAR16 line[LOG_LINE_MAX];
    char ascii[LOG_LINE_MAX];
    va_list args;

    va_start(args, fmt);
    UnicodeVSPrint(line, sizeof(line), fmt, args);
    va_end(args);

    UINTN i = 0;
    for (; line[i] && i + 1 < LOG_LINE_MAX; i++)
        ascii[i] = (line[i] >= 0x20 && line[i] < 0x7F) ? (char)line[i] : '?';
    ascii[i] = '\0';

    append(level_prefix[level]);
    append(ascii);
    append("\n");

    if (level != BOOT_LOG_DEBUG || verbose)
        Print(L"%a%s\r\n", level_prefix[level], line);
}

void log_set_verbose(bool enable)
{
    verbose = enable;
}

bool log_is_verbose(void)
{
    return verbose;
}

const char *log_text(UINTN *length)
{
    *length = log_length;
    return log_buffer;
}
