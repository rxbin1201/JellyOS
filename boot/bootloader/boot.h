/*
 * JellyOS Boot Manager - shared definitions.
 */

#ifndef BOOT_BOOT_H
#define BOOT_BOOT_H

#include <efi.h>
#include <efilib.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Memory types for our own allocations. The UEFI specification reserves
 * 0x80000000-0xFFFFFFFF for OS loaders, so these show up unchanged in the
 * final memory map and can be translated into boot_memory_type_t.
 */
#define BOOT_EFI_MEMORY_KERNEL    ((EFI_MEMORY_TYPE)0x80000001) /* kernel image, modules */
#define BOOT_EFI_MEMORY_BOOT_DATA ((EFI_MEMORY_TYPE)0x80000002) /* boot_info, page tables, stack */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Allocate zeroed, page-aligned physical memory. Returns NULL on failure.
 * Every allocation is recorded so a failed boot attempt can release them all.
 */
void *boot_alloc_pages(UINTN pages, EFI_MEMORY_TYPE type);

/* Free everything allocated with boot_alloc_pages() since the last release. */
void  boot_alloc_release_all(void);

static inline uint64_t align_down(uint64_t value, uint64_t align)
{
    return value & ~(align - 1);
}

static inline uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
}

#endif
