/*
 * ICMP (README section 32; RFC 792).
 *
 * Echo requests are answered. Echo replies go to the ping socket whose
 * identifier (its local "port") matches; the socket receives the whole ICMP
 * message, like Linux ping sockets. Destination unreachable messages are
 * passed to the transport of the packet that caused them. Undeliverable UDP
 * packets and unknown protocols are answered with destination unreachable.
 */

#include "net/ipv4/ipv4.h"
#include "net/sockets/socket.h"
#include "net/tcp/tcp.h"
#include "net/udp/udp.h"

#include "core/string.h"

#define ICMP_HEADER_SIZE 8

static void send_icmp(uint32_t destination, uint8_t type, uint8_t code, uint32_t rest, const void *body,
                      size_t body_length, uint32_t source)
{
    netbuf_t *packet = netbuf_alloc(ICMP_HEADER_SIZE + body_length);
    if (!packet)
        return;
    uint8_t *m = packet->data;
    m[0] = type;
    m[1] = code;
    put_be16(m + 2, 0);
    put_be32(m + 4, rest);
    memcpy(m + ICMP_HEADER_SIZE, body, body_length);
    put_be16(m + 2, checksum_finish(checksum_add(0, m, packet->length)));
    ipv4_output(packet, source, destination, IP_PROTO_ICMP, 0);
}

static void handle_unreachable(const uint8_t *message, size_t length)
{
    /* The original IPv4 header plus at least 8 bytes of its payload follow the ICMP header. */
    if (length < ICMP_HEADER_SIZE + IPV4_HEADER_SIZE + 8)
        return;
    const uint8_t *original = message + ICMP_HEADER_SIZE;
    size_t header_length = (size_t)(original[0] & 0xF) * 4;
    if (header_length < IPV4_HEADER_SIZE || length < ICMP_HEADER_SIZE + header_length + 8)
        return;
    const uint8_t *transport = original + header_length;
    uint32_t source = get_be32(original + 12);
    uint32_t destination = get_be32(original + 16);
    uint16_t source_port = get_be16(transport);
    uint16_t destination_port = get_be16(transport + 2);
    status_t error = message[1] == ICMP_CODE_PORT_UNREACHABLE || message[1] == ICMP_CODE_PROTO_UNREACHABLE
                         ? STATUS_CONNECTION_REFUSED
                         : STATUS_UNREACHABLE;

    if (original[9] == IP_PROTO_UDP)
        udp_error(source, source_port, destination, destination_port, error);
    else if (original[9] == IP_PROTO_TCP)
        tcp_error(source, source_port, destination, destination_port, error);
}

void icmp_input(netif_t *netif, netbuf_t *packet, uint32_t source, uint32_t destination)
{
    uint8_t *m = packet->data;
    size_t length = packet->length;

    if (length < ICMP_HEADER_SIZE || checksum_finish(checksum_add(0, m, length)) != 0) {
        netif->rx_dropped++;
        netbuf_free(packet);
        return;
    }

    switch (m[0]) {
    case ICMP_ECHO_REQUEST:
        if (destination != IPV4_BROADCAST && !(netif->netmask && (destination | netif->netmask) == IPV4_BROADCAST))
            send_icmp(source, ICMP_ECHO_REPLY, 0, get_be32(m + 4), m + ICMP_HEADER_SIZE, length - ICMP_HEADER_SIZE,
                      destination);
        break;
    case ICMP_ECHO_REPLY: {
        socket_t *s = socket_datagram_lookup(JELLY_IPPROTO_ICMP, destination, get_be16(m + 4), source, 0, netif);
        net_endpoint_t from = { source, 0 };
        if (s)
            socket_deliver(s, &from, m, length);
        break;
    }
    case ICMP_UNREACHABLE:
        handle_unreachable(m, length);
        break;
    default:
        break;
    }
    netbuf_free(packet);
}

void icmp_send_unreachable(netbuf_t *packet, uint8_t code)
{
    const uint8_t *h = packet->network_header;
    if (!h)
        return;
    uint32_t source = get_be32(h + 12);
    uint32_t destination = get_be32(h + 16);
    /* Never about broadcasts or from unspecified sources (RFC 1122 3.2.2). */
    if (destination == IPV4_BROADCAST || source == 0 || source == IPV4_BROADCAST)
        return;
    size_t header_length = (size_t)(h[0] & 0xF) * 4;
    size_t quoted = header_length + (packet->length < 8 ? packet->length : 8);
    send_icmp(source, ICMP_UNREACHABLE, code, 0, h, quoted, destination);
}

status_t icmp_send_echo(socket_t *socket, const void *message, size_t length, uint32_t destination)
{
    if (length < ICMP_HEADER_SIZE || ((const uint8_t *)message)[0] != ICMP_ECHO_REQUEST)
        return STATUS_INVALID_ARGUMENT;
    if (length > 65000)
        return STATUS_LIMIT_EXCEEDED;
    netbuf_t *packet = netbuf_alloc(length);
    if (!packet)
        return STATUS_OUT_OF_MEMORY;
    uint8_t *m = packet->data;
    memcpy(m, message, length);
    m[1] = 0;
    put_be16(m + 4, socket->local.port); /* identifier */
    put_be16(m + 2, 0);
    put_be16(m + 2, checksum_finish(checksum_add(0, m, length)));
    return ipv4_output(packet, socket->local.address, destination, IP_PROTO_ICMP, socket->interface);
}
