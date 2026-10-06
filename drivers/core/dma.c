/*
 * DMA buffers. x86 DMA is cache coherent, so the direct map can be used.
 * Without an IOMMU, device addresses equal physical addresses; the device
 * argument is kept for IOMMU support later.
 */

#include "drivers/core/device.h"

#include "core/export.h"
#include "core/string.h"
#include "memory/layout.h"
#include "memory/pmm.h"

status_t dma_alloc(device_t *device, uint64_t size, uint64_t max_address, dma_buffer_t *buffer)
{
    (void)device;
    uint64_t pages = align_up(size, PAGE_SIZE) / PAGE_SIZE;
    uint64_t phys;

    if (size == 0)
        return STATUS_INVALID_ARGUMENT;
    status_t status = pmm_alloc_pages_below(pages, max_address, &phys);
    if (STATUS_IS_ERROR(status))
        return status;

    buffer->phys = phys;
    buffer->size = pages * PAGE_SIZE;
    buffer->virt = phys_to_virt(phys);
    memset(buffer->virt, 0, buffer->size);
    return STATUS_SUCCESS;
}

void dma_free(dma_buffer_t *buffer)
{
    if (buffer->size)
        pmm_free_pages(buffer->phys, buffer->size / PAGE_SIZE);
    buffer->size = 0;
    buffer->virt = NULL;
}

EXPORT_SYMBOL(dma_alloc);
EXPORT_SYMBOL(dma_free);
