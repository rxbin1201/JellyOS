#include "memory/vmm.h"

#include "memory/layout.h"
#include "memory/pmm.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __kernel_end[];

static vm_space_t kernel_space;
static uint64_t next_stack_slot = KERNEL_STACK_REGION;
static uint64_t next_mmio = MMIO_REGION;

vm_space_t *vmm_kernel_space(void)
{
    return &kernel_space;
}

/* --- Kernel address space -------------------------------------------------- */

static status_t map_direct_range(uint64_t base, uint64_t length, uint64_t *mapped)
{
    uint64_t start = align_down(base, PAGE_SIZE);
    uint64_t end = align_up(base + length, PAGE_SIZE);

    for (uint64_t phys = start; phys < end;) {
        uint64_t size = (phys % LARGE_PAGE_SIZE == 0 && end - phys >= LARGE_PAGE_SIZE) ? LARGE_PAGE_SIZE : PAGE_SIZE;
        status_t status = arch_mmu_map(kernel_space.root, hhdm_base + phys, phys, size, VM_WRITE | VM_GLOBAL);
        if (STATUS_IS_ERROR(status))
            return status;
        phys += size;
        *mapped += size;
    }
    return STATUS_SUCCESS;
}

static status_t map_kernel_section(const boot_info_t *info, const char *start, const char *end, uint32_t flags)
{
    for (uint64_t virt = (uint64_t)(uintptr_t)start; virt < (uint64_t)(uintptr_t)end; virt += PAGE_SIZE) {
        uint64_t phys = info->kernel.phys_base + (virt - info->kernel.virt_base);
        status_t status = arch_mmu_map(kernel_space.root, virt, phys, PAGE_SIZE, flags | VM_GLOBAL);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    return STATUS_SUCCESS;
}

status_t vmm_init(const boot_info_t *info, const boot_memory_entry_t *entries, size_t count)
{
    uint64_t direct_bytes = 0;
    status_t status;

    arch_mmu_init();

    status = arch_mmu_create_root(0, &kernel_space.root);
    if (!STATUS_IS_ERROR(status))
        status = arch_mmu_prepare_kernel_half(kernel_space.root);

    /* Direct map: RAM only. MMIO holes are mapped explicitly via vmm_map_mmio(). */
    for (size_t i = 0; i < count && !STATUS_IS_ERROR(status); i++) {
        if (pmm_is_ram_type(entries[i].type))
            status = map_direct_range(entries[i].base, entries[i].length, &direct_bytes);
    }

    /* The boot framebuffer stays where the early screen console draws (drivers/graphics/early_fb.c). */
    uint64_t unused = 0;
    if (!STATUS_IS_ERROR(status) && info->framebuffer.phys_base && info->framebuffer.size)
        status = map_direct_range(info->framebuffer.phys_base, info->framebuffer.size, &unused);

    if (!STATUS_IS_ERROR(status))
        status = map_kernel_section(info, __text_start, __text_end, VM_EXEC);
    if (!STATUS_IS_ERROR(status))
        status = map_kernel_section(info, __rodata_start, __rodata_end, 0);
    if (!STATUS_IS_ERROR(status))
        status = map_kernel_section(info, __data_start, __kernel_end, VM_WRITE);
    if (STATUS_IS_ERROR(status)) {
        klog_error("vmm: building the kernel address space failed: %s", status_name(status));
        return status;
    }

    arch_mmu_activate(kernel_space.root);
    arch_mmu_enable_global();

    klog_info("vmm: kernel address space active, direct map %lu MiB of RAM", direct_bytes >> 20);
    return STATUS_SUCCESS;
}

/* --- Address spaces -------------------------------------------------------- */

status_t vmm_space_create(vm_space_t *space)
{
    return arch_mmu_create_root(kernel_space.root, &space->root);
}

static void release_frame(uint64_t phys)
{
    pmm_free_page(phys);
}

void vmm_space_destroy(vm_space_t *space)
{
    if (arch_mmu_current() == space->root)
        arch_mmu_activate(kernel_space.root);
    arch_mmu_destroy(space->root, release_frame);
    space->root = 0;
}

void vmm_space_activate(vm_space_t *space)
{
    arch_mmu_activate(space->root);
}

static bool range_allowed(const vm_space_t *space, uint64_t virt, uint64_t size, uint32_t flags)
{
    if (virt & (PAGE_SIZE - 1) || size == 0)
        return false;
    if (flags & VM_USER)
        return space != &kernel_space && virt >= USER_SPACE_START && virt < USER_SPACE_END &&
               size <= USER_SPACE_END - virt;
    return virt >= KERNEL_SPACE_START && virt + size - 1 >= virt;
}

status_t vmm_map(vm_space_t *space, uint64_t virt, uint64_t phys, uint32_t flags)
{
    if (!range_allowed(space, virt, PAGE_SIZE, flags) || (phys & (PAGE_SIZE - 1)))
        return STATUS_INVALID_ARGUMENT;
    return arch_mmu_map(space->root, virt, phys, PAGE_SIZE, flags);
}

status_t vmm_unmap(vm_space_t *space, uint64_t virt)
{
    uint64_t phys;
    uint32_t flags;
    status_t status = arch_mmu_unmap(space->root, virt, &phys, &flags);

    if (status == STATUS_SUCCESS && (flags & VM_OWNED))
        pmm_free_page(phys);
    return status;
}

bool vmm_query(vm_space_t *space, uint64_t virt, uint64_t *phys, uint32_t *flags)
{
    return arch_mmu_query(space->root, virt, phys, flags);
}

status_t vmm_alloc(vm_space_t *space, uint64_t virt, uint64_t size, uint32_t flags)
{
    size = align_up(size, PAGE_SIZE);
    if (!range_allowed(space, virt, size, flags))
        return STATUS_INVALID_ARGUMENT;

    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
        uint64_t phys;
        status_t status = pmm_alloc_page(&phys);
        if (!STATUS_IS_ERROR(status)) {
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            status = arch_mmu_map(space->root, virt + offset, phys, PAGE_SIZE, flags | VM_OWNED);
            if (STATUS_IS_ERROR(status))
                pmm_free_page(phys);
        }
        if (STATUS_IS_ERROR(status)) {
            vmm_free(space, virt, offset); /* roll back what was mapped */
            return status;
        }
    }
    return STATUS_SUCCESS;
}

