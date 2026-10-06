/*
 * Virtual memory manager (README section 14).
 *
 * The kernel address space maps the direct map (RAM only), the kernel image
 * with per-section rights, guarded kernel stacks and MMIO. User address
 * spaces share the kernel half and own their lower half.
 */

#ifndef MEMORY_VMM_H
#define MEMORY_VMM_H

#include "memory/mmu.h"

#include <jelly/boot_info.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    mmu_root_t root;
} vm_space_t;

/* Page fault access bits for vmm_page_fault(). */
#define VM_FAULT_PRESENT (1u << 0) /* protection violation, page was mapped */
#define VM_FAULT_WRITE   (1u << 1)
#define VM_FAULT_USER    (1u << 2)
#define VM_FAULT_EXEC    (1u << 3)

/* Build the kernel address space and switch to it. */
status_t    vmm_init(const boot_info_t *info, const boot_memory_entry_t *entries, size_t count);

vm_space_t *vmm_kernel_space(void);

/* User address spaces. Destroy frees page tables and all VM_OWNED frames. */
status_t    vmm_space_create(vm_space_t *space);
void        vmm_space_destroy(vm_space_t *space);
void        vmm_space_activate(vm_space_t *space);

/* Single 4 KiB pages. VM_USER mappings must lie in user space, others in kernel space. */
status_t    vmm_map(vm_space_t *space, uint64_t virt, uint64_t phys, uint32_t flags);
status_t    vmm_unmap(vm_space_t *space, uint64_t virt);
bool        vmm_query(vm_space_t *space, uint64_t virt, uint64_t *phys, uint32_t *flags);

/* Back [virt, virt + size) with zeroed, owned frames; all-or-nothing. */
status_t    vmm_alloc(vm_space_t *space, uint64_t virt, uint64_t size, uint32_t flags);
void        vmm_free(vm_space_t *space, uint64_t virt, uint64_t size);

/* Map device memory with VM_UNCACHED or VM_WRITE_COMBINING. Returns NULL on failure. */
volatile void *vmm_map_mmio(uint64_t phys, uint64_t size, uint32_t cache);

/* Allocate a KERNEL_STACK_PAGES stack below an unmapped guard page. */
status_t    vmm_alloc_kernel_stack(uint64_t *top);
bool        vmm_is_stack_guard(uint64_t address);

/* Try to resolve a page fault (no demand paging yet). Returns true if handled. */
bool        vmm_page_fault(uint64_t address, uint32_t access);

/* Human-readable cause of an unresolved page fault. */
const char *vmm_fault_cause(uint64_t address, uint32_t access);

#endif
