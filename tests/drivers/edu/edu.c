/*
 * Driver module for QEMU's "edu" educational PCI device (1234:11e8).
 *
 * Its probe exercises the whole driver path: matching, BAR mapping, MMIO,
 * an MSI interrupt and DMA in both directions. The device only reaches
 * RUNNING if every step works, which the driver tests check.
 *
 * Registers: 0x00 id, 0x04 liveness (returns the inverse), 0x08 factorial,
 * 0x20 status, 0x24 interrupt status, 0x60 raise, 0x64 acknowledge,
 * 0x80/0x88/0x90/0x98 DMA source/destination/count/command.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "scheduler/thread.h"
#include "time/clock.h"

#define EDU_ID            0x00
#define EDU_LIVENESS      0x04
#define EDU_FACTORIAL     0x08
#define EDU_STATUS        0x20
#define EDU_IRQ_STATUS    0x24
#define EDU_IRQ_RAISE     0x60
#define EDU_IRQ_ACK       0x64
#define EDU_DMA_SOURCE    0x80
#define EDU_DMA_DEST      0x88
#define EDU_DMA_COUNT     0x90
#define EDU_DMA_COMMAND   0x98

#define EDU_STATUS_BUSY   1u
#define EDU_DMA_START     1u
#define EDU_DMA_TO_RAM    2u
#define EDU_DMA_IRQ       4u
#define EDU_DMA_DONE_IRQ  0x100u
#define EDU_DEVICE_BUFFER 0x40000u
#define EDU_DMA_LIMIT     ((1ULL << 28) - 1) /* the device masks DMA addresses to 28 bits */
#define TEST_IRQ_VALUE    0x5Au
#define TIMEOUT_NS        1000000000ULL

typedef struct {
    pci_device_t      *pci;
    volatile uint8_t  *regs;
    uint32_t           irq;
    volatile uint32_t  interrupts;
    volatile uint32_t  last_status;
    dma_buffer_t       dma;
} edu_t;

static uint32_t read_reg(edu_t *e, uint32_t offset)
{
    return *(volatile uint32_t *)(e->regs + offset);
}

static void write_reg(edu_t *e, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(e->regs + offset) = value;
}

static void write_reg64(edu_t *e, uint32_t offset, uint64_t value)
{
    *(volatile uint64_t *)(e->regs + offset) = value;
}

static void edu_interrupt(void *context)
{
    edu_t *e = context;
    uint32_t status = read_reg(e, EDU_IRQ_STATUS);

    e->last_status = status;
    write_reg(e, EDU_IRQ_ACK, status);
    e->interrupts++;
}

static bool wait_for_interrupt(edu_t *e, uint32_t count)
{
    uint64_t deadline = clock_monotonic_ns() + TIMEOUT_NS;
    while (e->interrupts < count) {
        if (clock_monotonic_ns() > deadline)
            return false;
        thread_sleep(1000000);
    }
    return true;
}

static status_t check(device_t *device, bool ok, const char *what)
{
    if (!ok)
        klog_error("edu %s: %s failed", device->name, what);
    return ok ? STATUS_SUCCESS : STATUS_DEVICE_ERROR;
}

static status_t dma_round_trip(device_t *device, edu_t *e)
{
    status_t status = dma_alloc(device, 8192, EDU_DMA_LIMIT, &e->dma);
    if (STATUS_IS_ERROR(status))
        return status;

    uint8_t *out = e->dma.virt, *in = out + 4096;
    for (int i = 0; i < 4096; i++)
        out[i] = (uint8_t)(i * 13 + 1);

    /* RAM -> device buffer -> RAM, each completion signaled by an interrupt. */
    write_reg64(e, EDU_DMA_SOURCE, e->dma.phys);
    write_reg64(e, EDU_DMA_DEST, EDU_DEVICE_BUFFER);
    write_reg64(e, EDU_DMA_COUNT, 4096);
    write_reg64(e, EDU_DMA_COMMAND, EDU_DMA_START | EDU_DMA_IRQ);
    if (!wait_for_interrupt(e, 2))
        return check(device, false, "DMA to device");

    write_reg64(e, EDU_DMA_SOURCE, EDU_DEVICE_BUFFER);
    write_reg64(e, EDU_DMA_DEST, e->dma.phys + 4096);
    write_reg64(e, EDU_DMA_COMMAND, EDU_DMA_START | EDU_DMA_TO_RAM | EDU_DMA_IRQ);
    if (!wait_for_interrupt(e, 3))
        return check(device, false, "DMA to RAM");

    return check(device, e->last_status == EDU_DMA_DONE_IRQ && memcmp(in, out, 4096) == 0, "DMA data");
}

