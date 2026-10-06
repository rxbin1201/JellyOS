/*
 * Kernel command line ("key=value key2 ...") from the boot manager.
 */

#ifndef CORE_CMDLINE_H
#define CORE_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

void cmdline_init(const char *cmdline);
const char *cmdline_get(void);

/* Copy the value of "key=value" into value. Returns false if the key is absent. */
bool cmdline_value(const char *key, char *value, size_t size);

#endif
