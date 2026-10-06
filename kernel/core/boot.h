/*
 * Access to the boot manager's handoff data.
 *
 * Everything the boot manager passes lives in BOOTLOADER_RECLAIMABLE memory.
 * boot_accept() copies boot_info_t and the arrays it references (memory map,
 * module list, boot log) into the kernel, so that memory can be reclaimed.
 * After boot_accept() the *_phys array fields in boot_info() are zero; use
 * the accessors below. Module contents stay in KERNEL_AND_MODULES memory.
 */

#ifndef CORE_BOOT_H
#define CORE_BOOT_H

#include <jelly/boot_info.h>
#include <stdbool.h>
#include <stddef.h>

/* Minimum boot_info_t version this kernel needs (matches its ELF note). */
#define KERNEL_REQUIRED_BOOT_VERSION 1

#define BOOT_MAX_MEMORY_ENTRIES 512
#define BOOT_MAX_MODULES        16
#define BOOT_LOG_MAX            16384

/* Validate and copy the boot manager's data. Must run before any reclaim. */
bool boot_accept(const boot_info_t *loader_info);

const boot_info_t *boot_info(void);

const boot_memory_entry_t *boot_memory_map(size_t *count);
const boot_module_t       *boot_modules(size_t *count);
const char                *boot_log(size_t *length);
const char                *boot_cmdline(void);

/* Entries dropped because the kernel's copies were too small (logged once logging works). */
size_t boot_truncated_entries(void);

/* True if the boot manager provided the field (it lies within info->size). */
#define boot_has_field(field) \
    (boot_info()->size >= __builtin_offsetof(boot_info_t, field) + sizeof(boot_info()->field))

#endif
