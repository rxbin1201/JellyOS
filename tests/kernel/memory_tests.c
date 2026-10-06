/*
 * Kernel tests: physical memory, virtual memory and heap.
 */

#include "tests/kernel/ktest.h"

#include "core/arch.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#include <stdint.h>

#define TEST_USER_BASE 0x400000ULL

static uint64_t free_bytes(void)
{
    pmm_stats_t stats;
    pmm_get_stats(&stats);
    return stats.free_bytes;
}

/* --- PMM ------------------------------------------------------------------- */

KTEST(pmm_single_pages)
{
    uint64_t before = free_bytes();
    uint64_t a, b;

    KASSERT(pmm_alloc_page(&a) == STATUS_SUCCESS);
    KASSERT(pmm_alloc_page(&b) == STATUS_SUCCESS);
    KEXPECT(a != b);
    KEXPECT((a & (PAGE_SIZE - 1)) == 0 && (b & (PAGE_SIZE - 1)) == 0);
    KEXPECT(a >= 0x100000 && b >= 0x100000);
    KEXPECT(free_bytes() == before - 2 * PAGE_SIZE);

    pmm_free_page(a);
    pmm_free_page(b);
    KEXPECT(free_bytes() == before);
}

KTEST(pmm_contiguous_pages)
{
    uint64_t before = free_bytes();
    uint64_t base;

    KASSERT(pmm_alloc_pages(16, &base) == STATUS_SUCCESS);
    /* Every frame of the run must now be in use: single allocations avoid it. */
    uint64_t other;
    KASSERT(pmm_alloc_page(&other) == STATUS_SUCCESS);
    KEXPECT(other < base || other >= base + 16 * PAGE_SIZE);
    pmm_free_page(other);

    pmm_free_pages(base, 16);
    KEXPECT(free_bytes() == before);
    KEXPECT(pmm_alloc_pages(0, &base) == STATUS_INVALID_ARGUMENT);
}

/* --- VMM ------------------------------------------------------------------- */

KTEST(vmm_user_space_lifecycle)
{
    uint64_t before = free_bytes();
    vm_space_t space;
    uint64_t phys;
    uint32_t flags;

    KASSERT(vmm_space_create(&space) == STATUS_SUCCESS);
    KASSERT(vmm_alloc(&space, TEST_USER_BASE, 3 * PAGE_SIZE, VM_USER | VM_WRITE) == STATUS_SUCCESS);

    KASSERT(vmm_query(&space, TEST_USER_BASE + PAGE_SIZE + 0x10, &phys, &flags));
    KEXPECT((flags & (VM_USER | VM_WRITE | VM_OWNED)) == (VM_USER | VM_WRITE | VM_OWNED));
    KEXPECT(!(flags & VM_EXEC));
    KEXPECT((phys & (PAGE_SIZE - 1)) == 0x10);

    /* Write through the direct map, read back through the user mapping. */
    *(volatile uint32_t *)phys_to_virt(phys) = 0xC0FFEE;
    vmm_space_activate(&space);
    arch_user_access_begin(); /* SMAP */
    uint32_t seen = *(volatile uint32_t *)(uintptr_t)(TEST_USER_BASE + PAGE_SIZE + 0x10);
    uint32_t zero = *(volatile uint32_t *)(uintptr_t)TEST_USER_BASE;
    arch_user_access_end();
    vmm_space_activate(vmm_kernel_space());
    KEXPECT(seen == 0xC0FFEE);
    KEXPECT(zero == 0);

    KEXPECT(vmm_unmap(&space, TEST_USER_BASE + PAGE_SIZE) == STATUS_SUCCESS);
    KEXPECT(!vmm_query(&space, TEST_USER_BASE + PAGE_SIZE, &phys, &flags));
    KEXPECT(vmm_unmap(&space, TEST_USER_BASE + PAGE_SIZE) == STATUS_NOT_FOUND);

    /* The kernel half is shared: kernel code stays mapped in the new space. */
    KEXPECT(vmm_query(&space, (uint64_t)(uintptr_t)&free_bytes, &phys, &flags));

    vmm_space_destroy(&space);
    KEXPECT(free_bytes() == before);
}

