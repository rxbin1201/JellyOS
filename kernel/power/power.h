/*
 * System power control (README section 39, initial scope: shutdown and reboot).
 */

#ifndef POWER_POWER_H
#define POWER_POWER_H

#include <jelly/status.h>

/* Flush file systems, then power off through ACPI (S5). Returns only on failure. */
status_t power_off(void);

/* Flush file systems, then reset (ACPI reset register, 0xCF9, keyboard controller). */
status_t power_reboot(void);

#endif
