#include "drivers/bus/pci/pci.h"

#include "drivers/acpi/acpi.h"
#include "drivers/core/module.h"

#include "core/export.h"
#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "scheduler/thread.h"

#define MAX_ECAM_RANGES   4
#define CONFIG_ADDRESS    0xCF8
#define CONFIG_DATA       0xCFC
#define ECAM_BUS_SIZE     (1u << 20)

#define MSI_CONTROL_ENABLE    (1u << 0)
#define MSI_CONTROL_64BIT     (1u << 7)
#define MSI_CONTROL_MME_MASK  (7u << 4)
#define MSIX_CONTROL_ENABLE   (1u << 15)
#define MSIX_CONTROL_MASK_ALL (1u << 14)
#define MSIX_ENTRY_MASKED     1u

#define PM_CONTROL            4
#define PM_STATE_MASK         3u
#define PM_D3_RECOVERY_NS     10000000ULL /* 10 ms after D3hot -> D0 */

typedef struct {
    uint16_t segment;
    uint8_t  bus, slot, function;
} pci_location_t;

static acpi_mcfg_entry_t ecam[MAX_ECAM_RANGES];
static size_t ecam_count;
static volatile uint8_t *ecam_buses[MAX_ECAM_RANGES][256];
static bus_t pci_bus;
static device_t segment_root;
static unsigned device_count;

/* --- Configuration access ---------------------------------------------------------- */

static volatile uint8_t *ecam_address(const pci_location_t *loc, uint16_t offset)
{
    for (size_t i = 0; i < ecam_count; i++) {
        const acpi_mcfg_entry_t *e = &ecam[i];
        if (e->segment != loc->segment || loc->bus < e->start_bus || loc->bus > e->end_bus)
            continue;
        if (!ecam_buses[i][loc->bus]) {
            ecam_buses[i][loc->bus] = vmm_map_mmio(e->base + ((uint64_t)loc->bus << 20), ECAM_BUS_SIZE, VM_UNCACHED);
            if (!ecam_buses[i][loc->bus])
                return NULL;
        }
        return ecam_buses[i][loc->bus] + ((uint32_t)loc->slot << 15) + ((uint32_t)loc->function << 12) + offset;
    }
    return NULL;
}

static uint32_t port_address(const pci_location_t *loc, uint16_t offset)
{
    return 0x80000000u | ((uint32_t)loc->bus << 16) | ((uint32_t)loc->slot << 11) |
           ((uint32_t)loc->function << 8) | (offset & 0xFC);
}

static uint32_t config_read(const pci_location_t *loc, uint16_t offset, unsigned width)
{
    if (ecam_count) {
        volatile uint8_t *p = ecam_address(loc, offset);
        if (!p)
            return 0xFFFFFFFF;
        return width == 1 ? *p : width == 2 ? *(volatile uint16_t *)p : *(volatile uint32_t *)p;
    }
    if (loc->segment || offset >= 256)
        return 0xFFFFFFFF;

    uint64_t flags = arch_interrupts_save();
    arch_io_write32(CONFIG_ADDRESS, port_address(loc, offset));
    uint32_t value = width == 1 ? arch_io_read8(CONFIG_DATA + (offset & 3))
                   : width == 2 ? arch_io_read16(CONFIG_DATA + (offset & 2))
                                : arch_io_read32(CONFIG_DATA);
    arch_interrupts_restore(flags);
    return value;
}

static void config_write(const pci_location_t *loc, uint16_t offset, unsigned width, uint32_t value)
{
    if (ecam_count) {
        volatile uint8_t *p = ecam_address(loc, offset);
        if (!p)
            return;
        if (width == 1)
            *p = (uint8_t)value;
        else if (width == 2)
            *(volatile uint16_t *)p = (uint16_t)value;
        else
            *(volatile uint32_t *)p = value;
        return;
    }
    if (loc->segment || offset >= 256)
        return;

    uint64_t flags = arch_interrupts_save();
    arch_io_write32(CONFIG_ADDRESS, port_address(loc, offset));
    if (width == 1)
        arch_io_write8(CONFIG_DATA + (offset & 3), (uint8_t)value);
    else if (width == 2)
        arch_io_write16(CONFIG_DATA + (offset & 2), (uint16_t)value);
    else
        arch_io_write32(CONFIG_DATA, value);
    arch_interrupts_restore(flags);
}

