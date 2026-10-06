/*
 * JellyOS Boot Manager - boot log.
 *
 * Every message is kept in an in-memory log that is handed to the kernel
 * (boot_info_t.log_phys) and shown in diagnostics. Console output goes to the
 * UEFI console, which OVMF mirrors to the serial port; debug messages are only
 * printed in verbose mode ("debug=1").
 * Must not be used after ExitBootServices().
 */

#ifndef BOOT_LOG_H
#define BOOT_LOG_H

#include <efi.h>
#include <stdbool.h>

/* Size of the in-memory log including the terminating NUL. */
#define BOOT_LOG_CAPACITY 16384

typedef enum {
    BOOT_LOG_DEBUG,
    BOOT_LOG_INFO,
    BOOT_LOG_WARN,
    BOOT_LOG_ERROR,
} boot_log_level_t;

void boot_log(boot_log_level_t level, const CHAR16 *fmt, ...);

void log_set_verbose(bool verbose);
bool log_is_verbose(void);

/* Collected log text (ASCII, NUL terminated). */
const char *log_text(UINTN *length);

#define log_debug(...) boot_log(BOOT_LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  boot_log(BOOT_LOG_INFO, __VA_ARGS__)
#define log_warn(...)  boot_log(BOOT_LOG_WARN, __VA_ARGS__)
#define log_error(...) boot_log(BOOT_LOG_ERROR, __VA_ARGS__)

#endif