static status_t edu_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    edu_t *e = kcalloc(1, sizeof(*e));
    if (!e)
        return STATUS_OUT_OF_MEMORY;
    device->driver_data = e;
    e->pci = pci;

    pci_enable_device(pci, true);
    e->regs = (volatile uint8_t *)pci_map_bar(pci, 0);
    status_t status = check(device, e->regs != NULL, "BAR 0 mapping");

    /* MMIO */
    if (!STATUS_IS_ERROR(status))
        status = check(device, (read_reg(e, EDU_ID) & 0xFF) == 0xED, "identification");
    if (!STATUS_IS_ERROR(status)) {
        write_reg(e, EDU_LIVENESS, 0x12345678);
        status = check(device, read_reg(e, EDU_LIVENESS) == ~0x12345678u, "liveness check");
    }
    if (!STATUS_IS_ERROR(status)) {
        write_reg(e, EDU_FACTORIAL, 10);
        uint64_t deadline = clock_monotonic_ns() + TIMEOUT_NS;
        while ((read_reg(e, EDU_STATUS) & EDU_STATUS_BUSY) && clock_monotonic_ns() < deadline)
            thread_sleep(1000000);
        status = check(device, read_reg(e, EDU_FACTORIAL) == 3628800, "factorial");
    }

    /* MSI */
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msi(pci, edu_interrupt, e, &e->irq);
    if (!STATUS_IS_ERROR(status)) {
        write_reg(e, EDU_IRQ_RAISE, TEST_IRQ_VALUE);
        status = check(device, wait_for_interrupt(e, 1) && e->last_status == TEST_IRQ_VALUE, "MSI interrupt");
    }

    /* DMA */
    if (!STATUS_IS_ERROR(status))
        status = dma_round_trip(device, e);

    if (STATUS_IS_ERROR(status)) {
        pci_disable_msi(pci);
        dma_free(&e->dma);
        kfree(e);
        device->driver_data = NULL;
        return status;
    }
    klog_info("edu %s: MMIO, MSI vector %u and DMA verified", device->name, e->irq);
    return STATUS_SUCCESS;
}

static void edu_remove(device_t *device)
{
    edu_t *e = device->driver_data;

    pci_disable_msi(e->pci);
    dma_free(&e->dma);
    kfree(e);
}

static status_t edu_suspend(device_t *device)
{
    klog_info("edu %s: suspend", device->name);
    return STATUS_SUCCESS;
}

static status_t edu_resume(device_t *device)
{
    edu_t *e = device->driver_data;
    write_reg(e, EDU_LIVENESS, 0xCAFE);
    return check(device, read_reg(e, EDU_LIVENESS) == ~0xCAFEu, "liveness after resume");
}

static const device_match_t edu_ids[] = {
    DEVICE_MATCH_ID(0x1234, 0x11E8),
    DEVICE_MATCH_END,
};

static driver_t edu_driver = {
    .name = "edu",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_TEST,
    .ids = edu_ids,
    .probe = edu_probe,
    .remove = edu_remove,
    .suspend = edu_suspend,
    .resume = edu_resume,
};

static status_t edu_module_init(void)
{
    return driver_register(&edu_driver);
}

static void edu_module_exit(void)
{
    driver_unregister(&edu_driver);
}

static const char *const edu_dependencies[] = { "pci", NULL };

MODULE(.name = "edu", .description = "QEMU edu test device", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 5, 0), .dependencies = edu_dependencies,
       .init = edu_module_init, .exit = edu_module_exit);
