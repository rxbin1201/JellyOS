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
void arch_early_console_put(char c);
/* False on machines without COM1 (nothing is written then). */
bool arch_early_console_present(void);

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

/* With interrupts disabled: enable them and halt without a wakeup window. */
void     arch_idle(void);

/* --- Threads --------------------------------------------------------------- */

/* Saved CPU context (callee-saved registers, FPU/SSE state). Opaque to generic code. */
struct arch_thread;

/* New kernel thread: runs thread_kernel_start(entry, arg) on the given stack. */
status_t arch_thread_create_kernel(struct arch_thread **thread, uint64_t stack_top,
                                   void (*entry)(void *), void *arg);

/* New user thread: enters ring 3 at ip/sp with RDI, RSI, RDX = arg0..arg2. */
status_t arch_thread_create_user(struct arch_thread **thread, uint64_t stack_top, uint64_t ip, uint64_t sp,
                                 uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* Context for the code that is already running (the boot thread). */
status_t arch_thread_create_current(struct arch_thread **thread);

void     arch_thread_destroy(struct arch_thread *thread);

/* Save from, switch to `to`, whose kernel stack top is used for entries from user mode. */
void     arch_thread_switch(struct arch_thread *from, struct arch_thread *to, uint64_t to_stack_top);

/* --- I/O ports (x86; other architectures have none) ------------------------ */

uint8_t  arch_io_read8(uint16_t port);
uint16_t arch_io_read16(uint16_t port);
uint32_t arch_io_read32(uint16_t port);
void     arch_io_write8(uint16_t port, uint8_t value);
void     arch_io_write16(uint16_t port, uint16_t value);
void     arch_io_write32(uint16_t port, uint32_t value);

/* --- Device interrupts ----------------------------------------------------- */

/* Runs in interrupt context; the architecture layer sends the EOI afterwards. */
typedef void (*irq_handler_t)(void *context);

/* Platform interrupt routing (IOAPIC from ACPI). Missing hardware leaves only MSI. */
status_t arch_init_interrupt_routing(void);

/* Reserve an interrupt vector for a device handler. */
status_t arch_irq_allocate(irq_handler_t handler, void *context, uint32_t *irq);
void     arch_irq_free(uint32_t irq);

/* MSI/MSI-X message (address, data) that delivers irq. */
void     arch_irq_msi_message(uint32_t irq, uint64_t *address, uint32_t *data);

/* Route an interrupt controller input (global system interrupt) to irq. */
status_t arch_irq_route_gsi(uint32_t gsi, bool level_triggered, bool active_low, uint32_t irq);
/* Route a legacy ISA IRQ, applying firmware overrides. Reports the GSI used. */
status_t arch_irq_route_isa(uint8_t isa_irq, uint32_t irq, uint32_t *gsi);
void     arch_irq_mask_gsi(uint32_t gsi);

/* Bracket kernel accesses to user memory (SMAP). */
void     arch_user_access_begin(void);
void     arch_user_access_end(void);

unsigned arch_cpu_id(void);

/* Print registers and a stack trace. frame may be NULL (current context). */
void     arch_dump_state(const struct arch_interrupt_frame *frame);

/* Trigger a CPU fault for testing ("divide", "pagefault", "invalid-opcode", "breakpoint",
 * "write-rodata", "stack-overflow"). */
bool     arch_crash_test(const char *kind);

__attribute__((noreturn)) void arch_halt(void);

#endif
