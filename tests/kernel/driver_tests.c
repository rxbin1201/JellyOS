/*
 * Kernel tests: ACPI, interrupt routing, PCI, the device manager and
 * loadable driver modules (milestone M4: dynamic driver management).
 *
 * `make test` boots with the QEMU devices edu (1234:11e8) and e1000e
 * (8086:10d3) and passes the test modules from tests/drivers as boot modules.
 */

#include "tests/kernel/ktest.h"

#include "core/arch.h"
#include "core/string.h"
#include "drivers/acpi/acpi.h"
#include "drivers/bus/pci/pci.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"
#include "scheduler/thread.h"

#define EDU_VENDOR    0x1234
#define EDU_DEVICE    0x11E8
#define E1000E_VENDOR 0x8086
#define E1000E_DEVICE 0x10D3

static device_t *edu(void)
{
    return device_find("pci", EDU_VENDOR, EDU_DEVICE, 0);
}

static bool bound_to(const device_t *device, const char *driver)
{
    return device && device->state == DEVICE_RUNNING && device->driver && strcmp(device->driver->name, driver) == 0;
}

/* --- Firmware and interrupts ---------------------------------------------------- */

KTEST(acpi_tables_and_madt)
{
    acpi_madt_info_t madt;

    KEXPECT(acpi_find_table("APIC", 0) != NULL);
    KEXPECT(acpi_find_table("MCFG", 0) != NULL);
    KEXPECT(acpi_find_table("NONE", 0) == NULL);
    KASSERT(acpi_parse_madt(&madt) == STATUS_SUCCESS);
    KEXPECT(madt.cpu_count >= 1);
    KEXPECT(madt.ioapic_count >= 1);
}

static void count_interrupt(void *context)
{
    (*(volatile uint32_t *)context)++;
}

/* The PIT's channel 0 is unused by the kernel; route ISA IRQ 0 through the IOAPIC. */
KTEST(ioapic_routes_legacy_timer)
{
    volatile uint32_t count = 0;
    uint32_t irq, gsi;

    KASSERT(arch_irq_allocate(count_interrupt, (void *)&count, &irq) == STATUS_SUCCESS);
    arch_io_write8(0x43, 0x34);           /* channel 0, lo/hi, rate generator */
    arch_io_write8(0x40, 1193 & 0xFF);    /* ~1 kHz */
    arch_io_write8(0x40, 1193 >> 8);
    KASSERT(arch_irq_route_isa(0, irq, &gsi) == STATUS_SUCCESS);

    thread_sleep(50000000);
    arch_irq_mask_gsi(gsi);
    arch_io_write8(0x43, 0x30);           /* one-shot mode: stops after one more count */
    arch_io_write8(0x40, 0);
    arch_io_write8(0x40, 0);
    arch_irq_free(irq);

    KEXPECT(count >= 20);
}

/* --- PCI and resources ---------------------------------------------------------- */

KTEST(pci_enumeration_and_bars)
{
    device_t *device = edu();
    KASSERT(device != NULL);
    pci_device_t *pci = pci_from_device(device);

    KEXPECT(pci->bars[0].size == 1u << 20 && !pci->bars[0].io);
    KEXPECT(pci->cap_msi != 0);
    KEXPECT(device->resource_count >= 1 && device->resources[0].type == RESOURCE_MMIO);
    KEXPECT(strncmp(device->name, "pci0000:", 8) == 0);

    device_t *nic = device_find("pci", E1000E_VENDOR, E1000E_DEVICE, 0);
    KASSERT(nic != NULL);
    KEXPECT(pci_from_device(nic)->cap_msix != 0);
    KEXPECT(pci_from_device(nic)->cap_pm != 0);
}

