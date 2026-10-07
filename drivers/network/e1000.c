/*
 * Intel Gigabit Ethernet driver for the e1000e family (README section 32):
 * the 82574L (QEMU's "e1000e") and the network ports built into Intel
 * chipsets, I217, I218 and I219.
 *
 * All of them share the classic register layout and the legacy descriptor
 * rings used here: a receive ring of 256 descriptors pointing at 2 KiB
 * buffers, and a transmit ring whose frames are copied into bounce buffers.
 * The interrupt (MSI) only wakes the network thread, which takes the
 * received frames out of the ring.
 *
 * The chipset ports are left as the firmware configured them (MAC address,
 * PHY, link); only the 82574L is reset. No offloads: checksums are computed
 * by the stack.
 *
 * Not yet: PHY configuration beyond auto-negotiation, power management,
 * wake on LAN, jumbo frames, statistics. I225/I226 (2.5 Gbit) are a
 * different family.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "net/net.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "scheduler/thread.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200 /* 128 entries */
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_AUTO_SPEED  (1u << 5)
#define CTRL_LINK_UP     (1u << 6)  /* "set link up" */
#define CTRL_LINK_RESET  (1u << 3)
#define CTRL_RESET       (1u << 26)
#define CTRL_PHY_RESET   (1u << 31)
#define STATUS_LINK      (1u << 1)
#define STATUS_SPEED(s)  (((s) >> 6) & 3)

#define INT_LINK_CHANGE  (1u << 2)
#define INT_RX_THRESHOLD (1u << 4)
#define INT_RX_OVERRUN   (1u << 6)
#define INT_RX_TIMER     (1u << 7)

#define RCTL_ENABLE      (1u << 1)
#define RCTL_BROADCAST   (1u << 15)
#define RCTL_STRIP_CRC   (1u << 26)
#define TCTL_ENABLE      (1u << 1)
#define TCTL_PAD_SHORT   (1u << 3)

#define RX_DONE          0x01
#define RX_END_OF_PACKET 0x02
#define TX_CMD_END       0x01 /* end of packet */
#define TX_CMD_CRC       0x02 /* append the frame check sequence */
#define TX_CMD_REPORT    0x08 /* set "done" in the descriptor */
#define TX_DONE          0x01

#define RING_SIZE   256
#define BUFFER_SIZE 2048

typedef struct {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} rx_descriptor_t;

typedef struct {
    uint64_t address;
    uint16_t length;
    uint8_t  checksum_offset;
    uint8_t  command;
    uint8_t  status;
    uint8_t  checksum_start;
    uint16_t special;
} tx_descriptor_t;

typedef struct {
    volatile uint8_t *regs;
    uint32_t          irq;
    dma_buffer_t      rx_ring, tx_ring, rx_buffers, tx_buffers;
    uint32_t          rx_next, tx_next;
    bool              link;
    netif_t           netif;
} e1000_t;

static uint32_t rd(e1000_t *n, uint32_t reg)
{
    return *(volatile uint32_t *)(n->regs + reg);
}

static void wr(e1000_t *n, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(n->regs + reg) = value;
}

static void e1000_interrupt(void *context)
{
    e1000_t *n = context;
    rd(n, REG_ICR); /* reading acknowledges every cause */
    netif_receive_ready(&n->netif);
}

static bool e1000_link_up(netif_t *netif)
{
    e1000_t *n = netif->driver_data;
    bool up = rd(n, REG_STATUS) & STATUS_LINK;

    if (up != n->link) {
        static const char *const speeds[] = { "10", "100", "1000", "1000" };
        n->link = up;
        if (up)
            klog_info("e1000: %s: link up, %s Mbit/s", netif->name, speeds[STATUS_SPEED(rd(n, REG_STATUS))]);
        else
            klog_info("e1000: %s: link down", netif->name);
    }
    return up;
}

static netbuf_t *e1000_receive(netif_t *netif)
{
    e1000_t *n = netif->driver_data;
    volatile rx_descriptor_t *ring = n->rx_ring.virt;

    e1000_link_up(netif); /* notice changes (the interrupt brought us here for those, too) */
    while (ring[n->rx_next].status & RX_DONE) {
        volatile rx_descriptor_t *d = &ring[n->rx_next];
        uint32_t index = n->rx_next, length = d->length;
        bool good = (d->status & RX_END_OF_PACKET) && !d->errors && length > 0 && length <= BUFFER_SIZE;
        netbuf_t *frame = NULL;

        if (good) {
            frame = netbuf_alloc(length);
            if (frame)
                memcpy(frame->data, (uint8_t *)n->rx_buffers.virt + (size_t)index * BUFFER_SIZE, length);
        }
        if (!frame)
            netif->rx_dropped++;
        /* Hand the descriptor back: the tail is the last one the card may use. */
        d->status = 0;
        n->rx_next = (index + 1) % RING_SIZE;
        wr(n, REG_RDT, index);
        if (frame)
            return frame;
    }
    return NULL;
}

