/*
 * JellyOS kernel entry.
 *
 * Stage 1 (entry stack in .bss): copy boot data, bring up the CPU, memory
 * management and the timer, then switch to a guarded kernel stack.
 * Stage 2: reclaim boot memory, start the scheduler (this code becomes the
 * "kernel-main" thread), bring up devices and drivers, run optional
 * self-tests, then exit so only the idle thread and user processes remain.
 */

#include "core/arch.h"
#include "core/boot.h"
#include "core/cmdline.h"
#include "core/format.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/version.h"
#include "drivers/acpi/acpi.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"
#include "fs/initramfs/initramfs.h"
#include "net/net.h"
#include "time/rtc.h"
#include "drivers/graphics/display.h"
#include "fs/vfs/vfs.h"
#include "memory/layout.h"
#include "memory/memory.h"
#include "memory/vmm.h"
#include "process/process.h"
#include "process/spawn.h"
#include "scheduler/scheduler.h"
#include "time/clock.h"

#include "tests/kernel/ktest.h"

#define TIMER_CHECK_MS   100

static uint64_t main_stack_top;

static const char *const boot_modes[] = { "normal", "previous kernel", "recovery", "manual" };

static void apply_log_level(void)
{
    static const struct { const char *name; klog_level_t level; } levels[] = {
        { "debug", KLOG_DEBUG }, { "info", KLOG_INFO }, { "warn", KLOG_WARN }, { "error", KLOG_ERROR },
    };
    char value[16];

    if (!cmdline_value("loglevel", value, sizeof(value)))
        return;
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(value, levels[i].name) == 0) {
            klog_set_console_level(levels[i].level);
            return;
        }
    }
    klog_warn("unknown loglevel '%s'", value);
}

static void log_boot_info(const boot_info_t *info)
{
    size_t entries, modules, log_length;

    boot_memory_map(&entries);
    boot_modules(&modules);
    boot_log(&log_length);

    klog_info("boot: protocol %u, cmdline \"%s\"", info->version, cmdline_get());
    if (boot_has_field(entry_name))
        klog_info("boot: entry '%s' (%s)", info->entry_name,
                  info->boot_mode < 4 ? boot_modes[info->boot_mode] : "unknown");
    klog_info("boot: kernel at phys %p, %zu memory regions, %zu modules", (void *)info->kernel.phys_base,
              entries, modules);
    klog_debug("boot: %zu bytes of boot manager log preserved", log_length);
    if (boot_truncated_entries())
        klog_warn("boot: %zu memory map or module entries dropped", boot_truncated_entries());
}

/* Sleep on the timer, proving interrupts, EOI and scheduler wakeups work. */
static void check_timer(void)
{
    uint64_t start = clock_monotonic_ns();

    thread_sleep(TIMER_CHECK_MS * 1000000ull);
    klog_info("time: slept %lu ms, %lu ticks since start", (clock_monotonic_ns() - start) / 1000000,
              clock_ticks());
}

static void run_self_tests(void)
{
    char mode[16];

    if (!cmdline_value("selftest", mode, sizeof(mode)))
        return;

    bool passed = ktest_run_all();
    if (strcmp(mode, "exit") == 0)
        arch_test_exit(passed);
}

static void run_crash_test(void)
{
    char kind[32];

    if (!cmdline_value("crashtest", kind, sizeof(kind)))
        return;

    klog_warn("crashtest: triggering '%s'", kind);
    if (strcmp(kind, "panic") == 0)
        panic("crashtest requested a panic");
    if (strcmp(kind, "assert") == 0)
        ASSERT(strcmp(kind, "assert") != 0);
    if (!arch_crash_test(kind))
        klog_warn("crashtest: unknown kind '%s'", kind);
    else
        klog_info("crashtest: '%s' returned, execution continues", kind);
}

/* Root file system, device manager, built-in modules (buses and disks appear here), then boot modules. */
static void start_devices(void)
{
    if (STATUS_IS_ERROR(vfs_init()))
        panic("cannot create the root file system");
    /* Before the drivers: network drivers register their interfaces with the stack. */
    if (STATUS_IS_ERROR(net_init()))
        panic("cannot start the network stack");
    device_manager_init();
    module_init_builtin();
    unsigned loaded = module_load_boot_modules();
    if (loaded)
        klog_info("modules: %u loaded from boot modules", loaded);
    device_tree_dump();
}

