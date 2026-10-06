/*
 * Minimal printf-style formatting for the kernel.
 *
 * Supported: %s %c %d %i %u %x %X %p %%, length modifiers l, ll, z,
 * flags '-' and '0', and a field width.
 */

#ifndef CORE_FORMAT_H
#define CORE_FORMAT_H

#include <stdarg.h>
#include <stddef.h>

/* Like vsnprintf: always terminates, returns the untruncated length. */
size_t format_v(char *buffer, size_t size, const char *fmt, va_list args);
size_t format(char *buffer, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
