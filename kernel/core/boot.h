/*
 * Access to the boot manager's handoff data.
 *
 * boot_info_t lives in BOOTLOADER_RECLAIMABLE memory; boot_accept() copies
 * the top-level structure into the kernel so it stays valid. Arrays it
 * references (memory map, modules, log) are still in boot memory and must be
 * consumed or copied before that memory is reclaimed (Phase 3).
 */

#ifndef CORE_BOOT_H
#define CORE_BOOT_H

#include <jelly/boot_info.h>
#include <stdbool.h>

/* Minimum boot_info_t version this kernel needs (matches its ELF note). */
#define KERNEL_REQUIRED_BOOT_VERSION 1

/* Validate and copy the boot manager's boot_info_t. */
bool boot_accept(const boot_info_t *loader_info);

const boot_info_t *boot_info(void);

/* True if the boot manager provided the field (it lies within info->size). */
#define boot_has_field(field) \
    (boot_info()->size >= __builtin_offsetof(boot_info_t, field) + sizeof(boot_info()->field))

/* Physical address -> direct-map virtual address. */
void *boot_phys_to_virt(uint64_t phys);

#endif
