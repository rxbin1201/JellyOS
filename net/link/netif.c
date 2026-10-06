/*
 * Network interfaces and Ethernet (README section 32: network abstraction,
 * Ethernet).
 *
 * An interface is registered by its driver (or the loopback code) and never
 * goes away; index 0 is always "lo". Ethernet frames carry IPv4 and ARP;
 * everything else is dropped. Frames shorter than 60 bytes are padded by the
 * hardware (VirtIO) or by us on output.
 */

#include "net/net.h"
#include "net/arp/arp.h"
#include "net/ipv4/ipv4.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"

const uint8_t eth_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static netif_t *interfaces[NETIF_MAX];
static uint32_t interface_count;
static uint32_t ethernet_count;

status_t netif_register(netif_t *netif)
{
    if (interface_count == NETIF_MAX)
        return STATUS_LIMIT_EXCEEDED;
    mutex_lock(&net_lock);
    netif->index = interface_count;
    if (!netif->name[0])
        format(netif->name, sizeof(netif->name), "eth%u", ethernet_count++);
    if (!netif->mtu)
        netif->mtu = NET_MTU;
    interfaces[interface_count++] = netif;
    mutex_unlock(&net_lock);

    if (!netif->loopback)
        klog_info("net: %s %02x:%02x:%02x:%02x:%02x:%02x, link %s", netif->name, netif->mac[0], netif->mac[1],
                  netif->mac[2], netif->mac[3], netif->mac[4], netif->mac[5],
                  netif->ops->link_up(netif) ? "up" : "down");
    return STATUS_SUCCESS;
}

netif_t *netif_get(uint32_t index)
{
    return index < interface_count ? interfaces[index] : NULL;
}

uint32_t netif_count(void)
{
    return interface_count;
}

netif_t *netif_by_address(uint32_t address)
{
    for (uint32_t i = 0; i < interface_count; i++) {
        if (interfaces[i]->address && interfaces[i]->address == address)
            return interfaces[i];
    }
    return NULL;
}

void netif_transmit(netif_t *netif, netbuf_t *frame)
{
    netif->tx_packets++;
    netif->tx_bytes += frame->length;
    netif->ops->transmit(netif, frame);
}

void netif_configure(netif_t *netif, uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns)
{
    char a[16], m[16], g[16];

    if (address == 0)
        netmask = gateway = dns = 0;
    if (netif->address != address)
        arp_flush(netif);
    netif->address = address;
    netif->netmask = netmask;
    netif->gateway = gateway;
    netif->dns = dns;
    if (address)
        klog_info("net: %s configured %s/%s gateway %s", netif->name, ipv4_format(address, a),
                  ipv4_format(netmask, m), ipv4_format(gateway, g));
    else
        klog_info("net: %s unconfigured", netif->name);
    if (address && !netif->loopback)
        arp_announce(netif);
}

/* --- Ethernet ------------------------------------------------------------------- */

void ethernet_input(netif_t *netif, netbuf_t *frame)
{
    if (frame->length < ETH_HEADER_SIZE) {
        netif->rx_dropped++;
        netbuf_free(frame);
        return;
    }
    const uint8_t *header = frame->data;
    bool for_us = !memcmp(header, netif->mac, 6) || !memcmp(header, eth_broadcast, 6);
    uint16_t type = get_be16(header + 12);
    netbuf_pull(frame, ETH_HEADER_SIZE);

    if (!for_us) {
        netbuf_free(frame);
        return;
    }
    switch (type) {
    case ETH_TYPE_IPV4:
        ipv4_input(netif, frame);
        break;
    case ETH_TYPE_ARP:
        arp_input(netif, frame);
        break;
    default:
        netbuf_free(frame); /* IPv6 and others are not handled */
        break;
    }
}

void ethernet_output(netif_t *netif, const uint8_t destination[6], uint16_t type, netbuf_t *packet)
{
    /* Pad to the minimum frame size (60 bytes without the FCS). */
    if (packet->length < 46) {
        size_t old = packet->length;
        if ((size_t)(packet->data - packet->storage) + 46 <= packet->capacity) {
            memset(packet->data + old, 0, 46 - old);
            packet->length = 46;
        }
    }
    uint8_t *header = netbuf_push(packet, ETH_HEADER_SIZE);
    memcpy(header, destination, 6);
    memcpy(header + 6, netif->mac, 6);
    put_be16(header + 12, type);
    netif_transmit(netif, packet);
}
