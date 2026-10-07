/*
 * JellyOS Boot Manager - UEFI entry point.
 *
 * Boot flow:
 *   boot state -> configuration -> rollback decision -> menu (optional)
 *   -> load kernel and modules -> build page tables -> ExitBootServices()
 *   -> convert memory map -> jump to the kernel
 *
 * A failed attempt releases its memory and falls back: current kernel ->
 * previous kernel -> recovery entry -> boot menu.
 */

#include "boot.h"
#include "boot_tracker.h"
#include "config.h"
#include "cpu.h"
#include "diagnostics.h"
#include "elf_loader.h"
#include "file.h"
#include "firmware.h"
#include "handoff.h"
#include "log.h"
#include "memory_map.h"
#include "menu.h"
#include "paging.h"
#include "text.h"
#include "verify.h"

#include <jelly/boot_info.h>
#include <jelly/boot_layout.h>

#define BOOT_MANAGER_VERSION   L"0.3.0"

/* Extra descriptors for allocations made after sizing the memory map. */
#define MEMORY_MAP_HEADROOM    192

/* Countdown when a failure forces the menu but the configuration has timeout=0. */
#define FAILURE_MENU_TIMEOUT   10

#define GIB                    0x40000000ULL

typedef struct {
    EFI_HANDLE          image;
    const boot_entry_t *entry;
    const char         *kernel_path;
    uint32_t            mode;
    boot_info_t        *info;
    char               *log_copy;
    page_tables_t       tables;
    loaded_kernel_t     kernel;
    efi_memory_map_t    memory_map;
    uint64_t            stack_phys;
} boot_attempt_t;

static boot_config_t config;
static boot_tracker_t tracker;

static uint64_t direct_map(const boot_attempt_t *a, uint64_t phys)
{
    return a->info->hhdm_base + phys;
}

static uint64_t cmdline_flags(const char *cmdline)
{
    uint64_t flags = 0;

    if (text_has_token(cmdline, "debug=1"))
        flags |= BOOT_FLAG_DEBUG;
    if (text_has_token(cmdline, "safe_mode=1"))
        flags |= BOOT_FLAG_SAFE_MODE;
    if (text_has_token(cmdline, "recovery=1"))
        flags |= BOOT_FLAG_RECOVERY;
    return flags;
}

