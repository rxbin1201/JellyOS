#include "drivers/bus/virtio/virtio.h"

#include "core/export.h"
#include "core/log.h"
#include "memory/layout.h"

/* Vendor-specific capability types */
#define CAP_VENDOR           0x09
#define CAP_COMMON_CFG       1
#define CAP_NOTIFY_CFG       2
#define CAP_DEVICE_CFG       4

/* Common configuration layout */
#define COMMON_DEVICE_FEATURE_SELECT 0x00
#define COMMON_DEVICE_FEATURE        0x04
#define COMMON_DRIVER_FEATURE_SELECT 0x08
#define COMMON_DRIVER_FEATURE        0x0C
#define COMMON_MSIX_CONFIG           0x10
#define COMMON_DEVICE_STATUS         0x14
#define COMMON_QUEUE_SELECT          0x16
#define COMMON_QUEUE_SIZE            0x18
#define COMMON_QUEUE_MSIX_VECTOR     0x1A
#define COMMON_QUEUE_ENABLE          0x1C
#define COMMON_QUEUE_NOTIFY_OFF      0x1E
#define COMMON_QUEUE_DESC            0x20
#define COMMON_QUEUE_DRIVER          0x28
#define COMMON_QUEUE_DEVICE          0x30

#define STATUS_ACKNOWLEDGE  1
#define STATUS_DRIVER       2
#define STATUS_DRIVER_OK    4
#define STATUS_FEATURES_OK  8
#define STATUS_FAILED       128
#define NO_VECTOR           0xFFFF

static inline void w8(virtio_device_t *v, uint32_t o, uint8_t x)   { *(volatile uint8_t *)(v->common + o) = x; }
static inline void w16(virtio_device_t *v, uint32_t o, uint16_t x) { *(volatile uint16_t *)(v->common + o) = x; }
static inline void w32(virtio_device_t *v, uint32_t o, uint32_t x) { *(volatile uint32_t *)(v->common + o) = x; }
static inline void w64(virtio_device_t *v, uint32_t o, uint64_t x)
{
    w32(v, o, (uint32_t)x);
    w32(v, o + 4, (uint32_t)(x >> 32));
}
static inline uint8_t  r8(virtio_device_t *v, uint32_t o)  { return *(volatile uint8_t *)(v->common + o); }
static inline uint16_t r16(virtio_device_t *v, uint32_t o) { return *(volatile uint16_t *)(v->common + o); }
static inline uint32_t r32(virtio_device_t *v, uint32_t o) { return *(volatile uint32_t *)(v->common + o); }

static volatile uint8_t *map_structure(pci_device_t *pci, uint8_t cap)
{
    uint8_t bar = pci_read8(pci, cap + 4);
    uint32_t offset = pci_read32(pci, cap + 8);
    volatile uint8_t *base = pci_map_bar(pci, bar);
    return base ? base + offset : NULL;
}

