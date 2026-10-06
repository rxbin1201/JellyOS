/*
 * Interface between generic kernel code and the architecture layer.
 * Implemented in kernel/arch/<arch>/. Generic code must not use anything
 * architecture-specific beyond this header (README section 11).
 */

#ifndef CORE_ARCH_H
#define CORE_ARCH_H

#include <jelly/boot_info.h>
#include <jelly/status.h>
#include <stdbool.h>
#include <stdint.h>

struct arch_interrupt_frame;

/* Early console: usable before anything else is initialized. */
void arch_early_console_init(void);
void arch_early_console_write(const char *s);

/*
 * Boot CPU bring-up in two steps around memory initialization:
 *   arch_init_cpu():   CPU state, descriptor tables, exceptions, legacy PIC
 *   arch_init_timer(): interrupt controller and periodic timer (needs the VMM for MMIO)
 * Interrupts stay disabled.
 */
status_t arch_init_cpu(void);
status_t arch_init_timer(void);

/* Continue execution on another stack. Never returns. */
__attribute__((noreturn)) void arch_switch_stack(uint64_t stack_top, void (*entry)(void));

/* End a QEMU test run with the result (isa-debug-exit); halts otherwise. */
__attribute__((noreturn)) void arch_test_exit(bool passed);

void     arch_interrupts_enable(void);
void     arch_interrupts_disable(void);
uint64_t arch_interrupts_save(void);  /* disable and return the previous state */
void     arch_interrupts_restore(uint64_t state);

/* Sleep until the next interrupt. */
void     arch_wait_for_interrupt(void);

unsigned arch_cpu_id(void);

/* Print registers and a stack trace. frame may be NULL (current context). */
void     arch_dump_state(const struct arch_interrupt_frame *frame);

/* Trigger a CPU fault for testing ("divide", "pagefault", "invalid-opcode", "breakpoint",
 * "write-rodata", "stack-overflow"). */
bool     arch_crash_test(const char *kind);

__attribute__((noreturn)) void arch_halt(void);

#endif
