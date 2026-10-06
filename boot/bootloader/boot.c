#include "boot.h"

#include <jelly/boot_layout.h>

#define MAX_TRACKED_ALLOCATIONS 512

typedef struct {
    EFI_PHYSICAL_ADDRESS base;
    UINTN                pages;
} allocation_t;

static allocation_t allocations[MAX_TRACKED_ALLOCATIONS];
static UINTN allocation_count;

void *boot_alloc_pages(UINTN pages, EFI_MEMORY_TYPE type)
{
    EFI_PHYSICAL_ADDRESS addr = 0;

    if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, type, pages, &addr)))
        return NULL;
    ZeroMem((void *)(UINTN)addr, pages * BOOT_PAGE_SIZE);

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
}
