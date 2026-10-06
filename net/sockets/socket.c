/*
 * Sockets (README section 32).
 *
 * Stream sockets wrap a TCP connection block; datagram sockets (UDP and
 * ICMP echo) keep a queue of received datagrams and sit in one list for
 * demultiplexing. Blocking calls sleep on the socket object's wait queue
 * with net_lock released; the protocols wake them through socket_notify(),
 * which also wakes SYS_OBJECT_WAIT callers.
 */

#include "net/sockets/socket.h"
#include "net/ipv4/ipv4.h"
#include "net/tcp/tcp.h"
#include "net/udp/udp.h"

#include "core/arch.h"
#include "core/string.h"
#include "memory/heap.h"

static list_t datagram_sockets = { { &datagram_sockets.head, &datagram_sockets.head } };

/* --- Object ------------------------------------------------------------------------ */

static bool readable(socket_t *s)
{
    if (s->error)
        return true;
    if (s->type == JELLY_SOCK_DGRAM)
        return s->datagram_count > 0 || s->shut_read;
    tcp_pcb_t *pcb = s->tcp;
    if (!pcb)
        return true;
    if (pcb->state == TCP_LISTEN)
        return !list_empty(&pcb->accept_queue);
    return pcb->receive_length > 0 || pcb->fin_received || s->shut_read || pcb->error ||
           (pcb->state == TCP_CLOSED && pcb->remote_port);
}

static bool socket_signaled(object_t *object)
{
    return readable(container_of(object, socket_t, object));
}

static void free_datagrams(socket_t *s)
{
    while (!list_empty(&s->datagrams))
        kfree(container_of(list_pop_front(&s->datagrams), datagram_t, node));
    s->datagram_count = 0;
    s->queued_bytes = 0;
}

static void socket_destroy(object_t *object)
{
    socket_t *s = container_of(object, socket_t, object);

    mutex_lock(&net_lock);
    if (s->tcp)
        tcp_close(s->tcp);
    if (list_linked(&s->node))
        list_remove(&s->node);
    free_datagrams(s);
    mutex_unlock(&net_lock);
    kfree(s);
}

static const object_ops_t socket_ops = {
    .destroy = socket_destroy,
    .signaled = socket_signaled,
};

static socket_t *allocate(uint32_t type, uint32_t protocol)
{
    socket_t *s = kcalloc(1, sizeof(*s));
    if (!s)
        return NULL;
    object_init(&s->object, OBJECT_SOCKET, &socket_ops);
    s->type = type;
    s->protocol = protocol;
    list_init(&s->datagrams);
    return s;
}

status_t socket_create(uint32_t type, uint32_t protocol, socket_t **out)
{
    if (type == JELLY_SOCK_STREAM && (protocol == 0 || protocol == JELLY_IPPROTO_TCP))
        protocol = JELLY_IPPROTO_TCP;
    else if (type == JELLY_SOCK_DGRAM && (protocol == 0 || protocol == JELLY_IPPROTO_UDP))
        protocol = JELLY_IPPROTO_UDP;
    else if (!(type == JELLY_SOCK_DGRAM && protocol == JELLY_IPPROTO_ICMP))
        return STATUS_NOT_SUPPORTED;

    socket_t *s = allocate(type, protocol);
    if (!s)
        return STATUS_OUT_OF_MEMORY;
    if (type == JELLY_SOCK_STREAM) {
        mutex_lock(&net_lock);
        s->tcp = tcp_pcb_create(s);
        mutex_unlock(&net_lock);
        if (!s->tcp) {
            kfree(s);
            return STATUS_OUT_OF_MEMORY;
        }
    }
    *out = s;
    return STATUS_SUCCESS;
}

socket_t *socket_from_tcp(tcp_pcb_t *pcb)
{
    socket_t *s = allocate(JELLY_SOCK_STREAM, JELLY_IPPROTO_TCP);
    if (!s)
        return NULL;
    s->tcp = pcb;
    s->bound = s->connected = true;
    s->local = (net_endpoint_t){ pcb->local_address, pcb->local_port };
    s->remote = (net_endpoint_t){ pcb->remote_address, pcb->remote_port };
    s->interface = pcb->interface;
    pcb->socket = s;
    return s;
}

void socket_notify(socket_t *s)
{
    object_notify(&s->object);
}

