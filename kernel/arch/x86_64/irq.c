/*
 * Device interrupt vectors (0x30-0xEF) and MSI messages (x86_64).
 */

#include "interrupt.h"
#include "lapic.h"

#include "core/arch.h"
#include "core/export.h"
#include "core/log.h"

#define MSI_ADDRESS_BASE 0xFEE00000ULL

typedef struct {
    irq_handler_t handler;
    void         *context;
    uint64_t      count;
} irq_slot_t;

static irq_slot_t slots[VECTOR_DEVICE_LAST + 1];

static void device_interrupt(struct arch_interrupt_frame *frame)
{
    irq_slot_t *slot = &slots[frame->vector];

    slot->count++;
    if (slot->handler)
        slot->handler(slot->context);
    lapic_eoi();
}

status_t arch_irq_allocate(irq_handler_t handler, void *context, uint32_t *irq)
{
    if (!handler)
        return STATUS_INVALID_ARGUMENT; /* a slot without handler counts as free */

    uint64_t flags = arch_interrupts_save();

    for (uint32_t v = VECTOR_DEVICE_FIRST; v <= VECTOR_DEVICE_LAST; v++) {
        if (slots[v].handler)
            continue;
        slots[v] = (irq_slot_t){ handler, context, 0 };
        interrupt_set_handler((uint8_t)v, device_interrupt);
        arch_interrupts_restore(flags);
        *irq = v;
        return STATUS_SUCCESS;
    }
    arch_interrupts_restore(flags);
    return STATUS_LIMIT_EXCEEDED;
}

void arch_irq_free(uint32_t irq)
{
    if (irq < VECTOR_DEVICE_FIRST || irq > VECTOR_DEVICE_LAST)
        return;
    uint64_t flags = arch_interrupts_save();
    interrupt_set_handler((uint8_t)irq, NULL);
    slots[irq] = (irq_slot_t){ 0 };
    arch_interrupts_restore(flags);
}

void arch_irq_msi_message(uint32_t irq, uint64_t *address, uint32_t *data)
{
    /* Fixed delivery, edge triggered, physical destination: the boot CPU. */
    *address = MSI_ADDRESS_BASE | ((uint64_t)(lapic_id() & 0xFF) << 12);
    *data = irq & 0xFF;
}

EXPORT_SYMBOL(arch_irq_allocate);
EXPORT_SYMBOL(arch_irq_free);
EXPORT_SYMBOL(arch_irq_msi_message);
