/*
 * Realtek Gigabit Ethernet driver for the RTL8168/RTL8111 family (README
 * section 32): the network chip of most consumer mainboards (8111B ... 8111H).
 *
 * The chip works with two rings of 16-byte descriptors, each with an "own"
 * bit that hands the descriptor to the chip; received frames land in 2 KiB
 * buffers, frames to send are copied into bounce buffers. The interrupt
 * (MSI) only wakes the network thread. All DMA memory lies below 4 GiB, so
 * the chip's 64-bit addressing mode is not needed.
 *
 * Start-up follows what every member of the family needs: reset, MAC
 * address from the ID registers, ring addresses, receive and transmit
 * configuration, and a fresh auto-negotiation that offers every speed (the
 * previous system may have left the PHY offering 10 Mbit/s only, for wake
 * on LAN).
 *
 * The newer chips (8168G and later, which includes the 8111H) wake up in
 * "out of band" mode, in which the chip serves its management side and
 * hands nothing to the driver. For them the driver leaves that mode, waits
 * for the chip's internal lists, sets the FIFO sizes and opens the receive
 * gate, following the sequence of Linux's r8169 driver.
 *
 * QEMU does not emulate this chip: there is no automated test. The driver
 * runs on an RTL8111H (revision 0x541); the older chips of the family have
 * not been tried (docs/drivers/hardware.md). `nodriver=rtl8168` on the
 * kernel command line turns it off.
 *
 * Not yet: the per-chip tuning of Realtek's own driver (power saving, EEE,
 * firmware patches), checksum offload, jumbo frames, wake on LAN.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "net/net.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "scheduler/thread.h"

#define REG_MAC          0x00 /* 6 bytes */
#define REG_MULTICAST    0x08 /* 8 bytes */
#define REG_TX_RING      0x20 /* 64 bits */
#define REG_COMMAND      0x37
#define REG_TX_POLL      0x38
#define REG_INT_MASK     0x3C /* 16 bits */
#define REG_INT_STATUS   0x3E
#define REG_TX_CONFIG    0x40
#define REG_RX_CONFIG    0x44
#define REG_LOCK         0x50 /* "9346CR": 0xC0 unlocks the configuration registers */
#define REG_PHY_ACCESS   0x60
#define REG_PHY_STATUS   0x6C
#define REG_POWER        0x6F /* "PMCH" */
#define REG_ERI_DATA     0x70 /* extended registers of the newer chips */
#define REG_ERI_ADDRESS  0x74
#define REG_MAC_OCP      0xB0 /* the chip's internal registers */
#define REG_MCU          0xD3
#define REG_PHY_OCP      0xB8 /* PHY access of the newer chips */
#define REG_RX_MAX_SIZE  0xDA
#define REG_CPLUS        0xE0
#define REG_RX_RING      0xE4 /* 64 bits */
#define REG_TX_MAX_SIZE  0xEC
#define REG_MISC         0xF0

#define COMMAND_RESET    0x10
#define COMMAND_RX       0x08
#define COMMAND_TX       0x04
#define TX_POLL_NORMAL   0x40
#define MISC_RX_GATE     (1u << 19) /* "RXDV gate": while set, nothing is received */
#define MCU_OUT_OF_BAND  (1u << 7)  /* the chip is in its management mode */
#define MCU_LIST_READY   (1u << 1)
#define MCU_FIFOS_EMPTY  ((1u << 5) | (1u << 4))
#define TX_CONFIG_EMPTY  (1u << 11)

#define INT_RX_OK        (1u << 0)
#define INT_RX_ERROR     (1u << 1)
#define INT_TX_OK        (1u << 2)
#define INT_RX_NO_DESC   (1u << 4)
#define INT_LINK_CHANGE  (1u << 5)
#define INT_RX_OVERFLOW  (1u << 6)

#define RX_ACCEPT_MINE      (1u << 1)
#define RX_ACCEPT_BROADCAST (1u << 3)
#define RX_DMA_UNLIMITED    (7u << 8)
#define RX_EARLY_OFF        (1u << 11)
#define RX_MULTI_ENABLE     (1u << 14)
#define RX_128_INTERRUPT    (1u << 15)
#define TX_DMA_UNLIMITED    (7u << 8)
#define TX_AUTO_FIFO        (1u << 7)  /* newer chips */
#define TX_GAP_STANDARD     (3u << 24)

#define PHY_LINK         (1u << 1)
#define PHY_1000         (1u << 4)
#define PHY_100          (1u << 3)

