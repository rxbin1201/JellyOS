/*
 * VirtIO 1.x over PCI ("modern" transport) and split virtqueues.
 *
 * Shared by all VirtIO device drivers (block, network; input later).
 * Configuration structures are found through vendor-specific PCI
 * capabilities; queue interrupts use MSI-X.
 */

#ifndef DRIVERS_BUS_VIRTIO_VIRTIO_H
#define DRIVERS_BUS_VIRTIO_VIRTIO_H

#include "drivers/bus/pci/pci.h"

#define VIRTIO_VENDOR           0x1AF4
#define VIRTIO_F_VERSION_1      (1ULL << 32)

#define VIRTQ_DESC_F_NEXT       1
#define VIRTQ_DESC_F_WRITE      2 /* device writes into the buffer */

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct {
    uint16_t          flags;
    uint16_t          idx;
    virtq_used_elem_t ring[];
} virtq_used_t;

typedef struct {
    uint16_t                index;
    uint16_t                size;
    volatile virtq_desc_t  *desc;
    volatile virtq_avail_t *avail;
    volatile virtq_used_t  *used;
    volatile uint16_t      *notify;
    uint16_t                last_used;
    dma_buffer_t            memory;
} virtqueue_t;

typedef struct {
    pci_device_t     *pci;
    volatile uint8_t *common;
    volatile uint8_t *device_config;
    volatile uint8_t *notify_base;
    uint32_t          notify_multiplier;
    uint64_t          features;
} virtio_device_t;

/* Reset the device and negotiate features (VIRTIO_F_VERSION_1 is always required). */
status_t virtio_init(virtio_device_t *virtio, pci_device_t *pci, uint64_t wanted, device_t *owner);

/* Create queue `index` with at most max_size entries, interrupting through MSI-X entry msix_entry. */
status_t virtio_queue_setup(virtio_device_t *virtio, device_t *owner, uint16_t index, uint16_t max_size,
                            uint16_t msix_entry, virtqueue_t *queue);

/* Tell the device the driver is ready (after all queues are set up). */
void     virtio_driver_ok(virtio_device_t *virtio);

/* Publish descriptor chain `head` and notify the device. */
void     virtio_queue_submit(virtqueue_t *queue, uint16_t head);

/* True if the device returned a used buffer the driver has not consumed yet. */
bool     virtio_queue_has_used(virtqueue_t *queue);
void     virtio_queue_pop_used(virtqueue_t *queue);
/* Take the next used buffer: its head descriptor and the bytes the device wrote. False if none. */
bool     virtio_queue_next_used(virtqueue_t *queue, uint32_t *id, uint32_t *length);

void     virtio_reset(virtio_device_t *virtio);
void     virtio_queue_free(virtqueue_t *queue);

#endif
