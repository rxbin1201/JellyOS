/*
 * JellyOS Boot Manager - UEFI memory map capture and conversion.
 */

#ifndef BOOT_MEMORY_MAP_H
#define BOOT_MEMORY_MAP_H

#include "boot.h"

#include <jelly/boot_info.h>

typedef struct {
    UINT8               *buffer;      /* raw EFI_MEMORY_DESCRIPTORs */
    UINTN                capacity;    /* bytes */
    UINTN                size;        /* bytes used by the last fetch */
    UINTN                key;
    UINTN                desc_size;
    UINT32               desc_version;
    boot_memory_entry_t *entries;     /* converted map handed to the kernel */
    UINTN                entry_capacity;
} efi_memory_map_t;

/*
 * Allocate buffers large enough for the current map plus headroom for
 * allocations made before ExitBootServices(). No allocations happen afterwards.
 */
EFI_STATUS memory_map_prepare(efi_memory_map_t *map, UINTN headroom_entries);

/* Fetch the current map into the prepared buffer (allowed after a failed ExitBootServices). */
EFI_STATUS memory_map_fetch(efi_memory_map_t *map);

/* Highest physical end address of all RAM-like regions in the current map. */
uint64_t memory_map_highest_address(void);

/*
 * Translate the fetched map into boot_memory_entry_t: sorted, merged,
 * page aligned. Runs after ExitBootServices() and does not allocate.
 * Returns the number of entries written to map->entries.
 */
UINTN memory_map_convert(efi_memory_map_t *map);

#endif