static pci_location_t location_of(const pci_device_t *pci)
{
    return (pci_location_t){ pci->segment, pci->bus, pci->slot, pci->function };
}

uint8_t pci_read8(pci_device_t *pci, uint16_t offset)
{
    pci_location_t loc = location_of(pci);
    return (uint8_t)config_read(&loc, offset, 1);
}

uint16_t pci_read16(pci_device_t *pci, uint16_t offset)
{
    pci_location_t loc = location_of(pci);
    return (uint16_t)config_read(&loc, offset, 2);
}

uint32_t pci_read32(pci_device_t *pci, uint16_t offset)
{
    pci_location_t loc = location_of(pci);
    return config_read(&loc, offset, 4);
}

void pci_write8(pci_device_t *pci, uint16_t offset, uint8_t value)
{
    pci_location_t loc = location_of(pci);
    config_write(&loc, offset, 1, value);
}

void pci_write16(pci_device_t *pci, uint16_t offset, uint16_t value)
{
    pci_location_t loc = location_of(pci);
    config_write(&loc, offset, 2, value);
}

void pci_write32(pci_device_t *pci, uint16_t offset, uint32_t value)
{
    pci_location_t loc = location_of(pci);
    config_write(&loc, offset, 4, value);
}

/* --- Device setup --------------------------------------------------------------------- */

status_t pci_enable_device(pci_device_t *pci, bool bus_master)
{
    uint16_t command = pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_MEMORY | PCI_COMMAND_IO;
    if (bus_master)
        command |= PCI_COMMAND_BUS_MASTER;
    pci_write16(pci, PCI_COMMAND, command);
    return STATUS_SUCCESS;
}

volatile void *pci_map_bar(pci_device_t *pci, unsigned index)
{
    if (index >= PCI_MAX_BARS)
        return NULL;
    pci_bar_t *bar = &pci->bars[index];
    if (bar->io || bar->size == 0)
        return NULL;
    if (!bar->mapped)
        bar->mapped = vmm_map_mmio(bar->phys, bar->size, VM_UNCACHED);
    return bar->mapped;
}

/* --- MSI and MSI-X -------------------------------------------------------------------- */

static void disable_intx(pci_device_t *pci)
{
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_INTX_DISABLE);
}

status_t pci_enable_msi(pci_device_t *pci, irq_handler_t handler, void *context, uint32_t *irq)
{
    uint64_t address;
    uint32_t data, vector;

    if (!pci->cap_msi)
        return STATUS_NOT_SUPPORTED;
    if (pci->msi_enabled)
        return STATUS_BUSY;

    status_t status = arch_irq_allocate(handler, context, &vector);
    if (STATUS_IS_ERROR(status))
        return status;
    arch_irq_msi_message(vector, &address, &data);

    uint8_t cap = pci->cap_msi;
    uint16_t control = pci_read16(pci, cap + 2);
    pci_write32(pci, cap + 4, (uint32_t)address);
    if (control & MSI_CONTROL_64BIT) {
        pci_write32(pci, cap + 8, (uint32_t)(address >> 32));
        pci_write16(pci, cap + 12, (uint16_t)data);
    } else {
        pci_write16(pci, cap + 8, (uint16_t)data);
    }
    control = (control & ~MSI_CONTROL_MME_MASK) | MSI_CONTROL_ENABLE; /* a single vector */
    pci_write16(pci, cap + 2, control);
    disable_intx(pci);

    pci->msi_irq = vector;
    pci->msi_enabled = true;
    *irq = vector;
    return STATUS_SUCCESS;
}

void pci_disable_msi(pci_device_t *pci)
{
    if (!pci->msi_enabled)
        return;
    pci_write16(pci, pci->cap_msi + 2, pci_read16(pci, pci->cap_msi + 2) & ~MSI_CONTROL_ENABLE);
    arch_irq_free(pci->msi_irq);
    pci->msi_enabled = false;
    pci->msi_irq = 0;
}

