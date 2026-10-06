/*
 * Network stack core: the network thread, packet buffers, checksums and
 * address helpers.
 */

#include "net/net.h"
#include "net/arp/arp.h"
#include "net/dns/dns.h"
#include "net/ipv4/ipv4.h"
#include "net/tcp/tcp.h"

#include "core/arch.h"
#include "core/format.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/string.h"
#include "memory/heap.h"
#include "scheduler/thread.h"
#include "time/clock.h"

mutex_t net_lock;

static wait_queue_t net_wakeup;
static volatile bool receive_pending;
static uint64_t timer_deadline = WAIT_FOREVER;

/* --- Packet buffers ------------------------------------------------------------- */

netbuf_t *netbuf_alloc(size_t length)
{
    size_t capacity = NETBUF_HEADROOM + length;
    netbuf_t *buffer = kmalloc(sizeof(netbuf_t) + capacity);
    if (!buffer)
        return NULL;
    buffer->node.prev = buffer->node.next = NULL;
    buffer->netif = NULL;
    buffer->network_header = NULL;
    buffer->capacity = capacity;
    buffer->data = buffer->storage + NETBUF_HEADROOM;
    buffer->length = length;
    return buffer;
}

void netbuf_free(netbuf_t *buffer)
{
    kfree(buffer);
}

void *netbuf_push(netbuf_t *buffer, size_t size)
{
    ASSERT((size_t)(buffer->data - buffer->storage) >= size);
    buffer->data -= size;
    buffer->length += size;
    return buffer->data;
}

bool netbuf_pull(netbuf_t *buffer, size_t size)
{
    if (buffer->length < size)
        return false;
    buffer->data += size;
    buffer->length -= size;
    return true;
}

void netbuf_trim(netbuf_t *buffer, size_t length)
{
    if (length < buffer->length)
        buffer->length = length;
}

/* --- Checksums and addresses ---------------------------------------------------- */

uint32_t checksum_add(uint32_t sum, const void *data, size_t length)
{
    const uint8_t *p = data;
    while (length > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        length -= 2;
    }
    if (length)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return sum;
}

uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t checksum_pseudo(uint32_t source, uint32_t destination, uint8_t protocol, uint16_t length)
{
    uint32_t sum = (source >> 16) + (source & 0xFFFF) + (destination >> 16) + (destination & 0xFFFF);
    return sum + protocol + length;
}

const char *ipv4_format(uint32_t address, char *buffer)
{
    format(buffer, 16, "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xFF, (address >> 8) & 0xFF,
           address & 0xFF);
    return buffer;
}

bool ipv4_parse(const char *text, size_t length, uint32_t *address)
{
    uint32_t result = 0;
    size_t i = 0;

    for (int part = 0; part < 4; part++) {
        uint32_t value = 0;
        size_t digits = 0;
        while (i < length && text[i] >= '0' && text[i] <= '9' && digits < 3) {
            value = value * 10 + (uint32_t)(text[i++] - '0');
            digits++;
        }
        if (digits == 0 || value > 255)
            return false;
        result = result << 8 | value;
        if (part < 3) {
            if (i >= length || text[i] != '.')
                return false;
            i++;
        }
    }
    if (i != length)
        return false;
    *address = result;
    return true;
}

uint16_t net_ephemeral_port(bool (*in_use)(uint16_t port))
{
    static uint16_t next = 49152;
    for (uint32_t tries = 0; tries < 16384; tries++) {
        uint16_t port = next;
        next = next == 65535 ? 49152 : next + 1;
        if (!in_use(port))
            return port;
    }
    return 0;
}

/* --- Network thread ------------------------------------------------------------- */

void netif_receive_ready(netif_t *netif)
{
    (void)netif;
    uint64_t flags = arch_interrupts_save();
    receive_pending = true;
    wait_queue_wake_all(&net_wakeup, STATUS_SUCCESS);
    arch_interrupts_restore(flags);
}

void net_timer_request(uint64_t delay_ns)
{
    uint64_t deadline = clock_monotonic_ns() + delay_ns;
    if (deadline < timer_deadline) {
        timer_deadline = deadline;
        /* The network thread may be sleeping with a later deadline. */
        netif_receive_ready(NULL);
    }
}

static void network_thread(void *arg)
{
    (void)arg;
    for (;;) {
        mutex_lock(&net_lock);
        for (uint32_t i = 0; i < netif_count(); i++) {
            netif_t *netif = netif_get(i);
            netbuf_t *frame;
            /* Bounded batch so timers still run under heavy load. */
            for (int n = 0; n < 256 && (frame = netif->ops->receive(netif)); n++) {
                frame->netif = netif;
                netif->rx_packets++;
                netif->rx_bytes += frame->length;
                if (netif->loopback)
                    ipv4_input(netif, frame);
                else
                    ethernet_input(netif, frame);
            }
        }
        uint64_t now = clock_monotonic_ns();
        if (now >= timer_deadline) {
            timer_deadline = WAIT_FOREVER;
            arp_timer(now);
            tcp_timer(now);
        }
        uint64_t deadline = timer_deadline;
        mutex_unlock(&net_lock);

        uint64_t flags = arch_interrupts_save();
        if (!receive_pending)
            wait_queue_block_uninterruptible(&net_wakeup, deadline);
        receive_pending = false;
        arch_interrupts_restore(flags);
    }
}

status_t net_init(void)
{
    thread_t *thread;

    mutex_init(&net_lock);
    wait_queue_init(&net_wakeup);
    status_t status = loopback_init();
    if (STATUS_IS_ERROR(status))
        return status;
    status = thread_create_kernel("network", network_thread, NULL, THREAD_PRIORITY_KERNEL, &thread);
    if (STATUS_IS_ERROR(status))
        return status;
    thread_start(thread);
    object_release(&thread->object);
    klog_info("net: stack ready (loopback 127.0.0.1)");
    return STATUS_SUCCESS;
}
