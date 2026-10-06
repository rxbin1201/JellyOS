/*
 * I/O APIC: routes global system interrupts (GSIs) to vectors on the boot CPU.
 * Controllers and ISA overrides come from the ACPI MADT.
 */

#include "lapic.h"

#include "core/arch.h"
#include "core/log.h"
#include "drivers/acpi/acpi.h"
#include "memory/vmm.h"

#define IOAPIC_REGSEL       0
#define IOAPIC_WINDOW       4 /* in 32-bit words: offset 0x10 */
#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VERSION  0x01
#define IOAPIC_REG_REDIR    0x10

#define REDIR_MASKED        (1u << 16)
#define REDIR_LEVEL         (1u << 15)
#define REDIR_ACTIVE_LOW    (1u << 13)

typedef struct {
    volatile uint32_t *regs;
    uint32_t           gsi_base;
    uint32_t           inputs;
    uint8_t            id;
} ioapic_t;

static ioapic_t ioapics[ACPI_MAX_IOAPICS];
static uint32_t ioapic_count;
static acpi_madt_info_t madt;

static uint32_t read_reg(ioapic_t *io, uint32_t reg)
{
    io->regs[IOAPIC_REGSEL] = reg;
    return io->regs[IOAPIC_WINDOW];
}

static void write_reg(ioapic_t *io, uint32_t reg, uint32_t value)
{
    io->regs[IOAPIC_REGSEL] = reg;
    io->regs[IOAPIC_WINDOW] = value;
}

static ioapic_t *ioapic_for(uint32_t gsi)
{
    for (uint32_t i = 0; i < ioapic_count; i++) {
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].inputs)
            return &ioapics[i];
    }
    return NULL;
}

status_t arch_init_interrupt_routing(void)
{
    status_t status = acpi_parse_madt(&madt);
    if (STATUS_IS_ERROR(status)) {
        klog_warn("ioapic: no MADT, only MSI interrupts are available");
        return status;
    }

    for (uint32_t i = 0; i < madt.ioapic_count; i++) {
        ioapic_t *io = &ioapics[ioapic_count];
        io->regs = vmm_map_mmio(madt.ioapics[i].address, 0x20, VM_UNCACHED);
        if (!io->regs)
            return STATUS_OUT_OF_MEMORY;
        io->id = madt.ioapics[i].id;
        io->gsi_base = madt.ioapics[i].gsi_base;
        io->inputs = ((read_reg(io, IOAPIC_REG_VERSION) >> 16) & 0xFF) + 1;

        for (uint32_t pin = 0; pin < io->inputs; pin++)
            write_reg(io, IOAPIC_REG_REDIR + pin * 2, REDIR_MASKED);
        ioapic_count++;

        klog_info("ioapic: id %u at 0x%lx, GSIs %u-%u", io->id, madt.ioapics[i].address, io->gsi_base,
                  io->gsi_base + io->inputs - 1);
    }
    klog_info("ioapic: %u CPUs in MADT, %u interrupt source overrides%s", madt.cpu_count, madt.override_count,
              madt.legacy_pics ? ", legacy PICs present (masked)" : "");
    return STATUS_SUCCESS;
}

status_t arch_irq_route_gsi(uint32_t gsi, bool level_triggered, bool active_low, uint32_t irq)
{
    ioapic_t *io = ioapic_for(gsi);
    if (!io)
        return STATUS_NOT_FOUND;

    uint32_t pin = gsi - io->gsi_base;
    uint32_t low = (irq & 0xFF) | (level_triggered ? REDIR_LEVEL : 0) | (active_low ? REDIR_ACTIVE_LOW : 0);
    uint64_t flags = arch_interrupts_save();
    write_reg(io, IOAPIC_REG_REDIR + pin * 2 + 1, (lapic_id() & 0xFF) << 24); /* physical destination */
    write_reg(io, IOAPIC_REG_REDIR + pin * 2, low);
    arch_interrupts_restore(flags);
    return STATUS_SUCCESS;
}

status_t arch_irq_route_isa(uint8_t isa_irq, uint32_t irq, uint32_t *gsi)
{
    /* ISA default: identity mapping, edge triggered, active high. */
    bool level = false, active_low = false;
    *gsi = isa_irq;

    for (uint32_t i = 0; i < madt.override_count; i++) {
        const acpi_override_t *o = &madt.overrides[i];
        if (o->source_irq != isa_irq)
            continue;
        *gsi = o->gsi;
        level = (o->flags & ACPI_TRIGGER_MASK) == ACPI_TRIGGER_LEVEL;
        active_low = (o->flags & ACPI_POLARITY_MASK) == ACPI_POLARITY_LOW;
    }
    return arch_irq_route_gsi(*gsi, level, active_low, irq);
}

void arch_irq_mask_gsi(uint32_t gsi)
{
    ioapic_t *io = ioapic_for(gsi);
    if (io)
        write_reg(io, IOAPIC_REG_REDIR + (gsi - io->gsi_base) * 2, REDIR_MASKED);
}