static status_t setup_msix(pci_device_t *pci)
{
    uint8_t cap = pci->cap_msix;
    uint16_t control = pci_read16(pci, cap + 2);
    uint32_t table = pci_read32(pci, cap + 4);
    uint16_t size = (control & 0x7FF) + 1;

    volatile uint8_t *bar = pci_map_bar(pci, table & 7);
    if (!bar || (table & ~7u) + size * 16u > pci->bars[table & 7].size)
        return STATUS_NOT_SUPPORTED;

    pci->msix_irqs = kcalloc(size, sizeof(uint32_t));
    if (!pci->msix_irqs)
        return STATUS_OUT_OF_MEMORY;
    pci->msix_table = (volatile uint32_t *)(bar + (table & ~7u));
    pci->msix_size = size;

    /* Enable with all vectors masked, then mask every entry individually. */
    pci_write16(pci, cap + 2, control | MSIX_CONTROL_ENABLE | MSIX_CONTROL_MASK_ALL);
    for (uint16_t i = 0; i < size; i++)
        pci->msix_table[i * 4 + 3] = MSIX_ENTRY_MASKED;
    disable_intx(pci);
    return STATUS_SUCCESS;
}

status_t pci_enable_msix(pci_device_t *pci, uint16_t entry, irq_handler_t handler, void *context, uint32_t *irq)
{
    uint64_t address;
    uint32_t data, vector;

    if (!pci->cap_msix)
        return STATUS_NOT_SUPPORTED;
    if (!pci->msix_table) {
        status_t status = setup_msix(pci);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    if (entry >= pci->msix_size)
        return STATUS_INVALID_ARGUMENT;
    if (pci->msix_irqs[entry])
        return STATUS_BUSY;

    status_t status = arch_irq_allocate(handler, context, &vector);
    if (STATUS_IS_ERROR(status))
        return status;
    arch_irq_msi_message(vector, &address, &data);

    volatile uint32_t *e = &pci->msix_table[entry * 4];
    e[0] = (uint32_t)address;
    e[1] = (uint32_t)(address >> 32);
    e[2] = data;
    e[3] = 0; /* unmask */
    pci->msix_irqs[entry] = vector;

    uint8_t cap = pci->cap_msix;
    pci_write16(pci, cap + 2, pci_read16(pci, cap + 2) & ~MSIX_CONTROL_MASK_ALL);
    *irq = vector;
    return STATUS_SUCCESS;
}

void pci_disable_msix(pci_device_t *pci)
{
    if (!pci->msix_table)
        return;
    pci_write16(pci, pci->cap_msix + 2, pci_read16(pci, pci->cap_msix + 2) & ~MSIX_CONTROL_ENABLE);
    for (uint16_t i = 0; i < pci->msix_size; i++) {
        pci->msix_table[i * 4 + 3] = MSIX_ENTRY_MASKED;
        if (pci->msix_irqs[i])
            arch_irq_free(pci->msix_irqs[i]);
    }
    kfree(pci->msix_irqs);
    pci->msix_irqs = NULL;
    pci->msix_table = NULL;
    pci->msix_size = 0;
}

/* --- Power management ------------------------------------------------------------------- */

status_t pci_set_power(pci_device_t *pci, device_power_t power)
{
    if (!pci->cap_pm)
        return STATUS_NOT_SUPPORTED;

    uint16_t control = pci_read16(pci, pci->cap_pm + PM_CONTROL) & ~PM_STATE_MASK;
    if (power == DEVICE_POWER_D3) {
        for (unsigned i = 0; i < PCI_MAX_BARS; i++)
            pci->saved_bars[i] = pci_read32(pci, PCI_BAR0 + i * 4);
        pci->saved_command = pci_read16(pci, PCI_COMMAND);
        pci_write16(pci, pci->cap_pm + PM_CONTROL, control | 3);
        return STATUS_SUCCESS;
    }

    pci_write16(pci, pci->cap_pm + PM_CONTROL, control);
    thread_sleep(PM_D3_RECOVERY_NS);
    /* D3hot -> D0 may reset the function: restore decoding state. */
    for (unsigned i = 0; i < PCI_MAX_BARS; i++)
        pci_write32(pci, PCI_BAR0 + i * 4, pci->saved_bars[i]);
    pci_write16(pci, PCI_COMMAND, pci->saved_command);
    return STATUS_SUCCESS;
}

/* --- Bus operations --------------------------------------------------------------------- */

static status_t bus_set_power(device_t *device, device_power_t power)
{
    return pci_set_power(pci_from_device(device), power);
}

static void bus_detached(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);

    pci_disable_msi(pci);
    pci_disable_msix(pci);
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) & ~PCI_COMMAND_BUS_MASTER);
}

