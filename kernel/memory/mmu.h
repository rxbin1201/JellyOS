/*
 * Page-table primitives implemented by the architecture layer
 * (kernel/arch/<arch>/paging.c). The VMM builds address spaces on top.
 */

#ifndef MEMORY_MMU_H
#define MEMORY_MMU_H

#include <jelly/status.h>
#include <stdbool.h>
#include <stdint.h>

/* Generic mapping flags. Pages are always readable. */
#define VM_WRITE           (1u << 0)
#define VM_EXEC            (1u << 1)
#define VM_USER            (1u << 2)
#define VM_GLOBAL          (1u << 3) /* kernel mappings shared by every address space */
#define VM_UNCACHED        (1u << 4) /* device registers */
#define VM_WRITE_COMBINING (1u << 5) /* framebuffers */
#define VM_OWNED           (1u << 6) /* the mapping owns the frame: freed on unmap */

/* Physical address of a top-level page table. */
typedef uint64_t mmu_root_t;

/* Program cache attribute tables and CPU paging features. */
void       arch_mmu_init(void);

/* Enable global pages once the kernel tables are active. */
void       arch_mmu_enable_global(void);

/*
 * New top-level table. With a template, the kernel half is shared with it, so
 * kernel mappings made later through the template appear in the new space.
 */
status_t   arch_mmu_create_root(mmu_root_t template_root, mmu_root_t *root);

/* Allocate every kernel-half top-level entry so it can be shared. */
status_t   arch_mmu_prepare_kernel_half(mmu_root_t root);

/* Map one page; page_size is PAGE_SIZE or LARGE_PAGE_SIZE. Fails with BUSY if mapped. */
status_t   arch_mmu_map(mmu_root_t root, uint64_t virt, uint64_t phys, uint64_t page_size, uint32_t flags);

/* Unmap one 4 KiB page, returning what was mapped. */
status_t   arch_mmu_unmap(mmu_root_t root, uint64_t virt, uint64_t *phys, uint32_t *flags);

/* Translate a virtual address. */
bool       arch_mmu_query(mmu_root_t root, uint64_t virt, uint64_t *phys, uint32_t *flags);

/* Free the user half: page tables, and frames mapped with VM_OWNED via release(). */
void       arch_mmu_destroy(mmu_root_t root, void (*release)(uint64_t phys));

void       arch_mmu_activate(mmu_root_t root);
mmu_root_t arch_mmu_current(void);

#endif