/* Sleep until notified, a timeout or a kill. net_lock held; released while sleeping. */
static status_t wait_for(socket_t *s, uint64_t deadline)
{
    mutex_unlock(&net_lock);
    uint64_t flags = arch_interrupts_save();
    status_t status = wait_queue_block(&s->object.waiters, deadline);
    arch_interrupts_restore(flags);
    mutex_lock(&net_lock);
    return status;
}

static uint64_t deadline_for(uint64_t timeout)
{
    return timeout ? wait_deadline(timeout) : WAIT_FOREVER;
}

/* Report a pending datagram error once. */
static status_t take_error(socket_t *s)
{
    status_t error = s->error;
    s->error = STATUS_SUCCESS;
    return error;
}

/* --- Datagram demultiplexing -------------------------------------------------------- */

bool socket_datagram_port_in_use(uint32_t protocol, uint16_t port)
{
    list_for_each(node, &datagram_sockets) {
        socket_t *s = container_of(node, socket_t, node);
        if (s->protocol == protocol && s->local.port == port)
            return true;
    }
    return false;
}

static bool udp_port_in_use(uint16_t port)
{
    return socket_datagram_port_in_use(JELLY_IPPROTO_UDP, port);
}

static bool icmp_id_in_use(uint16_t port)
{
    return socket_datagram_port_in_use(JELLY_IPPROTO_ICMP, port);
}

socket_t *socket_datagram_lookup(uint32_t protocol, uint32_t local_address, uint16_t local_port,
                                 uint32_t remote_address, uint16_t remote_port, netif_t *netif)
{
    socket_t *best = NULL;
    int best_score = -1;

    list_for_each(node, &datagram_sockets) {
        socket_t *s = container_of(node, socket_t, node);
        if (s->protocol != protocol || s->local.port != local_port)
            continue;
        if (s->local.address && s->local.address != local_address)
            continue;
        if (netif && s->interface && s->interface - 1 != netif->index)
            continue;
        int score = s->local.address ? 1 : 0;
        if (s->connected) {
            if (s->remote.address != remote_address ||
                (protocol != JELLY_IPPROTO_ICMP && s->remote.port != remote_port))
                continue;
            score += 2;
        }
        if (score > best_score) {
            best = s;
            best_score = score;
        }
    }
    return best;
}

void socket_deliver(socket_t *s, const net_endpoint_t *from, const void *data, size_t length)
{
    if (s->shut_read || s->queued_bytes + length > SOCKET_DATAGRAM_QUEUE_MAX)
        return; /* dropped like a full network queue */
    datagram_t *d = kmalloc(sizeof(*d) + length);
    if (!d)
        return;
    d->from = *from;
    d->length = length;
    memcpy(d->data, data, length);
    list_push_back(&s->datagrams, &d->node);
    s->datagram_count++;
    s->queued_bytes += length;
    socket_notify(s);
}

/* --- Operations ------------------------------------------------------------------ */

static bool local_address_ok(uint32_t address)
{
    return address == IPV4_ANY || (address >> 24) == 127 || netif_by_address(address);
}

static status_t bind_datagram(socket_t *s, const net_endpoint_t *local)
{
    bool (*in_use)(uint16_t) = s->protocol == JELLY_IPPROTO_UDP ? udp_port_in_use : icmp_id_in_use;
    uint16_t port = local->port;

    if (port == 0) {
        port = net_ephemeral_port(in_use);
        if (port == 0)
            return STATUS_ADDRESS_IN_USE;
    } else if (in_use(port) && !s->reuse_address) {
        return STATUS_ADDRESS_IN_USE;
    }
    s->local = (net_endpoint_t){ local->address, port };
    s->bound = true;
    list_push_back(&datagram_sockets, &s->node);
    return STATUS_SUCCESS;
}

static status_t bind_locked(socket_t *s, const net_endpoint_t *local)
{
    if (s->bound)
        return STATUS_INVALID_ARGUMENT;
    if (!local_address_ok(local->address))
        return STATUS_UNREACHABLE;
    if (s->type == JELLY_SOCK_DGRAM)
        return bind_datagram(s, local);

    status_t status = tcp_bind(s->tcp, local->address, local->port, s->reuse_address);
    if (!STATUS_IS_ERROR(status)) {
        s->local = (net_endpoint_t){ s->tcp->local_address, s->tcp->local_port };
        s->bound = true;
    }
    return status;
}

status_t socket_bind(socket_t *s, const net_endpoint_t *local)
{
    mutex_lock(&net_lock);
    status_t status = bind_locked(s, local);
    mutex_unlock(&net_lock);
    return status;
}

