/*
 * MSI-X test driver for the Intel 82574L (e1000e, 8086:10d3) in QEMU.
 *
 * Not a network driver: it only proves MSI-X delivery. The probe maps
 * "other" interrupt causes to MSI-X vector 0 (IVAR), enables the link
 * status change cause together with its OTHER summary bit, and sets it
 * through the Interrupt Cause Set register.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"

#include "core/log.h"
#include "memory/heap.h"
#include "scheduler/thread.h"
#include "time/clock.h"

#define E1000_ICR        0x000C0 /* interrupt cause read (read clears) */
#define E1000_ICS        0x000C8 /* interrupt cause set */
#define E1000_IMS        0x000D0 /* interrupt mask set */
#define E1000_IMC        0x000D8 /* interrupt mask clear */
#define E1000_IVAR       0x000E4 /* interrupt vector allocation */

#define ICR_LSC          (1u << 2)
#define ICR_OTHER        (1u << 24) /* MSI-X: summary bit for causes routed through IVAR "other" */
#define IVAR_OTHER_SHIFT 16
#define IVAR_VALID       0x8u
#define TIMEOUT_NS       1000000000ULL

typedef struct {
    pci_device_t     *pci;
    volatile uint8_t *regs;
    uint32_t          irq;
    volatile uint32_t interrupts;
} e1000e_t;

static uint32_t read_reg(e1000e_t *n, uint32_t offset)
{
    return *(volatile uint32_t *)(n->regs + offset);
}

static void write_reg(e1000e_t *n, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(n->regs + offset) = value;
}

static void e1000e_interrupt(void *context)
{
    e1000e_t *n = context;
    read_reg(n, E1000_ICR); /* acknowledge */
    n->interrupts++;
}

static status_t e1000e_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    e1000e_t *n = kcalloc(1, sizeof(*n));
    if (!n)
        return STATUS_OUT_OF_MEMORY;
    device->driver_data = n;
    n->pci = pci;

    pci_enable_device(pci, false);
    n->regs = (volatile uint8_t *)pci_map_bar(pci, 0);
    status_t status = n->regs ? STATUS_SUCCESS : STATUS_DEVICE_ERROR;
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msix(pci, 0, e1000e_interrupt, n, &n->irq);

    if (!STATUS_IS_ERROR(status)) {
        write_reg(n, E1000_IMC, 0xFFFFFFFF);
        read_reg(n, E1000_ICR);
        write_reg(n, E1000_IVAR, IVAR_VALID << IVAR_OTHER_SHIFT); /* other causes -> vector 0 */
        write_reg(n, E1000_IMS, ICR_LSC | ICR_OTHER);
        write_reg(n, E1000_ICS, ICR_LSC);

        uint64_t deadline = clock_monotonic_ns() + TIMEOUT_NS;
        while (!n->interrupts && clock_monotonic_ns() < deadline)
            thread_sleep(1000000);
        write_reg(n, E1000_IMC, 0xFFFFFFFF);
        if (!n->interrupts) {
            klog_error("e1000e %s: no MSI-X interrupt", device->name);
            status = STATUS_DEVICE_ERROR;
        }
    }

    if (STATUS_IS_ERROR(status)) {
        pci_disable_msix(pci);
        kfree(n);
        device->driver_data = NULL;
        return status;
    }
    klog_info("e1000e %s: MSI-X entry 0 -> vector %u verified (%u of %u entries)", device->name, n->irq,
              1u, (unsigned)pci->msix_size);
    return STATUS_SUCCESS;
}

static void e1000e_remove(device_t *device)
{
    e1000e_t *n = device->driver_data;
    pci_disable_msix(n->pci);
    kfree(n);
}

static const device_match_t e1000e_ids[] = {
    DEVICE_MATCH_ID(0x8086, 0x10D3),
    DEVICE_MATCH_END,
};

static driver_t e1000e_driver = {
    .name = "e1000e-msix-test",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_TEST,
    .ids = e1000e_ids,
    .probe = e1000e_probe,
    .remove = e1000e_remove,
};

static status_t module_start(void)
{
    return driver_register(&e1000e_driver);
}

static void module_stop(void)
{
    driver_unregister(&e1000e_driver);
}

static const char *const dependencies[] = { "pci", NULL };

MODULE(.name = "e1000e_msix", .description = "MSI-X delivery test on e1000e", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 5, 0), .dependencies = dependencies,
       .init = module_start, .exit = module_stop);
