/*
 * JellyOS Boot Manager - UEFI entry point.
 *
 * Boot flow (Phase 1 core path):
 *   load kernel ELF -> validate -> build page tables -> collect firmware info
 *   -> ExitBootServices() -> convert memory map -> jump to the kernel
 *
 * Boot configuration, boot menu, boot state tracking and recovery are added
 * on top of this path in later Phase 1 steps.
 */

#include "boot.h"
#include "cpu.h"
#include "elf_loader.h"
#include "file.h"
#include "firmware.h"
#include "handoff.h"
#include "log.h"
#include "memory_map.h"
#include "paging.h"

#include <jelly/boot_info.h>
#include <jelly/boot_layout.h>

#define BOOT_MANAGER_VERSION L"0.2.0"
#define KERNEL_PATH          L"\\boot\\kernels\\kernel-current.elf"

/* Replaced by the boot configuration parser. */
#define DEFAULT_CMDLINE      "loglevel=info"

/* Extra descriptors for allocations made after sizing the memory map. */
#define MEMORY_MAP_HEADROOM  64

#define GIB                  0x40000000ULL

typedef struct {
    EFI_HANDLE        image;
    boot_info_t      *info;
    page_tables_t     tables;
    loaded_kernel_t   kernel;
    efi_memory_map_t  memory_map;
    uint64_t          stack_phys;
} boot_context_t;

void *boot_alloc_pages(UINTN pages, EFI_MEMORY_TYPE type)
{
    EFI_PHYSICAL_ADDRESS addr = 0;

    if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, type, pages, &addr)))
        return NULL;
    ZeroMem((void *)(UINTN)addr, pages * BOOT_PAGE_SIZE);
    return (void *)(UINTN)addr;
}

static uint64_t direct_map(const boot_context_t *ctx, uint64_t phys)
{
    return ctx->info->hhdm_base + phys;
}

