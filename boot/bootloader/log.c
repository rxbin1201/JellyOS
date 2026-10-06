#include "log.h"

#include <efilib.h>
#include <stdarg.h>

static const CHAR16 *const level_prefix[] = {
    [BOOT_LOG_DEBUG] = L"[debug] ",
    [BOOT_LOG_INFO]  = L"[info ] ",
    [BOOT_LOG_WARN]  = L"[warn ] ",
    [BOOT_LOG_ERROR] = L"[error] ",
};

void boot_log(boot_log_level_t level, const CHAR16 *fmt, ...)
{
    va_list args;

    Print(L"%s", level_prefix[level]);
    va_start(args, fmt);
    VPrint(fmt, args);
    va_end(args);
    Print(L"\r\n");
}
