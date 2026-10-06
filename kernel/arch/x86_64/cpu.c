/*
 * CPU feature detection and basic CPU state (README section 11).
 */

#include "cpu.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"

cpu_features_t cpu_features;

static void detect_features(cpu_features_t *f)
{
    uint32_t a, b, c, d, max_ext;

    cpu_cpuid(0, 0, &a, &b, &c, &d);
    memcpy(f->vendor, &b, 4);
    memcpy(f->vendor + 4, &d, 4);
    memcpy(f->vendor + 8, &c, 4);
    f->vendor[12] = '\0';

    cpu_cpuid(1, 0, &a, &b, &c, &d);
    f->tsc = d & (1u << 4);
    f->apic = d & (1u << 9);
    f->x2apic = c & (1u << 21);

    cpu_cpuid(0x80000000, 0, &max_ext, &b, &c, &d);
    if (max_ext >= 0x80000001) {
        cpu_cpuid(0x80000001, 0, &a, &b, &c, &d);
        f->nx = d & (1u << 20);
        f->page_1g = d & (1u << 26);
    }
    if (max_ext >= 0x80000004) {
        uint32_t *brand = (uint32_t *)f->brand;
        for (uint32_t i = 0; i < 3; i++)
            cpu_cpuid(0x80000002 + i, 0, &brand[i * 4], &brand[i * 4 + 1], &brand[i * 4 + 2], &brand[i * 4 + 3]);
        f->brand[48] = '\0';
        for (int i = 47; i >= 0 && (f->brand[i] == ' ' || f->brand[i] == '\0'); i--)
            f->brand[i] = '\0';
    }
    if (max_ext >= 0x80000007) {
        cpu_cpuid(0x80000007, 0, &a, &b, &c, &d);
        f->invariant_tsc = d & (1u << 8);
    }
}

void cpu_init(void)
{
    detect_features(&cpu_features);

    /* Honor read-only pages in ring 0 as well (the boot protocol leaves WP as found). */
    cpu_write_cr0(cpu_read_cr0() | CR0_WP);

    const char *brand = cpu_features.brand;
    while (*brand == ' ')
        brand++;
    klog_info("cpu: %s (%s)", *brand ? brand : "unknown", cpu_features.vendor);
    klog_info("cpu: features apic=%d x2apic=%d nx=%d tsc=%d invariant_tsc=%d 1g_pages=%d",
              cpu_features.apic, cpu_features.x2apic, cpu_features.nx, cpu_features.tsc,
              cpu_features.invariant_tsc, cpu_features.page_1g);
    if (cpu_features.nx && !(cpu_read_msr(MSR_EFER) & EFER_NXE))
        klog_warn("cpu: NX supported but EFER.NXE is not set");
}

void arch_interrupts_enable(void)
{
    __asm__ volatile("sti" : : : "memory");
}

void arch_interrupts_disable(void)
{
    __asm__ volatile("cli" : : : "memory");
}

uint64_t arch_interrupts_save(void)
{
    uint64_t flags = cpu_read_rflags();
    arch_interrupts_disable();
    return flags & RFLAGS_IF;
}

void arch_interrupts_restore(uint64_t state)
{
    if (state & RFLAGS_IF)
        arch_interrupts_enable();
}

void arch_wait_for_interrupt(void)
{
    __asm__ volatile("hlt" : : : "memory");
}

void arch_halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
