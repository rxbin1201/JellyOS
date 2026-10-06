/*
 * JellyOS kernel entry (Phase 2: minimal kernel).
 *
 * Brings up the boot CPU (descriptor tables, exceptions, interrupt
 * controller, timer) and idles with interrupts enabled. Memory management
 * follows in Phase 3.
 */

#include "core/arch.h"
#include "core/boot.h"
#include "core/cmdline.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/string.h"
#include "time/clock.h"

#define KERNEL_VERSION   "0.2.0"
#define TIMER_CHECK_MS   100

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
    uint64_t usable = 0;
    const uint8_t *entries = boot_phys_to_virt(info->memory.entries_phys);

    for (uint64_t i = 0; i < info->memory.entry_count; i++) {
        const boot_memory_entry_t *e = (const void *)(entries + i * info->memory.entry_size);
        if (e->type == BOOT_MEMORY_USABLE)
            usable += e->length;
    }

    klog_info("boot: protocol %u, cmdline \"%s\"", info->version, cmdline_get());
    if (boot_has_field(entry_name))
        klog_info("boot: entry '%s' (%s)", info->entry_name,
                  info->boot_mode < 4 ? boot_modes[info->boot_mode] : "unknown");
    klog_info("boot: kernel at phys %p, %lu MiB usable memory in %lu regions",
              (void *)info->kernel.phys_base, usable >> 20, info->memory.entry_count);
    if (boot_has_field(log_length))
        klog_debug("boot: %lu bytes of boot manager log available", info->log_length);
}

/* Wait until the timer has advanced, proving interrupts and EOI work. */
static void check_timer(void)
{
    uint64_t start = clock_monotonic_ns();

    while (clock_monotonic_ns() - start < TIMER_CHECK_MS * 1000000ull)
        arch_wait_for_interrupt();
    klog_info("time: timer running, %lu ticks after %u ms", clock_ticks(), TIMER_CHECK_MS);
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

void kernel_main(const boot_info_t *loader_info)
{
    arch_early_console_init();

    if (!boot_accept(loader_info))
        panic("invalid or incompatible boot_info (magic, version or size)");

    const boot_info_t *info = boot_info();
    cmdline_init(boot_phys_to_virt(info->cmdline_phys));
    apply_log_level();

    klog_info("JellyOS kernel " KERNEL_VERSION " starting");
    log_boot_info(info);

    status_t status = arch_init(info);
    if (STATUS_IS_ERROR(status))
        panic("architecture initialization failed: %s", status_name(status));

    arch_interrupts_enable();
    check_timer();
    run_crash_test();

    klog_info("kernel: Phase 2 initialization complete, idling");
    for (;;)
        arch_wait_for_interrupt();
}