/* --- Enumeration ------------------------------------------------------------------------ */

static void add_resource(pci_device_t *pci, resource_type_t type, uint64_t start, uint64_t size, uint32_t flags)
{
    device_t *d = &pci->device;
    if (d->resource_count < DEVICE_MAX_RESOURCES)
        d->resources[d->resource_count++] = (resource_t){ type, flags, start, size };
}

/* Size each BAR by writing all ones with decoding disabled. */
static void read_bars(pci_device_t *pci, unsigned count)
{
    uint16_t command = pci_read16(pci, PCI_COMMAND);
    pci_write16(pci, PCI_COMMAND, command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));

    for (unsigned i = 0; i < count; i++) {
        uint16_t offset = PCI_BAR0 + i * 4;
        uint32_t original = pci_read32(pci, offset);
        pci_bar_t *bar = &pci->bars[i];

        pci_write32(pci, offset, 0xFFFFFFFF);
        uint32_t mask = pci_read32(pci, offset);
        pci_write32(pci, offset, original);
        if (mask == 0 || mask == 0xFFFFFFFF)
            continue;

        if (original & 1) {
            bar->io = true;
            bar->phys = original & ~3u;
            bar->size = (~(mask & ~3u) + 1) & 0xFFFF;
            add_resource(pci, RESOURCE_IO, bar->phys, bar->size, 0);
            continue;
        }

        bar->prefetchable = original & 8;
        bar->is_64bit = ((original >> 1) & 3) == 2;
        uint64_t base = original & ~0xFu;
        uint64_t size_mask = mask & ~0xFu;
        if (bar->is_64bit && i + 1 < count) {
            uint32_t original_high = pci_read32(pci, offset + 4);
            pci_write32(pci, offset + 4, 0xFFFFFFFF);
            uint32_t mask_high = pci_read32(pci, offset + 4);
            pci_write32(pci, offset + 4, original_high);
            base |= (uint64_t)original_high << 32;
            size_mask |= (uint64_t)mask_high << 32;
            bar->size = ~size_mask + 1;
            i++; /* the next BAR holds the upper half */
        } else {
            bar->size = (uint32_t)(~(uint32_t)size_mask + 1);
        }
        bar->phys = base;
        add_resource(pci, RESOURCE_MMIO, base, bar->size,
                     (bar->prefetchable ? RESOURCE_PREFETCHABLE : 0) | (bar->is_64bit ? RESOURCE_64BIT : 0));
    }
    pci_write16(pci, PCI_COMMAND, command);
}

static void read_capabilities(pci_device_t *pci)
{
    if (!(pci_read16(pci, PCI_STATUS) & (1u << 4)))
        return;

    uint8_t offset = pci_read8(pci, PCI_CAPABILITIES) & ~3u;
    for (int guard = 0; offset && guard < 48; guard++) {
        uint8_t id = pci_read8(pci, offset);
        if (id == PCI_CAP_PM && !pci->cap_pm)
            pci->cap_pm = offset;
        else if (id == PCI_CAP_MSI && !pci->cap_msi)
            pci->cap_msi = offset;
        else if (id == PCI_CAP_MSIX && !pci->cap_msix)
            pci->cap_msix = offset;
        else if (id == PCI_CAP_PCIE && !pci->cap_pcie)
            pci->cap_pcie = offset;
        offset = pci_read8(pci, offset + 1) & ~3u;
    }
}

static void scan_bus(uint16_t segment, uint8_t bus, device_t *parent, unsigned depth);

