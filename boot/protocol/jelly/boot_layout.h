/*
 * JellyOS Boot Protocol - virtual address layout established at kernel entry.
 *
 * See docs/architecture/memory-layout.md. These constants are part of the
 * boot contract; the kernel must still read hhdm_base from boot_info_t
 * instead of assuming BOOT_HHDM_BASE.
 */

#ifndef JELLY_BOOT_LAYOUT_H
#define JELLY_BOOT_LAYOUT_H

#define BOOT_PAGE_SIZE        0x1000ULL

/* Direct map of all physical memory (PML4 slot 256). */
#define BOOT_HHDM_BASE        0xFFFF800000000000ULL
#define BOOT_HHDM_MAX_SIZE    0x0000400000000000ULL /* 64 TiB */
#define BOOT_HHDM_MIN_SIZE    0x0000000100000000ULL /* always cover the low 4 GiB */

/* Kernel image: top 2 GiB, required by -mcmodel=kernel. */
#define BOOT_KERNEL_VMA_MIN   0xFFFFFFFF80000000ULL

/* Boot stack handed to the kernel. */
#define BOOT_STACK_SIZE       0x10000ULL /* 64 KiB */

#endif
