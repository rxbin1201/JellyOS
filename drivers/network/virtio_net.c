/*
 * VirtIO network device driver (README sections 32 and 47: QEMU's primary
 * virtual NIC).
 *
 * Queue 0 receives into RX_SLOTS fixed 2 KiB DMA buffers; its MSI-X
 * interrupt only wakes the network thread, which copies frames out and
 * re-posts the buffers. Queue 1 transmits from TX_SLOTS bounce buffers
 * without an interrupt: finished slots are reclaimed on the next send.
 * No offloads (checksums are computed by the stack), no mergeable buffers.
 */

#include "drivers/bus/virtio/virtio.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"
#include "net/net.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define VIRTIO_NET_F_MAC    (1ULL << 5)
#define VIRTIO_NET_F_STATUS (1ULL << 16)
#define VIRTIO_NET_S_LINK_UP 1

#define NET_HEADER_SIZE 12  /* struct virtio_net_hdr with num_buffers (VERSION_1) */
#define SLOT_SIZE       2048
#define RX_SLOTS        128
#define TX_SLOTS        128
#define NO_VECTOR       0xFFFF

typedef struct {
    virtio_device_t virtio;
    virtqueue_t     rx, tx;
    dma_buffer_t    rx_buffers, tx_buffers;
    uint16_t        rx_count, tx_count;
    uint16_t        tx_free[TX_SLOTS];
    uint16_t        tx_free_count;
    uint32_t        irq;
    netif_t         netif;
} vnet_t;

static void vnet_interrupt(void *context)
{
    vnet_t *v = context;
    netif_receive_ready(&v->netif);
}

static void post_rx(vnet_t *v, uint16_t slot)
{
    v->rx.desc[slot] = (virtq_desc_t){ v->rx_buffers.phys + (uint64_t)slot * SLOT_SIZE, SLOT_SIZE,
                                       VIRTQ_DESC_F_WRITE, 0 };
    virtio_queue_submit(&v->rx, slot);
}

static netbuf_t *vnet_receive(netif_t *netif)
{
    vnet_t *v = netif->driver_data;
    uint32_t id, length;

    while (virtio_queue_next_used(&v->rx, &id, &length)) {
        netbuf_t *frame = NULL;
        if (id < v->rx_count && length > NET_HEADER_SIZE && length <= SLOT_SIZE) {
            frame = netbuf_alloc(length - NET_HEADER_SIZE);
            if (frame)
                memcpy(frame->data, (uint8_t *)v->rx_buffers.virt + id * SLOT_SIZE + NET_HEADER_SIZE,
                       length - NET_HEADER_SIZE);
            else
                netif->rx_dropped++;
        }
        if (id < v->rx_count)
            post_rx(v, (uint16_t)id);
        if (frame)
            return frame;
    }
    return NULL;
}

static void reclaim_tx(vnet_t *v)
{
    uint32_t id, length;
    while (virtio_queue_next_used(&v->tx, &id, &length)) {
        if (id < v->tx_count)
            v->tx_free[v->tx_free_count++] = (uint16_t)id;
    }
}

static void vnet_transmit(netif_t *netif, netbuf_t *frame)
{
    vnet_t *v = netif->driver_data;

    reclaim_tx(v);
    if (v->tx_free_count == 0 || frame->length > SLOT_SIZE - NET_HEADER_SIZE) {
        netif->tx_dropped++;
        netbuf_free(frame);
        return;
    }
    uint16_t slot = v->tx_free[--v->tx_free_count];
    uint8_t *buffer = (uint8_t *)v->tx_buffers.virt + slot * SLOT_SIZE;
    memset(buffer, 0, NET_HEADER_SIZE); /* no offloads */
    memcpy(buffer + NET_HEADER_SIZE, frame->data, frame->length);
    v->tx.desc[slot] = (virtq_desc_t){ v->tx_buffers.phys + (uint64_t)slot * SLOT_SIZE,
                                       (uint32_t)(NET_HEADER_SIZE + frame->length), 0, 0 };
    virtio_queue_submit(&v->tx, slot);
    netbuf_free(frame);
}

static bool vnet_link_up(netif_t *netif)
{
    vnet_t *v = netif->driver_data;
    if (!(v->virtio.features & VIRTIO_NET_F_STATUS))
        return true;
    return *(volatile uint16_t *)(v->virtio.device_config + 6) & VIRTIO_NET_S_LINK_UP;
}

static const netif_ops_t vnet_ops = {
    .transmit = vnet_transmit,
    .receive = vnet_receive,
    .link_up = vnet_link_up,
};

static void release(device_t *device, vnet_t *v)
{
    if (v->virtio.common)
        virtio_reset(&v->virtio);
    pci_disable_msix(v->virtio.pci);
    virtio_queue_free(&v->rx);
    virtio_queue_free(&v->tx);
    dma_free(&v->rx_buffers);
    dma_free(&v->tx_buffers);
    kfree(v);
    device->driver_data = NULL;
}

static status_t vnet_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    vnet_t *v = kcalloc(1, sizeof(*v));
    if (!v)
        return STATUS_OUT_OF_MEMORY;
    device->driver_data = v;

    status_t status = virtio_init(&v->virtio, pci, VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS, device);
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msix(pci, 0, vnet_interrupt, v, &v->irq);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, 0, RX_SLOTS, 0, &v->rx);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, 1, TX_SLOTS, NO_VECTOR, &v->tx);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, (uint64_t)RX_SLOTS * SLOT_SIZE, ~0ULL, &v->rx_buffers);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, (uint64_t)TX_SLOTS * SLOT_SIZE, ~0ULL, &v->tx_buffers);
    if (STATUS_IS_ERROR(status)) {
        release(device, v);
        return status;
    }

    if (v->virtio.features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++)
            v->netif.mac[i] = v->virtio.device_config[i];
    } else {
        /* Locally administered address derived from the PCI location. */
        uint8_t mac[6] = { 0x02, 0x4A, 0x45, 0x4C, (uint8_t)pci->bus, (uint8_t)(pci->slot << 3 | pci->function) };
        memcpy(v->netif.mac, mac, 6);
    }

    v->rx_count = v->rx.size;
    v->tx_count = v->tx.size;
    for (uint16_t i = 0; i < v->tx_count; i++)
        v->tx_free[v->tx_free_count++] = i;
    virtio_driver_ok(&v->virtio);
    for (uint16_t i = 0; i < v->rx_count; i++)
        post_rx(v, i);

    v->netif.ops = &vnet_ops;
    v->netif.driver_data = v;
    v->netif.mtu = NET_MTU;
    return netif_register(&v->netif);
}

static const device_match_t vnet_ids[] = {
    DEVICE_MATCH_ID(VIRTIO_VENDOR, 0x1000), /* transitional */
    DEVICE_MATCH_ID(VIRTIO_VENDOR, 0x1041), /* modern */
    DEVICE_MATCH_END,
};

static driver_t vnet_driver = {
    .name = "virtio-net",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_NETWORK,
    .ids = vnet_ids,
    .probe = vnet_probe,
};

static status_t vnet_module_init(void)
{
    return driver_register(&vnet_driver);
}

static const char *const vnet_dependencies[] = { "pci", NULL };

MODULE(.name = "virtio_net", .description = "VirtIO network devices", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 8, 0), .dependencies = vnet_dependencies,
       .init = vnet_module_init);
