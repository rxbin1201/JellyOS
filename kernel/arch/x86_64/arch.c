/*
 * x86_64 architecture bring-up (README section 12):
 *
 *   arch_init_cpu():   CPU state -> GDT/TSS -> IDT -> exception handlers
 *                      -> legacy PIC remapped and masked
 *   (memory initialization: PMM, VMM, heap)
 *   arch_init_timer(): PIT as reference timer -> Local APIC -> periodic APIC timer
 *
 * The early console (serial) is up before all of this. Single CPU only;
 * SMP follows once the basic kernel is stable.
 */

#include "cpu.h"
#include "gdt.h"
#include "interrupt.h"
#include "io.h"
#include "lapic.h"
#include "pic.h"

#include "core/arch.h"

#define TIMER_HZ          1000
#define DEBUG_EXIT_PORT   0xF4 /* QEMU isa-debug-exit */

void syscall_init(void);

status_t arch_init_cpu(void)
{
    cpu_init();
    gdt_init();
    idt_init();
    exceptions_init();
    syscall_init();
    pic_disable();
    return STATUS_SUCCESS;
}

status_t arch_init_timer(void)
{
    status_t status = lapic_init();
    if (STATUS_IS_ERROR(status))
        return status;
    return lapic_timer_start(TIMER_HZ);
}

unsigned arch_cpu_id(void)
{
    return lapic_id();
}

void arch_switch_stack(uint64_t stack_top, void (*entry)(void))
{
    __asm__ volatile(
        "mov %0, %%rsp\n"
        "xor %%ebp, %%ebp\n"   /* new stack, new stack trace */
        "call *%1\n"
        "1: cli; hlt; jmp 1b\n"
        :
        : "r"(stack_top), "r"(entry)
        : "memory");
    __builtin_unreachable();
}

void arch_test_exit(bool passed)
{
    /* QEMU exits with status (value << 1) | 1: 1 = passed, 3 = failed. */
    outb(DEBUG_EXIT_PORT, passed ? 0 : 1);
    arch_halt();
}