#define DESC_OWN         (1u << 31)
#define DESC_END_OF_RING (1u << 30)
#define DESC_FIRST       (1u << 29)
#define DESC_LAST        (1u << 28)
#define DESC_RX_ERROR    (1u << 21)

#define BMCR_NEGOTIATE   0x1200 /* auto-negotiation on, restart it; the power-down bit is cleared */
#define ADVERTISE_10_100 0x0DE1 /* 10 and 100 Mbit/s, half and full duplex, flow control */
#define ADVERTISE_1000   0x0300 /* 1000 Mbit/s, half and full duplex */

#define RING_SIZE   256
#define BUFFER_SIZE 2048
#define DMA_LIMIT   0xFFFFFFFFull

typedef struct {
    uint32_t flags;   /* own, ring end, first/last segment, length */
    uint32_t vlan;
    uint64_t address;
} rtl_descriptor_t;

typedef struct {
    volatile uint8_t *regs;
    uint32_t          irq;
    bool              modern;   /* 8168G and later */
    dma_buffer_t      rx_ring, tx_ring, rx_buffers, tx_buffers;
    uint32_t          rx_next, tx_next;
    bool              link;
    netif_t           netif;
} rtl_t;

static uint8_t r8(rtl_t *n, uint32_t reg)
{
    return n->regs[reg];
}

static uint16_t r16(rtl_t *n, uint32_t reg)
{
    return *(volatile uint16_t *)(n->regs + reg);
}

static uint32_t r32(rtl_t *n, uint32_t reg)
{
    return *(volatile uint32_t *)(n->regs + reg);
}

static void w8(rtl_t *n, uint32_t reg, uint8_t value)
{
    n->regs[reg] = value;
}

static void w16(rtl_t *n, uint32_t reg, uint16_t value)
{
    *(volatile uint16_t *)(n->regs + reg) = value;
}

static void w32(rtl_t *n, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(n->regs + reg) = value;
}

/* Write a standard PHY register (the two generations reach the PHY through different registers). */
static void phy_write(rtl_t *n, uint32_t reg, uint16_t value)
{
    if (n->modern) {
        /* The standard registers sit at 0xA400 + 2 * reg of the PHY's internal address space. */
        w32(n, REG_PHY_OCP, 0x80000000u | ((0xA400 + reg * 2) / 2) << 16 | value);
        for (int i = 0; i < 100 && (r32(n, REG_PHY_OCP) & 0x80000000u); i++)
            thread_sleep(100000);
    } else {
        w32(n, REG_PHY_ACCESS, 0x80000000u | (reg & 0x1F) << 16 | value);
        for (int i = 0; i < 100 && (r32(n, REG_PHY_ACCESS) & 0x80000000u); i++)
            thread_sleep(100000);
    }
}

/* Poll an 8-bit register until (value & mask) == wanted; about 50 ms at most. */
static bool wait8(rtl_t *n, uint32_t reg, uint8_t mask, uint8_t wanted)
{
    for (int i = 0; i < 50; i++) {
        if ((r8(n, reg) & mask) == wanted)
            return true;
        thread_sleep(1000000);
    }
    return false;
}

/* Extended register interface (8168G and later): `mask` selects the bytes of the 32-bit value to write. */
static void eri_write(rtl_t *n, uint32_t address, uint32_t mask, uint32_t value)
{
    w32(n, REG_ERI_DATA, value);
    w32(n, REG_ERI_ADDRESS, 0x80000000u | mask << 12 | address);
    for (int i = 0; i < 100 && (r32(n, REG_ERI_ADDRESS) & 0x80000000u); i++)
        thread_sleep(100000);
}

static uint32_t eri_read(rtl_t *n, uint32_t address)
{
    w32(n, REG_ERI_ADDRESS, 0xFu << 12 | address);
    for (int i = 0; i < 100 && !(r32(n, REG_ERI_ADDRESS) & 0x80000000u); i++)
        thread_sleep(100000);
    return r32(n, REG_ERI_DATA);
}

/* The chip's internal 16-bit registers (8168G and later). */
static uint16_t mac_ocp_read(rtl_t *n, uint32_t reg)
{
    w32(n, REG_MAC_OCP, reg << 15);
    return (uint16_t)r32(n, REG_MAC_OCP);
}

static void mac_ocp_modify(rtl_t *n, uint32_t reg, uint16_t clear, uint16_t set)
{
    uint16_t value = (uint16_t)((mac_ocp_read(n, reg) & ~clear) | set);
    w32(n, REG_MAC_OCP, 0x80000000u | reg << 15 | value);
}

