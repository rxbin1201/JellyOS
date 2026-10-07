/*
 * Kernel log (README section 44).
 *
 * Messages are written to the early console and kept in a ring buffer so
 * they can later be read from userspace, recovery and diagnostics.
 * Format: "[seconds.micros] level: message".
 */

#ifndef CORE_LOG_H
#define CORE_LOG_H

#include <stddef.h>

typedef enum {
    KLOG_DEBUG,
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR,
} klog_level_t;

void klog(klog_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define klog_debug(...) klog(KLOG_DEBUG, __VA_ARGS__)
#define klog_info(...)  klog(KLOG_INFO, __VA_ARGS__)
#define klog_warn(...)  klog(KLOG_WARN, __VA_ARGS__)
#define klog_error(...) klog(KLOG_ERROR, __VA_ARGS__)

/* Messages below this level are kept in the buffer but not printed. */
void klog_set_console_level(klog_level_t level);

/* Write raw text to the console, bypassing levels and the buffer (panic path). */
void klog_raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Console output: the early (serial) console plus an optional mirror such as
 * the framebuffer console. Callers keep interrupts disabled around a line.
 */
void kconsole_write(const char *text, size_t length);
void kconsole_set_mirror(void (*write)(const char *text, size_t length));
/* Only the mirror (for drivers that write the serial port themselves). */
void kconsole_mirror_char(char c);
/* Feed the log buffer's contents to `write` (a new console catching up). */
void klog_replay(void (*write)(const char *text, size_t length));

/* Copy up to `size` bytes of the retained log, starting `offset` bytes after its oldest byte. Returns the count. */
size_t klog_read(size_t offset, char *buffer, size_t size);

#endif