void vmm_free(vm_space_t *space, uint64_t virt, uint64_t size)
{
    for (uint64_t offset = 0; offset < align_up(size, PAGE_SIZE); offset += PAGE_SIZE)
        vmm_unmap(space, virt + offset);
}

/* --- Kernel regions -------------------------------------------------------- */

void *vmm_map_physical(uint64_t phys, uint64_t size, uint32_t flags)
{
    uint64_t start = align_down(phys, PAGE_SIZE);
    uint64_t length = align_up(phys + size, PAGE_SIZE) - start;
    uint64_t irq = arch_interrupts_save();

    if (size == 0 || (flags & (VM_USER | VM_OWNED | VM_EXEC)) || next_mmio + length > MMIO_REGION + MMIO_REGION_SIZE) {
        arch_interrupts_restore(irq);
        return NULL;
    }
    uint64_t virt = next_mmio;
    next_mmio += length;
    arch_interrupts_restore(irq);

    for (uint64_t offset = 0; offset < length; offset += PAGE_SIZE) {
        if (STATUS_IS_ERROR(arch_mmu_map(kernel_space.root, virt + offset, start + offset, PAGE_SIZE,
                                         flags | VM_GLOBAL)))
            return NULL;
    }
    return (void *)(uintptr_t)(virt + (phys - start));
}

volatile void *vmm_map_mmio(uint64_t phys, uint64_t size, uint32_t cache)
{
    if (!(cache & (VM_UNCACHED | VM_WRITE_COMBINING)))
        return NULL;
    return vmm_map_physical(phys, size, VM_WRITE | (cache & (VM_UNCACHED | VM_WRITE_COMBINING)));
}

