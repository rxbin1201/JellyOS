/*
 * IPv4 (RFC 791) and ICMP (RFC 792). All calls need net_lock.
 */

#ifndef NET_IPV4_H
#define NET_IPV4_H

#include "net/net.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define IPV4_HEADER_SIZE 20
#define IPV4_DEFAULT_TTL 64

struct socket;

/*
 * Choose the interface, next hop and source address for a destination.
 * interface: index + 1 to force one, 0 for the routing table. UNREACHABLE if
 * there is no route.
 */
status_t ipv4_route(uint32_t destination, uint32_t interface, netif_t **netif, uint32_t *next_hop,
                    uint32_t *source);

/* Prepend the IPv4 header and send (takes the buffer; on errors it is freed). source 0: the route's. */
status_t ipv4_output(netbuf_t *packet, uint32_t source, uint32_t destination, uint8_t protocol, uint32_t interface);

void     ipv4_input(netif_t *netif, netbuf_t *packet);

/* Largest transport payload (header included) for a destination. */
uint32_t ipv4_mtu_for(uint32_t destination, uint32_t interface);

/* --- ICMP ------------------------------------------------------------------------ */

#define ICMP_ECHO_REPLY       0
#define ICMP_UNREACHABLE      3
#define ICMP_ECHO_REQUEST     8
#define ICMP_TIME_EXCEEDED    11

#define ICMP_CODE_NET_UNREACHABLE  0
#define ICMP_CODE_HOST_UNREACHABLE 1
#define ICMP_CODE_PROTO_UNREACHABLE 2
#define ICMP_CODE_PORT_UNREACHABLE 3

void     icmp_input(netif_t *netif, netbuf_t *packet, uint32_t source, uint32_t destination);
/* Report an undeliverable received packet (its IPv4 header must still be in front of packet->data). */
void     icmp_send_unreachable(netbuf_t *packet, uint8_t code);
/* Send an echo request from a ping socket: message = ICMP header + payload; the identifier is set. */
status_t icmp_send_echo(struct socket *socket, const void *message, size_t length, uint32_t destination);

#endif
