/*
 * JellyOS Boot Manager - firmware information for boot_info_t.
 */

#ifndef BOOT_FIRMWARE_H
#define BOOT_FIRMWARE_H

#include "boot.h"

#include <jelly/boot_info.h>

/* Fill info->acpi, info->smbios, info->uefi and the Secure Boot flag. */
void firmware_collect(EFI_SYSTEM_TABLE *st, boot_info_t *info);

/* Secure Boot state reported by the firmware. */
bool firmware_secure_boot_enabled(void);

/*
 * Switch the screen to the resolution of boot.cfg: "max" (the monitor's native
 * resolution if the firmware has it, else the largest it offers that the
 * monitor can show; at most 1920x1080 if the monitor is unknown), "keep" (whatever the firmware set) or "WIDTHxHEIGHT".
 */
void firmware_select_resolution(const char *setting);

/* Describe the current GOP mode. Leaves fb zeroed if no linear framebuffer exists. */
void firmware_get_framebuffer(boot_framebuffer_t *fb);

#endif
