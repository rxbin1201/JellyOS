/*
 * IPv4 (README section 32; RFC 791, RFC 1122).
 *
 * Routing is derived from the interface configuration: loopback for 127/8
 * and our own addresses, directly for on-link destinations, otherwise via
 * the first configured gateway. Outgoing packets set Don't Fragment and are
 * never fragmented; the transports keep within the MTU. Incoming fragments
 * and IP options are not supported (fragments are dropped, options skipped).
 *
 * An interface without an address accepts every packet addressed to it, so
 * a DHCP client can receive its offer before it has an address.
 */

#include "net/ipv4/ipv4.h"
#include "net/arp/arp.h"
#include "net/tcp/tcp.h"
#include "net/udp/udp.h"

#include "core/string.h"

static uint16_t next_id = 1;

static bool on_link(const netif_t *netif, uint32_t destination)
{
    return netif->address && netif->netmask &&
           (destination & netif->netmask) == (netif->address & netif->netmask);
}

static bool is_local_broadcast(const netif_t *netif, uint32_t destination)
{
    return destination == IPV4_BROADCAST ||
           (netif->address && netif->netmask && on_link(netif, destination) &&
            (destination | netif->netmask) == IPV4_BROADCAST);
}

status_t ipv4_route(uint32_t destination, uint32_t interface, netif_t **out_netif, uint32_t *next_hop,
                    uint32_t *source)
{
    netif_t *netif = NULL;
    uint32_t hop = destination;

    if (interface) {
        netif = netif_get(interface - 1);
        if (!netif)
            return STATUS_UNREACHABLE;
        if (!netif->loopback && !is_local_broadcast(netif, destination) && !on_link(netif, destination)) {
            if (!netif->gateway)
                return STATUS_UNREACHABLE;
            hop = netif->gateway;
        }
    } else if ((destination >> 24) == 127 || netif_by_address(destination)) {
        netif = netif_get(0); /* loopback */
    } else if (destination == IPV4_BROADCAST) {
        /* Limited broadcast: the first Ethernet interface (configured ones first). */
        for (uint32_t i = 1; i < netif_count(); i++) {
            netif_t *n = netif_get(i);
            if (!netif || (!netif->address && n->address))
                netif = n;
        }
    } else {
        for (uint32_t i = 1; i < netif_count() && !netif; i++) {
            if (on_link(netif_get(i), destination))
                netif = netif_get(i);
        }
        for (uint32_t i = 1; i < netif_count() && !netif; i++) {
            if (netif_get(i)->address && netif_get(i)->gateway) {
                netif = netif_get(i);
                hop = netif->gateway;
            }
        }
    }
    if (!netif)
        return STATUS_UNREACHABLE;

    *out_netif = netif;
    if (next_hop)
        *next_hop = hop;
    if (source) {
        /* Loopback to one of our addresses keeps that address as the source. */
        if (netif->loopback && (destination >> 24) != 127)
            *source = destination;
        else
            *source = netif->address;
    }
    return STATUS_SUCCESS;
}

uint32_t ipv4_mtu_for(uint32_t destination, uint32_t interface)
{
    netif_t *netif;
    if (STATUS_IS_ERROR(ipv4_route(destination, interface, &netif, NULL, NULL)))
        return NET_MTU;
    return netif->mtu;
}

status_t ipv4_output(netbuf_t *packet, uint32_t source, uint32_t destination, uint8_t protocol, uint32_t interface)
{
    netif_t *netif;
    uint32_t next_hop, route_source;

    status_t status = ipv4_route(destination, interface, &netif, &next_hop, &route_source);
    if (!STATUS_IS_ERROR(status) && packet->length + IPV4_HEADER_SIZE > netif->mtu)
        status = STATUS_LIMIT_EXCEEDED;
    if (STATUS_IS_ERROR(status)) {
        netbuf_free(packet);
        return status;
    }
    if (source == IPV4_ANY)
        source = route_source;

    uint8_t *h = netbuf_push(packet, IPV4_HEADER_SIZE);
    h[0] = 0x45;
    h[1] = 0;
    put_be16(h + 2, (uint16_t)packet->length);
    put_be16(h + 4, next_id++);
    put_be16(h + 6, destination == IPV4_BROADCAST ? 0 : 0x4000); /* Don't Fragment */
    h[8] = IPV4_DEFAULT_TTL;
    h[9] = protocol;
    put_be16(h + 10, 0);
    put_be32(h + 12, source);
    put_be32(h + 16, destination);
    put_be16(h + 10, checksum_finish(checksum_add(0, h, IPV4_HEADER_SIZE)));

    if (netif->loopback)
        netif_transmit(netif, packet);
    else
        arp_output(netif, next_hop, packet);
    return STATUS_SUCCESS;
}

static bool accepts(const netif_t *netif, uint32_t destination)
{
    if (netif->loopback)
        return (destination >> 24) == 127 || netif_by_address(destination);
    if (!netif->address)
        return true; /* unconfigured (DHCP) */
    return destination == netif->address || is_local_broadcast(netif, destination);
}

void ipv4_input(netif_t *netif, netbuf_t *packet)
{
    const uint8_t *h = packet->data;

    if (packet->length < IPV4_HEADER_SIZE || (h[0] >> 4) != 4)
        goto drop;
    size_t header_length = (size_t)(h[0] & 0xF) * 4;
    size_t total = get_be16(h + 2);
    if (header_length < IPV4_HEADER_SIZE || total < header_length || total > packet->length ||
        checksum_finish(checksum_add(0, h, header_length)) != 0)
        goto drop;
    if (get_be16(h + 6) & 0x3FFF) /* More Fragments or an offset */
        goto drop;

    uint32_t source = get_be32(h + 12);
    uint32_t destination = get_be32(h + 16);
    if (!accepts(netif, destination))
        goto drop;

    netbuf_trim(packet, total);
    packet->network_header = packet->data;
    netbuf_pull(packet, header_length);

    switch (h[9]) {
    case IP_PROTO_ICMP:
        icmp_input(netif, packet, source, destination);
        return;
    case IP_PROTO_UDP:
        udp_input(netif, packet, source, destination);
        return;
    case IP_PROTO_TCP:
        tcp_input(netif, packet, source, destination);
        return;
    default:
        if (destination == netif->address)
            icmp_send_unreachable(packet, ICMP_CODE_PROTO_UNREACHABLE);
        netbuf_free(packet);
        return;
    }

drop:
    netif->rx_dropped++;
    netbuf_free(packet);
}