/* boot_info_t and the command line share one BOOT_DATA page. */
static EFI_STATUS create_boot_info(boot_attempt_t *a)
{
    _Static_assert(sizeof(boot_info_t) + CONFIG_CMDLINE_MAX <= BOOT_PAGE_SIZE, "boot data exceeds one page");

    uint8_t *page = boot_alloc_pages(1, BOOT_EFI_MEMORY_BOOT_DATA);
    if (!page)
        return EFI_OUT_OF_RESOURCES;

    boot_info_t *info = (boot_info_t *)page;
    info->magic = BOOT_INFO_MAGIC;
    info->version = BOOT_INFO_VERSION;
    info->size = sizeof(boot_info_t);
    info->hhdm_base = BOOT_HHDM_BASE;
    info->memory.entry_size = sizeof(boot_memory_entry_t);
    info->modules.entry_size = sizeof(boot_module_t);

    char *cmdline = (char *)(page + sizeof(boot_info_t));
    text_copy(cmdline, CONFIG_CMDLINE_MAX, a->entry->cmdline);
    info->cmdline_phys = (uint64_t)(UINTN)cmdline;
    info->cmdline_length = text_length(cmdline);

    info->flags = cmdline_flags(cmdline);
    if (a->mode == BOOT_MODE_FALLBACK)
        info->flags |= BOOT_FLAG_ROLLBACK;
    info->boot_mode = a->mode;
    text_copy(info->entry_name, sizeof(info->entry_name), a->entry->name);

    diagnostics_collect_cpu(&info->cpu);
    diagnostics_collect_boot_device(a->image, &info->boot_device);

    a->info = info;
    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel(boot_attempt_t *a)
{
    CHAR16 path[CONFIG_PATH_MAX];
    void *file;
    UINTN file_size;
    EFI_STATUS status;

    if (!text_to_path(path, ARRAY_SIZE(path), a->kernel_path))
        return EFI_INVALID_PARAMETER;

    status = file_read_all(a->image, path, &file, &file_size);
    if (EFI_ERROR(status)) {
        log_error(L"Cannot read kernel %s: %r", path, status);
        return status;
    }

    status = verify_image(VERIFY_KERNEL, path, file, file_size);
    if (!EFI_ERROR(status))
        status = elf_validate_kernel(file, file_size, &a->kernel);
    if (!EFI_ERROR(status))
        status = elf_load_kernel(file, &a->tables, &a->kernel);
    FreePool(file);
    if (EFI_ERROR(status))
        return status;

    a->info->kernel.phys_base = a->kernel.phys_base;
    a->info->kernel.virt_base = a->kernel.virt_base;
    a->info->kernel.size = a->kernel.size;

    log_info(L"Kernel %s: %ld KiB at 0x%lx, entry 0x%lx, protocol %d%s", path, a->kernel.size / 1024,
             a->kernel.phys_base, a->kernel.entry, a->kernel.required_boot_version,
             (a->kernel.note_flags & BOOT_NOTE_FLAG_REPORTS_SUCCESS) ? L", reports success" : L"");
    return EFI_SUCCESS;
}

static const char *base_name(const char *path)
{
    const char *name = path;

    for (; *path; path++) {
        if (*path == '/')
            name = path + 1;
    }
    return name;
}

static EFI_STATUS load_module(boot_attempt_t *a, boot_module_t *module, const char *path_text,
                              const char *name, verify_kind_t kind)
{
    CHAR16 path[CONFIG_PATH_MAX];
    void *data;
    UINTN size;
    EFI_STATUS status;

    if (!text_to_path(path, ARRAY_SIZE(path), path_text))
        return EFI_INVALID_PARAMETER;

    status = file_load_pages(a->image, path, BOOT_EFI_MEMORY_KERNEL, &data, &size);
    if (EFI_ERROR(status)) {
        log_error(L"Cannot load %s: %r", path, status);
        return status;
    }
    status = verify_image(kind, path, data, size);
    if (EFI_ERROR(status))
        return status;

    module->phys_base = (uint64_t)(UINTN)data;
    module->size = size;
    text_copy(module->name, sizeof(module->name), name);
    log_info(L"Module '%a': %s, %ld KiB at 0x%lx", module->name, path, (uint64_t)size / 1024, module->phys_base);
    return EFI_SUCCESS;
}

/* Initramfs first, then early modules in configuration order. */
static EFI_STATUS load_modules(boot_attempt_t *a)
{
    const boot_entry_t *e = a->entry;
    UINTN count = (e->initrd[0] ? 1 : 0) + e->module_count;
    EFI_STATUS status;

    if (count == 0)
        return EFI_SUCCESS;

    UINTN bytes = count * sizeof(boot_module_t);
    boot_module_t *modules = boot_alloc_pages(align_up(bytes, BOOT_PAGE_SIZE) / BOOT_PAGE_SIZE,
                                              BOOT_EFI_MEMORY_BOOT_DATA);
    if (!modules)
        return EFI_OUT_OF_RESOURCES;

    UINTN n = 0;
    if (e->initrd[0]) {
        status = load_module(a, &modules[n++], e->initrd, "initrd", VERIFY_INITRD);
        if (EFI_ERROR(status))
            return status;
    }
    for (UINTN i = 0; i < e->module_count; i++) {
        status = load_module(a, &modules[n++], e->modules[i], base_name(e->modules[i]), VERIFY_MODULE);
        if (EFI_ERROR(status))
            return status;
    }

    a->info->modules.modules_phys = (uint64_t)(UINTN)modules;
    a->info->modules.module_count = n;
    return EFI_SUCCESS;
}

static EFI_STATUS map_direct_region(boot_attempt_t *a)
{
    const boot_framebuffer_t *fb = &a->info->framebuffer;
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

    a->info->hhdm_size = size;
    return paging_map_direct(&a->tables, a->info->hhdm_base, size);
}

/* Identity map the handoff trampoline: it is executing while CR3 changes. */
static EFI_STATUS map_trampoline(boot_attempt_t *a)
{
    uint64_t start = align_down((uint64_t)(UINTN)boot_handoff, BOOT_PAGE_SIZE);
    uint64_t end = align_up((uint64_t)(UINTN)boot_handoff_end, BOOT_PAGE_SIZE);

    for (uint64_t page = start; page < end; page += BOOT_PAGE_SIZE) {
        EFI_STATUS status = paging_map_page(&a->tables, page, page, MAP_EXECUTABLE);
        if (EFI_ERROR(status))
            return status;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS allocate_handoff_data(boot_attempt_t *a)
{
    void *stack = boot_alloc_pages(BOOT_STACK_SIZE / BOOT_PAGE_SIZE, BOOT_EFI_MEMORY_BOOT_DATA);
    a->log_copy = boot_alloc_pages(align_up(BOOT_LOG_CAPACITY, BOOT_PAGE_SIZE) / BOOT_PAGE_SIZE,
                                   BOOT_EFI_MEMORY_BOOT_DATA);
    if (!stack || !a->log_copy)
        return EFI_OUT_OF_RESOURCES;

    a->stack_phys = (uint64_t)(UINTN)stack;
    return EFI_SUCCESS;
}

/* Everything that may fail and still allows another attempt. */
static EFI_STATUS prepare_boot(boot_attempt_t *a)
{
    EFI_STATUS status;

    if (cpu_five_level_paging()) {
        log_error(L"5-level paging is active; JellyOS requires 4-level paging");
        return EFI_UNSUPPORTED;
    }

    status = create_boot_info(a);
    if (!EFI_ERROR(status))
        status = paging_create(&a->tables, cpu_supports_nx());
    if (!EFI_ERROR(status))
        status = load_kernel(a);
    if (!EFI_ERROR(status))
        status = load_modules(a);
    if (EFI_ERROR(status))
        return status;

    boot_tracker_begin(&tracker, a->mode, a->kernel.note_flags & BOOT_NOTE_FLAG_REPORTS_SUCCESS);
    boot_tracker_set(&tracker, KERNEL_LOADED);

    firmware_collect(ST, a->info);
    firmware_get_framebuffer(&a->info->framebuffer);

    status = map_direct_region(a);
    if (!EFI_ERROR(status))
        status = map_trampoline(a);
    if (!EFI_ERROR(status))
        status = allocate_handoff_data(a);
    if (EFI_ERROR(status)) {
        log_error(L"Cannot set up kernel address space: %r", status);
        return status;
    }

    if (a->info->flags & BOOT_FLAG_DEBUG)
        diagnostics_print(a->info, a->entry, a->kernel_path, &tracker);

    /* Must be the last allocation before ExitBootServices(). */
    status = memory_map_prepare(&a->memory_map, MEMORY_MAP_HEADROOM);
    if (EFI_ERROR(status))
        log_error(L"Cannot allocate memory map buffers: %r", status);
    return status;
}

/* After the first ExitBootServices() attempt only GetMemoryMap() may be called. */
static EFI_STATUS exit_boot_services(boot_attempt_t *a)
{
    EFI_STATUS status = EFI_ABORTED;

    for (int attempt = 0; attempt < 4; attempt++) {
        status = memory_map_fetch(&a->memory_map);
        if (EFI_ERROR(status))
            return status;

        status = BS->ExitBootServices(a->image, a->memory_map.key);
        if (status != EFI_INVALID_PARAMETER)
            return status;
    }
    return status;
}

/*
 * Progress marks around ExitBootServices(), when nothing can be printed any
 * more: the top edge of the screen is a bar of four segments, drawn straight
 * into the framebuffer. On a machine that hangs during the handoff they show
 * how far it got:
 *   1  about to leave the firmware (the framebuffer works)
 *   2  boot services exited          (red instead: ExitBootServices failed)
 *   3  memory map converted, about to switch page tables and jump
 *   4  drawn by the kernel as its very first action (kernel_main)
 * The kernel clears the screen as soon as its own console works.
 */
static void progress_mark(const boot_framebuffer_t *fb, UINTN index, UINT32 color)
{
    if (!fb->phys_base || fb->bpp != 32 || fb->width < 64 || fb->height < 64)
        return;
    volatile UINT32 *pixels = (volatile UINT32 *)(UINTN)fb->phys_base;
    UINTN stride = fb->pitch / 4, segment = fb->width / 4, left = (index - 1) * segment;
    for (UINTN y = 0; y < 24; y++) {
        for (UINTN x = left + 4; x < left + segment - 4; x++)
            pixels[y * stride + x] = color;
    }
}

__attribute__((noreturn)) static void halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Returns only if the attempt failed before ExitBootServices(). */
static EFI_STATUS boot(EFI_HANDLE image, const menu_action_t *action)
{
    boot_attempt_t a = {
        .image = image,
        .entry = &config.entries[action->entry],
        .mode = action->mode,
    };
    a.kernel_path = a.mode == BOOT_MODE_FALLBACK ? config.fallback_kernel : a.entry->kernel;

    log_set_verbose(text_has_token(a.entry->cmdline, "debug=1"));
    log_info(L"Booting '%a' (%s)", a.entry->name, boot_mode_name(a.mode));

    EFI_STATUS status = prepare_boot(&a);
    if (EFI_ERROR(status))
        return status;

    boot_tracker_set(&tracker, KERNEL_STARTED);
    log_info(L"Starting kernel");

    /* Last log line handed to the kernel; no more logging from here on. */
    UINTN log_length;
    const char *log = log_text(&log_length);
    CopyMem(a.log_copy, log, log_length + 1);
    a.info->log_phys = (uint64_t)(UINTN)a.log_copy;
    a.info->log_length = log_length;

    progress_mark(&a.info->framebuffer, 1, 0x00FFFFFF);
    status = exit_boot_services(&a);
    if (EFI_ERROR(status)) {
        progress_mark(&a.info->framebuffer, 2, 0x00FF0000); /* red (or blue, depending on the pixel format) */
        halt(); /* firmware services are unusable now, nothing can be reported */
    }
    progress_mark(&a.info->framebuffer, 2, 0x00FFFFFF);

    /* ---- No firmware boot services beyond this point ---- */

    a.info->memory.entries_phys = (uint64_t)(UINTN)a.memory_map.entries;
    a.info->memory.entry_count = memory_map_convert(&a.memory_map);

    if (a.tables.nx_supported)
        cpu_enable_nx();
    progress_mark(&a.info->framebuffer, 3, 0x00FFFFFF);

    boot_handoff(paging_root(&a.tables),
                 direct_map(&a, a.stack_phys + BOOT_STACK_SIZE),
                 direct_map(&a, (uint64_t)(UINTN)a.info),
                 a.kernel.entry);
}

/*
 * Next automatic attempt after a failed one: current kernel -> previous
 * kernel -> recovery. Returns false when only the menu is left.
 */
static bool next_automatic_attempt(menu_action_t *action)
{
    if (action->mode == BOOT_MODE_NORMAL && config.fallback_kernel[0]) {
        action->mode = BOOT_MODE_FALLBACK;
        return true;
    }
    if (action->mode != BOOT_MODE_RECOVERY && config.recovery_index != CONFIG_NO_ENTRY) {
        action->entry = config.recovery_index;
        action->mode = BOOT_MODE_RECOVERY;
        return true;
    }
    return false;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    boot_decision_t decision;

    InitializeLib(image, st);

    /* Disable the 5-minute UEFI watchdog while the boot manager is active. */
    BS->SetWatchdogTimer(0, 0, 0, NULL);

    ST->ConOut->ClearScreen(ST->ConOut);
    Print(L"JellyOS Boot Manager " BOOT_MANAGER_VERSION L"\r\n\r\n");

    boot_tracker_init(&tracker);
    config_load(image, &config);

    /* Changing the mode clears the screen, so it happens before anything worth reading is on it. */
    firmware_select_resolution(config.resolution);
    boot_tracker_set(&tracker, BOOT_CONFIGURATION_LOADED);

    boot_tracker_decide(&tracker, &config, &decision);
    if (decision.reason[0])
        log_warn(L"%s", decision.reason);

    menu_action_t action = { .kind = ACTION_BOOT, .entry = decision.entry, .mode = decision.mode };
    bool show_menu = decision.show_menu || config.menu == MENU_ALWAYS;
    UINTN countdown = config.timeout;

    if (decision.show_menu && countdown == 0)
        countdown = FAILURE_MENU_TIMEOUT;
    if (!show_menu && config.menu == MENU_AUTO && config.timeout > 0)
        show_menu = menu_prompt(&config, &decision);
    if (show_menu && !decision.show_menu && config.menu == MENU_AUTO)
        countdown = 0; /* the user interrupted the countdown */

    for (;;) {
        if (show_menu) {
            action = menu_run(image, &config, &tracker, &decision, countdown);
            countdown = 0;
        }

        if (action.kind == ACTION_REBOOT)
            RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        if (action.kind == ACTION_EXIT)
            return EFI_ABORTED;

        EFI_STATUS status = boot(image, &action);

        /* The attempt failed before ExitBootServices(): release it and fall back. */
        boot_alloc_release_all();
        log_error(L"Booting '%a' (%s) failed: %r", config.entries[action.entry].name,
                  boot_mode_name(action.mode), status);
        SPrint(decision.reason, sizeof(decision.reason), L"Booting '%a' (%s) failed: %r",
               config.entries[action.entry].name, boot_mode_name(action.mode), status);

        if (!action.chosen_by_user && next_automatic_attempt(&action)) {
            log_warn(L"Trying '%a' (%s)", config.entries[action.entry].name, boot_mode_name(action.mode));
            continue;
        }

        decision.entry = action.entry;
        decision.mode = action.mode;
        show_menu = true;
    }
}