/* 8168G and later: out of the management mode, and wait until the chip is ready to be driven. */
static void leave_out_of_band(rtl_t *n)
{
    w32(n, REG_RX_CONFIG, r32(n, REG_RX_CONFIG) & ~0x3Fu); /* accept nothing for now */
    w32(n, REG_MISC, r32(n, REG_MISC) | MISC_RX_GATE);
    thread_sleep(2000000);
    for (int i = 0; i < 50 && !(r32(n, REG_TX_CONFIG) & TX_CONFIG_EMPTY); i++)
        thread_sleep(1000000);
    wait8(n, REG_MCU, MCU_FIFOS_EMPTY, MCU_FIFOS_EMPTY);

    w8(n, REG_COMMAND, r8(n, REG_COMMAND) & ~(COMMAND_RX | COMMAND_TX));
    thread_sleep(1000000);
    w8(n, REG_MCU, r8(n, REG_MCU) & ~MCU_OUT_OF_BAND);
    mac_ocp_modify(n, 0xE8DE, 1u << 14, 0);
    if (!wait8(n, REG_MCU, MCU_LIST_READY, MCU_LIST_READY))
        klog_warn("rtl8168: the chip's link list is not ready (1)");
    mac_ocp_modify(n, 0xE8DE, 0, 1u << 15);
    if (!wait8(n, REG_MCU, MCU_LIST_READY, MCU_LIST_READY))
        klog_warn("rtl8168: the chip's link list is not ready (2)");
    w8(n, REG_POWER, r8(n, REG_POWER) | 0xC0); /* keep the PHY's clock running */
}

/* 8168G and later, with the configuration registers unlocked: FIFO sizes, flow control marks, packet filter. */
static void configure_modern(rtl_t *n)
{
    eri_write(n, 0xC8, 0xF, 0x00080002); /* receive FIFO */
    eri_write(n, 0xE8, 0xF, 0x00100006); /* transmit FIFO */
    eri_write(n, 0xCC, 0x1, 0x38);       /* pause thresholds */
    eri_write(n, 0xD0, 0x1, 0x48);
    uint32_t filter = eri_read(n, 0xDC);
    eri_write(n, 0xDC, 0xF, filter & ~1u); /* reset the packet filter */
    eri_write(n, 0xDC, 0xF, filter | 1u | 0x1C);
    eri_write(n, 0x5F0, 0x3, 0x4F87);
}

static void rtl_interrupt(void *context)
{
    rtl_t *n = context;
    w16(n, REG_INT_STATUS, r16(n, REG_INT_STATUS)); /* acknowledge everything */
    netif_receive_ready(&n->netif);
}

static bool rtl_link_up(netif_t *netif)
{
    rtl_t *n = netif->driver_data;
    uint8_t status = r8(n, REG_PHY_STATUS);
    bool up = status & PHY_LINK;

    if (up != n->link) {
        n->link = up;
        if (up)
            klog_info("rtl8168: %s: link up, %s Mbit/s", netif->name,
                      (status & PHY_1000) ? "1000" : (status & PHY_100) ? "100" : "10");
        else
            klog_info("rtl8168: %s: link down", netif->name);
    }
    return up;
}

static uint32_t ring_end(uint32_t index)
{
    return index == RING_SIZE - 1 ? DESC_END_OF_RING : 0;
}

static netbuf_t *rtl_receive(netif_t *netif)
{
    rtl_t *n = netif->driver_data;
    volatile rtl_descriptor_t *ring = n->rx_ring.virt;

    rtl_link_up(netif);
    while (!(ring[n->rx_next].flags & DESC_OWN)) {
        uint32_t index = n->rx_next, flags = ring[index].flags;
        uint32_t length = flags & 0x3FFF; /* with the 4-byte frame check sequence */
        bool good = (flags & DESC_FIRST) && (flags & DESC_LAST) && !(flags & DESC_RX_ERROR) && length > 4 &&
                    length <= BUFFER_SIZE;
        netbuf_t *frame = NULL;

        if (good) {
            frame = netbuf_alloc(length - 4);
            if (frame)
                memcpy(frame->data, (uint8_t *)n->rx_buffers.virt + (size_t)index * BUFFER_SIZE, length - 4);
        }
        if (!frame)
            netif->rx_dropped++;
        /* Give the descriptor back to the chip. */
        ring[index].vlan = 0;
        ring[index].flags = DESC_OWN | ring_end(index) | BUFFER_SIZE;
        n->rx_next = (index + 1) % RING_SIZE;
        if (frame)
            return frame;
    }
    return NULL;
}

