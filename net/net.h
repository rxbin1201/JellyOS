/*
 * JellyOS network stack (README section 32): shared definitions.
 *
 *     NIC driver (drivers/network) ─▶ network interface (net/link)
 *         ─▶ Ethernet (net/link) ─▶ ARP (net/arp) ─▶ IPv4 + ICMP (net/ipv4)
 *         ─▶ UDP (net/udp), TCP (net/tcp) ─▶ sockets (net/sockets) ─▶ applications
 *
 * Concurrency: all protocol state is protected by one mutex (net_lock).
 * Drivers never run protocol code in interrupt context: an interrupt only
 * wakes the network thread, which pulls received frames from the drivers,
 * runs them up the stack and handles the protocol timers. Sending happens in
 * the context of the caller (with net_lock held).
 *
 * IPv4 addresses are kept in host byte order inside the stack and converted
 * at the packet and system call boundaries.
 */

#ifndef NET_NET_H
#define NET_NET_H

#include "core/list.h"
#include "scheduler/mutex.h"

#include <jelly/status.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Byte order ------------------------------------------------------------------ */

static inline uint16_t net_swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t net_swap32(uint32_t v) { return __builtin_bswap32(v); }

#define htons(v) net_swap16(v)
#define ntohs(v) net_swap16(v)
#define htonl(v) net_swap32(v)
#define ntohl(v) net_swap32(v)

/* Unaligned big-endian access to packet fields. */
static inline uint16_t get_be16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)(b[0] << 8 | b[1]);
}

static inline uint32_t get_be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static inline void put_be16(void *p, uint16_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static inline void put_be32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

#define IPV4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define IPV4_ANY         0u
#define IPV4_BROADCAST   0xFFFFFFFFu
#define IPV4_LOOPBACK    IPV4(127, 0, 0, 1)

/* "a.b.c.d" into buffer (at least 16 bytes); returns buffer. */
const char *ipv4_format(uint32_t address, char *buffer);
/* Parse "a.b.c.d"; returns false for anything else. */
bool        ipv4_parse(const char *text, size_t length, uint32_t *address);

/* --- Internet checksum (RFC 1071) ---------------------------------------------- */

/* Add data to a running one's complement sum (start with 0). */
uint32_t checksum_add(uint32_t sum, const void *data, size_t length);
/* Fold and complement; 0 means valid when the sum covered the checksum field. */
uint16_t checksum_finish(uint32_t sum);
/* Sum of the IPv4 pseudo header for TCP and UDP. */
uint32_t checksum_pseudo(uint32_t source, uint32_t destination, uint8_t protocol, uint16_t length);

/* --- Packet buffers ------------------------------------------------------------- */

#define NETBUF_HEADROOM 128   /* Ethernet + IPv4 + TCP with options */
#define NET_MTU         1500
#define ETH_HEADER_SIZE 14
#define ETH_FRAME_MAX   (ETH_HEADER_SIZE + NET_MTU)

struct netif;

typedef struct netbuf {
    list_node_t   node;
    struct netif *netif;    /* receiving interface */
    uint8_t      *network_header; /* received packets: the IPv4 header (for ICMP errors) */
    uint8_t      *data;     /* current start (moves with push/pull) */
    size_t        length;
    size_t        capacity; /* of storage */
    uint8_t       storage[];
} netbuf_t;

/* A buffer for `length` bytes of payload behind NETBUF_HEADROOM bytes of header space. */
netbuf_t *netbuf_alloc(size_t length);
void      netbuf_free(netbuf_t *buffer);
/* Prepend `size` bytes (header); returns the new start. */
void     *netbuf_push(netbuf_t *buffer, size_t size);
/* Remove `size` bytes from the front; false if the buffer is shorter. */
bool      netbuf_pull(netbuf_t *buffer, size_t size);
/* Cut the buffer to `length` bytes (Ethernet padding, IP total length). */
void      netbuf_trim(netbuf_t *buffer, size_t length);

/* --- Network interfaces (net/link) -------------------------------------------- */

#define NETIF_MAX 8

typedef struct netif_ops {
    /* Send one frame (Ethernet header included; loopback: an IPv4 packet). Takes the buffer. */
    void      (*transmit)(struct netif *netif, netbuf_t *frame);
    /* Return the next received frame or NULL. Called by the network thread only. */
    netbuf_t *(*receive)(struct netif *netif);
    /* Carrier state. */
    bool      (*link_up)(struct netif *netif);
} netif_ops_t;

typedef struct netif {
    uint32_t           index;
    char               name[16];
    uint8_t            mac[6];
    uint16_t           mtu;
    bool               loopback;
    const netif_ops_t *ops;
    void              *driver_data;

    /* IPv4 configuration (host order, 0 = none) */
    uint32_t           address;
    uint32_t           netmask;
    uint32_t           gateway;
    uint32_t           dns;

    uint64_t           rx_packets, rx_bytes, rx_dropped;
    uint64_t           tx_packets, tx_bytes, tx_dropped;
} netif_t;

/* Make an interface known (index and the name "eth<n>" are assigned unless set). */
status_t netif_register(netif_t *netif);
netif_t *netif_get(uint32_t index);
uint32_t netif_count(void);
/* Drivers call this (also from interrupt handlers) when frames are waiting. */
void     netif_receive_ready(netif_t *netif);
/* Send a frame and count it. net_lock held. */
void     netif_transmit(netif_t *netif, netbuf_t *frame);
/* Set or clear (address 0) the IPv4 configuration. net_lock held. */
void     netif_configure(netif_t *netif, uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns);
/* The interface that owns this address, or NULL. */
netif_t *netif_by_address(uint32_t address);

/* --- Ethernet (net/link) -------------------------------------------------------- */

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806

extern const uint8_t eth_broadcast[6];

void ethernet_input(netif_t *netif, netbuf_t *frame);
/* Prepend the Ethernet header and transmit. Takes the buffer. */
void ethernet_output(netif_t *netif, const uint8_t destination[6], uint16_t type, netbuf_t *packet);

/* Loopback interface "lo" (127.0.0.1/8). */
status_t loopback_init(void);

/* --- Stack core (net/link/net.c) ------------------------------------------------ */

extern mutex_t net_lock;

status_t net_init(void);
/* Ask the network thread to run the timers within `delay_ns` (net_lock held). */
void     net_timer_request(uint64_t delay_ns);
/* Ephemeral ports 49152-65535 */
uint16_t net_ephemeral_port(bool (*in_use)(uint16_t port));

#endif