/* Unpack the boot module "initrd" (cpio) into the root file system. */
static bool unpack_initramfs(void)
{
    size_t count;
    const boot_module_t *modules = boot_modules(&count);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(modules[i].name, "initrd") != 0)
            continue;
        unsigned entries;
        status_t status = initramfs_unpack(phys_to_virt(modules[i].phys_base), modules[i].size, &entries);
        klog_info("initramfs: %u entries unpacked (%lu KiB)%s%s", entries, modules[i].size / 1024,
                  STATUS_IS_ERROR(status) ? ", error: " : "", STATUS_IS_ERROR(status) ? status_name(status) : "");
        return !STATUS_IS_ERROR(status);
    }
    klog_warn("initramfs: none passed by the boot manager");
    return false;
}

/*
 * Start /init (README section 29) with /dev/console as stdin, stdout and
 * stderr. Its environment carries the kernel command line, so init can
 * honor recovery and safe mode.
 */
static void start_init(void)
{
    static char cmdline_variable[1100];
    file_t *input, *output;
    process_t *init;

    if (!unpack_initramfs())
        return;

    if (STATUS_IS_ERROR(vfs_open("/dev/console", JELLY_OPEN_READ, 0, NULL, &input)))
        panic("cannot open /dev/console for init");
    if (STATUS_IS_ERROR(vfs_open("/dev/console", JELLY_OPEN_WRITE, 0, NULL, &output)))
        panic("cannot open /dev/console for init");

    format(cmdline_variable, sizeof(cmdline_variable), "JELLY_CMDLINE=%s", cmdline_get());
    char *const argv[] = { "/init" };
    char *const envp[] = { "PATH=/bin:/sbin", "HOME=/", cmdline_variable };
    credentials_t root = { UID_ROOT, GID_ROOT };
    spawn_request_t request = {
        .path = "/init",
        .argv = argv,
        .argc = 1,
        .envp = envp,
        .envc = 3,
        .objects = { &input->object, &output->object, &output->object },
        .rights = { JELLY_RIGHT_READ | JELLY_RIGHT_DUPLICATE, JELLY_RIGHT_WRITE | JELLY_RIGHT_DUPLICATE,
                    JELLY_RIGHT_WRITE | JELLY_RIGHT_DUPLICATE },
        .handle_count = 3,
        .credentials = &root,
        .cwd = "/",
    };

    status_t status = process_spawn(&request, &init);
    object_release(&input->object);
    object_release(&output->object);
    if (STATUS_IS_ERROR(status))
        panic("cannot start /init: %s", status_name(status));

    init->critical = true;
    klog_info("init: started as process %lu", init->pid);
    object_release(&init->object);
}

__attribute__((noreturn)) static void kernel_stage2(void)
{
    memory_reclaim_boot();
    scheduler_init(main_stack_top);
    if (STATUS_IS_ERROR(process_init()))
        panic("cannot start the process reaper");

    arch_interrupts_enable();
    check_timer();
    start_devices();
    run_self_tests();
    run_crash_test();

    klog_info("kernel: initialization complete");
    start_init();
    thread_exit();
}

void kernel_main(const boot_info_t *loader_info)
{
    arch_early_console_init();

    if (!boot_accept(loader_info))
        panic("invalid or incompatible boot_info (magic, version or size)");

    const boot_info_t *info = boot_info();
    cmdline_init(boot_cmdline());
    apply_log_level();

    klog_info("JellyOS kernel " KERNEL_VERSION_STRING " starting");
    log_boot_info(info);

    status_t status = arch_init_cpu();
    if (STATUS_IS_ERROR(status))
        panic("CPU initialization failed: %s", status_name(status));

    size_t entry_count;
    const boot_memory_entry_t *entries = boot_memory_map(&entry_count);
    status = memory_init(info, entries, entry_count);
    if (STATUS_IS_ERROR(status))
        panic("memory initialization failed: %s", status_name(status));

    rtc_init();

    /* The screen: the GOP framebuffer becomes display 0 with the kernel console on it. */
    display_init_boot_framebuffer();

    /* Firmware tables: interrupt controllers and PCI configuration ranges. */
    if (STATUS_IS_ERROR(acpi_init(info->acpi.rsdp_phys)))
        klog_warn("acpi: unavailable, continuing without IOAPIC and ECAM");

    status = arch_init_timer();
    if (STATUS_IS_ERROR(status))
        panic("timer initialization failed: %s", status_name(status));
    arch_init_interrupt_routing();

    /* Leave the unguarded entry stack for one with a guard page below it. */
    status = vmm_alloc_kernel_stack(&main_stack_top);
    if (STATUS_IS_ERROR(status))
        panic("cannot allocate the kernel stack: %s", status_name(status));
    arch_switch_stack(main_stack_top, kernel_stage2);
}
