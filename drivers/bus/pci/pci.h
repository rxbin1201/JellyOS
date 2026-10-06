/*
 * PCI bus driver (README section 24).
 *
 * Configuration access through ECAM (ACPI MCFG) or legacy port I/O,
 * recursive enumeration behind bridges, BAR discovery, capability lists,
 * MSI and MSI-X, and PCI power management. Legacy INTx routing needs the
 * ACPI _PRT tables (AML) and is not supported yet; drivers use MSI/MSI-X.
 */

#ifndef DRIVERS_BUS_PCI_PCI_H
#define DRIVERS_BUS_PCI_PCI_H

#include "drivers/core/device.h"

#include "core/arch.h"
#include "core/list.h"

#define PCI_MAX_BARS      6

/* Configuration space registers */
#define PCI_VENDOR_ID     0x00
#define PCI_DEVICE_ID     0x02
#define PCI_COMMAND       0x04
#define PCI_STATUS        0x06
#define PCI_REVISION      0x08
#define PCI_PROG_IF       0x09
#define PCI_SUBCLASS      0x0A
#define PCI_CLASS         0x0B
#define PCI_HEADER_TYPE   0x0E
#define PCI_BAR0          0x10
#define PCI_SECONDARY_BUS 0x19
#define PCI_SUBSYSTEM_VENDOR 0x2C
#define PCI_SUBSYSTEM_ID  0x2E
#define PCI_CAPABILITIES  0x34

#define PCI_COMMAND_IO           (1u << 0)
#define PCI_COMMAND_MEMORY       (1u << 1)
#define PCI_COMMAND_BUS_MASTER   (1u << 2)
#define PCI_COMMAND_INTX_DISABLE (1u << 10)

/* Capability IDs */
#define PCI_CAP_PM        0x01
#define PCI_CAP_MSI       0x05
#define PCI_CAP_PCIE      0x10
#define PCI_CAP_MSIX      0x11

typedef struct {
    uint64_t       phys;
    uint64_t       size;
    bool           io;
    bool           prefetchable;
    bool           is_64bit;
    volatile void *mapped;
} pci_bar_t;

typedef struct pci_device {
    device_t   device;

    uint16_t   segment;
    uint8_t    bus;
    uint8_t    slot;
    uint8_t    function;
    uint8_t    header_type;
    uint16_t   subsystem_vendor;
    uint16_t   subsystem_id;

    pci_bar_t  bars[PCI_MAX_BARS];

    uint8_t    cap_pm;     /* capability offsets, 0 if absent */
    uint8_t    cap_msi;
    uint8_t    cap_msix;
    uint8_t    cap_pcie;

    /* Interrupts enabled through this bus driver */
    /* Configuration restored after D3hot -> D0 */
    uint32_t   saved_bars[PCI_MAX_BARS];
    uint16_t   saved_command;

    uint32_t   msi_irq;
    bool       msi_enabled;
    volatile uint32_t *msix_table;
    uint16_t   msix_size;
    uint32_t  *msix_irqs;  /* per entry, 0 if unused */
} pci_device_t;

#define pci_from_device(d) container_of((d), pci_device_t, device)

uint8_t  pci_read8(pci_device_t *pci, uint16_t offset);
uint16_t pci_read16(pci_device_t *pci, uint16_t offset);
uint32_t pci_read32(pci_device_t *pci, uint16_t offset);
void     pci_write8(pci_device_t *pci, uint16_t offset, uint8_t value);
void     pci_write16(pci_device_t *pci, uint16_t offset, uint16_t value);
void     pci_write32(pci_device_t *pci, uint16_t offset, uint32_t value);

/* Offset of the next capability with this ID after `after` (0: from the start); 0 if none. */
uint8_t  pci_find_capability(pci_device_t *pci, uint8_t id, uint8_t after);

/* Enable memory/I/O decoding and optionally bus mastering (DMA). */
status_t pci_enable_device(pci_device_t *pci, bool bus_master);

/* Map a memory BAR uncached (once; later calls return the same mapping). */
volatile void *pci_map_bar(pci_device_t *pci, unsigned bar);

/* One MSI vector for the device; disables INTx. */
status_t pci_enable_msi(pci_device_t *pci, irq_handler_t handler, void *context, uint32_t *irq);
void     pci_disable_msi(pci_device_t *pci);

/* One MSI-X table entry; enables MSI-X on first use. */
status_t pci_enable_msix(pci_device_t *pci, uint16_t entry, irq_handler_t handler, void *context, uint32_t *irq);
void     pci_disable_msix(pci_device_t *pci);

status_t pci_set_power(pci_device_t *pci, device_power_t power);

#endif
