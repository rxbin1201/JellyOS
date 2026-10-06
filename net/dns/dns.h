/*
 * DNS resolver (RFC 1035): host names to IPv4 addresses.
 *
 * Called without net_lock (it sends and waits through a UDP socket).
 */

#ifndef NET_DNS_H
#define NET_DNS_H

#include <jelly/status.h>
#include <stddef.h>
#include <stdint.h>

/* Dotted quads and "localhost" are answered directly, everything else by the name server. */
status_t dns_resolve(const char *name, size_t length, uint32_t *address);

/* Use this server instead of the interfaces' (tests); address 0 restores the default. */
void     dns_set_server_override(uint32_t address, uint16_t port);
void     dns_cache_clear(void);

/* Build a query for `name` (type A) into buffer; returns its length or 0. Exposed for tests. */
size_t   dns_build_query(uint16_t id, const char *name, size_t length, uint8_t *buffer, size_t size);

#endif
