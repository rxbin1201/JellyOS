#include "paging.h"

#include <jelly/boot_layout.h>

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_LARGE     (1ULL << 7)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define LARGE_PAGE_SIZE 0x200000ULL

static inline unsigned table_index(uint64_t virt, unsigned level)
{
    /* level 4 = PML4, 3 = PDPT, 2 = PD, 1 = PT */
    return (unsigned)(virt >> (12 + 9 * (level - 1))) & 0x1FF;
}

static uint64_t *alloc_table(void)
{
    return boot_alloc_pages(1, BOOT_EFI_MEMORY_BOOT_DATA);
}

/* Return the next-level table referenced by table[index], creating it if needed. */
static uint64_t *next_table(uint64_t *table, unsigned index)
{
    uint64_t entry = table[index];

    if (entry & PTE_PRESENT) {
        if (entry & PTE_LARGE)
            return NULL; /* conflicts with an existing large mapping */
        return (uint64_t *)(UINTN)(entry & PTE_ADDR_MASK);
    }

    uint64_t *next = alloc_table();
    if (!next)
        return NULL;

    /* Intermediate entries are permissive; leaves decide the final rights. */
    table[index] = (uint64_t)(UINTN)next | PTE_PRESENT | PTE_WRITABLE;
    return next;
}

static uint64_t leaf_flags(const page_tables_t *pt, uint32_t map_flags)
{
    uint64_t flags = PTE_PRESENT;

    if (map_flags & MAP_WRITABLE)
        flags |= PTE_WRITABLE;
    if (!(map_flags & MAP_EXECUTABLE) && pt->nx_supported)
        flags |= PTE_NX;
    return flags;
}

EFI_STATUS paging_create(page_tables_t *pt, bool nx_supported)
{
    pt->pml4 = alloc_table();
    pt->nx_supported = nx_supported;
    return pt->pml4 ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

EFI_STATUS paging_map_page(page_tables_t *pt, uint64_t virt, uint64_t phys, uint32_t map_flags)
{
    if ((virt | phys) & (BOOT_PAGE_SIZE - 1))
        return EFI_INVALID_PARAMETER;

    uint64_t *table = pt->pml4;
    for (unsigned level = 4; level > 1; level--) {
        table = next_table(table, table_index(virt, level));
        if (!table)
            return EFI_OUT_OF_RESOURCES;
    }

    uint64_t *entry = &table[table_index(virt, 1)];
    if (*entry & PTE_PRESENT)
        return EFI_ALREADY_STARTED;

    *entry = phys | leaf_flags(pt, map_flags);
    return EFI_SUCCESS;
}

EFI_STATUS paging_map_direct(page_tables_t *pt, uint64_t virt_base, uint64_t size)
{
    uint64_t flags = leaf_flags(pt, MAP_WRITABLE) | PTE_LARGE;

    if ((virt_base | size) & (LARGE_PAGE_SIZE - 1))
        return EFI_INVALID_PARAMETER;

    for (uint64_t offset = 0; offset < size; offset += LARGE_PAGE_SIZE) {
        uint64_t virt = virt_base + offset;
        uint64_t *table = pt->pml4;

        for (unsigned level = 4; level > 2; level--) {
            table = next_table(table, table_index(virt, level));
            if (!table)
                return EFI_OUT_OF_RESOURCES;
        }

        uint64_t *entry = &table[table_index(virt, 2)];
        if (*entry & PTE_PRESENT)
            return EFI_ALREADY_STARTED;
        *entry = offset | flags;
    }
    return EFI_SUCCESS;
}
