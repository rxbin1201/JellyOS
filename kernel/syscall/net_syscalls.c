/*
 * Networking system calls (syscall ABI version 4).
 *
 * User data is copied through kernel bounce buffers (at most NET_IO_MAX
 * bytes per call), so the socket layer only sees kernel memory and never
 * touches user pages while holding net_lock.
 */

#include "syscall/internal.h"

#include "core/string.h"
#include "memory/heap.h"
#include "net/dns/dns.h"
#include "net/net.h"
#include "net/sockets/socket.h"
#include "process/process.h"
#include "process/usercopy.h"

#include <jelly/syscall.h>

#define NET_IO_MAX (64 * 1024)

static status_t get_socket(uint64_t handle, uint32_t rights, socket_t **socket)
{
    object_t *object;
    status_t status = handle_get(syscall_handles(), (handle_t)handle, OBJECT_SOCKET, rights, &object, NULL);
    if (!STATUS_IS_ERROR(status))
        *socket = container_of(object, socket_t, object);
    return status;
}

static status_t read_address(uint64_t pointer, net_endpoint_t *endpoint)
{
    jelly_sockaddr_in_t address;
    status_t status = copy_from_user(&address, pointer, sizeof(address));
    if (STATUS_IS_ERROR(status))
        return status;
    if (address.family != JELLY_AF_INET)
        return STATUS_NOT_SUPPORTED;
    endpoint->address = ntohl(address.address);
    endpoint->port = ntohs(address.port);
    return STATUS_SUCCESS;
}

static status_t write_address(uint64_t pointer, const net_endpoint_t *endpoint)
{
    jelly_sockaddr_in_t address;
    memset(&address, 0, sizeof(address));
    address.family = JELLY_AF_INET;
    address.port = htons(endpoint->port);
    address.address = htonl(endpoint->address);
    return copy_to_user(pointer, &address, sizeof(address));
}

