/*
 * JellyOS Boot Manager - boot log.
 *
 * Output goes to the UEFI console, which OVMF mirrors to the serial port.
 * Must not be used after ExitBootServices().
 */

#ifndef BOOT_LOG_H
#define BOOT_LOG_H

#include <efi.h>

typedef enum {
    BOOT_LOG_DEBUG,
    BOOT_LOG_INFO,
    BOOT_LOG_WARN,
    BOOT_LOG_ERROR,
} boot_log_level_t;

void boot_log(boot_log_level_t level, const CHAR16 *fmt, ...);

#define log_debug(...) boot_log(BOOT_LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  boot_log(BOOT_LOG_INFO, __VA_ARGS__)
#define log_warn(...)  boot_log(BOOT_LOG_WARN, __VA_ARGS__)
#define log_error(...) boot_log(BOOT_LOG_ERROR, __VA_ARGS__)

#endif