static void e1000_transmit(netif_t *netif, netbuf_t *frame)
{
    e1000_t *n = netif->driver_data;
    volatile tx_descriptor_t *d = (volatile tx_descriptor_t *)n->tx_ring.virt + n->tx_next;

    /* A descriptor is free if it was never used or the card has sent it. */
    if (frame->length > BUFFER_SIZE || (d->command && !(d->status & TX_DONE))) {
        netif->tx_dropped++;
        netbuf_free(frame);
        return;
    }
    memcpy((uint8_t *)n->tx_buffers.virt + (size_t)n->tx_next * BUFFER_SIZE, frame->data, frame->length);
    d->length = (uint16_t)frame->length;
    d->status = 0;
    d->command = TX_CMD_END | TX_CMD_CRC | TX_CMD_REPORT;
    n->tx_next = (n->tx_next + 1) % RING_SIZE;
    wr(n, REG_TDT, n->tx_next);
    netbuf_free(frame);
}

static const netif_ops_t e1000_ops = {
    .transmit = e1000_transmit,
    .receive = e1000_receive,
    .link_up = e1000_link_up,
};

static void read_mac(e1000_t *n, pci_device_t *pci)
{
    uint32_t low = rd(n, REG_RAL0), high = rd(n, REG_RAH0);
    uint8_t mac[6] = { (uint8_t)low, (uint8_t)(low >> 8), (uint8_t)(low >> 16), (uint8_t)(low >> 24), (uint8_t)high,
                       (uint8_t)(high >> 8) };
    bool zero = !(low | (high & 0xFFFF)), ones = low == 0xFFFFFFFF && (high & 0xFFFF) == 0xFFFF;

    if (zero || ones || (mac[0] & 1)) {
        /* Nothing loaded from the card's memory: a locally administered address from the PCI location. */
        uint8_t made[6] = { 0x02, 0x4A, 0x45, 0x4C, (uint8_t)pci->bus, (uint8_t)(pci->slot << 3 | pci->function) };
        memcpy(mac, made, 6);
        wr(n, REG_RAL0, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
        wr(n, REG_RAH0, (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | 1u << 31);
    }
    memcpy(n->netif.mac, mac, 6);
}

static status_t e1000_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    bool discrete = device->id.device == 0x10D3; /* 82574L; everything else in the table is a chipset port */
    e1000_t *n = kcalloc(1, sizeof(*n));

    if (!n)
        return STATUS_OUT_OF_MEMORY;
    status_t status = pci_enable_device(pci, true);
    if (!STATUS_IS_ERROR(status)) {
        n->regs = (volatile uint8_t *)pci_map_bar(pci, 0);
        if (!n->regs)
            status = STATUS_DEVICE_ERROR;
    }
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, RING_SIZE * sizeof(rx_descriptor_t), ~0ull, &n->rx_ring);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, RING_SIZE * sizeof(tx_descriptor_t), ~0ull, &n->tx_ring);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, (uint64_t)RING_SIZE * BUFFER_SIZE, ~0ull, &n->rx_buffers);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, (uint64_t)RING_SIZE * BUFFER_SIZE, ~0ull, &n->tx_buffers);
    if (STATUS_IS_ERROR(status))
        goto fail;

    /* Quiet first. A full reset would also wipe what the firmware set up in a chipset port's PHY. */
    wr(n, REG_IMC, 0xFFFFFFFF);
    wr(n, REG_RCTL, 0);
    wr(n, REG_TCTL, 0);
    if (discrete) {
        wr(n, REG_CTRL, rd(n, REG_CTRL) | CTRL_RESET);
        for (int i = 0; i < 100 && (rd(n, REG_CTRL) & CTRL_RESET); i++)
            thread_sleep(1000000);
        thread_sleep(5000000);
        wr(n, REG_IMC, 0xFFFFFFFF);
    } else {
        thread_sleep(10000000); /* frames in flight */
    }
    rd(n, REG_ICR);
    read_mac(n, pci);
    wr(n, REG_CTRL, (rd(n, REG_CTRL) | CTRL_LINK_UP | CTRL_AUTO_SPEED) & ~(CTRL_LINK_RESET | CTRL_PHY_RESET));
    for (uint32_t i = 0; i < 128; i++)
        wr(n, REG_MTA + 4 * i, 0); /* no multicast groups */

    rx_descriptor_t *rx = n->rx_ring.virt;
    tx_descriptor_t *tx = n->tx_ring.virt;
    for (uint32_t i = 0; i < RING_SIZE; i++) {
        rx[i].address = n->rx_buffers.phys + (uint64_t)i * BUFFER_SIZE;
        tx[i].address = n->tx_buffers.phys + (uint64_t)i * BUFFER_SIZE;
    }
    wr(n, REG_RDBAL, (uint32_t)n->rx_ring.phys);
    wr(n, REG_RDBAH, (uint32_t)(n->rx_ring.phys >> 32));
    wr(n, REG_RDLEN, RING_SIZE * sizeof(rx_descriptor_t));
    wr(n, REG_RDH, 0);
    wr(n, REG_RDT, RING_SIZE - 1);
    wr(n, REG_TDBAL, (uint32_t)n->tx_ring.phys);
    wr(n, REG_TDBAH, (uint32_t)(n->tx_ring.phys >> 32));
    wr(n, REG_TDLEN, RING_SIZE * sizeof(tx_descriptor_t));
    wr(n, REG_TDH, 0);
    wr(n, REG_TDT, 0);

    status = pci_enable_msi(pci, e1000_interrupt, n, &n->irq);
    if (STATUS_IS_ERROR(status)) {
        klog_error("e1000: %s: no MSI interrupt: %s", device->name, status_name(status));
        goto fail;
    }
    n->netif.ops = &e1000_ops;
    n->netif.driver_data = n;
    n->netif.mtu = NET_MTU;
    status = netif_register(&n->netif);
    if (STATUS_IS_ERROR(status)) {
        pci_disable_msi(pci);
        goto fail;
    }

    wr(n, REG_TIPG, 0x00602008); /* inter-packet gaps for copper */
    wr(n, REG_TCTL, TCTL_ENABLE | TCTL_PAD_SHORT | 0x0Fu << 4 | 0x3Fu << 12); /* collision threshold and distance */
    wr(n, REG_RCTL, RCTL_ENABLE | RCTL_BROADCAST | RCTL_STRIP_CRC);            /* 2048-byte buffers */
    wr(n, REG_IMS, INT_LINK_CHANGE | INT_RX_THRESHOLD | INT_RX_OVERRUN | INT_RX_TIMER);

    klog_info("e1000: %s: Intel %s (8086:%04x), %02x:%02x:%02x:%02x:%02x:%02x", n->netif.name,
              discrete ? "82574L" : "chipset Ethernet", device->id.device, n->netif.mac[0], n->netif.mac[1],
              n->netif.mac[2], n->netif.mac[3], n->netif.mac[4], n->netif.mac[5]);
    device->driver_data = n;
    e1000_link_up(&n->netif);
    return STATUS_SUCCESS;