static void rtl_transmit(netif_t *netif, netbuf_t *frame)
{
    rtl_t *n = netif->driver_data;
    volatile rtl_descriptor_t *d = (volatile rtl_descriptor_t *)n->tx_ring.virt + n->tx_next;

    /* Still the chip's: the ring is full. */
    if (frame->length > BUFFER_SIZE || (d->flags & DESC_OWN)) {
        netif->tx_dropped++;
        netbuf_free(frame);
        return;
    }
    uint8_t *buffer = (uint8_t *)n->tx_buffers.virt + (size_t)n->tx_next * BUFFER_SIZE;
    uint32_t length = (uint32_t)frame->length;
    memcpy(buffer, frame->data, length);
    if (length < 60) { /* the chip does not pad short frames reliably */
        memset(buffer + length, 0, 60 - length);
        length = 60;
    }
    d->vlan = 0;
    d->flags = DESC_OWN | DESC_FIRST | DESC_LAST | ring_end(n->tx_next) | length;
    n->tx_next = (n->tx_next + 1) % RING_SIZE;
    w8(n, REG_TX_POLL, TX_POLL_NORMAL);
    netbuf_free(frame);
}

static const netif_ops_t rtl_ops = {
    .transmit = rtl_transmit,
    .receive = rtl_receive,
    .link_up = rtl_link_up,
};