static status_t ensure_bound(socket_t *s)
{
    net_endpoint_t any = { IPV4_ANY, 0 };
    return s->bound ? STATUS_SUCCESS : bind_locked(s, &any);
}

status_t socket_connect(socket_t *s, const net_endpoint_t *remote)
{
    status_t status;

    mutex_lock(&net_lock);
    if (s->type == JELLY_SOCK_DGRAM) {
        status = ensure_bound(s);
        if (!STATUS_IS_ERROR(status)) {
            s->remote = *remote;
            s->connected = remote->address != IPV4_ANY;
            s->error = STATUS_SUCCESS;
        }
        mutex_unlock(&net_lock);
        return status;
    }

    tcp_pcb_t *pcb = s->tcp;
    if (pcb->state == TCP_LISTEN) {
        status = STATUS_INVALID_ARGUMENT;
    } else if (pcb->state != TCP_CLOSED || s->connected) {
        status = STATUS_ALREADY_EXISTS;
    } else {
        pcb->interface = s->interface;
        status = tcp_connect(pcb, remote->address, remote->port);
    }
    if (STATUS_IS_ERROR(status)) {
        mutex_unlock(&net_lock);
        return status;
    }
    s->bound = s->connected = true;
    s->local = (net_endpoint_t){ pcb->local_address, pcb->local_port };
    s->remote = *remote;
    if (s->nonblocking) {
        mutex_unlock(&net_lock);
        return STATUS_WOULD_BLOCK; /* in progress; wait for writability via SYS_SOCKET_INFO */
    }

    uint64_t deadline = deadline_for(s->send_timeout);
    while (pcb->state == TCP_SYN_SENT || pcb->state == TCP_SYN_RECEIVED) {
        status = wait_for(s, deadline);
        if (STATUS_IS_ERROR(status))
            break;
    }
    if (!STATUS_IS_ERROR(status) && pcb->state != TCP_CLOSED) {
        status = STATUS_SUCCESS;
    } else {
        if (!STATUS_IS_ERROR(status))
            status = pcb->error ? pcb->error : STATUS_CONNECTION_REFUSED;
        /* Start over with a fresh connection block (the old one is closed in the background). */
        tcp_close(pcb);
        s->tcp = tcp_pcb_create(s);
        s->connected = s->bound = false;
        if (!s->tcp)
            s->error = STATUS_OUT_OF_MEMORY;
    }
    mutex_unlock(&net_lock);
    return status;
}

status_t socket_listen(socket_t *s, uint32_t backlog)
{
    if (s->type != JELLY_SOCK_STREAM)
        return STATUS_NOT_SUPPORTED;
    mutex_lock(&net_lock);
    status_t status = s->tcp ? tcp_listen(s->tcp, backlog) : STATUS_OUT_OF_MEMORY;
    if (!STATUS_IS_ERROR(status)) {
        s->bound = true;
        s->local = (net_endpoint_t){ s->tcp->local_address, s->tcp->local_port };
    }
    mutex_unlock(&net_lock);
    return status;
}

status_t socket_accept(socket_t *s, socket_t **connection, net_endpoint_t *peer)
{
    status_t status = STATUS_SUCCESS;

    if (s->type != JELLY_SOCK_STREAM)
        return STATUS_NOT_SUPPORTED;
    mutex_lock(&net_lock);
    uint64_t deadline = deadline_for(s->receive_timeout);
    for (;;) {
        if (!s->tcp || s->tcp->state != TCP_LISTEN) {
            status = STATUS_INVALID_ARGUMENT;
            break;
        }
        tcp_pcb_t *child = tcp_accept(s->tcp);
        if (child) {
            socket_t *c = socket_from_tcp(child);
            if (!c) {
                child->socket = NULL;
                tcp_close(child);
                status = STATUS_OUT_OF_MEMORY;
                break;
            }
            *connection = c;
            if (peer)
                *peer = c->remote;
            break;
        }
        if (s->nonblocking) {
            status = STATUS_WOULD_BLOCK;
            break;
        }
        status = wait_for(s, deadline);
        if (STATUS_IS_ERROR(status))
            break;
    }
    mutex_unlock(&net_lock);
    return status;
}

