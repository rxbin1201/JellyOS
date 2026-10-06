/*
 * JellyOS Boot Manager - kernel ELF validation and loading.
 */

#ifndef BOOT_ELF_LOADER_H
#define BOOT_ELF_LOADER_H

#include "boot.h"
#include "paging.h"

typedef struct {
    uint64_t entry;
    uint64_t phys_base;
    uint64_t virt_base;
    uint64_t size;
    uint32_t required_boot_version; /* from the JellyOS kernel note */
    uint32_t note_flags;            /* BOOT_NOTE_FLAG_* */
} loaded_kernel_t;

/*
 * Validate a kernel image. Checks the ELF header, all PT_LOAD segments,
 * the entry point and the JellyOS boot protocol note. Logs the reason on failure.
 */
EFI_STATUS elf_validate_kernel(const void *file, UINTN file_size, loaded_kernel_t *kernel);

/* Copy a validated kernel into fresh memory and map it into pt. */
EFI_STATUS elf_load_kernel(const void *file, page_tables_t *pt, loaded_kernel_t *kernel);

#endif
