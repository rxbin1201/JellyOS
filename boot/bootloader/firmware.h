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

/* Describe the current GOP mode. Leaves fb zeroed if no linear framebuffer exists. */
void firmware_get_framebuffer(boot_framebuffer_t *fb);

#endif
