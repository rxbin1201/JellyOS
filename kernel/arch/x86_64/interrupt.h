/*
 * x86_64 interrupt frame, vector layout and handler registration.
 * See docs/architecture/x86_64.md.
 */

#ifndef ARCH_X86_64_INTERRUPT_H
#define ARCH_X86_64_INTERRUPT_H

#include <stdint.h>

/* Vector layout */
#define VECTOR_EXCEPTION_LAST   31
#define VECTOR_PIC_BASE         0x20 /* legacy 8259, remapped and masked */
#define VECTOR_PIC_SPURIOUS_1   0x27
#define VECTOR_PIC_SPURIOUS_2   0x2F
#define VECTOR_DEVICE_FIRST     0x30 /* device interrupts (Phase 5) */
#define VECTOR_DEVICE_LAST      0xEF
#define VECTOR_LAPIC_TIMER      0xF0
#define VECTOR_LAPIC_ERROR      0xFE
#define VECTOR_LAPIC_SPURIOUS   0xFF

/* Saved state; layout must match isr.S. */
struct arch_interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code; /* 0 for vectors without an error code */
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*interrupt_handler_t)(struct arch_interrupt_frame *frame);

void idt_init(void);

/* Install a handler for a vector. Exceptions without a handler panic. */
void interrupt_set_handler(uint8_t vector, interrupt_handler_t handler);

/* Called from isr.S for every interrupt and exception. */
void interrupt_dispatch(struct arch_interrupt_frame *frame);

/* Installs the default exception handlers (exceptions.c). */
void exceptions_init(void);
void exception_handle(struct arch_interrupt_frame *frame);

#endif
