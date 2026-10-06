/*
 * JellyOS Boot Manager - system information and diagnostics screen (README section 8).
 */

#ifndef BOOT_DIAGNOSTICS_H
#define BOOT_DIAGNOSTICS_H

#include "boot.h"
#include "boot_tracker.h"
#include "config.h"

#include <jelly/boot_info.h>

void diagnostics_collect_cpu(boot_cpu_info_t *cpu);
void diagnostics_collect_boot_device(EFI_HANDLE image, boot_device_t *device);

/* Print everything known about this boot. kernel_path may differ from entry->kernel (rollback). */
void diagnostics_print(const boot_info_t *info, const boot_entry_t *entry, const char *kernel_path,
                       const boot_tracker_t *tracker);

/* Full-screen diagnostics for the default entry; waits for a key. */
void diagnostics_show(EFI_HANDLE image, const boot_config_t *config, const boot_tracker_t *tracker);

#endif
