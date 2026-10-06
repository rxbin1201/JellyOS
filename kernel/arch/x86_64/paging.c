/*
 * x86_64 4-level page tables (README section 11: page-table primitives).
 *
 * Cache attributes use the PAT, programmed as:
 *   index 0 WB, 1 WC, 2 UC-, 3 UC (repeated for 4-7)
 * so PWT selects write-combining and PCD|PWT selects uncached.
 */

#include "cpu.h"

#include "memory/layout.h"
#include "memory/mmu.h"
#include "memory/pmm.h"

#include "core/log.h"
#include "core/string.h"

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITE     (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_LARGE     (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_OWNED     (1ULL << 9)  /* software bit */
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define MSR_PAT       0x277
#define PAT_VALUE     0x0007010600070106ULL /* UC UC- WC WB | UC UC- WC WB */
#define CR4_PGE       (1ULL << 7)

#define KERNEL_HALF_FIRST 256
#define ENTRIES           512

static bool nx_enabled;
static bool pat_enabled;

static inline uint64_t *table(uint64_t phys)
{
    return phys_to_virt(phys & PTE_ADDR_MASK);
}

static inline unsigned index_at(uint64_t virt, int level)
{
    return (virt >> (12 + 9 * (level - 1))) & 0x1FF;
}

void arch_mmu_init(void)
{
    uint32_t a, b, c, d;

    nx_enabled = cpu_features.nx;
    if (nx_enabled && !(cpu_read_msr(MSR_EFER) & EFER_NXE))
        cpu_write_msr(MSR_EFER, cpu_read_msr(MSR_EFER) | EFER_NXE);

    cpu_cpuid(1, 0, &a, &b, &c, &d);
    pat_enabled = d & (1u << 16);
    if (pat_enabled)
        cpu_write_msr(MSR_PAT, PAT_VALUE); /* takes full effect with the CR3 switch */
    else
        klog_warn("mmu: no PAT, write-combining falls back to uncached");
}

void arch_mmu_enable_global(void)
{
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | CR4_PGE) : "memory");
}

static status_t alloc_table(uint64_t *phys)
{
    status_t status = pmm_alloc_page(phys);
    if (status == STATUS_SUCCESS)
        memset(phys_to_virt(*phys), 0, PAGE_SIZE);
    return status;
}

static uint64_t leaf_bits(uint32_t flags)
{
    uint64_t bits = PTE_PRESENT;

    if (flags & VM_WRITE)
        bits |= PTE_WRITE;
    if (flags & VM_USER)
        bits |= PTE_USER;
    else if (flags & VM_GLOBAL)
        bits |= PTE_GLOBAL;
    if (!(flags & VM_EXEC) && nx_enabled)
        bits |= PTE_NX;
    if (flags & VM_UNCACHED)
        bits |= PTE_PCD | PTE_PWT;
    else if (flags & VM_WRITE_COMBINING)
        bits |= pat_enabled ? PTE_PWT : PTE_PCD | PTE_PWT;
    if (flags & VM_OWNED)
        bits |= PTE_OWNED;
    return bits;
}

static uint32_t leaf_flags(uint64_t entry)
{
    uint32_t flags = 0;

    if (entry & PTE_WRITE)
        flags |= VM_WRITE;
    if (entry & PTE_USER)
        flags |= VM_USER;
    if (entry & PTE_GLOBAL)
        flags |= VM_GLOBAL;
    if (!(entry & PTE_NX))
        flags |= VM_EXEC;
    if ((entry & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT))
        flags |= VM_UNCACHED;
    else if (entry & PTE_PWT)
        flags |= VM_WRITE_COMBINING;
    if (entry & PTE_OWNED)
        flags |= VM_OWNED;
    return flags;
}

/*
 * Walk to the entry at target_level (1 = 4 KiB, 2 = 2 MiB) for virt.
 * Missing tables are created if requested. Returns NULL if a table is
 * missing (create == false) or a large page is in the way.
 */
static uint64_t *walk(mmu_root_t root, uint64_t virt, int target_level, bool create, status_t *status)
{
    uint64_t *t = table(root);
    *status = STATUS_SUCCESS;

    for (int level = 4; level > target_level; level--) {
        uint64_t *entry = &t[index_at(virt, level)];

        if (!(*entry & PTE_PRESENT)) {
            if (!create) {
                *status = STATUS_NOT_FOUND;
                return NULL;
            }
            uint64_t phys;
            *status = alloc_table(&phys);
            if (STATUS_IS_ERROR(*status))
                return NULL;
            /* Intermediate entries allow everything; leaves restrict. */
            *entry = phys | PTE_PRESENT | PTE_WRITE | (virt < USER_SPACE_END ? PTE_USER : 0);
        } else if (*entry & PTE_LARGE) {
            *status = STATUS_BUSY;
            return NULL;
        }
        t = table(*entry);
    }
    return &t[index_at(virt, target_level)];
}

