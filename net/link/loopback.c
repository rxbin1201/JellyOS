/*
 * Loopback interface "lo" (127.0.0.1/8).
 *
 * Transmitted IPv4 packets are queued and handed back to the stack by the
 * network thread, never processed recursively inside the sender.
 */

#include "net/net.h"

#include "core/string.h"

#define LOOPBACK_QUEUE_MAX 512

static netif_t loopback;
static list_t queue;
static uint32_t queued;

static void loopback_transmit(netif_t *netif, netbuf_t *packet)
{
    if (queued >= LOOPBACK_QUEUE_MAX) {
        netif->tx_dropped++;
        netbuf_free(packet);
        return;
    }
    list_push_back(&queue, &packet->node);
    queued++;
    netif_receive_ready(netif);
}

static netbuf_t *loopback_receive(netif_t *netif)
{
    (void)netif;
    if (list_empty(&queue))
        return NULL;
    queued--;
    return container_of(list_pop_front(&queue), netbuf_t, node);
}

static bool loopback_link_up(netif_t *netif)
{
    (void)netif;
    return true;
}

static const netif_ops_t loopback_ops = {
    .transmit = loopback_transmit,
    .receive = loopback_receive,
    .link_up = loopback_link_up,
};

status_t loopback_init(void)
{
    list_init(&queue);
    strcpy(loopback.name, "lo");
    loopback.loopback = true;
    loopback.mtu = 16384;
    loopback.ops = &loopback_ops;
    status_t status = netif_register(&loopback);
    if (STATUS_IS_ERROR(status))
        return status;
    loopback.address = IPV4_LOOPBACK;
    loopback.netmask = IPV4(255, 0, 0, 0);
    return STATUS_SUCCESS;
}
