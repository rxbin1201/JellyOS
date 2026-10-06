#include "interrupt.h"

#include "gdt.h"

#include "core/log.h"

#define GATE_INTERRUPT 0x8E /* present, ring 0, 64-bit interrupt gate */

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_gate_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_pointer_t;

extern const uint64_t isr_stub_table[256];

static idt_gate_t idt[256] __attribute__((aligned(16)));
static interrupt_handler_t handlers[256];

static void set_gate(unsigned vector, uint64_t handler, uint8_t ist)
{
    idt_gate_t *g = &idt[vector];

    g->offset_low = handler & 0xFFFF;
    g->selector = GDT_KERNEL_CODE;
    g->ist = ist;
    g->type = GATE_INTERRUPT;
    g->offset_mid = (handler >> 16) & 0xFFFF;
    g->offset_high = handler >> 32;
    g->reserved = 0;
}

void idt_init(void)
{
    for (unsigned v = 0; v < 256; v++)
        set_gate(v, isr_stub_table[v], 0);

    /* Faults that may hit a broken stack run on their own stacks. */
    idt[2].ist = IST_NMI;
    idt[8].ist = IST_DOUBLE_FAULT;
    idt[18].ist = IST_MACHINE_CHECK;

    idt_pointer_t pointer = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile("lidt %0" : : "m"(pointer));
    klog_debug("idt: loaded, 256 vectors");
}

void interrupt_set_handler(uint8_t vector, interrupt_handler_t handler)
{
    handlers[vector] = handler;
}

void interrupt_dispatch(struct arch_interrupt_frame *frame)
{
    uint8_t vector = (uint8_t)frame->vector;

    if (handlers[vector]) {
        handlers[vector](frame);
        return;
    }
    if (vector <= VECTOR_EXCEPTION_LAST) {
        exception_handle(frame);
        return;
    }
    /* Spurious IRQ 7/15 from the masked legacy PIC need no acknowledgement. */
    if (vector == VECTOR_PIC_SPURIOUS_1 || vector == VECTOR_PIC_SPURIOUS_2)
        return;
    klog_warn("interrupt: unexpected vector %u", vector);
}
