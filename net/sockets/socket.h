/*
 * Sockets (README section 32): the interface between the protocols and
 * applications.
 *
 * A socket is a kernel object (OBJECT_SOCKET) behind a handle. It is
 * waitable: signaled while it is readable (data, a datagram, a pending
 * connection, end of stream or an error).
 *
 * The socket_* functions are used by the system calls, by kernel users
 * (DNS resolver, tests) and take kernel buffers. They take net_lock
 * themselves and may block (with the socket's timeouts).
 */

#ifndef NET_SOCKETS_SOCKET_H
#define NET_SOCKETS_SOCKET_H

#include "core/object.h"
#include "net/net.h"

#include <jelly/syscall.h>

struct tcp_pcb;

typedef struct {
    uint32_t address; /* host order */
    uint16_t port;
} net_endpoint_t;

typedef struct socket {
    object_t        object;
    uint32_t        type;          /* JELLY_SOCK_STREAM / DGRAM */
    uint32_t        protocol;      /* JELLY_IPPROTO_TCP / UDP / ICMP */
    list_node_t     node;          /* UDP and ICMP: in the protocol's socket list */

    net_endpoint_t  local;
    net_endpoint_t  remote;
    bool            bound;
    bool            connected;     /* datagram: default destination set */

    uint32_t        interface;     /* index + 1, 0 = any */
    bool            broadcast;
    bool            reuse_address;
    bool            nonblocking;
    uint64_t        receive_timeout; /* ns, 0 = forever */
    uint64_t        send_timeout;

    bool            shut_read;
    bool            shut_write;
    status_t        error;         /* pending error reported once (datagram) or for good (stream) */

    /* datagram sockets */
    list_t          datagrams;
    uint32_t        datagram_count;
    size_t          queued_bytes;

    /* stream sockets */
    struct tcp_pcb *tcp;
} socket_t;

typedef struct {
    list_node_t    node;
    net_endpoint_t from;
    size_t         length;
    uint8_t        data[];
} datagram_t;

#define SOCKET_DATAGRAM_QUEUE_MAX (256 * 1024)

status_t socket_create(uint32_t type, uint32_t protocol, socket_t **socket);
status_t socket_bind(socket_t *socket, const net_endpoint_t *local);
status_t socket_connect(socket_t *socket, const net_endpoint_t *remote);
status_t socket_listen(socket_t *socket, uint32_t backlog);
status_t socket_accept(socket_t *socket, socket_t **connection, net_endpoint_t *peer);
status_t socket_send(socket_t *socket, const void *data, size_t size, const net_endpoint_t *to, uint32_t flags,
                     size_t *done);
status_t socket_receive(socket_t *socket, void *buffer, size_t size, net_endpoint_t *from, uint32_t flags,
                        size_t *done);
status_t socket_shutdown(socket_t *socket, uint32_t how);
status_t socket_set_option(socket_t *socket, uint32_t option, uint64_t value);
void     socket_info(socket_t *socket, jelly_socket_info_t *info);

/* --- For the protocols (net_lock held) ---------------------------------------- */

/* Queue a datagram (UDP payload or ICMP message); drops it when the queue is full. */
void     socket_deliver(socket_t *socket, const net_endpoint_t *from, const void *data, size_t length);
/* Wake waiters: readability or connection state changed. */
void     socket_notify(socket_t *socket);
/* Create the socket for a connection accepted by a listener (pcb already set up). */
socket_t *socket_from_tcp(struct tcp_pcb *pcb);

/* Ports in use by datagram sockets of a protocol (UDP, ICMP identifiers). */
bool     socket_datagram_port_in_use(uint32_t protocol, uint16_t port);
/* Find the datagram socket for an incoming packet (exact remote match first). */
socket_t *socket_datagram_lookup(uint32_t protocol, uint32_t local_address, uint16_t local_port,
                                 uint32_t remote_address, uint16_t remote_port, netif_t *netif);

#endif
