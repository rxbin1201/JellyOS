/*
 * ARP (RFC 826): IPv4 to Ethernet address resolution. All calls need net_lock.
 */

#ifndef NET_ARP_H
#define NET_ARP_H

#include "net/net.h"

/* Send an IPv4 packet to next_hop on netif, resolving its MAC first if needed (takes the buffer). */
void arp_output(netif_t *netif, uint32_t next_hop, netbuf_t *packet);
void arp_input(netif_t *netif, netbuf_t *packet);
void arp_timer(uint64_t now);
/* Forget all entries of an interface (address change). */
void arp_flush(netif_t *netif);
/* Gratuitous ARP for a newly configured address. */
void arp_announce(netif_t *netif);
/* Look up a resolved entry (tests, diagnostics). */
bool arp_lookup(netif_t *netif, uint32_t address, uint8_t mac[6]);

#endif