KTEST(vmm_rejects_invalid_mappings)
{
    vm_space_t space;
    uint64_t phys;

    KASSERT(vmm_space_create(&space) == STATUS_SUCCESS);
    KASSERT(pmm_alloc_page(&phys) == STATUS_SUCCESS);

    KEXPECT(vmm_map(&space, 0, phys, VM_USER) == STATUS_INVALID_ARGUMENT);              /* null page */
    KEXPECT(vmm_map(&space, KERNEL_SPACE_START, phys, VM_USER) == STATUS_INVALID_ARGUMENT); /* user flag in kernel half */
    KEXPECT(vmm_map(&space, TEST_USER_BASE, phys, 0) == STATUS_INVALID_ARGUMENT);       /* kernel mapping in user half */
    KEXPECT(vmm_map(&space, TEST_USER_BASE + 1, phys, VM_USER) == STATUS_INVALID_ARGUMENT);
    KEXPECT(vmm_map(vmm_kernel_space(), TEST_USER_BASE, phys, VM_USER) == STATUS_INVALID_ARGUMENT);
    KEXPECT(vmm_map(&space, TEST_USER_BASE, phys, VM_USER) == STATUS_SUCCESS);
    KEXPECT(vmm_map(&space, TEST_USER_BASE, phys, VM_USER) == STATUS_BUSY);              /* already mapped */

    vmm_space_destroy(&space); /* not VM_OWNED: the frame stays ours */
    pmm_free_page(phys);
}

KTEST(vmm_shared_mapping)
{
    vm_space_t a, b;
    uint64_t phys, seen;
    uint32_t flags;

    KASSERT(vmm_space_create(&a) == STATUS_SUCCESS);
    KASSERT(vmm_space_create(&b) == STATUS_SUCCESS);
    KASSERT(pmm_alloc_page(&phys) == STATUS_SUCCESS);

    KEXPECT(vmm_map(&a, TEST_USER_BASE, phys, VM_USER | VM_WRITE) == STATUS_SUCCESS);
    KEXPECT(vmm_map(&b, TEST_USER_BASE * 2, phys, VM_USER) == STATUS_SUCCESS);
    KEXPECT(vmm_query(&b, TEST_USER_BASE * 2, &seen, &flags) && seen == phys);
    KEXPECT(!(flags & VM_WRITE));

    vmm_space_destroy(&a);
    vmm_space_destroy(&b);
    pmm_free_page(phys);
}

KTEST(vmm_kernel_stack_guard)
{
    uint64_t top, phys;
    uint32_t flags;

    KASSERT(vmm_alloc_kernel_stack(&top) == STATUS_SUCCESS);
    uint64_t bottom = top - KERNEL_STACK_PAGES * PAGE_SIZE;

    KEXPECT(vmm_query(vmm_kernel_space(), top - 8, &phys, &flags));
    KEXPECT(vmm_query(vmm_kernel_space(), bottom, &phys, &flags));
    KEXPECT(!vmm_query(vmm_kernel_space(), bottom - 1, &phys, &flags));
    KEXPECT(vmm_is_stack_guard(bottom - 1));
    KEXPECT(vmm_is_stack_guard(bottom - PAGE_SIZE));
    KEXPECT(!vmm_is_stack_guard(bottom));
    KEXPECT(!vmm_is_stack_guard(top - 8));
}

