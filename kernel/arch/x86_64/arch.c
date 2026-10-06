/*
 * x86_64 architecture bring-up (README section 12):
 *
 *   CPU state -> GDT/TSS -> IDT -> exception handlers -> (serial: early console)
 *   -> legacy PIC remapped and masked -> PIT as reference timer
 *   -> Local APIC -> periodic Local APIC timer
 *
 * Single CPU only; SMP follows once the basic kernel is stable.
 */

#include "cpu.h"
#include "early_paging.h"
#include "gdt.h"
#include "interrupt.h"
#include "lapic.h"
#include "pic.h"

#include "core/arch.h"
#include "core/log.h"

#define TIMER_HZ 1000

status_t arch_init(const boot_info_t *info)
{
    status_t status;

    cpu_init();
    early_paging_init(info->hhdm_base, info->hhdm_size);

    gdt_init();
    idt_init();
    exceptions_init();

    pic_disable();

    status = lapic_init();
    if (STATUS_IS_ERROR(status))
        return status;

    return lapic_timer_start(TIMER_HZ);
}

unsigned arch_cpu_id(void)
{
    return lapic_id();
}