/* boot_info_t and the command line share one BOOT_DATA page. */
static EFI_STATUS create_boot_info(boot_context_t *ctx)
{
    static const char cmdline[] = DEFAULT_CMDLINE;

    _Static_assert(sizeof(boot_info_t) + sizeof(cmdline) <= BOOT_PAGE_SIZE, "boot data exceeds one page");

    uint8_t *page = boot_alloc_pages(1, BOOT_EFI_MEMORY_BOOT_DATA);
    if (!page)
        return EFI_OUT_OF_RESOURCES;

    boot_info_t *info = (boot_info_t *)page;
    info->magic = BOOT_INFO_MAGIC;
    info->version = BOOT_INFO_VERSION;
    info->size = sizeof(boot_info_t);
    info->hhdm_base = BOOT_HHDM_BASE;

    char *cmdline_copy = (char *)(page + sizeof(boot_info_t));
    CopyMem(cmdline_copy, cmdline, sizeof(cmdline));
    info->cmdline_phys = (uint64_t)(UINTN)cmdline_copy;
    info->cmdline_length = sizeof(cmdline) - 1;

    info->modules.entry_size = sizeof(boot_module_t);
    info->memory.entry_size = sizeof(boot_memory_entry_t);

    ctx->info = info;
    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel(boot_context_t *ctx)
{
    void *file;
    UINTN file_size;
    EFI_STATUS status;

    status = file_read_all(ctx->image, KERNEL_PATH, &file, &file_size);
    if (EFI_ERROR(status)) {
        log_error(L"Cannot read %s: %r", KERNEL_PATH, status);
        return status;
    }

    status = elf_validate_kernel(file, file_size, &ctx->kernel);
    if (!EFI_ERROR(status))
        status = elf_load_kernel(file, &ctx->tables, &ctx->kernel);
    FreePool(file);
    if (EFI_ERROR(status))
        return status;

    ctx->info->kernel.phys_base = ctx->kernel.phys_base;
    ctx->info->kernel.virt_base = ctx->kernel.virt_base;
    ctx->info->kernel.size = ctx->kernel.size;

    log_info(L"Kernel %s: %ld KiB at phys 0x%lx, entry 0x%lx (needs boot protocol %d)",
             KERNEL_PATH, ctx->kernel.size / 1024, ctx->kernel.phys_base, ctx->kernel.entry,
             ctx->kernel.required_boot_version);
    return EFI_SUCCESS;
}

static EFI_STATUS map_direct_region(boot_context_t *ctx)
{
    const boot_framebuffer_t *fb = &ctx->info->framebuffer;
    uint64_t highest = memory_map_highest_address();

    if (fb->phys_base && fb->phys_base + fb->size > highest)
        highest = fb->phys_base + fb->size;
    if (highest < BOOT_HHDM_MIN_SIZE)
        highest = BOOT_HHDM_MIN_SIZE;

    uint64_t size = align_up(highest, GIB);
    if (size > BOOT_HHDM_MAX_SIZE) {
        log_error(L"Physical address space of %ld GiB exceeds the direct map", size / GIB);
        return EFI_UNSUPPORTED;
    }

    ctx->info->hhdm_size = size;
    return paging_map_direct(&ctx->tables, ctx->info->hhdm_base, size);
}

/* Identity map the handoff trampoline: it is executing while CR3 changes. */
static EFI_STATUS map_trampoline(boot_context_t *ctx)
{
    uint64_t start = align_down((uint64_t)(UINTN)boot_handoff, BOOT_PAGE_SIZE);
    uint64_t end = align_up((uint64_t)(UINTN)boot_handoff_end, BOOT_PAGE_SIZE);

    for (uint64_t page = start; page < end; page += BOOT_PAGE_SIZE) {
        EFI_STATUS status = paging_map_page(&ctx->tables, page, page, MAP_EXECUTABLE);
        if (EFI_ERROR(status))
            return status;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS allocate_stack(boot_context_t *ctx)
{
    void *stack = boot_alloc_pages(BOOT_STACK_SIZE / BOOT_PAGE_SIZE, BOOT_EFI_MEMORY_BOOT_DATA);

    if (!stack)
        return EFI_OUT_OF_RESOURCES;
    ctx->stack_phys = (uint64_t)(UINTN)stack;
    return EFI_SUCCESS;
}

static void log_summary(const boot_info_t *info)
{
    const boot_framebuffer_t *fb = &info->framebuffer;

    log_info(L"Firmware: %a, UEFI %d.%d", info->uefi.firmware_vendor,
             info->uefi.uefi_revision >> 16, info->uefi.uefi_revision & 0xFFFF);
    if (fb->phys_base)
        log_info(L"Framebuffer: %dx%d, pitch %d at 0x%lx", fb->width, fb->height, fb->pitch, fb->phys_base);
    log_info(L"ACPI RSDP: 0x%lx (revision %d)", info->acpi.rsdp_phys, info->acpi.revision);
    log_info(L"SMBIOS %d entry: 0x%lx", info->smbios.major, info->smbios.entry_phys);
    log_info(L"Secure Boot: %s", (info->flags & BOOT_FLAG_SECURE_BOOT) ? L"enabled" : L"disabled");
    log_info(L"Direct map: %ld GiB at 0x%lx", info->hhdm_size / GIB, info->hhdm_base);
    log_info(L"Command line: %a", (const char *)(UINTN)info->cmdline_phys);
}

/* Everything that may fail and still allows returning to the firmware. */
static EFI_STATUS prepare_boot(boot_context_t *ctx)
{
    EFI_STATUS status;

    if (cpu_five_level_paging()) {
        log_error(L"5-level paging is active; JellyOS requires 4-level paging");
        return EFI_UNSUPPORTED;
    }

    status = create_boot_info(ctx);
    if (EFI_ERROR(status))
        return status;

    status = paging_create(&ctx->tables, cpu_supports_nx());
    if (EFI_ERROR(status))
        return status;

    status = load_kernel(ctx);
    if (EFI_ERROR(status))
        return status;

    firmware_collect(ST, ctx->info);
    firmware_get_framebuffer(&ctx->info->framebuffer);

    status = map_direct_region(ctx);
    if (!EFI_ERROR(status))
        status = map_trampoline(ctx);
    if (!EFI_ERROR(status))
        status = allocate_stack(ctx);
    if (EFI_ERROR(status)) {
        log_error(L"Cannot set up kernel address space: %r", status);
        return status;
    }

    log_summary(ctx->info);

    /* Must be the last allocation before ExitBootServices(). */
    status = memory_map_prepare(&ctx->memory_map, MEMORY_MAP_HEADROOM);
    if (EFI_ERROR(status))
        log_error(L"Cannot allocate memory map buffers: %r", status);
    return status;
}

/* After the first ExitBootServices() attempt only GetMemoryMap() may be called. */
static EFI_STATUS exit_boot_services(boot_context_t *ctx)
{
    EFI_STATUS status = EFI_ABORTED;

    for (int attempt = 0; attempt < 4; attempt++) {
        status = memory_map_fetch(&ctx->memory_map);
        if (EFI_ERROR(status))
            return status;

        status = BS->ExitBootServices(ctx->image, ctx->memory_map.key);
        if (status != EFI_INVALID_PARAMETER)
            return status;
    }
    return status;
}

__attribute__((noreturn)) static void halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void wait_for_key(void)
{
    UINTN index;
    EFI_INPUT_KEY key;

    ST->ConIn->Reset(ST->ConIn, FALSE);
    BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &index);
    ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    boot_context_t ctx = { .image = image };
    EFI_STATUS status;

    InitializeLib(image, st);

    /* Disable the 5-minute UEFI watchdog while the boot manager is active. */
    BS->SetWatchdogTimer(0, 0, 0, NULL);

    ST->ConOut->ClearScreen(ST->ConOut);
    Print(L"JellyOS Boot Manager " BOOT_MANAGER_VERSION L"\r\n\r\n");

    status = prepare_boot(&ctx);
    if (EFI_ERROR(status)) {
        log_error(L"Boot failed: %r. Press any key to return to firmware.", status);
        wait_for_key();
        return status;
    }

    log_info(L"Starting kernel");

    status = exit_boot_services(&ctx);
    if (EFI_ERROR(status))
        halt(); /* firmware services are unusable now, nothing can be reported */

    /* ---- No firmware boot services beyond this point ---- */

    ctx.info->memory.entries_phys = (uint64_t)(UINTN)ctx.memory_map.entries;
    ctx.info->memory.entry_count = memory_map_convert(&ctx.memory_map);

    if (ctx.tables.nx_supported)
        cpu_enable_nx();

    boot_handoff(paging_root(&ctx.tables),
                 direct_map(&ctx, ctx.stack_phys + BOOT_STACK_SIZE),
                 direct_map(&ctx, (uint64_t)(UINTN)ctx.info),
                 ctx.kernel.entry);
}