KTEST(vmm_mmio_is_uncached)
{
    uint64_t phys;
    uint32_t flags;

    volatile void *regs = vmm_map_mmio(0xFEE00000, 0x1000, VM_UNCACHED);
    KASSERT(regs != NULL);
    KEXPECT(vmm_query(vmm_kernel_space(), (uint64_t)(uintptr_t)regs, &phys, &flags));
    KEXPECT(phys == 0xFEE00000);
    KEXPECT(flags & VM_UNCACHED);
    KEXPECT(!(flags & VM_EXEC));

    volatile void *fb = vmm_map_mmio(0x80000123, 0x2000, VM_WRITE_COMBINING);
    KASSERT(fb != NULL);
    KEXPECT(((uintptr_t)fb & 0xFFF) == 0x123);
    KEXPECT(vmm_query(vmm_kernel_space(), (uint64_t)(uintptr_t)fb, &phys, &flags) && (flags & VM_WRITE_COMBINING));
    KEXPECT(vmm_map_mmio(0xFEE00000, 0x1000, 0) == NULL); /* cache mode is mandatory */
}

KTEST(vmm_kernel_image_permissions)
{
    extern char __text_start[], __rodata_start[], __data_start[];
    uint64_t phys;
    uint32_t flags;

    KASSERT(vmm_query(vmm_kernel_space(), (uint64_t)(uintptr_t)__text_start, &phys, &flags));
    KEXPECT((flags & VM_EXEC) && !(flags & VM_WRITE));
    KASSERT(vmm_query(vmm_kernel_space(), (uint64_t)(uintptr_t)__rodata_start, &phys, &flags));
    KEXPECT(!(flags & VM_EXEC) && !(flags & VM_WRITE));
    KASSERT(vmm_query(vmm_kernel_space(), (uint64_t)(uintptr_t)__data_start, &phys, &flags));
    KEXPECT(!(flags & VM_EXEC) && (flags & VM_WRITE));
}

/* --- Heap ------------------------------------------------------------------ */

KTEST(heap_sizes_and_alignment)
{
    static const size_t sizes[] = { 1, 15, 16, 17, 100, 1000, 2032, 2033, 4096, 10000, 100000 };
    void *blocks[sizeof(sizes) / sizeof(sizes[0])];
    heap_stats_t before, after;

    heap_get_stats(&before);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        blocks[i] = kmalloc(sizes[i]);
        KASSERT(blocks[i] != NULL);
        KEXPECT(((uintptr_t)blocks[i] & (HEAP_ALIGNMENT - 1)) == 0);
        memset(blocks[i], (int)i, sizes[i]);
    }
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const uint8_t *bytes = blocks[i];
        KEXPECT(bytes[0] == i && bytes[sizes[i] - 1] == i);
        kfree(blocks[i]);
    }
    heap_get_stats(&after);
    KEXPECT(after.allocations == before.allocations);
    KEXPECT(after.bytes_in_use == before.bytes_in_use);
    KEXPECT(kmalloc(0) == NULL);
}

KTEST(heap_reuses_freed_blocks)
{
    void *a = kmalloc(48);
    KASSERT(a != NULL);
    kfree(a);
    void *b = kmalloc(40);
    KEXPECT(a == b); /* same size class, LIFO free list */
    kfree(b);
}

KTEST(heap_realloc_keeps_data)
{
    char *p = kmalloc(10);
    KASSERT(p != NULL);
    memcpy(p, "jellyfish", 10);

    char *same = krealloc(p, 16);  /* still fits the 32-byte class (16 header + 16 payload) */
    KEXPECT(same == p);
    char *grown = krealloc(same, 5000);
    KASSERT(grown != NULL);
    KEXPECT(memcmp(grown, "jellyfish", 10) == 0);
    KEXPECT(krealloc(grown, 0) == NULL); /* frees */
}

KTEST(heap_calloc)
{
    uint32_t *values = kcalloc(1000, sizeof(uint32_t));
    KASSERT(values != NULL);
    bool zero = true;
    for (int i = 0; i < 1000; i++)
        zero &= values[i] == 0;
    KEXPECT(zero);
    kfree(values);

    KEXPECT(kcalloc(SIZE_MAX / 2, 4) == NULL); /* overflow */
}
