/*
 * ARP (README section 32; RFC 826).
 *
 * A small cache of ARP_ENTRIES entries. An unresolved destination gets a
 * pending entry that holds up to ARP_QUEUE packets while requests are
 * retried every second (ARP_RETRIES times); then the packets are dropped.
 * Resolved entries expire after ARP_LIFETIME. Requests for our address are
 * answered, and every ARP packet from a known sender refreshes its entry.
 */

#include "net/arp/arp.h"

#include "core/string.h"
#include "time/clock.h"

#define ARP_ENTRIES      32
#define ARP_QUEUE        4
#define ARP_RETRIES      3
#define ARP_RETRY_NS     1000000000ULL
#define ARP_LIFETIME_NS  (300ULL * 1000000000ULL)
#define ARP_PACKET_SIZE  28

#define ARP_REQUEST 1
#define ARP_REPLY   2

typedef enum { ENTRY_FREE, ENTRY_PENDING, ENTRY_RESOLVED } entry_state_t;

typedef struct {
    entry_state_t state;
    netif_t      *netif;
    uint32_t      address;
    uint8_t       mac[6];
    uint64_t      deadline;   /* PENDING: next retry, RESOLVED: expiry */
    uint32_t      retries;
    list_t        queue;
    uint32_t      queued;
} arp_entry_t;

static arp_entry_t entries[ARP_ENTRIES];

static void send_arp(netif_t *netif, uint16_t operation, const uint8_t target_mac[6], uint32_t target_ip,
                     const uint8_t destination[6])
{
    netbuf_t *packet = netbuf_alloc(ARP_PACKET_SIZE);
    if (!packet)
        return;
    uint8_t *p = packet->data;
    put_be16(p, 1);                 /* Ethernet */
    put_be16(p + 2, ETH_TYPE_IPV4);
    p[4] = 6;
    p[5] = 4;
    put_be16(p + 6, operation);
    memcpy(p + 8, netif->mac, 6);
    put_be32(p + 14, netif->address);
    memcpy(p + 18, target_mac, 6);
    put_be32(p + 24, target_ip);
    ethernet_output(netif, destination, ETH_TYPE_ARP, packet);
}

static void send_request(arp_entry_t *e)
{
    static const uint8_t unknown[6] = { 0 };
    send_arp(e->netif, ARP_REQUEST, unknown, e->address, eth_broadcast);
}

static void drop_queue(arp_entry_t *e)
{
    while (!list_empty(&e->queue)) {
        netbuf_t *packet = container_of(list_pop_front(&e->queue), netbuf_t, node);
        e->netif->tx_dropped++;
        netbuf_free(packet);
    }
    e->queued = 0;
}

static void free_entry(arp_entry_t *e)
{
    drop_queue(e);
    e->state = ENTRY_FREE;
}

static arp_entry_t *find(netif_t *netif, uint32_t address)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (entries[i].state != ENTRY_FREE && entries[i].netif == netif && entries[i].address == address)
            return &entries[i];
    }
    return NULL;
}

/* A free entry, or the resolved one that expires first. */
static arp_entry_t *allocate(void)
{
    arp_entry_t *victim = NULL;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        arp_entry_t *e = &entries[i];
        if (e->state == ENTRY_FREE) {
            list_init(&e->queue);
            e->queued = 0;
            return e;
        }
        if (e->state == ENTRY_RESOLVED && (!victim || e->deadline < victim->deadline))
            victim = e;
    }
    if (victim)
        free_entry(victim);
    return victim;
}

static void update(netif_t *netif, uint32_t address, const uint8_t mac[6], bool create)
{
    arp_entry_t *e = find(netif, address);
    if (!e) {
        if (!create || !(e = allocate()))
            return;
        e->netif = netif;
        e->address = address;
    }
    memcpy(e->mac, mac, 6);
    e->state = ENTRY_RESOLVED;
    e->deadline = clock_monotonic_ns() + ARP_LIFETIME_NS;

    while (!list_empty(&e->queue)) {
        netbuf_t *packet = container_of(list_pop_front(&e->queue), netbuf_t, node);
        ethernet_output(netif, e->mac, ETH_TYPE_IPV4, packet);
    }
    e->queued = 0;
}