KTEST(resource_conflicts_are_detected)
{
    device_t a = { .name = "test-a" }, b = { .name = "test-b" };
    resource_t window = { RESOURCE_MMIO, 0, 0xF00000000ULL, 0x2000 };
    resource_t overlap = { RESOURCE_MMIO, 0, 0xF00001000ULL, 0x2000 };
    resource_t io_port = { RESOURCE_IO, 0, 0xF00001000ULL, 0x10 }; /* other type: no conflict */

    KEXPECT(resource_claim(&window, &a) == STATUS_SUCCESS);
    KEXPECT(resource_claim(&overlap, &b) == STATUS_BUSY);
    KEXPECT(resource_claim(&io_port, &b) == STATUS_SUCCESS);
    resource_release_all(&a);
    KEXPECT(resource_claim(&overlap, &b) == STATUS_SUCCESS);
    resource_release_all(&b);
}

/* --- Drivers and modules ---------------------------------------------------------- */

KTEST(module_driver_probes_device)
{
    /* edu's probe verified MMIO, an MSI interrupt and DMA before reporting success. */
    KEXPECT(bound_to(edu(), "edu"));
    module_t *m = module_find("edu");
    KASSERT(m != NULL);
    KEXPECT(m->state == MODULE_RUNNING && !m->builtin);
}

KTEST(msix_interrupt_delivery)
{
    KEXPECT(bound_to(device_find("pci", E1000E_VENDOR, E1000E_DEVICE, 0), "e1000e-msix-test"));
}

KTEST(invalid_modules_are_rejected)
{
    static const char garbage[] = "not an ELF module";

    KEXPECT(module_load_boot_module("bad_api.ko") == STATUS_NOT_SUPPORTED);
    KEXPECT(module_load_boot_module("bad_dependency.ko") == STATUS_NOT_FOUND);
    KEXPECT(module_load_boot_module("bad_symbol.ko") == STATUS_NOT_FOUND);
    KEXPECT(module_load(garbage, sizeof(garbage), NULL) == STATUS_INVALID_ARGUMENT);
    KEXPECT(module_find("bad_api") == NULL && module_find("bad_dependency") == NULL &&
            module_find("bad_symbol") == NULL);
    KEXPECT(module_load_boot_module("edu.ko") == STATUS_BUSY); /* already loaded */
}

KTEST(module_dependencies_are_tracked)
{
    module_t *pci = module_find("pci");
    KASSERT(pci != NULL);
    KEXPECT(pci->builtin && pci->users >= 2); /* edu and e1000e_msix */
    KEXPECT(module_unload("pci") == STATUS_NOT_SUPPORTED);
}

KTEST(module_unload_and_reload)
{
    device_t *device = edu();
    KASSERT(bound_to(device, "edu"));

    KEXPECT(module_unload("edu") == STATUS_SUCCESS);
    KEXPECT(module_find("edu") == NULL);
    KEXPECT(device->state == DEVICE_DISCOVERED && device->driver == NULL);
    KEXPECT(module_unload("edu") == STATUS_NOT_FOUND);

    KEXPECT(module_load_boot_module("edu.ko") == STATUS_SUCCESS);
    KEXPECT(bound_to(device, "edu"));
}

KTEST(device_suspend_and_resume)
{
    device_t *device = edu();
    KASSERT(bound_to(device, "edu"));
    KEXPECT(device_suspend(device) == STATUS_SUCCESS && device->state == DEVICE_SUSPENDED);
    KEXPECT(device_suspend(device) == STATUS_INVALID_ARGUMENT);
    KEXPECT(device_resume(device) == STATUS_SUCCESS && device->state == DEVICE_RUNNING); /* edu re-checks MMIO */

    /* e1000e has PCI power management: D3hot and back, BARs restored. */
    device_t *nic = device_find("pci", E1000E_VENDOR, E1000E_DEVICE, 0);
    KASSERT(nic != NULL);
    pci_device_t *pci = pci_from_device(nic);
    uint32_t bar0 = pci_read32(pci, PCI_BAR0);
    KEXPECT(device_suspend(nic) == STATUS_SUCCESS && nic->power == DEVICE_POWER_D3);
    KEXPECT((pci_read16(pci, pci->cap_pm + 4) & 3) == 3);
    KEXPECT(device_resume(nic) == STATUS_SUCCESS && nic->power == DEVICE_POWER_D0);
    KEXPECT((pci_read16(pci, pci->cap_pm + 4) & 3) == 0);
    KEXPECT(pci_read32(pci, PCI_BAR0) == bar0);
}
