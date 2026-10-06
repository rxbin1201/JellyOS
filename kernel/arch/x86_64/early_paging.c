#include "early_paging.h"

#include "cpu.h"

#define PTE_PRESENT   (1ULL << 0)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_LARGE     (1ULL << 7)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PAGE_SIZE     0x1000ULL

static uint64_t direct_base;
static uint64_t direct_size;

void early_paging_init(uint64_t hhdm_base, uint64_t hhdm_size)
{
    direct_base = hhdm_base;
    direct_size = hhdm_size;
}

static uint64_t *table_at(uint64_t entry)
{
    return (uint64_t *)(uintptr_t)(direct_base + (entry & PTE_ADDR_MASK));
}

/* Find the leaf entry mapping virt (any page size). */
static uint64_t *leaf_entry(uint64_t virt)
{
    uint64_t *table = table_at(cpu_read_cr3());

    for (int level = 4; level >= 1; level--) {
        uint64_t *entry = &table[(virt >> (12 + 9 * (level - 1))) & 0x1FF];
        if (!(*entry & PTE_PRESENT))
            return 0;
        if (level == 1 || (level <= 3 && (*entry & PTE_LARGE)))
            return entry;
        table = table_at(*entry);
    }
    return 0;
}

volatile void *early_map_mmio(uint64_t phys, uint64_t size)
{
    if (!size || phys >= direct_size || size > direct_size - phys)
        return 0;

    for (uint64_t page = phys & ~(PAGE_SIZE - 1); page < phys + size; page += PAGE_SIZE) {
        uint64_t virt = direct_base + page;
        uint64_t *entry = leaf_entry(virt);
        if (!entry)
            return 0;
        /* PCD + PWT selects uncached with the power-on PAT. */
        *entry |= PTE_PCD | PTE_PWT;
        cpu_invlpg(virt);
    }
    return (volatile void *)(uintptr_t)(direct_base + phys);
}
