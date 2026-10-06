/*
 * JellyOS kernel entry (Phase 1 handoff stub).
 *
 * Verifies the boot_info_t contract and reports what the boot manager passed.
 * Real initialization (GDT, IDT, exceptions, ...) starts in Phase 2.
 */

#include "core/arch.h"

#include <jelly/boot_info.h>
#include <stddef.h>
#include <stdint.h>

#define KERNEL_REQUIRED_BOOT_VERSION 1

static void print(const char *s)
{
    arch_early_console_write(s);
}

static void print_hex(uint64_t value)
{
    char buf[19] = "0x";
    for (int i = 0; i < 16; i++)
        buf[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 0xF];
    buf[18] = '\0';
    print(buf);
}

static void print_dec(uint64_t value)
{
    char buf[21];
    int i = 20;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(&buf[i]);
}

static const void *phys_to_virt(const boot_info_t *info, uint64_t phys)
{
    return (const void *)(uintptr_t)(info->hhdm_base + phys);
}

static int boot_info_valid(const boot_info_t *info)
{
    return info && info->magic == BOOT_INFO_MAGIC &&
           info->version >= KERNEL_REQUIRED_BOOT_VERSION &&
           info->size >= sizeof(boot_info_t) &&
           info->memory.entry_size >= sizeof(boot_memory_entry_t);
}

static void report_memory(const boot_info_t *info)
{
    const uint8_t *entries = phys_to_virt(info, info->memory.entries_phys);
    uint64_t usable = 0, reclaimable = 0;

    for (uint64_t i = 0; i < info->memory.entry_count; i++) {
        const boot_memory_entry_t *e = (const void *)(entries + i * info->memory.entry_size);
        if (e->type == BOOT_MEMORY_USABLE)
            usable += e->length;
        else if (e->type == BOOT_MEMORY_BOOTLOADER_RECLAIMABLE)
            reclaimable += e->length;
    }

    print("memory map:   ");
    print_dec(info->memory.entry_count);
    print(" entries, ");
    print_dec(usable / (1024 * 1024));
    print(" MiB usable, ");
    print_dec(reclaimable / (1024 * 1024));
    print(" MiB reclaimable\n");
}

/* Fields appended in boot_info_t version 2. */
static void report_v2(const boot_info_t *info)
{
    static const char *const modes[] = { "normal", "previous kernel", "recovery", "manual" };

    if (info->version < 2 || info->size < offsetof(boot_info_t, log_length) + sizeof(info->log_length))
        return;

    print("entry:        ");
    print(info->entry_name);
    print(" (");
    print(info->boot_mode < 4 ? modes[info->boot_mode] : "unknown");
    print(")\n");

    print("cpu:          ");
    print(info->cpu.brand[0] ? info->cpu.brand : info->cpu.vendor);
    print(", ");
    print_dec(info->cpu.logical_cpus);
    print(" logical CPUs\n");

    print("modules:      ");
    print_dec(info->modules.module_count);
    print("\n");

    print("boot log:     ");
    print_dec(info->log_length);
    print(" bytes from the boot manager\n");
}

static void fill_framebuffer(const boot_info_t *info)
{
    const boot_framebuffer_t *fb = &info->framebuffer;

    if (!fb->phys_base || fb->bpp != 32)
        return;

    /* JellyOS purple, encoded with the pixel layout reported by the boot manager. */
    uint32_t color = (0x6Au << fb->red_shift) | (0x4Cu << fb->green_shift) | (0x93u << fb->blue_shift);
    uint8_t *base = (uint8_t *)(uintptr_t)(info->hhdm_base + fb->phys_base);

    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t *row = (uint32_t *)(base + (uint64_t)y * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++)
            row[x] = color;
    }
}

void kernel_main(const boot_info_t *info)
{
    arch_early_console_init();
    print("\nJellyOS kernel started\n");

    if (!boot_info_valid(info)) {
        print("panic: invalid or incompatible boot_info\n");
        arch_halt();
    }

    print("boot_info:    version ");
    print_dec(info->version);
    print(", ");
    print_dec(info->size);
    print(" bytes\n");

    print("cmdline:      ");
    print(phys_to_virt(info, info->cmdline_phys));
    print("\n");

    print("kernel:       phys ");
    print_hex(info->kernel.phys_base);
    print(" -> virt ");
    print_hex(info->kernel.virt_base);
    print("\n");

    print("direct map:   ");
    print_hex(info->hhdm_base);
    print(", ");
    print_dec(info->hhdm_size >> 30);
    print(" GiB\n");

    report_memory(info);

    print("framebuffer:  ");
    print_dec(info->framebuffer.width);
    print("x");
    print_dec(info->framebuffer.height);
    print("\n");

    print("ACPI RSDP:    ");
    print_hex(info->acpi.rsdp_phys);
    print("\n");

    report_v2(info);
    fill_framebuffer(info);

    print("Phase 1 handoff complete, halting.\n");
    arch_halt();
}
