/*
 * JellyOS Boot Protocol - boot_info_t ABI.
 *
 * Shared contract between the JellyOS Boot Manager and the kernel.
 * See docs/architecture/boot-protocol.md for the full specification.
 *
 * Rules:
 *  - Fixed-width types only (the loader is built with the MS ABI, the kernel with SysV).
 *  - All addresses inside boot_info_t are PHYSICAL addresses stored as uint64_t.
 *    The kernel converts them with hhdm_base. No raw pointers cross the boundary.
 *  - Fields are only ever appended. Existing fields never change meaning.
 *  - Arrays carry an entry_size so their element types can grow too.
 */

#ifndef JELLY_BOOT_INFO_H
#define JELLY_BOOT_INFO_H

#include <stdint.h>

/* "JELLYBI\0" little-endian. Changes only on an incompatible protocol break. */
#define BOOT_INFO_MAGIC   0x004942594C4C454AULL

/* Current boot_info_t version written by the boot manager. */
#define BOOT_INFO_VERSION 1

/* --- Kernel compatibility note ---------------------------------------------
 *
 * Every JellyOS kernel ELF must contain a PT_NOTE with:
 *   name = "JellyOS", type = BOOT_NOTE_TYPE_PROTOCOL, desc = boot_note_protocol_t
 * The boot manager refuses kernels without the note or kernels that require a
 * newer boot_info version than it can provide.
 */

#define BOOT_NOTE_NAME          "JellyOS"
#define BOOT_NOTE_TYPE_PROTOCOL 1

typedef struct {
    uint32_t required_version; /* minimum boot_info_t version the kernel needs */
    uint32_t reserved;
} boot_note_protocol_t;

/* --- Boot flags ------------------------------------------------------------ */

#define BOOT_FLAG_DEBUG       (1ULL << 0) /* verbose boot, "debug=1" */
#define BOOT_FLAG_SAFE_MODE   (1ULL << 1) /* minimal driver set, "safe_mode=1" */
#define BOOT_FLAG_RECOVERY    (1ULL << 2) /* recovery boot, "recovery=1" */
#define BOOT_FLAG_SECURE_BOOT (1ULL << 3) /* firmware reports Secure Boot enabled */

/* --- Memory map ------------------------------------------------------------ */

typedef enum {
    BOOT_MEMORY_USABLE                 = 1, /* free RAM */
    BOOT_MEMORY_RESERVED               = 2, /* never touch */
    BOOT_MEMORY_ACPI_RECLAIMABLE       = 3, /* usable after ACPI tables were parsed */
    BOOT_MEMORY_ACPI_NVS               = 4, /* must be preserved */
    BOOT_MEMORY_BAD                    = 5, /* defective RAM */
    BOOT_MEMORY_BOOTLOADER_RECLAIMABLE = 6, /* boot data, page tables, boot stack, firmware boot services */
    BOOT_MEMORY_KERNEL_AND_MODULES     = 7, /* loaded kernel image and boot modules */
    BOOT_MEMORY_FIRMWARE_RUNTIME       = 8, /* UEFI runtime services code/data */
} boot_memory_type_t;

typedef struct {
    uint64_t base;   /* physical, page aligned */
    uint64_t length; /* bytes, multiple of the page size */
    uint32_t type;   /* boot_memory_type_t */
    uint32_t reserved;
} boot_memory_entry_t;

typedef struct {
    uint64_t entries_phys; /* array of boot_memory_entry_t, sorted by base, non-overlapping */
    uint64_t entry_count;
    uint32_t entry_size;   /* stride in bytes, >= sizeof(boot_memory_entry_t) */
    uint32_t reserved;
} boot_memory_map_t;

/* --- Framebuffer ----------------------------------------------------------- */

typedef struct {
    uint64_t phys_base;       /* 0 if no framebuffer is available */
    uint64_t size;            /* bytes */
    uint32_t width;           /* pixels */
    uint32_t height;          /* pixels */
    uint32_t pitch;           /* bytes per scanline */
    uint32_t bpp;             /* bits per pixel, currently always 32 */
    uint8_t  red_shift,   red_size;
    uint8_t  green_shift, green_size;
    uint8_t  blue_shift,  blue_size;
    uint8_t  reserved[2];
} boot_framebuffer_t;

/* --- Firmware tables ------------------------------------------------------- */

typedef struct {
    uint64_t rsdp_phys;  /* 0 if not found */
    uint32_t revision;   /* RSDP revision: 0 = ACPI 1.0, >= 2 = ACPI 2.0+ (XSDT) */
    uint32_t reserved;
} boot_acpi_info_t;

typedef struct {
    uint64_t entry_phys; /* SMBIOS entry point structure, 0 if not found */
    uint32_t major;      /* 3 = SMBIOS 3.x (64-bit entry), 2 = SMBIOS 2.x */
    uint32_t reserved;
} boot_smbios_info_t;

typedef struct {
    uint64_t system_table_phys;
    uint32_t uefi_revision;     /* EFI_SYSTEM_TABLE.Hdr.Revision */
    uint32_t firmware_revision;
    char     firmware_vendor[64]; /* ASCII, NUL terminated */
} boot_uefi_info_t;

/* --- Kernel image and modules ---------------------------------------------- */

typedef struct {
    uint64_t phys_base; /* physical start of the loaded image */
    uint64_t virt_base; /* virtual address phys_base is mapped at */
    uint64_t size;      /* bytes, page aligned */
} boot_kernel_info_t;

typedef struct {
    uint64_t phys_base;
    uint64_t size;
    char     name[64];  /* ASCII, NUL terminated, e.g. "initrd" */
} boot_module_t;

typedef struct {
    uint64_t modules_phys; /* array of boot_module_t, 0 if module_count == 0 */
    uint64_t module_count;
    uint32_t entry_size;
    uint32_t reserved;
} boot_module_list_t;

/* --- Top level ------------------------------------------------------------- */

typedef struct {
    uint64_t magic;   /* BOOT_INFO_MAGIC */
    uint32_t version; /* BOOT_INFO_VERSION of the boot manager */
    uint32_t size;    /* sizeof(boot_info_t) as written by the boot manager */

    uint64_t flags;     /* BOOT_FLAG_* */
    uint64_t hhdm_base; /* virtual base of the direct map of physical memory */
    uint64_t hhdm_size; /* bytes of physical address space covered by the direct map */

    boot_kernel_info_t kernel;
    boot_memory_map_t  memory;
    boot_framebuffer_t framebuffer;
    boot_acpi_info_t   acpi;
    boot_smbios_info_t smbios;
    boot_uefi_info_t   uefi;
    boot_module_list_t modules;

    uint64_t cmdline_phys;   /* ASCII, NUL terminated, never 0 */
    uint64_t cmdline_length; /* without the terminating NUL */

    /* version 1 ends here - new fields are appended below */
} boot_info_t;

#endif