static void flush(mmu_root_t root, uint64_t virt)
{
    if (root == arch_mmu_current() || virt >= KERNEL_SPACE_START)
        cpu_invlpg(virt);
}

status_t arch_mmu_create_root(mmu_root_t template_root, mmu_root_t *root)
{
    status_t status = alloc_table(root);

    if (status == STATUS_SUCCESS && template_root) {
        uint64_t *dst = table(*root);
        const uint64_t *src = table(template_root);
        for (unsigned i = KERNEL_HALF_FIRST; i < ENTRIES; i++)
            dst[i] = src[i];
    }
    return status;
}

status_t arch_mmu_prepare_kernel_half(mmu_root_t root)
{
    uint64_t *top = table(root);

    for (unsigned i = KERNEL_HALF_FIRST; i < ENTRIES; i++) {
        if (top[i] & PTE_PRESENT)
            continue;
        uint64_t phys;
        status_t status = alloc_table(&phys);
        if (STATUS_IS_ERROR(status))
            return status;
        top[i] = phys | PTE_PRESENT | PTE_WRITE;
    }
    return STATUS_SUCCESS;
}

status_t arch_mmu_map(mmu_root_t root, uint64_t virt, uint64_t phys, uint64_t page_size, uint32_t flags)
{
    int level = page_size == LARGE_PAGE_SIZE ? 2 : 1;
    status_t status;

    if ((virt | phys) & (page_size - 1))
        return STATUS_INVALID_ARGUMENT;

    uint64_t *entry = walk(root, virt, level, true, &status);
    if (!entry)
        return status;
    if (*entry & PTE_PRESENT)
        return STATUS_BUSY;

    *entry = phys | leaf_bits(flags) | (level == 2 ? PTE_LARGE : 0);
    flush(root, virt);
    return STATUS_SUCCESS;
}

status_t arch_mmu_unmap(mmu_root_t root, uint64_t virt, uint64_t *phys, uint32_t *flags)
{
    status_t status;
    uint64_t *entry = walk(root, virt, 1, false, &status);

    if (!entry)
        return status == STATUS_BUSY ? STATUS_NOT_SUPPORTED : STATUS_NOT_FOUND;
    if (!(*entry & PTE_PRESENT))
        return STATUS_NOT_FOUND;

    *phys = *entry & PTE_ADDR_MASK;
    *flags = leaf_flags(*entry);
    *entry = 0;
    flush(root, virt);
    return STATUS_SUCCESS;
}

bool arch_mmu_query(mmu_root_t root, uint64_t virt, uint64_t *phys, uint32_t *flags)
{
    uint64_t *t = table(root);

    for (int level = 4; level >= 1; level--) {
        uint64_t entry = t[index_at(virt, level)];
        if (!(entry & PTE_PRESENT))
            return false;
        if (level == 1 || (entry & PTE_LARGE)) {
            uint64_t page_mask = (1ULL << (12 + 9 * (level - 1))) - 1;
            *phys = (entry & PTE_ADDR_MASK & ~page_mask) | (virt & page_mask);
            *flags = leaf_flags(entry);
            return true;
        }
        t = table(entry);
    }
    return false;
}

static void destroy_level(uint64_t table_phys, int level, void (*release)(uint64_t))
{
    uint64_t *t = table(table_phys);

    for (unsigned i = 0; i < ENTRIES; i++) {
        uint64_t entry = t[i];
        if (!(entry & PTE_PRESENT))
            continue;
        if (level == 1 || (entry & PTE_LARGE)) {
            if ((entry & PTE_OWNED) && release)
                release(entry & PTE_ADDR_MASK);
            continue;
        }
        destroy_level(entry & PTE_ADDR_MASK, level - 1, release);
    }
    pmm_free_page(table_phys);
}

void arch_mmu_destroy(mmu_root_t root, void (*release)(uint64_t))
{
    uint64_t *top = table(root);

    for (unsigned i = 0; i < KERNEL_HALF_FIRST; i++) {
        if (top[i] & PTE_PRESENT)
            destroy_level(top[i] & PTE_ADDR_MASK, 3, release);
    }
    pmm_free_page(root);
}

void arch_mmu_activate(mmu_root_t root)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}

mmu_root_t arch_mmu_current(void)
{
    return cpu_read_cr3() & PTE_ADDR_MASK;
}
