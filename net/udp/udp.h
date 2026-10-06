/*
 * UDP (RFC 768). All calls need net_lock.
 */

#ifndef NET_UDP_H
#define NET_UDP_H

#include "net/net.h"

#define UDP_HEADER_SIZE 8

struct socket;

void     udp_input(netif_t *netif, netbuf_t *packet, uint32_t source, uint32_t destination);
status_t udp_output(struct socket *socket, const void *data, size_t length, uint32_t destination, uint16_t port);
/* ICMP reported that a datagram we sent could not be delivered. */
void     udp_error(uint32_t local_address, uint16_t local_port, uint32_t remote_address, uint16_t remote_port,
                   status_t error);

#endif
