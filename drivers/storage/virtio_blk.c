/*
 * VirtIO block device driver (README section 25; QEMU's primary virtual disk).
 *
 * One request queue with MSI-X. Requests are serialized by a mutex and use
 * a 3-descriptor chain (header, data, status). Data goes through a DMA
 * bounce buffer, so callers may pass any kernel buffer.
 */

#include "drivers/bus/virtio/virtio.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"
#include "fs/block/block.h"

#include "core/arch.h"
#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "scheduler/mutex.h"

#define VIRTIO_BLK_F_RO       (1ULL << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1ULL << 6)
#define VIRTIO_BLK_F_FLUSH    (1ULL << 9)

#define VIRTIO_BLK_T_IN       0
#define VIRTIO_BLK_T_OUT      1
#define VIRTIO_BLK_T_FLUSH    4
#define VIRTIO_BLK_S_OK       0

#define SECTOR_SIZE           512
#define QUEUE_SIZE            128
#define BOUNCE_SIZE           (64 * 1024)
#define REQUEST_TIMEOUT_NS    5000000000ULL

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} request_header_t;

typedef struct {
    virtio_device_t virtio;
    virtqueue_t     queue;
    block_device_t  block;
    mutex_t         lock;
    wait_queue_t    completion;
    dma_buffer_t    request;   /* header at 0, status byte at 16 */
    dma_buffer_t    bounce;
    uint32_t        irq;
} vblk_t;

static unsigned disk_count;

static void vblk_interrupt(void *context)
{
    vblk_t *v = context;
    wait_queue_wake_all(&v->completion, STATUS_SUCCESS);
}

static void set_desc(vblk_t *v, uint16_t i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next)
{
    v->queue.desc[i] = (virtq_desc_t){ addr, len, flags, next };
}

/* Submit the prepared chain and sleep until the device returns it. Lock held. */
static status_t run_request(vblk_t *v)
{
    volatile uint8_t *status_byte = (volatile uint8_t *)v->request.virt + sizeof(request_header_t);
    *status_byte = 0xFF;

    virtio_queue_submit(&v->queue, 0);

    uint64_t deadline = wait_deadline(REQUEST_TIMEOUT_NS);
    status_t status = STATUS_SUCCESS;
    uint64_t flags = arch_interrupts_save();
    while (!virtio_queue_has_used(&v->queue) && status == STATUS_SUCCESS)
        status = wait_queue_block_uninterruptible(&v->completion, deadline);
    arch_interrupts_restore(flags);

    if (status != STATUS_SUCCESS) {
        klog_error("block %s: request timed out", v->block.name);
        return STATUS_TIMEOUT;
    }
    virtio_queue_pop_used(&v->queue);
    return *status_byte == VIRTIO_BLK_S_OK ? STATUS_SUCCESS : STATUS_IO_ERROR;
}

static status_t transfer(vblk_t *v, uint32_t type, uint64_t lba, uint32_t count, void *buffer)
{
    request_header_t *header = v->request.virt;
    uint8_t *data = buffer;
    status_t status = STATUS_SUCCESS;

    mutex_lock(&v->lock);
    while (count && !STATUS_IS_ERROR(status)) {
        uint32_t chunk = count < BOUNCE_SIZE / SECTOR_SIZE ? count : BOUNCE_SIZE / SECTOR_SIZE;
        uint32_t bytes = chunk * SECTOR_SIZE;

        if (type == VIRTIO_BLK_T_OUT)
            memcpy(v->bounce.virt, data, bytes);
        *header = (request_header_t){ type, 0, lba };
        set_desc(v, 0, v->request.phys, sizeof(*header), VIRTQ_DESC_F_NEXT, 1);
        set_desc(v, 1, v->bounce.phys, bytes, VIRTQ_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0), 2);
        set_desc(v, 2, v->request.phys + sizeof(*header), 1, VIRTQ_DESC_F_WRITE, 0);

        status = run_request(v);
        if (!STATUS_IS_ERROR(status) && type == VIRTIO_BLK_T_IN)
            memcpy(data, v->bounce.virt, bytes);
        data += bytes;
        lba += chunk;
        count -= chunk;
    }
    mutex_unlock(&v->lock);
    return status;
}