static status_t send_datagram(socket_t *s, const void *data, size_t size, const net_endpoint_t *to)
{
    net_endpoint_t destination;

    if (s->error)
        return take_error(s);
    if (s->shut_write)
        return STATUS_PEER_CLOSED;
    if (to)
        destination = *to;
    else if (s->connected)
        destination = s->remote;
    else
        return STATUS_NOT_CONNECTED;
    if (destination.address == IPV4_ANY)
        return STATUS_INVALID_ARGUMENT;
    if (destination.address == IPV4_BROADCAST && !s->broadcast)
        return STATUS_ACCESS_DENIED;

    status_t status = ensure_bound(s);
    if (STATUS_IS_ERROR(status))
        return status;
    if (s->protocol == JELLY_IPPROTO_ICMP)
        return icmp_send_echo(s, data, size, destination.address);
    if (destination.port == 0)
        return STATUS_INVALID_ARGUMENT;
    return udp_output(s, data, size, destination.address, destination.port);
}

static status_t stream_send_error(socket_t *s)
{
    tcp_pcb_t *pcb = s->tcp;
    if (!pcb)
        return STATUS_OUT_OF_MEMORY;
    if (pcb->error)
        return pcb->error;
    if (s->shut_write || pcb->fin_queued || (pcb->state == TCP_CLOSED && pcb->remote_port))
        return STATUS_PEER_CLOSED;
    return STATUS_NOT_CONNECTED;
}

status_t socket_send(socket_t *s, const void *data, size_t size, const net_endpoint_t *to, uint32_t flags,
                     size_t *done)
{
    status_t status = STATUS_SUCCESS;
    *done = 0;

    mutex_lock(&net_lock);
    if (s->type == JELLY_SOCK_DGRAM) {
        status = send_datagram(s, data, size, to);
        if (!STATUS_IS_ERROR(status))
            *done = size;
        mutex_unlock(&net_lock);
        return status;
    }

    const uint8_t *bytes = data;
    uint64_t deadline = deadline_for(s->send_timeout);
    while (*done < size) {
        if (!s->tcp || !tcp_can_send(s->tcp) || s->shut_write) {
            status = stream_send_error(s);
            break;
        }
        size_t written = tcp_write(s->tcp, bytes + *done, size - *done);
        *done += written;
        if (*done == size)
            break;
        if (written)
            continue;
        if (s->nonblocking || (flags & JELLY_MSG_DONTWAIT)) {
            status = STATUS_WOULD_BLOCK;
            break;
        }
        status = wait_for(s, deadline);
        if (STATUS_IS_ERROR(status))
            break;
    }
    mutex_unlock(&net_lock);
    /* Partial success counts as success; the caller sees how much went out. */
    return *done ? STATUS_SUCCESS : status;
}

static status_t receive_datagram(socket_t *s, void *buffer, size_t size, net_endpoint_t *from, uint32_t flags,
                                 size_t *done)
{
    uint64_t deadline = deadline_for(s->receive_timeout);
    for (;;) {
        if (!list_empty(&s->datagrams)) {
            datagram_t *d = container_of(list_front(&s->datagrams), datagram_t, node);
            size_t n = d->length < size ? d->length : size; /* the rest of a long datagram is lost */
            memcpy(buffer, d->data, n);
            if (from)
                *from = d->from;
            *done = n;
            if (!(flags & JELLY_MSG_PEEK)) {
                list_remove(&d->node);
                s->datagram_count--;
                s->queued_bytes -= d->length;
                kfree(d);
            }
            return STATUS_SUCCESS;
        }
        if (s->error)
            return take_error(s);
        if (s->shut_read)
            return STATUS_SUCCESS;
        if (s->nonblocking || (flags & JELLY_MSG_DONTWAIT))
            return STATUS_WOULD_BLOCK;
        status_t status = wait_for(s, deadline);
        if (STATUS_IS_ERROR(status))
            return status;
    }
}

static status_t receive_stream(socket_t *s, void *buffer, size_t size, net_endpoint_t *from, uint32_t flags,
                               size_t *done)
{
    uint64_t deadline = deadline_for(s->receive_timeout);
    for (;;) {
        tcp_pcb_t *pcb = s->tcp;
        if (!pcb)
            return STATUS_OUT_OF_MEMORY;
        if (pcb->state == TCP_LISTEN)
            return STATUS_INVALID_ARGUMENT;
        if (pcb->receive_length) {
            *done = tcp_read(pcb, buffer, size, flags & JELLY_MSG_PEEK);
            if (from)
                *from = s->remote;
            return STATUS_SUCCESS;
        }
        if (pcb->fin_received || s->shut_read)
            return STATUS_SUCCESS; /* end of stream */
        if (pcb->error)
            return pcb->error;
        if (pcb->state == TCP_CLOSED)
            return s->connected ? STATUS_SUCCESS : STATUS_NOT_CONNECTED;
        if (s->nonblocking || (flags & JELLY_MSG_DONTWAIT))
            return STATUS_WOULD_BLOCK;
        status_t status = wait_for(s, deadline);
        if (STATUS_IS_ERROR(status))
            return status;
    }
}