void arp_output(netif_t *netif, uint32_t next_hop, netbuf_t *packet)
{
    if (next_hop == IPV4_BROADCAST ||
        (netif->netmask && (next_hop | netif->netmask) == IPV4_BROADCAST &&
         (next_hop & netif->netmask) == (netif->address & netif->netmask))) {
        ethernet_output(netif, eth_broadcast, ETH_TYPE_IPV4, packet);
        return;
    }

    arp_entry_t *e = find(netif, next_hop);
    if (e && e->state == ENTRY_RESOLVED && clock_monotonic_ns() >= e->deadline) {
        free_entry(e); /* expired: resolve again */
        e = NULL;
    }
    if (e && e->state == ENTRY_RESOLVED) {
        ethernet_output(netif, e->mac, ETH_TYPE_IPV4, packet);
        return;
    }
    if (!e) {
        e = allocate();
        if (!e) {
            netif->tx_dropped++;
            netbuf_free(packet);
            return;
        }
        e->state = ENTRY_PENDING;
        e->netif = netif;
        e->address = next_hop;
        e->retries = 0;
        e->deadline = clock_monotonic_ns() + ARP_RETRY_NS;
        send_request(e);
        net_timer_request(ARP_RETRY_NS);
    }
    if (e->queued == ARP_QUEUE) {
        /* Keep the newest packets: drop the oldest. */
        netbuf_t *oldest = container_of(list_pop_front(&e->queue), netbuf_t, node);
        netif->tx_dropped++;
        netbuf_free(oldest);
        e->queued--;
    }
    list_push_back(&e->queue, &packet->node);
    e->queued++;
}

void arp_input(netif_t *netif, netbuf_t *packet)
{
    const uint8_t *p = packet->data;

    if (packet->length < ARP_PACKET_SIZE || get_be16(p) != 1 || get_be16(p + 2) != ETH_TYPE_IPV4 || p[4] != 6 ||
        p[5] != 4) {
        netif->rx_dropped++;
        netbuf_free(packet);
        return;
    }
    uint16_t operation = get_be16(p + 6);
    uint8_t sender_mac[6];
    memcpy(sender_mac, p + 8, 6);
    uint32_t sender_ip = get_be32(p + 14);
    uint32_t target_ip = get_be32(p + 24);
    netbuf_free(packet);

    if (sender_ip == 0 || !netif->address)
        return;
    bool for_us = target_ip == netif->address;
    /* RFC 826: update an existing entry from any packet, create one only if we are the target. */
    update(netif, sender_ip, sender_mac, for_us);
    if (for_us && operation == ARP_REQUEST)
        send_arp(netif, ARP_REPLY, sender_mac, sender_ip, sender_mac);
}

void arp_timer(uint64_t now)
{
    bool pending = false;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        arp_entry_t *e = &entries[i];
        if (e->state == ENTRY_RESOLVED && now >= e->deadline) {
            free_entry(e);
        } else if (e->state == ENTRY_PENDING) {
            if (now >= e->deadline) {
                if (++e->retries >= ARP_RETRIES) {
                    free_entry(e); /* unreachable: the queued packets are lost */
                    continue;
                }
                e->deadline = now + ARP_RETRY_NS;
                send_request(e);
            }
            pending = true;
        }
    }
    if (pending)
        net_timer_request(ARP_RETRY_NS / 4);
}

void arp_flush(netif_t *netif)
{
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (entries[i].state != ENTRY_FREE && entries[i].netif == netif)
            free_entry(&entries[i]);
    }
}

void arp_announce(netif_t *netif)
{
    static const uint8_t unknown[6] = { 0 };
    send_arp(netif, ARP_REQUEST, unknown, netif->address, eth_broadcast);
}

bool arp_lookup(netif_t *netif, uint32_t address, uint8_t mac[6])
{
    arp_entry_t *e = find(netif, address);
    if (!e || e->state != ENTRY_RESOLVED)
        return false;
    memcpy(mac, e->mac, 6);
    return true;
}