static status_t vblk_read(block_device_t *block, uint64_t lba, uint32_t count, void *buffer)
{
    return transfer(block->driver_data, VIRTIO_BLK_T_IN, lba, count, buffer);
}

static status_t vblk_write(block_device_t *block, uint64_t lba, uint32_t count, const void *buffer)
{
    return transfer(block->driver_data, VIRTIO_BLK_T_OUT, lba, count, (void *)buffer);
}

static status_t vblk_flush(block_device_t *block)
{
    vblk_t *v = block->driver_data;

    if (!(v->virtio.features & VIRTIO_BLK_F_FLUSH))
        return STATUS_SUCCESS;

    mutex_lock(&v->lock);
    request_header_t *header = v->request.virt;
    *header = (request_header_t){ VIRTIO_BLK_T_FLUSH, 0, 0 };
    set_desc(v, 0, v->request.phys, sizeof(*header), VIRTQ_DESC_F_NEXT, 1);
    set_desc(v, 1, v->request.phys + sizeof(*header), 1, VIRTQ_DESC_F_WRITE, 0);
    status_t status = run_request(v);
    mutex_unlock(&v->lock);
    return status;
}

static const block_ops_t vblk_ops = {
    .read = vblk_read,
    .write = vblk_write,
    .flush = vblk_flush,
};

static void release(device_t *device, vblk_t *v)
{
    if (v->virtio.common)
        virtio_reset(&v->virtio);
    pci_disable_msix(v->virtio.pci);
    virtio_queue_free(&v->queue);
    dma_free(&v->request);
    dma_free(&v->bounce);
    kfree(v);
    device->driver_data = NULL;
}

static status_t vblk_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    vblk_t *v = kcalloc(1, sizeof(*v));
    if (!v)
        return STATUS_OUT_OF_MEMORY;
    device->driver_data = v;
    mutex_init(&v->lock);
    wait_queue_init(&v->completion);

    status_t status = virtio_init(&v->virtio, pci, VIRTIO_BLK_F_RO | VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_BLK_SIZE, device);
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msix(pci, 0, vblk_interrupt, v, &v->irq);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, 0, QUEUE_SIZE, 0, &v->queue);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, 4096, ~0ULL, &v->request);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, BOUNCE_SIZE, ~0ULL, &v->bounce);
    if (STATUS_IS_ERROR(status)) {
        release(device, v);
        return status;
    }

    uint32_t block_size = SECTOR_SIZE;
    if (v->virtio.features & VIRTIO_BLK_F_BLK_SIZE)
        block_size = *(volatile uint32_t *)(v->virtio.device_config + 20);
    if (block_size != SECTOR_SIZE) {
        klog_error("block %s: logical block size %u is not supported", device->name, block_size);
        release(device, v);
        return STATUS_NOT_SUPPORTED;
    }
    virtio_driver_ok(&v->virtio);

    format(v->block.name, sizeof(v->block.name), "virtio%u", disk_count++);
    v->block.sector_size = SECTOR_SIZE;
    v->block.sector_count = *(volatile uint64_t *)v->virtio.device_config; /* capacity in 512-byte sectors */
    v->block.read_only = v->virtio.features & VIRTIO_BLK_F_RO;
    v->block.ops = &vblk_ops;
    v->block.driver_data = v;
    return block_register(&v->block);
}

static const device_match_t vblk_ids[] = {
    DEVICE_MATCH_ID(VIRTIO_VENDOR, 0x1001), /* transitional */
    DEVICE_MATCH_ID(VIRTIO_VENDOR, 0x1042), /* modern */
    DEVICE_MATCH_END,
};

static driver_t vblk_driver = {
    .name = "virtio-blk",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_STORAGE,
    .ids = vblk_ids,
    .probe = vblk_probe,
};

static status_t vblk_module_init(void)
{
    return driver_register(&vblk_driver);
}

static const char *const vblk_dependencies[] = { "pci", NULL };

MODULE(.name = "virtio_blk", .description = "VirtIO block devices", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 6, 0), .dependencies = vblk_dependencies,
       .init = vblk_module_init);