void *vmm_phys_to_kernel(uint64_t phys, uint64_t size)
{
    uint64_t mapped_phys;
    uint32_t flags;

    /* RAM ranges are in the direct map; check both ends. */
    if (size && vmm_query(&kernel_space, hhdm_base + phys, &mapped_phys, &flags) &&
        vmm_query(&kernel_space, hhdm_base + phys + size - 1, &mapped_phys, &flags))
        return phys_to_virt(phys);
    return vmm_map_physical(phys, size, 0);
}

status_t vmm_protect(vm_space_t *space, uint64_t virt, uint64_t size, uint32_t flags)
{
    if ((virt & (PAGE_SIZE - 1)) || !range_allowed(space, virt, align_up(size, PAGE_SIZE), flags))
        return STATUS_INVALID_ARGUMENT;

    for (uint64_t offset = 0; offset < align_up(size, PAGE_SIZE); offset += PAGE_SIZE) {
        status_t status = arch_mmu_protect(space->root, virt + offset, flags);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    return STATUS_SUCCESS;
}

#define MAX_FREE_STACK_SLOTS 1024

static uint64_t free_stack_slots[MAX_FREE_STACK_SLOTS];
static size_t free_stack_slot_count;

status_t vmm_alloc_kernel_stack(uint64_t *top)
{
    uint64_t flags = arch_interrupts_save();
    uint64_t slot;

    if (free_stack_slot_count) {
        slot = free_stack_slots[--free_stack_slot_count];
    } else if (next_stack_slot + KERNEL_STACK_SLOT <= KERNEL_STACK_REGION + KERNEL_STACK_REGION_SIZE) {
        slot = next_stack_slot;
        next_stack_slot += KERNEL_STACK_SLOT;
    } else {
        arch_interrupts_restore(flags);
        return STATUS_OUT_OF_MEMORY;
    }
    arch_interrupts_restore(flags);

    /* The first page of every slot stays unmapped as the guard. */
    uint64_t base = slot + PAGE_SIZE;
    status_t status = vmm_alloc(&kernel_space, base, KERNEL_STACK_PAGES * PAGE_SIZE, VM_WRITE | VM_GLOBAL);
    if (status == STATUS_SUCCESS)
        *top = base + KERNEL_STACK_PAGES * PAGE_SIZE;
    return status;
}

void vmm_free_kernel_stack(uint64_t top)
{
    uint64_t base = top - KERNEL_STACK_PAGES * PAGE_SIZE;
    uint64_t slot = base - PAGE_SIZE;

    vmm_free(&kernel_space, base, KERNEL_STACK_PAGES * PAGE_SIZE);

    uint64_t flags = arch_interrupts_save();
    if (free_stack_slot_count < MAX_FREE_STACK_SLOTS)
        free_stack_slots[free_stack_slot_count++] = slot; /* otherwise the slot is not reused */
    arch_interrupts_restore(flags);
}

bool vmm_is_stack_guard(uint64_t address)
{
    if (address < KERNEL_STACK_REGION || address >= next_stack_slot)
        return false;
    return (address - KERNEL_STACK_REGION) % KERNEL_STACK_SLOT < PAGE_SIZE;
}

/* --- Page faults ----------------------------------------------------------- */

bool vmm_page_fault(uint64_t address, uint32_t access)
{
    (void)address;
    (void)access;
    return false; /* demand paging and copy-on-write arrive with processes */
}

const char *vmm_fault_cause(uint64_t address, uint32_t access)
{
    if (address < USER_SPACE_START)
        return "null pointer dereference";
    if (vmm_is_stack_guard(address))
        return "kernel stack overflow (guard page)";
    if (access & VM_FAULT_PRESENT) {
        if (access & VM_FAULT_WRITE)
            return "write to read-only page";
        if (access & VM_FAULT_EXEC)
            return "execution of non-executable page";
        if (access & VM_FAULT_USER)
            return "user access to kernel page";
        return "protection violation";
    }
    return address >= KERNEL_SPACE_START ? "unmapped kernel address" : "unmapped user address";
}