status_t sys_socket_create(const uint64_t *a)
{
    socket_t *s;

    if (a[0] != JELLY_AF_INET)
        return STATUS_NOT_SUPPORTED;
    if (!user_range_ok(a[3], sizeof(jelly_handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = socket_create((uint32_t)a[1], (uint32_t)a[2], &s);
    if (STATUS_IS_ERROR(status))
        return status;
    /* The handle takes over our reference. */
    return syscall_give_handle(&s->object, JELLY_RIGHT_READ | JELLY_RIGHT_WRITE | JELLY_RIGHT_WAIT |
                                               JELLY_RIGHT_DUPLICATE, a[3]);
}

status_t sys_socket_bind(const uint64_t *a)
{
    socket_t *s;
    net_endpoint_t local;
    status_t status = read_address(a[1], &local);
    if (STATUS_IS_ERROR(status) || STATUS_IS_ERROR(status = get_socket(a[0], JELLY_RIGHT_WRITE, &s)))
        return status;
    status = socket_bind(s, &local);
    object_release(&s->object);
    return status;
}

status_t sys_socket_connect(const uint64_t *a)
{
    socket_t *s;
    net_endpoint_t remote;
    status_t status = read_address(a[1], &remote);
    if (STATUS_IS_ERROR(status) || STATUS_IS_ERROR(status = get_socket(a[0], JELLY_RIGHT_WRITE, &s)))
        return status;
    status = socket_connect(s, &remote);
    object_release(&s->object);
    return status;
}

status_t sys_socket_listen(const uint64_t *a)
{
    socket_t *s;
    status_t status = get_socket(a[0], JELLY_RIGHT_WRITE, &s);
    if (STATUS_IS_ERROR(status))
        return status;
    status = socket_listen(s, (uint32_t)a[1]);
    object_release(&s->object);
    return status;
}

status_t sys_socket_accept(const uint64_t *a)
{
    socket_t *s, *connection;
    net_endpoint_t peer;

    if (!user_range_ok(a[1], sizeof(jelly_handle_t), true) ||
        (a[2] && !user_range_ok(a[2], sizeof(jelly_sockaddr_in_t), true)))
        return STATUS_INVALID_ARGUMENT;
    status_t status = get_socket(a[0], JELLY_RIGHT_READ, &s);
    if (STATUS_IS_ERROR(status))
        return status;
    status = socket_accept(s, &connection, &peer);
    object_release(&s->object);
    if (STATUS_IS_ERROR(status))
        return status;

    status = syscall_give_handle(&connection->object, JELLY_RIGHT_READ | JELLY_RIGHT_WRITE | JELLY_RIGHT_WAIT |
                                                          JELLY_RIGHT_DUPLICATE, a[1]);
    if (!STATUS_IS_ERROR(status) && a[2])
        status = write_address(a[2], &peer);
    return status;
}

/* send/receive through a bounce buffer; used by the socket calls and by SYS_FILE_READ/WRITE. */
static status_t socket_io(socket_t *s, bool write, uint64_t buffer, uint64_t size, const net_endpoint_t *to,
                          net_endpoint_t *from, uint32_t flags, size_t *done)
{
    *done = 0;
    if (size > NET_IO_MAX) {
        if (s->type == JELLY_SOCK_DGRAM && write)
            return STATUS_LIMIT_EXCEEDED;
        size = NET_IO_MAX; /* streams: a partial transfer, datagrams: truncated */
    }
    uint8_t *bounce = kmalloc(size ? size : 1);
    if (!bounce)
        return STATUS_OUT_OF_MEMORY;

    status_t status;
    if (write) {
        status = copy_from_user(bounce, buffer, size);
        if (!STATUS_IS_ERROR(status))
            status = socket_send(s, bounce, size, to, flags, done);
    } else {
        status = socket_receive(s, bounce, size, from, flags, done);
        if (!STATUS_IS_ERROR(status) && *done)
            status = copy_to_user(buffer, bounce, *done);
    }
    kfree(bounce);
    return status;
}

static status_t transfer(const uint64_t *a, bool write)
{
    socket_t *s;
    net_endpoint_t peer = { 0, 0 };
    size_t done;

    if (!user_range_ok(a[5], sizeof(uint64_t), true) || (a[2] && !user_range_ok(a[1], a[2], !write)) ||
        (a[3] && !user_range_ok(a[3], sizeof(jelly_sockaddr_in_t), !write)))
        return STATUS_INVALID_ARGUMENT;
    if (a[4] & ~(uint64_t)(JELLY_MSG_PEEK | JELLY_MSG_DONTWAIT))
        return STATUS_INVALID_ARGUMENT;
    status_t status = STATUS_SUCCESS;
    if (write && a[3])
        status = read_address(a[3], &peer);
    if (STATUS_IS_ERROR(status) ||
        STATUS_IS_ERROR(status = get_socket(a[0], write ? JELLY_RIGHT_WRITE : JELLY_RIGHT_READ, &s)))
        return status;

    status = socket_io(s, write, a[1], a[2], write && a[3] ? &peer : NULL, &peer, (uint32_t)a[4], &done);
    object_release(&s->object);
    if (STATUS_IS_ERROR(status))
        return status;
    if (!write && a[3])
        status = write_address(a[3], &peer);
    return STATUS_IS_ERROR(status) ? status : put_user_u64(a[5], done);
}

status_t sys_socket_send(const uint64_t *a)
{
    return transfer(a, true);
}

status_t sys_socket_receive(const uint64_t *a)
{
    return transfer(a, false);
}

status_t sys_socket_shutdown(const uint64_t *a)
{
    socket_t *s;
    status_t status = get_socket(a[0], 0, &s);
    if (STATUS_IS_ERROR(status))
        return status;
    status = socket_shutdown(s, (uint32_t)a[1]);
    object_release(&s->object);
    return status;
}

status_t sys_socket_set_option(const uint64_t *a)
{
    socket_t *s;
    status_t status = get_socket(a[0], JELLY_RIGHT_WRITE, &s);
    if (STATUS_IS_ERROR(status))
        return status;
    status = socket_set_option(s, (uint32_t)a[1], a[2]);
    object_release(&s->object);
    return status;
}

status_t sys_socket_info(const uint64_t *a)
{
    socket_t *s;
    jelly_socket_info_t info;
    status_t status = get_socket(a[0], 0, &s);
    if (STATUS_IS_ERROR(status))
        return status;
    socket_info(s, &info);
    object_release(&s->object);
    return copy_to_user(a[1], &info, sizeof(info));
}

/* SYS_FILE_READ / SYS_FILE_WRITE on a socket handle: recv/send without an address. */
status_t syscall_socket_file_io(uint64_t handle, bool write, uint64_t buffer, uint64_t size, uint64_t *done)
{
    socket_t *s;
    size_t transferred;
    status_t status = get_socket(handle, write ? JELLY_RIGHT_WRITE : JELLY_RIGHT_READ, &s);
    if (STATUS_IS_ERROR(status))
        return status;
    status = socket_io(s, write, buffer, size, NULL, NULL, 0, &transferred);
    object_release(&s->object);
    *done = transferred;
    return status;
}

/* --- Interfaces and names ------------------------------------------------------- */

status_t sys_net_interface_info(const uint64_t *a)
{
    jelly_netif_info_t info;
    memset(&info, 0, sizeof(info));

    mutex_lock(&net_lock);
    netif_t *netif = netif_get((uint32_t)a[0]);
    if (netif) {
        info.index = netif->index;
        info.flags = JELLY_NETIF_UP | (netif->loopback ? JELLY_NETIF_LOOPBACK : 0) |
                     (netif->ops->link_up(netif) ? JELLY_NETIF_LINK : 0) |
                     (netif->address ? JELLY_NETIF_CONFIGURED : 0);
        memcpy(info.name, netif->name, sizeof(info.name));
        memcpy(info.mac, netif->mac, 6);
        info.mtu = netif->mtu;
        info.address = htonl(netif->address);
        info.netmask = htonl(netif->netmask);
        info.gateway = htonl(netif->gateway);
        info.dns = htonl(netif->dns);
        info.rx_packets = netif->rx_packets;
        info.rx_bytes = netif->rx_bytes;
        info.rx_dropped = netif->rx_dropped;
        info.tx_packets = netif->tx_packets;
        info.tx_bytes = netif->tx_bytes;
        info.tx_dropped = netif->tx_dropped;
    }
    mutex_unlock(&net_lock);
    if (!netif)
        return STATUS_NOT_FOUND;
    return copy_to_user(a[1], &info, sizeof(info));
}

status_t sys_net_configure(const uint64_t *a)
{
    jelly_netif_config_t config;

    if (process_current()->credentials.uid != UID_ROOT)
        return STATUS_ACCESS_DENIED;
    status_t status = copy_from_user(&config, a[1], sizeof(config));
    if (STATUS_IS_ERROR(status))
        return status;

    uint32_t address = ntohl(config.address), netmask = ntohl(config.netmask);
    uint32_t gateway = ntohl(config.gateway), dns = ntohl(config.dns);
    /* The netmask must be contiguous; gateways must be on the link. */
    if (address && (netmask == 0 || (~netmask & (~netmask + 1)) != 0))
        return STATUS_INVALID_ARGUMENT;
    if (address && gateway && (gateway & netmask) != (address & netmask))
        return STATUS_INVALID_ARGUMENT;

    mutex_lock(&net_lock);
    netif_t *netif = netif_get((uint32_t)a[0]);
    if (!netif)
        status = STATUS_NOT_FOUND;
    else if (netif->loopback)
        status = STATUS_ACCESS_DENIED;
    else
        netif_configure(netif, address, netmask, gateway, dns);
    mutex_unlock(&net_lock);
    if (!STATUS_IS_ERROR(status))
        dns_cache_clear();
    return status;
}

status_t sys_net_resolve(const uint64_t *a)
{
    char name[256];
    uint32_t address;

    if (a[1] == 0 || a[1] >= sizeof(name) || !user_range_ok(a[2], sizeof(uint32_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = copy_from_user(name, a[0], a[1]);
    if (STATUS_IS_ERROR(status))
        return status;
    name[a[1]] = '\0';
    status = dns_resolve(name, a[1], &address);
    return STATUS_IS_ERROR(status) ? status : put_user_u32(a[2], htonl(address));
}
