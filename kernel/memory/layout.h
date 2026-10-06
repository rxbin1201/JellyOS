/*
 * Kernel virtual memory layout (x86_64, 48-bit canonical addresses).
 * See docs/architecture/memory-layout.md. All region bases live here;
 * no other file hardcodes virtual addresses.
 */

#ifndef MEMORY_LAYOUT_H
#define MEMORY_LAYOUT_H

#include <stdint.h>

#define PAGE_SIZE                0x1000ULL
#define LARGE_PAGE_SIZE          0x200000ULL

/* User space: page 0 is never mapped (null pointer guard). */
#define USER_SPACE_START         0x0000000000001000ULL
#define USER_SPACE_END           0x0000800000000000ULL

/* Kernel half: direct map at hhdm_base (from boot_info), regions below. */
#define KERNEL_SPACE_START       0xFFFF800000000000ULL

/* Kernel stacks, each preceded by an unmapped guard page. */
#define KERNEL_STACK_REGION      0xFFFFC00000000000ULL
#define KERNEL_STACK_REGION_SIZE 0x0000010000000000ULL /* 1 TiB */
#define KERNEL_STACK_PAGES       16                    /* 64 KiB per stack */
#define KERNEL_STACK_SLOT        ((KERNEL_STACK_PAGES + 1) * PAGE_SIZE)

/* Device registers and framebuffers, mapped with explicit cache attributes. */
#define MMIO_REGION              0xFFFFC10000000000ULL
#define MMIO_REGION_SIZE         0x0000010000000000ULL /* 1 TiB */

/*
 * Loadable modules: inside the top 2 GiB next to the kernel image, so module
 * code built with -mcmodel=kernel can reach kernel symbols with 32-bit
 * relocations.
 */
#define MODULE_REGION            0xFFFFFFFFA0000000ULL
#define MODULE_REGION_SIZE       0x0000000020000000ULL /* 512 MiB */

static inline uint64_t align_down(uint64_t value, uint64_t align)
{
    return value & ~(align - 1);
}

static inline uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
}

/* Direct map of physical memory (set once from boot_info). */
extern uint64_t hhdm_base;

static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(hhdm_base + phys);
}

static inline uint64_t virt_to_phys(const void *virt)
{
    return (uint64_t)(uintptr_t)virt - hhdm_base;
}

#endif