static status_t find_structures(virtio_device_t *v)
{
    for (uint8_t cap = pci_find_capability(v->pci, CAP_VENDOR, 0); cap;
         cap = pci_find_capability(v->pci, CAP_VENDOR, cap)) {
        uint8_t type = pci_read8(v->pci, cap + 3);
        if (type == CAP_COMMON_CFG && !v->common) {
            v->common = map_structure(v->pci, cap);
        } else if (type == CAP_NOTIFY_CFG && !v->notify_base) {
            v->notify_base = map_structure(v->pci, cap);
            v->notify_multiplier = pci_read32(v->pci, cap + 16);
        } else if (type == CAP_DEVICE_CFG && !v->device_config) {
            v->device_config = map_structure(v->pci, cap);
        }
    }
    return (v->common && v->notify_base) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

void virtio_reset(virtio_device_t *v)
{
    w8(v, COMMON_DEVICE_STATUS, 0);
    while (r8(v, COMMON_DEVICE_STATUS) != 0)
        __asm__ volatile("pause");
}

status_t virtio_init(virtio_device_t *v, pci_device_t *pci, uint64_t wanted, device_t *owner)
{
    v->pci = pci;
    pci_enable_device(pci, true);

    status_t status = find_structures(v);
    if (STATUS_IS_ERROR(status)) {
        klog_error("virtio %s: no modern (VirtIO 1.x) configuration structures", owner->name);
        return status;
    }

    virtio_reset(v);
    w8(v, COMMON_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
    w8(v, COMMON_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    w32(v, COMMON_DEVICE_FEATURE_SELECT, 0);
    uint64_t offered = r32(v, COMMON_DEVICE_FEATURE);
    w32(v, COMMON_DEVICE_FEATURE_SELECT, 1);
    offered |= (uint64_t)r32(v, COMMON_DEVICE_FEATURE) << 32;

    v->features = offered & (wanted | VIRTIO_F_VERSION_1);
    if (!(v->features & VIRTIO_F_VERSION_1)) {
        w8(v, COMMON_DEVICE_STATUS, STATUS_FAILED);
        return STATUS_NOT_SUPPORTED;
    }
    w32(v, COMMON_DRIVER_FEATURE_SELECT, 0);
    w32(v, COMMON_DRIVER_FEATURE, (uint32_t)v->features);
    w32(v, COMMON_DRIVER_FEATURE_SELECT, 1);
    w32(v, COMMON_DRIVER_FEATURE, (uint32_t)(v->features >> 32));

    w8(v, COMMON_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(r8(v, COMMON_DEVICE_STATUS) & STATUS_FEATURES_OK)) {
        w8(v, COMMON_DEVICE_STATUS, STATUS_FAILED);
        return STATUS_NOT_SUPPORTED;
    }
    w16(v, COMMON_MSIX_CONFIG, NO_VECTOR); /* no configuration change interrupts */
    return STATUS_SUCCESS;
}

status_t virtio_queue_setup(virtio_device_t *v, device_t *owner, uint16_t index, uint16_t max_size,
                            uint16_t msix_entry, virtqueue_t *q)
{
    w16(v, COMMON_QUEUE_SELECT, index);
    uint16_t size = r16(v, COMMON_QUEUE_SIZE);
    if (size == 0)
        return STATUS_NOT_FOUND;
    if (size > max_size)
        size = max_size;

    /* Descriptors and available ring in the first page(s), used ring page aligned after them. */
    uint64_t avail_offset = 16ULL * size;
    uint64_t used_offset = align_up(avail_offset + 6 + 2ULL * size, PAGE_SIZE);
    status_t status = dma_alloc(owner, used_offset + 6 + 8ULL * size, ~0ULL, &q->memory);
    if (STATUS_IS_ERROR(status))
        return status;

    q->index = index;
    q->size = size;
    q->last_used = 0;
    q->desc = (volatile virtq_desc_t *)q->memory.virt;
    q->avail = (volatile virtq_avail_t *)((uint8_t *)q->memory.virt + avail_offset);
    q->used = (volatile virtq_used_t *)((uint8_t *)q->memory.virt + used_offset);

    w16(v, COMMON_QUEUE_SIZE, size);
    w64(v, COMMON_QUEUE_DESC, q->memory.phys);
    w64(v, COMMON_QUEUE_DRIVER, q->memory.phys + avail_offset);
    w64(v, COMMON_QUEUE_DEVICE, q->memory.phys + used_offset);
    w16(v, COMMON_QUEUE_MSIX_VECTOR, msix_entry);
    if (r16(v, COMMON_QUEUE_MSIX_VECTOR) != msix_entry) {
        dma_free(&q->memory);
        return STATUS_NOT_SUPPORTED;
    }

    uint16_t notify_offset = r16(v, COMMON_QUEUE_NOTIFY_OFF);
    q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)notify_offset * v->notify_multiplier);
    w16(v, COMMON_QUEUE_ENABLE, 1);
    return STATUS_SUCCESS;
}

void virtio_driver_ok(virtio_device_t *v)
{
    w8(v, COMMON_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
}

void virtio_queue_submit(virtqueue_t *q, uint16_t head)
{
    q->avail->ring[q->avail->idx % q->size] = head;
    __sync_synchronize(); /* descriptors and ring entry before the index */
    q->avail->idx = q->avail->idx + 1;
    __sync_synchronize(); /* index before the notification */
    *q->notify = q->index;
}

bool virtio_queue_has_used(virtqueue_t *q)
{
    __sync_synchronize();
    return q->used->idx != q->last_used;
}

void virtio_queue_pop_used(virtqueue_t *q)
{
    q->last_used++;
}

bool virtio_queue_next_used(virtqueue_t *q, uint32_t *id, uint32_t *length)
{
    if (!virtio_queue_has_used(q))
        return false;
    volatile virtq_used_elem_t *e = &q->used->ring[q->last_used % q->size];
    *id = e->id;
    *length = e->len;
    q->last_used++;
    return true;
}

void virtio_queue_free(virtqueue_t *q)
{
    dma_free(&q->memory);
}

EXPORT_SYMBOL(virtio_init);
EXPORT_SYMBOL(virtio_queue_setup);
EXPORT_SYMBOL(virtio_driver_ok);
EXPORT_SYMBOL(virtio_queue_submit);
EXPORT_SYMBOL(virtio_queue_has_used);
EXPORT_SYMBOL(virtio_queue_pop_used);
EXPORT_SYMBOL(virtio_queue_next_used);
EXPORT_SYMBOL(virtio_reset);
EXPORT_SYMBOL(virtio_queue_free);