fail:
    dma_free(&n->rx_ring);
    dma_free(&n->tx_ring);
    dma_free(&n->rx_buffers);
    dma_free(&n->tx_buffers);
    kfree(n);
    return status;
}

#define INTEL(id) DEVICE_MATCH_ID(0x8086, id)

static const device_match_t e1000_ids[] = {
    INTEL(0x10D3),                                                                 /* 82574L */
    INTEL(0x153A), INTEL(0x153B),                                                  /* I217-LM, I217-V */
    INTEL(0x155A), INTEL(0x1559), INTEL(0x15A0), INTEL(0x15A1), INTEL(0x15A2), INTEL(0x15A3), /* I218 */
    /* I219-LM and I219-V, generations 1 to 23 */
    INTEL(0x156F), INTEL(0x1570), INTEL(0x15B7), INTEL(0x15B8), INTEL(0x15B9), INTEL(0x15D7), INTEL(0x15D8),
    INTEL(0x15E3), INTEL(0x15D6), INTEL(0x15BD), INTEL(0x15BE), INTEL(0x15BB), INTEL(0x15BC), INTEL(0x15DF),
    INTEL(0x15E0), INTEL(0x15E1), INTEL(0x15E2), INTEL(0x0D4E), INTEL(0x0D4F), INTEL(0x0D4C), INTEL(0x0D4D),
    INTEL(0x0D53), INTEL(0x0D55), INTEL(0x15FB), INTEL(0x15FC), INTEL(0x15F9), INTEL(0x15FA), INTEL(0x15F4),
    INTEL(0x15F5), INTEL(0x1A1E), INTEL(0x1A1F), INTEL(0x1A1C), INTEL(0x1A1D), INTEL(0x550A), INTEL(0x550B),
    INTEL(0x550C), INTEL(0x550D), INTEL(0x57A0), INTEL(0x57A1), INTEL(0x57B3), INTEL(0x57B4),
    DEVICE_MATCH_END,
};

static driver_t e1000_driver = {
    .name = "e1000",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_NETWORK,
    .ids = e1000_ids,
    .probe = e1000_probe,
};

static status_t e1000_module_init(void)
{
    return driver_register(&e1000_driver);
}

static const char *const e1000_dependencies[] = { "pci", NULL };

MODULE(.name = "e1000", .description = "Intel Gigabit Ethernet (82574L, I217, I218, I219)", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = e1000_dependencies, .init = e1000_module_init);
