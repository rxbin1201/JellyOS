/*
 * UDP (README section 32; RFC 768).
 *
 * Datagrams are delivered to the socket bound to the destination port
 * (a connected socket only accepts its peer). Without a socket the sender
 * gets ICMP port unreachable. Checksums are always generated and checked
 * when present.
 */

#include "net/udp/udp.h"
#include "net/ipv4/ipv4.h"
#include "net/sockets/socket.h"

#include "core/string.h"

void udp_input(netif_t *netif, netbuf_t *packet, uint32_t source, uint32_t destination)
{
    const uint8_t *h = packet->data;

    if (packet->length < UDP_HEADER_SIZE)
        goto drop;
    uint16_t length = get_be16(h + 4);
    if (length < UDP_HEADER_SIZE || length > packet->length)
        goto drop;
    netbuf_trim(packet, length);
    if (get_be16(h + 6) != 0) {
        uint32_t sum = checksum_pseudo(source, destination, IP_PROTO_UDP, length);
        if (checksum_finish(checksum_add(sum, h, length)) != 0)
            goto drop;
    }

    uint16_t source_port = get_be16(h);
    uint16_t destination_port = get_be16(h + 2);
    socket_t *s = socket_datagram_lookup(JELLY_IPPROTO_UDP, destination, destination_port, source, source_port,
                                         netif);
    if (s) {
        net_endpoint_t from = { source, source_port };
        socket_deliver(s, &from, h + UDP_HEADER_SIZE, length - UDP_HEADER_SIZE);
    } else if (netif_by_address(destination)) {
        icmp_send_unreachable(packet, ICMP_CODE_PORT_UNREACHABLE);
    }
    netbuf_free(packet);
    return;

drop:
    netif->rx_dropped++;
    netbuf_free(packet);
}

status_t udp_output(socket_t *socket, const void *data, size_t length, uint32_t destination, uint16_t port)
{
    netif_t *netif;
    uint32_t source;

    if (length > 65535 - UDP_HEADER_SIZE - IPV4_HEADER_SIZE)
        return STATUS_LIMIT_EXCEEDED;
    status_t status = ipv4_route(destination, socket->interface, &netif, NULL, &source);
    if (STATUS_IS_ERROR(status))
        return status;
    if (socket->local.address)
        source = socket->local.address;

    netbuf_t *packet = netbuf_alloc(UDP_HEADER_SIZE + length);
    if (!packet)
        return STATUS_OUT_OF_MEMORY;
    uint8_t *h = packet->data;
    put_be16(h, socket->local.port);
    put_be16(h + 2, port);
    put_be16(h + 4, (uint16_t)(UDP_HEADER_SIZE + length));
    put_be16(h + 6, 0);
    memcpy(h + UDP_HEADER_SIZE, data, length);
    uint16_t sum = checksum_finish(checksum_add(
        checksum_pseudo(source, destination, IP_PROTO_UDP, (uint16_t)packet->length), h, packet->length));
    put_be16(h + 6, sum ? sum : 0xFFFF);
    return ipv4_output(packet, source, destination, IP_PROTO_UDP, socket->interface);
}

void udp_error(uint32_t local_address, uint16_t local_port, uint32_t remote_address, uint16_t remote_port,
               status_t error)
{
    socket_t *s = socket_datagram_lookup(JELLY_IPPROTO_UDP, local_address, local_port, remote_address, remote_port,
                                         NULL);
    /* Only connected sockets learn about errors (as in BSD). */
    if (s && s->connected && s->remote.address == remote_address && s->remote.port == remote_port) {
        s->error = error;
        socket_notify(s);
    }
}
