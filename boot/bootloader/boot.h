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
 * What our own allocations are for. These are not memory types of the
 * firmware: everything is allocated as EfiLoaderData. The UEFI
 * specification does reserve the types 0x80000000-0xFFFFFFFF for OS
 * loaders, but real firmware is not reliable with them (a Lenovo
 * ThinkCentre hung in ExitBootServices()). The boot manager remembers the
 * kernel's ranges itself and marks them in the map it hands over
 * (memory_map_convert()).
 */
#define BOOT_EFI_MEMORY_KERNEL    ((EFI_MEMORY_TYPE)0x80000001) /* kernel image, modules */
#define BOOT_EFI_MEMORY_BOOT_DATA ((EFI_MEMORY_TYPE)0x80000002) /* boot_info, page tables, stack */

typedef struct {
    uint64_t base;
    uint64_t length;
} boot_range_t;

/* The ranges allocated as BOOT_EFI_MEMORY_KERNEL. */
const boot_range_t *boot_kernel_ranges(UINTN *count);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Allocate zeroed, page-aligned physical memory for one of the purposes
 * above (or with a real firmware memory type). Returns NULL on failure.
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
