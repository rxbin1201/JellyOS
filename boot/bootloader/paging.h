/*
 * JellyOS Boot Manager - x86_64 4-level page tables for the kernel handoff.
 *
 * Tables are allocated as boot data (BOOT_EFI_MEMORY_BOOT_DATA) and become
 * BOOTLOADER_RECLAIMABLE in the kernel's memory map.
 */

#ifndef BOOT_PAGING_H
#define BOOT_PAGING_H

#include "boot.h"

typedef struct {
    uint64_t *pml4;
    bool      nx_supported;
} page_tables_t;

#define MAP_WRITABLE   (1u << 0)
#define MAP_EXECUTABLE (1u << 1)

EFI_STATUS paging_create(page_tables_t *pt, bool nx_supported);

/* Map one 4 KiB page. Fails if the page is already mapped. */
EFI_STATUS paging_map_page(page_tables_t *pt, uint64_t virt, uint64_t phys, uint32_t map_flags);

/* Map [0, size) physical at virt_base with 2 MiB pages, read/write, non-executable. */
EFI_STATUS paging_map_direct(page_tables_t *pt, uint64_t virt_base, uint64_t size);

static inline uint64_t paging_root(const page_tables_t *pt)
{
    return (uint64_t)(UINTN)pt->pml4;
}

#endif
