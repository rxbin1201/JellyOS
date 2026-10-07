#include "boot.h"

#include <jelly/boot_layout.h>

#define MAX_TRACKED_ALLOCATIONS 512

typedef struct {
    EFI_PHYSICAL_ADDRESS base;
    UINTN                pages;
} allocation_t;

#define MAX_KERNEL_RANGES 64

static allocation_t allocations[MAX_TRACKED_ALLOCATIONS];
static UINTN allocation_count;
static boot_range_t kernel_ranges[MAX_KERNEL_RANGES];
static UINTN kernel_range_count;

const boot_range_t *boot_kernel_ranges(UINTN *count)
{
    *count = kernel_range_count;
    return kernel_ranges;
}

void *boot_alloc_pages(UINTN pages, EFI_MEMORY_TYPE type)
{
    EFI_PHYSICAL_ADDRESS addr = 0;
    bool kernel = type == BOOT_EFI_MEMORY_KERNEL;

    /* The kernel must know which memory holds its image: without a slot to remember it, fail. */
    if (kernel && kernel_range_count == MAX_KERNEL_RANGES)
        return NULL;
    if (kernel || type == BOOT_EFI_MEMORY_BOOT_DATA)
        type = EfiLoaderData;
    if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, type, pages, &addr)))
        return NULL;
    ZeroMem((void *)(UINTN)addr, pages * BOOT_PAGE_SIZE);
    if (kernel)
        kernel_ranges[kernel_range_count++] = (boot_range_t){ addr, pages * BOOT_PAGE_SIZE };

    /* Untracked allocations are only leaked if a boot attempt fails. */
    if (allocation_count < MAX_TRACKED_ALLOCATIONS)
        allocations[allocation_count++] = (allocation_t){ addr, pages };
    return (void *)(UINTN)addr;
}

void boot_alloc_release_all(void)
{
    while (allocation_count) {
        allocation_t *a = &allocations[--allocation_count];
        BS->FreePages(a->base, a->pages);
    }
    kernel_range_count = 0;
}
