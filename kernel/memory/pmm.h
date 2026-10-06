/*
 * Physical memory manager (README section 13).
 *
 * A bitmap with one bit per 4 KiB frame (1 = in use). Frame states:
 *   free        - usable RAM, available for allocation
 *   allocated   - handed out by pmm_alloc_*
 *   reserved    - firmware, MMIO holes, kernel image, low memory, the bitmap
 *   reclaimable - boot-time data that becomes free via pmm_reclaim()
 */

#ifndef MEMORY_PMM_H
#define MEMORY_PMM_H

#include <jelly/boot_info.h>
#include <jelly/status.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t total_bytes;       /* RAM described by the memory map */
    uint64_t free_bytes;
    uint64_t allocated_bytes;
    uint64_t reclaimable_bytes; /* not yet reclaimed */
    uint64_t reserved_bytes;
} pmm_stats_t;

status_t pmm_init(const boot_memory_entry_t *entries, size_t count);

/* One frame. */
status_t pmm_alloc_page(uint64_t *phys);
void     pmm_free_page(uint64_t phys);

/* Physically contiguous frames. */
status_t pmm_alloc_pages(size_t count, uint64_t *phys);
void     pmm_free_pages(uint64_t phys, size_t count);

/* Release all regions of a reclaimable boot memory type. Returns bytes freed. */
uint64_t pmm_reclaim(boot_memory_type_t type);

void     pmm_get_stats(pmm_stats_t *stats);

/* True for memory map types that describe RAM (as opposed to MMIO or holes). */
static inline int pmm_is_ram_type(uint32_t type)
{
    return type == BOOT_MEMORY_USABLE || type == BOOT_MEMORY_BOOTLOADER_RECLAIMABLE ||
           type == BOOT_MEMORY_KERNEL_AND_MODULES || type == BOOT_MEMORY_ACPI_RECLAIMABLE ||
           type == BOOT_MEMORY_ACPI_NVS || type == BOOT_MEMORY_FIRMWARE_RUNTIME;
}

#endif