static status_t rtl_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    rtl_t *n = kcalloc(1, sizeof(*n));
    unsigned bar = PCI_MAX_BARS;

    if (!n)
        return STATUS_OUT_OF_MEMORY;
    status_t status = pci_enable_device(pci, true);
    /* The registers are in the first memory BAR (BAR 0 is the I/O port variant). */
    for (unsigned i = 0; i < PCI_MAX_BARS && bar == PCI_MAX_BARS; i++) {
        if (!pci->bars[i].io && pci->bars[i].size)
            bar = i;
    }
    if (!STATUS_IS_ERROR(status)) {
        n->regs = bar < PCI_MAX_BARS ? (volatile uint8_t *)pci_map_bar(pci, bar) : NULL;
        if (!n->regs)
            status = STATUS_DEVICE_ERROR;
    }
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, RING_SIZE * sizeof(rtl_descriptor_t), DMA_LIMIT, &n->rx_ring);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, RING_SIZE * sizeof(rtl_descriptor_t), DMA_LIMIT, &n->tx_ring);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, (uint64_t)RING_SIZE * BUFFER_SIZE, DMA_LIMIT, &n->rx_buffers);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, (uint64_t)RING_SIZE * BUFFER_SIZE, DMA_LIMIT, &n->tx_buffers);
    if (STATUS_IS_ERROR(status))
        goto fail;

    /* Which chip: the hardware revision is in the transmit configuration register. */
    uint32_t revision = (r32(n, REG_TX_CONFIG) >> 20) & 0xFCF;
    n->modern = (revision & 0x7C0) >= 0x4C0; /* 8168G, 8168H (8111H), 8168EP, 8168FP */

    w16(n, REG_INT_MASK, 0);
    if (n->modern)
        leave_out_of_band(n);
    w8(n, REG_COMMAND, COMMAND_RESET);
    for (int i = 0; i < 100 && (r8(n, REG_COMMAND) & COMMAND_RESET); i++)
        thread_sleep(1000000);
    if (r8(n, REG_COMMAND) & COMMAND_RESET) {
        status = STATUS_TIMEOUT;
        goto fail;
    }
    w16(n, REG_INT_STATUS, 0xFFFF);
    for (int i = 0; i < 6; i++)
        n->netif.mac[i] = r8(n, REG_MAC + (uint32_t)i);

    rtl_descriptor_t *rx = n->rx_ring.virt, *tx = n->tx_ring.virt;
    for (uint32_t i = 0; i < RING_SIZE; i++) {
        rx[i].address = n->rx_buffers.phys + (uint64_t)i * BUFFER_SIZE;
        rx[i].flags = DESC_OWN | ring_end(i) | BUFFER_SIZE;
        tx[i].address = n->tx_buffers.phys + (uint64_t)i * BUFFER_SIZE;
        tx[i].flags = ring_end(i);
    }

    status = pci_enable_msi(pci, rtl_interrupt, n, &n->irq);
    if (STATUS_IS_ERROR(status)) {
        klog_error("rtl8168: %s: no MSI interrupt: %s", device->name, status_name(status));
        goto fail;
    }
    n->netif.ops = &rtl_ops;
    n->netif.driver_data = n;
    n->netif.mtu = NET_MTU;
    status = netif_register(&n->netif);
    if (STATUS_IS_ERROR(status)) {
        pci_disable_msi(pci);
        goto fail;
    }

    w8(n, REG_LOCK, 0xC0); /* unlock the configuration registers */
    w16(n, REG_CPLUS, r16(n, REG_CPLUS) & ~(uint16_t)((1u << 6) | (1u << 5))); /* no VLAN stripping, no checksum offload */
    if (n->modern)
        configure_modern(n);
    w16(n, REG_RX_MAX_SIZE, 1536);
    w8(n, REG_TX_MAX_SIZE, 0x3B);
    w32(n, REG_TX_RING, (uint32_t)n->tx_ring.phys);
    w32(n, REG_TX_RING + 4, 0);
    w32(n, REG_RX_RING, (uint32_t)n->rx_ring.phys);
    w32(n, REG_RX_RING + 4, 0);
    w8(n, REG_COMMAND, COMMAND_RX | COMMAND_TX);
    w32(n, REG_TX_CONFIG, TX_GAP_STANDARD | TX_DMA_UNLIMITED | (n->modern ? TX_AUTO_FIFO : 0));
    w32(n, REG_RX_CONFIG, RX_ACCEPT_MINE | RX_ACCEPT_BROADCAST | RX_DMA_UNLIMITED |
                              (n->modern ? RX_128_INTERRUPT | RX_MULTI_ENABLE | RX_EARLY_OFF : 7u << 13));
    for (uint32_t i = 0; i < 8; i++)
        w8(n, REG_MULTICAST + i, 0); /* no multicast groups */
    w8(n, REG_LOCK, 0x00);

    if (n->modern)
        w32(n, REG_MISC, r32(n, REG_MISC) & ~MISC_RX_GATE); /* let received frames through */
    if (!n->modern)
        phy_write(n, 0x1F, 0); /* standard register page (the newer chips address it directly) */
    phy_write(n, 0x04, ADVERTISE_10_100);
    phy_write(n, 0x09, ADVERTISE_1000);
    phy_write(n, 0x00, BMCR_NEGOTIATE);

    w16(n, REG_INT_MASK, INT_RX_OK | INT_RX_ERROR | INT_RX_NO_DESC | INT_LINK_CHANGE | INT_RX_OVERFLOW);

    klog_info("rtl8168: %s: Realtek RTL8168/8111 (10ec:%04x, revision 0x%03x%s), %02x:%02x:%02x:%02x:%02x:%02x",
              n->netif.name, device->id.device, revision, n->modern ? ", 8168G or later" : "", n->netif.mac[0],
              n->netif.mac[1], n->netif.mac[2], n->netif.mac[3], n->netif.mac[4], n->netif.mac[5]);
    /* For finding out where it stops if it does not work: registers after start-up. */
    klog_info("rtl8168: %s: command 0x%x, mcu 0x%x, misc 0x%x, rx config 0x%x, tx config 0x%x, phy status 0x%x",
              n->netif.name, r8(n, REG_COMMAND), r8(n, REG_MCU), r32(n, REG_MISC), r32(n, REG_RX_CONFIG),
              r32(n, REG_TX_CONFIG), r8(n, REG_PHY_STATUS));
    device->driver_data = n;
    rtl_link_up(&n->netif);
    return STATUS_SUCCESS;

fail:
    dma_free(&n->rx_ring);
    dma_free(&n->tx_ring);
    dma_free(&n->rx_buffers);
    dma_free(&n->tx_buffers);
    kfree(n);
    return status;
}

static const device_match_t rtl_ids[] = {
    DEVICE_MATCH_ID(0x10EC, 0x8168), /* RTL8168 / RTL8111, all revisions */
    DEVICE_MATCH_ID(0x10EC, 0x8161), /* RTL8111 on some boards */
    DEVICE_MATCH_END,
};

static driver_t rtl_driver = {
    .name = "rtl8168",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_NETWORK,
    .ids = rtl_ids,
    .probe = rtl_probe,
};

static status_t rtl_module_init(void)
{
    return driver_register(&rtl_driver);
}

static const char *const rtl_dependencies[] = { "pci", NULL };

MODULE(.name = "rtl8168", .description = "Realtek Gigabit Ethernet (RTL8168/RTL8111)", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = rtl_dependencies, .init = rtl_module_init);