status_t socket_receive(socket_t *s, void *buffer, size_t size, net_endpoint_t *from, uint32_t flags, size_t *done)
{
    *done = 0;
    mutex_lock(&net_lock);
    status_t status = s->type == JELLY_SOCK_DGRAM ? receive_datagram(s, buffer, size, from, flags, done)
                                                   : receive_stream(s, buffer, size, from, flags, done);
    mutex_unlock(&net_lock);
    return status;
}

status_t socket_shutdown(socket_t *s, uint32_t how)
{
    if (how > JELLY_SHUT_BOTH)
        return STATUS_INVALID_ARGUMENT;
    mutex_lock(&net_lock);
    status_t status = STATUS_SUCCESS;
    if (s->type == JELLY_SOCK_STREAM && (!s->tcp || !s->connected))
        status = STATUS_NOT_CONNECTED;
    if (!STATUS_IS_ERROR(status)) {
        if (how != JELLY_SHUT_WRITE) {
            s->shut_read = true;
            free_datagrams(s);
        }
        if (how != JELLY_SHUT_READ) {
            s->shut_write = true;
            if (s->tcp)
                tcp_shutdown_write(s->tcp);
        }
        socket_notify(s);
    }
    mutex_unlock(&net_lock);
    return status;
}

status_t socket_set_option(socket_t *s, uint32_t option, uint64_t value)
{
    status_t status = STATUS_SUCCESS;
    mutex_lock(&net_lock);
    switch (option) {
    case JELLY_SO_RECEIVE_TIMEOUT:
        s->receive_timeout = value;
        break;
    case JELLY_SO_SEND_TIMEOUT:
        s->send_timeout = value;
        break;
    case JELLY_SO_NONBLOCKING:
        s->nonblocking = value != 0;
        break;
    case JELLY_SO_BROADCAST:
        s->broadcast = value != 0;
        break;
    case JELLY_SO_REUSE_ADDRESS:
        s->reuse_address = value != 0;
        break;
    case JELLY_SO_INTERFACE:
        if (value > netif_count()) {
            status = STATUS_NOT_FOUND;
            break;
        }
        s->interface = (uint32_t)value;
        if (s->tcp)
            s->tcp->interface = s->interface;
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    mutex_unlock(&net_lock);
    return status;
}

static void to_sockaddr(const net_endpoint_t *e, jelly_sockaddr_in_t *out)
{
    memset(out, 0, sizeof(*out));
    out->family = JELLY_AF_INET;
    out->port = htons(e->port);
    out->address = htonl(e->address);
}

void socket_info(socket_t *s, jelly_socket_info_t *info)
{
    memset(info, 0, sizeof(*info));
    mutex_lock(&net_lock);
    info->type = s->type;
    info->protocol = s->protocol;
    to_sockaddr(&s->local, &info->local);
    to_sockaddr(&s->remote, &info->remote);

    if (s->type == JELLY_SOCK_DGRAM) {
        info->state = s->connected ? JELLY_SOCKET_STATE_CONNECTED : JELLY_SOCKET_STATE_UNCONNECTED;
        info->pending_error = s->error;
        info->readable = s->datagram_count;
    } else if (s->tcp) {
        tcp_pcb_t *pcb = s->tcp;
        switch (pcb->state) {
        case TCP_CLOSED:
            info->state = s->connected ? JELLY_SOCKET_STATE_CLOSED : JELLY_SOCKET_STATE_UNCONNECTED;
            break;
        case TCP_LISTEN:
            info->state = JELLY_SOCKET_STATE_LISTENING;
            break;
        case TCP_SYN_SENT:
        case TCP_SYN_RECEIVED:
            info->state = JELLY_SOCKET_STATE_CONNECTING;
            break;
        case TCP_ESTABLISHED:
            info->state = JELLY_SOCKET_STATE_CONNECTED;
            break;
        default:
            info->state = JELLY_SOCKET_STATE_CLOSING;
            break;
        }
        info->pending_error = pcb->error;
        info->readable = pcb->state == TCP_LISTEN ? !list_empty(&pcb->accept_queue) : pcb->receive_length;
    }
    mutex_unlock(&net_lock);
}
