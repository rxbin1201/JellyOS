/*
 * Early page-table primitives on top of the boot manager's tables.
 *
 * Phase 3 replaces the boot tables with the kernel's own virtual memory
 * manager. Until then, MMIO is reached through the direct map, which the boot
 * manager maps write-back; early_map_mmio() switches the affected direct-map
 * pages to uncached so device registers behave correctly.
 */

#ifndef ARCH_X86_64_EARLY_PAGING_H
#define ARCH_X86_64_EARLY_PAGING_H

#include <stdint.h>

void early_paging_init(uint64_t hhdm_base, uint64_t hhdm_size);

/* Return an uncached virtual address for [phys, phys + size), or NULL if outside the direct map. */
volatile void *early_map_mmio(uint64_t phys, uint64_t size);

#endif