static void add_function(const pci_location_t *loc, device_t *parent, unsigned depth)
{
    pci_device_t *pci = kcalloc(1, sizeof(*pci));
    if (!pci)
        return;

    pci->segment = loc->segment;
    pci->bus = loc->bus;
    pci->slot = loc->slot;
    pci->function = loc->function;

    device_t *d = &pci->device;
    d->id.vendor = pci_read16(pci, PCI_VENDOR_ID);
    d->id.device = pci_read16(pci, PCI_DEVICE_ID);
    d->id.revision = pci_read8(pci, PCI_REVISION);
    d->id.prog_if = pci_read8(pci, PCI_PROG_IF);
    d->id.subclass = pci_read8(pci, PCI_SUBCLASS);
    d->id.class_code = pci_read8(pci, PCI_CLASS);
    pci->header_type = pci_read8(pci, PCI_HEADER_TYPE) & 0x7F;
    format(d->name, sizeof(d->name), "pci%04x:%02x:%02x.%x", loc->segment, loc->bus, loc->slot, loc->function);

    if (pci->header_type == 0) {
        pci->subsystem_vendor = pci_read16(pci, PCI_SUBSYSTEM_VENDOR);
        pci->subsystem_id = pci_read16(pci, PCI_SUBSYSTEM_ID);
        read_bars(pci, 6);
    } else if (pci->header_type == 1) {
        read_bars(pci, 2);
    }
    read_capabilities(pci);

    device_count++;
    device_register(d, &pci_bus, parent);

    /* PCI-to-PCI bridge: continue behind it. */
    if (pci->header_type == 1) {
        uint8_t secondary = pci_read8(pci, PCI_SECONDARY_BUS);
        if (secondary > loc->bus && depth < 16)
            scan_bus(loc->segment, secondary, d, depth + 1);
    }
}

static void scan_bus(uint16_t segment, uint8_t bus, device_t *parent, unsigned depth)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        pci_location_t loc = { segment, bus, slot, 0 };
        if ((config_read(&loc, PCI_VENDOR_ID, 2) & 0xFFFF) == 0xFFFF)
            continue;

        uint8_t functions = (config_read(&loc, PCI_HEADER_TYPE, 1) & 0x80) ? 8 : 1;
        for (uint8_t fn = 0; fn < functions; fn++) {
            loc.function = fn;
            if ((config_read(&loc, PCI_VENDOR_ID, 2) & 0xFFFF) != 0xFFFF)
                add_function(&loc, parent, depth);
        }
    }
}

static status_t pci_init(void)
{
    pci_bus.name = "pci";
    pci_bus.set_power = bus_set_power;
    pci_bus.detached = bus_detached;
    status_t status = bus_register(&pci_bus);
    if (STATUS_IS_ERROR(status))
        return status;

    ecam_count = acpi_parse_mcfg(ecam, MAX_ECAM_RANGES);
    memcpy(segment_root.name, "pci-segment0", 13);
    device_register(&segment_root, NULL, NULL);

    scan_bus(0, 0, &segment_root, 0);
    if (ecam_count)
        klog_info("pci: ECAM at 0x%lx (buses %u-%u), %u functions", ecam[0].base, ecam[0].start_bus,
                  ecam[0].end_bus, device_count);
    else
        klog_info("pci: port I/O configuration access, %u functions", device_count);
    return STATUS_SUCCESS;
}

MODULE(.name = "pci", .description = "PCI bus", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 5, 0), .init = pci_init);

EXPORT_SYMBOL(pci_read8);
EXPORT_SYMBOL(pci_read16);
EXPORT_SYMBOL(pci_read32);
EXPORT_SYMBOL(pci_write8);
EXPORT_SYMBOL(pci_write16);
EXPORT_SYMBOL(pci_write32);
EXPORT_SYMBOL(pci_enable_device);
EXPORT_SYMBOL(pci_map_bar);
EXPORT_SYMBOL(pci_enable_msi);
EXPORT_SYMBOL(pci_disable_msi);
EXPORT_SYMBOL(pci_enable_msix);
EXPORT_SYMBOL(pci_disable_msix);
EXPORT_SYMBOL(pci_set_power);
