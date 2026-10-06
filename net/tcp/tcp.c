/*
 * TCP (README section 32; RFC 793, RFC 1122, RFC 5681, RFC 6298).
 *
 * - Connections: active and passive open, the full close state machine,
 *   TIME_WAIT, reset handling. A socket closed by its application leaves
 *   an orphaned pcb that finishes the close in the background.
 * - Sending: 64 KiB buffer; segments up to the MSS within the peer's window
 *   and the congestion window (slow start, congestion avoidance, fast
 *   retransmit after three duplicate ACKs). Nagle's algorithm is not used.
 * - Retransmission: RTO from smoothed RTT (Karn's rule), exponential
 *   backoff, go-back-N on timeout, zero-window probes.
 * - Receiving: in-order data only; segments beyond rcv_nxt are dropped and
 *   answered with a duplicate ACK. Every data segment is ACKed at once.
 * - Options: MSS only (no window scaling, timestamps or SACK).
 */

#include "net/tcp/tcp.h"
#include "net/ipv4/ipv4.h"
#include "net/sockets/socket.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "time/clock.h"

#define TCP_HEADER_SIZE      20
#define TCP_DEFAULT_MSS      536
#define TCP_INITIAL_RTO      1000000000ULL
#define TCP_MIN_RTO          200000000ULL
#define TCP_MAX_RTO          60000000000ULL
#define TCP_SYN_RETRIES      5
#define TCP_MAX_RETRIES      10
#define TCP_TIME_WAIT_NS     10000000000ULL
#define TCP_ORPHAN_TIMEOUT   60000000000ULL
#define TCP_MAX_WINDOW       65535u

#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_PSH 0x08
#define FLAG_ACK 0x10

#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((int32_t)((a) - (b)) > 0)
#define SEQ_GE(a, b) ((int32_t)((a) - (b)) >= 0)

static list_t pcbs = { { &pcbs.head, &pcbs.head } };
static uint32_t pcb_count;

typedef struct {
    uint32_t seq, ack;
    uint8_t  flags;
    uint16_t window;
    uint16_t mss;          /* from the MSS option, 0 if absent */
    const uint8_t *data;
    uint32_t length;       /* payload bytes */
} segment_t;

const char *tcp_state_name(tcp_state_t state)
{
    static const char *const names[] = { "CLOSED",     "LISTEN",     "SYN_SENT", "SYN_RECEIVED",
                                         "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT",
                                         "CLOSING",    "LAST_ACK",   "TIME_WAIT" };
    return state <= TCP_TIME_WAIT ? names[state] : "?";
}

uint32_t tcp_pcb_count(void)
{
    return pcb_count;
}

/* --- Connection blocks ---------------------------------------------------------- */

static uint32_t new_iss(void)
{
    static uint32_t counter;
    counter += 64000;
    return (uint32_t)(clock_monotonic_ns() / 4000) + counter; /* RFC 793: a 4 µs clock */
}

tcp_pcb_t *tcp_pcb_create(socket_t *socket)
{
    tcp_pcb_t *pcb = kcalloc(1, sizeof(*pcb));
    if (!pcb)
        return NULL;
    pcb->send_buffer = kmalloc(TCP_BUFFER_SIZE);
    pcb->receive_buffer = kmalloc(TCP_BUFFER_SIZE);
    if (!pcb->send_buffer || !pcb->receive_buffer) {
        kfree(pcb->send_buffer);
        kfree(pcb->receive_buffer);
        kfree(pcb);
        return NULL;
    }
    pcb->socket = socket;
    pcb->state = TCP_CLOSED;
    pcb->rto = TCP_INITIAL_RTO;
    pcb->mss = TCP_DEFAULT_MSS;
    list_init(&pcb->accept_queue);
    pcb_count++;
    return pcb;
}

static void list_pcb(tcp_pcb_t *pcb)
{
    if (!pcb->listed) {
        list_push_back(&pcbs, &pcb->node);
        pcb->listed = true;
    }
}

static void unlist_pcb(tcp_pcb_t *pcb)
{
    if (pcb->listed) {
        list_remove(&pcb->node);
        pcb->listed = false;
    }
}

static void destroy(tcp_pcb_t *pcb)
{
    unlist_pcb(pcb);
    kfree(pcb->send_buffer);
    kfree(pcb->receive_buffer);
    kfree(pcb);
    pcb_count--;
}

static void notify(tcp_pcb_t *pcb)
{
    if (pcb->socket)
        socket_notify(pcb->socket);
    else if (pcb->listener && pcb->listener->socket)
        socket_notify(pcb->listener->socket);
}

/*
 * The connection is over. A pcb with a socket stays (CLOSED, unlisted) until
 * the socket is closed; orphans and unaccepted children are freed.
 */
static void finish(tcp_pcb_t *pcb, status_t error)
{
    if (error && !pcb->error)
        pcb->error = error;
    pcb->state = TCP_CLOSED;
    pcb->retransmit_at = 0;
    unlist_pcb(pcb);

    if (pcb->listener) {
        tcp_pcb_t *listener = pcb->listener;
        if (list_linked(&pcb->accept_node))
            list_remove(&pcb->accept_node);
        listener->children--;
        pcb->listener = NULL;
        destroy(pcb);
        return;
    }
    if (pcb->socket)
        socket_notify(pcb->socket);
    else
        destroy(pcb);
}

/* --- Output --------------------------------------------------------------------- */

static uint32_t receive_window(const tcp_pcb_t *pcb)
{
    uint32_t space = TCP_BUFFER_SIZE - pcb->receive_length;
    return space > TCP_MAX_WINDOW ? TCP_MAX_WINDOW : space;
}

static void send_raw(uint32_t source, uint16_t source_port, uint32_t destination, uint16_t destination_port,
                     uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window, uint16_t mss_option,
                     const uint8_t *ring, uint32_t ring_offset, uint32_t length, uint32_t interface)
{
    uint32_t options = mss_option ? 4 : 0;
    netbuf_t *packet = netbuf_alloc(TCP_HEADER_SIZE + options + length);
    if (!packet)
        return;
    uint8_t *h = packet->data;
    memset(h, 0, TCP_HEADER_SIZE + options);
    put_be16(h, source_port);
    put_be16(h + 2, destination_port);
    put_be32(h + 4, seq);
    put_be32(h + 8, (flags & FLAG_ACK) ? ack : 0);
    h[12] = (uint8_t)(((TCP_HEADER_SIZE + options) / 4) << 4);
    h[13] = flags;
    put_be16(h + 14, window);
    if (mss_option) {
        h[20] = 2;
        h[21] = 4;
        put_be16(h + 22, mss_option);
    }
    /* Payload from the ring buffer, which may wrap. */
    uint8_t *payload = h + TCP_HEADER_SIZE + options;
    for (uint32_t done = 0; done < length;) {
        uint32_t position = (ring_offset + done) % TCP_BUFFER_SIZE;
        uint32_t chunk = TCP_BUFFER_SIZE - position;
        if (chunk > length - done)
            chunk = length - done;
        memcpy(payload + done, ring + position, chunk);
        done += chunk;
    }
    uint32_t sum = checksum_pseudo(source, destination, IP_PROTO_TCP, (uint16_t)packet->length);
    put_be16(h + 16, checksum_finish(checksum_add(sum, h, packet->length)));
    ipv4_output(packet, source, destination, IP_PROTO_TCP, interface);
}

static uint16_t our_mss(const tcp_pcb_t *pcb)
{
    uint32_t mtu = ipv4_mtu_for(pcb->remote_address, pcb->interface);
    return (uint16_t)(mtu - IPV4_HEADER_SIZE - TCP_HEADER_SIZE);
}

/* A segment of this connection: flags plus `length` bytes from send buffer offset `offset`. */
static void send_segment(tcp_pcb_t *pcb, uint32_t seq, uint8_t flags, uint32_t offset, uint32_t length)
{
    uint16_t mss = (flags & FLAG_SYN) ? our_mss(pcb) : 0;
    pcb->advertised_window = receive_window(pcb);
    send_raw(pcb->local_address, pcb->local_port, pcb->remote_address, pcb->remote_port, seq, pcb->rcv_nxt, flags,
             (uint16_t)pcb->advertised_window, mss, pcb->send_buffer, pcb->send_start + offset, length,
             pcb->interface);
}

static void send_ack(tcp_pcb_t *pcb)
{
    send_segment(pcb, pcb->snd_nxt, FLAG_ACK, 0, 0);
}

static void send_reset_for(uint32_t source, uint32_t destination, const uint8_t *header, const segment_t *s)
{
    if (s->flags & FLAG_RST)
        return;
    uint16_t source_port = get_be16(header), destination_port = get_be16(header + 2);
    if (s->flags & FLAG_ACK) {
        send_raw(destination, destination_port, source, source_port, s->ack, 0, FLAG_RST, 0, 0, NULL, 0, 0, 0);
    } else {
        uint32_t length = s->length + ((s->flags & FLAG_SYN) ? 1 : 0) + ((s->flags & FLAG_FIN) ? 1 : 0);
        send_raw(destination, destination_port, source, source_port, 0, s->seq + length, FLAG_RST | FLAG_ACK, 0, 0,
                 NULL, 0, 0, 0);
    }
}

static void arm_retransmit(tcp_pcb_t *pcb)
{
    pcb->retransmit_at = clock_monotonic_ns() + pcb->rto;
    net_timer_request(pcb->rto);
}

static bool states_sending(tcp_state_t state)
{
    return state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT || state == TCP_FIN_WAIT_1 ||
           state == TCP_CLOSING || state == TCP_LAST_ACK;
}

/* Send what the windows allow, then the FIN once all data is out. */
static void output(tcp_pcb_t *pcb, uint32_t forced_window)
{
    if (!states_sending(pcb->state))
        return;

    uint32_t window = pcb->snd_wnd < pcb->cwnd ? pcb->snd_wnd : pcb->cwnd;
    if (forced_window > window)
        window = forced_window;

    for (;;) {
        uint32_t offset = pcb->snd_nxt - pcb->snd_una;
        uint32_t in_flight = offset;
        if (offset > pcb->send_length)
            break; /* the FIN is already out */
        uint32_t unsent = pcb->send_length - offset;
        if (unsent == 0) {
            if (pcb->fin_queued) {
                send_segment(pcb, pcb->snd_nxt, FLAG_FIN | FLAG_ACK, 0, 0);
                pcb->fin_sent = true;
                pcb->fin_seq = pcb->snd_nxt;
                pcb->snd_nxt++;
                if (SEQ_GT(pcb->snd_nxt, pcb->snd_max))
                    pcb->snd_max = pcb->snd_nxt;
            }
            break;
        }
        if (in_flight >= window)
            break;
        uint32_t length = unsent;
        if (length > pcb->mss)
            length = pcb->mss;
        if (length > window - in_flight)
            length = window - in_flight;
        uint8_t flags = FLAG_ACK | (length == unsent ? FLAG_PSH : 0);
        send_segment(pcb, pcb->snd_nxt, flags, offset, length);
        if (!pcb->rtt_measuring) {
            pcb->rtt_measuring = true;
            pcb->rtt_seq = pcb->snd_nxt + length;
            pcb->rtt_start = clock_monotonic_ns();
        }
        pcb->snd_nxt += length;
        if (SEQ_GT(pcb->snd_nxt, pcb->snd_max))
            pcb->snd_max = pcb->snd_nxt;
    }

    bool outstanding = pcb->snd_max != pcb->snd_una;
    bool blocked = pcb->send_length > pcb->snd_nxt - pcb->snd_una && pcb->snd_wnd == 0;
    if ((outstanding || blocked) && !pcb->retransmit_at)
        arm_retransmit(pcb);
}

static void update_rtt(tcp_pcb_t *pcb, uint64_t sample)
{
    if (pcb->srtt == 0) {
        pcb->srtt = sample;
        pcb->rttvar = sample / 2;
    } else {
        uint64_t delta = pcb->srtt > sample ? pcb->srtt - sample : sample - pcb->srtt;
        pcb->rttvar = (3 * pcb->rttvar + delta) / 4;
        pcb->srtt = (7 * pcb->srtt + sample) / 8;
    }
    uint64_t rto = pcb->srtt + (4 * pcb->rttvar > 10000000ULL ? 4 * pcb->rttvar : 10000000ULL);
    pcb->rto = rto < TCP_MIN_RTO ? TCP_MIN_RTO : rto > TCP_MAX_RTO ? TCP_MAX_RTO : rto;
}

/* --- Application interface ------------------------------------------------------ */

static bool port_in_use(uint32_t address, uint16_t port, bool reuse)
{
    list_for_each(node, &pcbs) {
        tcp_pcb_t *p = container_of(node, tcp_pcb_t, node);
        if (p->local_port != port)
            continue;
        if (reuse && p->state == TCP_TIME_WAIT)
            continue;
        if (!address || !p->local_address || address == p->local_address)
            return true;
    }
    return false;
}

static bool ephemeral_in_use(uint16_t port)
{
    return port_in_use(0, port, false);
}

status_t tcp_bind(tcp_pcb_t *pcb, uint32_t address, uint16_t port, bool reuse)
{
    if (port == 0) {
        port = net_ephemeral_port(ephemeral_in_use);
        if (port == 0)
            return STATUS_ADDRESS_IN_USE;
    } else if (port_in_use(address, port, reuse)) {
        return STATUS_ADDRESS_IN_USE;
    }
    pcb->local_address = address;
    pcb->local_port = port;
    return STATUS_SUCCESS;
}

status_t tcp_connect(tcp_pcb_t *pcb, uint32_t address, uint16_t port)
{
    netif_t *netif;
    uint32_t source;

    if (address == 0 || address == IPV4_BROADCAST || port == 0)
        return STATUS_INVALID_ARGUMENT;
    status_t status = ipv4_route(address, pcb->interface, &netif, NULL, &source);
    if (STATUS_IS_ERROR(status))
        return status;
    if (!pcb->local_address) {
        if (!source)
            return STATUS_UNREACHABLE; /* the interface has no address yet */
        pcb->local_address = source;
    }
    if (!pcb->local_port && STATUS_IS_ERROR(status = tcp_bind(pcb, pcb->local_address, 0, false)))
        return status;

    pcb->remote_address = address;
    pcb->remote_port = port;
    pcb->iss = new_iss();
    pcb->snd_una = pcb->iss;
    pcb->snd_nxt = pcb->snd_max = pcb->iss + 1;
    pcb->send_start = 0;
    pcb->send_length = 0;
    pcb->mss = our_mss(pcb) < TCP_DEFAULT_MSS ? our_mss(pcb) : TCP_DEFAULT_MSS;
    pcb->state = TCP_SYN_SENT;
    list_pcb(pcb);
    send_segment(pcb, pcb->iss, FLAG_SYN, 0, 0);
    arm_retransmit(pcb);
    return STATUS_SUCCESS;
}

status_t tcp_listen(tcp_pcb_t *pcb, uint32_t backlog)
{
    if (pcb->state != TCP_CLOSED && pcb->state != TCP_LISTEN)
        return STATUS_INVALID_ARGUMENT;
    if (!pcb->local_port) {
        status_t status = tcp_bind(pcb, pcb->local_address, 0, false);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    pcb->backlog = backlog == 0 ? 1 : backlog > 128 ? 128 : backlog;
    pcb->state = TCP_LISTEN;
    list_pcb(pcb);
    return STATUS_SUCCESS;
}

tcp_pcb_t *tcp_accept(tcp_pcb_t *listener)
{
    if (list_empty(&listener->accept_queue))
        return NULL;
    tcp_pcb_t *child = container_of(list_pop_front(&listener->accept_queue), tcp_pcb_t, accept_node);
    child->accept_node.prev = child->accept_node.next = NULL;
    child->listener = NULL;
    listener->children--;
    return child;
}

size_t tcp_send_space(const tcp_pcb_t *pcb)
{
    return TCP_BUFFER_SIZE - pcb->send_length;
}

bool tcp_can_send(const tcp_pcb_t *pcb)
{
    return (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_CLOSE_WAIT) && !pcb->fin_queued;
}

size_t tcp_write(tcp_pcb_t *pcb, const void *data, size_t length)
{
    const uint8_t *in = data;
    size_t space = tcp_send_space(pcb);
    if (length > space)
        length = space;
    for (size_t done = 0; done < length;) {
        uint32_t position = (pcb->send_start + pcb->send_length) % TCP_BUFFER_SIZE;
        size_t chunk = TCP_BUFFER_SIZE - position;
        if (chunk > length - done)
            chunk = length - done;
        memcpy(pcb->send_buffer + position, in + done, chunk);
        pcb->send_length += (uint32_t)chunk;
        done += chunk;
    }
    if (length)
        output(pcb, 0);
    return length;
}

size_t tcp_read(tcp_pcb_t *pcb, void *buffer, size_t length, bool peek)
{
    uint8_t *out = buffer;
    if (length > pcb->receive_length)
        length = pcb->receive_length;
    for (size_t done = 0; done < length;) {
        uint32_t position = (pcb->receive_start + (uint32_t)done) % TCP_BUFFER_SIZE;
        size_t chunk = TCP_BUFFER_SIZE - position;
        if (chunk > length - done)
            chunk = length - done;
        memcpy(out + done, pcb->receive_buffer + position, chunk);
        done += chunk;
    }
    if (peek || length == 0)
        return length;

    pcb->receive_start = (pcb->receive_start + (uint32_t)length) % TCP_BUFFER_SIZE;
    pcb->receive_length -= (uint32_t)length;
    /* Window update once the window has grown noticeably (also out of a zero window). */
    uint32_t window = receive_window(pcb);
    if (pcb->state != TCP_CLOSED && pcb->state != TCP_LISTEN &&
        (window >= pcb->advertised_window + 2 * pcb->mss || (pcb->advertised_window < pcb->mss && window >= pcb->mss)))
        send_ack(pcb);
    return length;
}

void tcp_shutdown_write(tcp_pcb_t *pcb)
{
    switch (pcb->state) {
    case TCP_SYN_SENT:
        finish(pcb, STATUS_SUCCESS);
        return;
    case TCP_SYN_RECEIVED:
    case TCP_ESTABLISHED:
        pcb->state = TCP_FIN_WAIT_1;
        break;
    case TCP_CLOSE_WAIT:
        pcb->state = TCP_LAST_ACK;
        break;
    default:
        return;
    }
    pcb->fin_queued = true;
    output(pcb, 0);
}

static void abort_connection(tcp_pcb_t *pcb, status_t error)
{
    if (pcb->state != TCP_CLOSED && pcb->state != TCP_LISTEN && pcb->state != TCP_SYN_SENT &&
        pcb->state != TCP_TIME_WAIT)
        send_raw(pcb->local_address, pcb->local_port, pcb->remote_address, pcb->remote_port, pcb->snd_nxt, 0,
                 FLAG_RST, 0, 0, NULL, 0, 0, pcb->interface);
    finish(pcb, error);
}

void tcp_close(tcp_pcb_t *pcb)
{
    pcb->socket = NULL;

    if (pcb->state == TCP_LISTEN) {
        while (!list_empty(&pcb->accept_queue))
            abort_connection(container_of(list_front(&pcb->accept_queue), tcp_pcb_t, accept_node),
                             STATUS_CONNECTION_RESET);
        /* Children still in SYN_RECEIVED point at the listener: reset them as well. */
        list_node_t *node, *next;
        for (node = pcbs.head.next; node != &pcbs.head; node = next) {
            next = node->next;
            tcp_pcb_t *p = container_of(node, tcp_pcb_t, node);
            if (p->listener == pcb)
                abort_connection(p, STATUS_CONNECTION_RESET);
        }
        finish(pcb, STATUS_SUCCESS);
        return;
    }
    if (pcb->state == TCP_CLOSED || pcb->state == TCP_SYN_SENT) {
        finish(pcb, STATUS_SUCCESS);
        return;
    }
    /* Unread data: tell the peer it was lost (RFC 2525 2.17). */
    if (pcb->receive_length) {
        abort_connection(pcb, STATUS_CONNECTION_RESET);
        return;
    }
    tcp_shutdown_write(pcb);
    if (pcb->state == TCP_FIN_WAIT_2) {
        pcb->close_at = clock_monotonic_ns() + TCP_ORPHAN_TIMEOUT;
        net_timer_request(TCP_ORPHAN_TIMEOUT);
    }
}

/* --- Input ---------------------------------------------------------------------- */

static bool parse(netbuf_t *packet, uint32_t source, uint32_t destination, segment_t *s)
{
    const uint8_t *h = packet->data;
    if (packet->length < TCP_HEADER_SIZE)
        return false;
    uint32_t header_length = (uint32_t)(h[12] >> 4) * 4;
    if (header_length < TCP_HEADER_SIZE || header_length > packet->length)
        return false;
    uint32_t sum = checksum_pseudo(source, destination, IP_PROTO_TCP, (uint16_t)packet->length);
    if (checksum_finish(checksum_add(sum, h, packet->length)) != 0)
        return false;

    s->seq = get_be32(h + 4);
    s->ack = get_be32(h + 8);
    s->flags = h[13] & 0x3F;
    s->window = get_be16(h + 14);
    s->mss = 0;
    for (uint32_t i = TCP_HEADER_SIZE; i < header_length;) {
        uint8_t kind = h[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= header_length || h[i + 1] < 2 || i + h[i + 1] > header_length)
            break;
        if (kind == 2 && h[i + 1] == 4)
            s->mss = get_be16(h + i + 2);
        i += h[i + 1];
    }
    s->data = h + header_length;
    s->length = (uint32_t)packet->length - header_length;
    return true;
}

static tcp_pcb_t *lookup(uint32_t local_address, uint16_t local_port, uint32_t remote_address, uint16_t remote_port)
{
    tcp_pcb_t *listener = NULL;
    list_for_each(node, &pcbs) {
        tcp_pcb_t *p = container_of(node, tcp_pcb_t, node);
        if (p->local_port != local_port)
            continue;
        if (p->state == TCP_LISTEN) {
            if (!p->local_address || p->local_address == local_address)
                listener = p;
        } else if (p->remote_port == remote_port && p->remote_address == remote_address &&
                   p->local_address == local_address) {
            return p;
        }
    }
    return listener;
}

static void apply_peer_mss(tcp_pcb_t *pcb, uint16_t peer_mss)
{
    uint32_t mss = peer_mss ? peer_mss : TCP_DEFAULT_MSS;
    if (mss > our_mss(pcb))
        mss = our_mss(pcb);
    pcb->mss = mss < 64 ? 64 : mss;
    pcb->cwnd = 3 * pcb->mss; /* RFC 5681 initial window (RFC 3390 for MSS > 1095 would allow 3 too) */
    pcb->ssthresh = TCP_MAX_WINDOW;
}

static void listen_input(tcp_pcb_t *listener, uint32_t source, uint32_t destination, const uint8_t *header,
                         const segment_t *s)
{
    if (s->flags & FLAG_RST)
        return;
    if (s->flags & FLAG_ACK) {
        send_reset_for(source, destination, header, s);
        return;
    }
    if (!(s->flags & FLAG_SYN) || listener->children >= listener->backlog)
        return; /* a full backlog drops the SYN; the peer retries */

    tcp_pcb_t *child = tcp_pcb_create(NULL);
    if (!child)
        return;
    child->listener = listener;
    listener->children++;
    child->local_address = destination;
    child->local_port = listener->local_port;
    child->remote_address = source;
    child->remote_port = get_be16(header);
    child->interface = listener->interface;
    child->irs = s->seq;
    child->rcv_nxt = s->seq + 1;
    child->iss = new_iss();
    child->snd_una = child->iss;
    child->snd_nxt = child->snd_max = child->iss + 1;
    child->snd_wnd = s->window;
    child->snd_wl1 = s->seq;
    child->snd_wl2 = 0;
    apply_peer_mss(child, s->mss);
    child->state = TCP_SYN_RECEIVED;
    list_pcb(child);
    send_segment(child, child->iss, FLAG_SYN | FLAG_ACK, 0, 0);
    arm_retransmit(child);
}

static void syn_sent_input(tcp_pcb_t *pcb, uint32_t source, uint32_t destination, const uint8_t *header,
                           const segment_t *s)
{
    bool ack_ok = (s->flags & FLAG_ACK) && s->ack == pcb->iss + 1;

    if ((s->flags & FLAG_ACK) && !ack_ok) {
        send_reset_for(source, destination, header, s);
        return;
    }
    if (s->flags & FLAG_RST) {
        if (ack_ok)
            finish(pcb, STATUS_CONNECTION_REFUSED);
        return;
    }
    if (!(s->flags & FLAG_SYN))
        return;

    pcb->irs = s->seq;
    pcb->rcv_nxt = s->seq + 1;
    pcb->snd_wnd = s->window;
    pcb->snd_wl1 = s->seq;
    pcb->snd_wl2 = s->ack;
    apply_peer_mss(pcb, s->mss);
    if (ack_ok) {
        pcb->snd_una = s->ack;
        pcb->retransmit_at = 0;
        pcb->retries = 0;
        pcb->state = TCP_ESTABLISHED;
        send_ack(pcb);
        notify(pcb);
    } else {
        /* Simultaneous open */
        pcb->state = TCP_SYN_RECEIVED;
        send_segment(pcb, pcb->iss, FLAG_SYN | FLAG_ACK, 0, 0);
        arm_retransmit(pcb);
    }
}

/* Remove acknowledged bytes; returns true if the FIN was acknowledged with them. */
static bool process_ack(tcp_pcb_t *pcb, const segment_t *s)
{
    uint32_t acked = s->ack - pcb->snd_una;
    bool fin_acked = false;

    if (acked > pcb->send_length) {
        fin_acked = pcb->fin_queued;
        acked = pcb->send_length;
        pcb->snd_una = s->ack;
    } else {
        pcb->snd_una += acked;
    }
    pcb->send_start = (pcb->send_start + acked) % TCP_BUFFER_SIZE;
    pcb->send_length -= acked;
    if (SEQ_GT(pcb->snd_una, pcb->snd_nxt))
        pcb->snd_nxt = pcb->snd_una; /* ACK for data sent before a go-back-N */

    if (pcb->rtt_measuring && SEQ_GE(s->ack, pcb->rtt_seq)) {
        pcb->rtt_measuring = false;
        update_rtt(pcb, clock_monotonic_ns() - pcb->rtt_start);
    }
    /* Congestion control: leave fast recovery, else slow start below ssthresh, then about one MSS per RTT. */
    if (pcb->dup_acks >= 3)
        pcb->cwnd = pcb->ssthresh;
    else if (pcb->cwnd < pcb->ssthresh)
        pcb->cwnd += pcb->mss;
    else
        pcb->cwnd += pcb->mss * pcb->mss / pcb->cwnd + 1;
    if (pcb->cwnd > 4 * TCP_MAX_WINDOW)
        pcb->cwnd = 4 * TCP_MAX_WINDOW;

    pcb->retries = 0;
    pcb->dup_acks = 0;
    pcb->retransmit_at = 0;
    if (pcb->snd_una != pcb->snd_max)
        arm_retransmit(pcb);
    notify(pcb); /* room in the send buffer */
    return fin_acked;
}

static bool fin_is_acknowledged(const tcp_pcb_t *pcb)
{
    return pcb->fin_sent && SEQ_GT(pcb->snd_una, pcb->fin_seq);
}

static void enter_time_wait(tcp_pcb_t *pcb)
{
    pcb->state = TCP_TIME_WAIT;
    pcb->retransmit_at = 0;
    pcb->close_at = clock_monotonic_ns() + TCP_TIME_WAIT_NS;
    net_timer_request(TCP_TIME_WAIT_NS);
    notify(pcb);
}

static void synchronized_input(tcp_pcb_t *pcb, segment_t *s)
{
    uint32_t window = receive_window(pcb);
    uint32_t segment_length = s->length + ((s->flags & FLAG_SYN) ? 1 : 0) + ((s->flags & FLAG_FIN) ? 1 : 0);

    /* 1. Sequence check (RFC 793 page 69). */
    bool acceptable;
    if (segment_length == 0)
        acceptable = window == 0 ? s->seq == pcb->rcv_nxt
                                 : SEQ_GE(s->seq, pcb->rcv_nxt) && SEQ_LT(s->seq, pcb->rcv_nxt + window);
    else
        acceptable = window != 0 && ((SEQ_GE(s->seq, pcb->rcv_nxt) && SEQ_LT(s->seq, pcb->rcv_nxt + window)) ||
                                     (SEQ_GE(s->seq + segment_length - 1, pcb->rcv_nxt) &&
                                      SEQ_LT(s->seq + segment_length - 1, pcb->rcv_nxt + window)));
    if (!acceptable) {
        if (!(s->flags & FLAG_RST))
            send_ack(pcb);
        return;
    }

    /* 2. Reset */
    if (s->flags & FLAG_RST) {
        finish(pcb, pcb->state == TCP_SYN_RECEIVED && !pcb->socket ? STATUS_SUCCESS : STATUS_CONNECTION_RESET);
        return;
    }
    /* 3. A SYN in the window: challenge ACK (RFC 5961) */
    if (s->flags & FLAG_SYN) {
        send_ack(pcb);
        return;
    }
    /* 4. ACK */
    if (!(s->flags & FLAG_ACK))
        return;

    if (pcb->state == TCP_SYN_RECEIVED) {
        if (!SEQ_GT(s->ack, pcb->snd_una) || SEQ_GT(s->ack, pcb->snd_nxt)) {
            send_raw(pcb->local_address, pcb->local_port, pcb->remote_address, pcb->remote_port, s->ack, 0, FLAG_RST,
                     0, 0, NULL, 0, 0, pcb->interface);
            return;
        }
        pcb->snd_una = s->ack;
        pcb->retransmit_at = 0;
        pcb->retries = 0;
        pcb->snd_wnd = s->window;
        pcb->snd_wl1 = s->seq;
        pcb->snd_wl2 = s->ack;
        pcb->state = pcb->fin_queued ? TCP_FIN_WAIT_1 : TCP_ESTABLISHED;
        if (pcb->listener) {
            list_push_back(&pcb->listener->accept_queue, &pcb->accept_node);
            if (pcb->listener->socket)
                socket_notify(pcb->listener->socket);
        }
        notify(pcb);
    }

    bool fin_acked = false;
    if (SEQ_GT(s->ack, pcb->snd_max)) {
        send_ack(pcb); /* acknowledges something not yet sent */
        return;
    }
    if (SEQ_GT(s->ack, pcb->snd_una)) {
        fin_acked = process_ack(pcb, s);
    } else if (s->ack == pcb->snd_una && s->length == 0 && s->window == pcb->snd_wnd && s->window != 0 &&
               pcb->snd_max != pcb->snd_una && !(s->flags & FLAG_FIN)) {
        /* Duplicate ACK: fast retransmit after three (RFC 5681). */
        if (++pcb->dup_acks == 3) {
            uint32_t in_flight = pcb->snd_max - pcb->snd_una;
            pcb->ssthresh = in_flight / 2 > 2 * pcb->mss ? in_flight / 2 : 2 * pcb->mss;
            pcb->cwnd = pcb->ssthresh + 3 * pcb->mss;
            uint32_t length = pcb->send_length < pcb->mss ? pcb->send_length : pcb->mss;
            if (length)
                send_segment(pcb, pcb->snd_una, FLAG_ACK, 0, length);
            pcb->rtt_measuring = false;
        } else if (pcb->dup_acks > 3) {
            pcb->cwnd += pcb->mss;
        }
    }
    /* Window update (RFC 793: only from newer segments) */
    if (SEQ_LT(pcb->snd_wl1, s->seq) || (pcb->snd_wl1 == s->seq && SEQ_LE(pcb->snd_wl2, s->ack))) {
        pcb->snd_wnd = s->window;
        pcb->snd_wl1 = s->seq;
        pcb->snd_wl2 = s->ack;
    }

    fin_acked = fin_acked || fin_is_acknowledged(pcb);
    switch (pcb->state) {
    case TCP_FIN_WAIT_1:
        if (fin_acked) {
            pcb->state = TCP_FIN_WAIT_2;
            if (!pcb->socket) {
                pcb->close_at = clock_monotonic_ns() + TCP_ORPHAN_TIMEOUT;
                net_timer_request(TCP_ORPHAN_TIMEOUT);
            }
        }
        break;
    case TCP_CLOSING:
        if (fin_acked) {
            enter_time_wait(pcb);
            return;
        }
        break;
    case TCP_LAST_ACK:
        if (fin_acked) {
            finish(pcb, STATUS_SUCCESS);
            return;
        }
        break;
    case TCP_TIME_WAIT:
        if (s->flags & FLAG_FIN) { /* retransmitted FIN: ACK again and restart the timer */
            send_ack(pcb);
            enter_time_wait(pcb);
        }
        return;
    default:
        break;
    }

    /* 5. Data */
    bool need_ack = false;
    if (s->length && (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_FIN_WAIT_1 || pcb->state == TCP_FIN_WAIT_2)) {
        if (!pcb->socket && !pcb->listener) {
            /* Closed by the application: nobody will read this. */
            abort_connection(pcb, STATUS_CONNECTION_RESET);
            return;
        }
        if (s->seq != pcb->rcv_nxt && SEQ_LT(s->seq, pcb->rcv_nxt)) {
            uint32_t skip = pcb->rcv_nxt - s->seq; /* partly old data */
            s->data += skip;
            s->length -= skip;
            s->seq = pcb->rcv_nxt;
        }
        if (s->seq == pcb->rcv_nxt) {
            uint32_t take = s->length < receive_window(pcb) ? s->length : receive_window(pcb);
            for (uint32_t done = 0; done < take;) {
                uint32_t position = (pcb->receive_start + pcb->receive_length) % TCP_BUFFER_SIZE;
                uint32_t chunk = TCP_BUFFER_SIZE - position;
                if (chunk > take - done)
                    chunk = take - done;
                memcpy(pcb->receive_buffer + position, s->data + done, chunk);
                pcb->receive_length += chunk;
                done += chunk;
            }
            pcb->rcv_nxt += take;
            if (take < s->length)
                s->flags &= (uint8_t)~FLAG_FIN; /* the FIN lies beyond what we took */
            if (take)
                notify(pcb);
        } else {
            s->flags &= (uint8_t)~FLAG_FIN; /* out of order: dropped, duplicate ACK below */
        }
        need_ack = true;
    }

    /* 6. FIN */
    if ((s->flags & FLAG_FIN) && s->seq + s->length == pcb->rcv_nxt) {
        pcb->rcv_nxt++;
        pcb->fin_received = true;
        need_ack = true;
        switch (pcb->state) {
        case TCP_SYN_RECEIVED:
        case TCP_ESTABLISHED:
            pcb->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            if (fin_is_acknowledged(pcb)) {
                send_ack(pcb);
                enter_time_wait(pcb);
                return;
            }
            pcb->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2:
            send_ack(pcb);
            enter_time_wait(pcb);
            return;
        default:
            break;
        }
        notify(pcb);
    }

    if (need_ack)
        send_ack(pcb);
    output(pcb, 0);
}

void tcp_input(netif_t *netif, netbuf_t *packet, uint32_t source, uint32_t destination)
{
    segment_t s;

    if (!parse(packet, source, destination, &s) || destination == IPV4_BROADCAST) {
        netif->rx_dropped++;
        netbuf_free(packet);
        return;
    }
    const uint8_t *header = packet->data;
    tcp_pcb_t *pcb = lookup(destination, get_be16(header + 2), source, get_be16(header));

    if (!pcb)
        send_reset_for(source, destination, header, &s);
    else if (pcb->state == TCP_LISTEN)
        listen_input(pcb, source, destination, header, &s);
    else if (pcb->state == TCP_SYN_SENT)
        syn_sent_input(pcb, source, destination, header, &s);
    else
        synchronized_input(pcb, &s);
    netbuf_free(packet);
}

void tcp_error(uint32_t local_address, uint16_t local_port, uint32_t remote_address, uint16_t remote_port,
               status_t error)
{
    tcp_pcb_t *pcb = lookup(local_address, local_port, remote_address, remote_port);
    /* Hard errors only matter while connecting; later they are transient (RFC 1122 4.2.3.9). */
    if (pcb && pcb->state == TCP_SYN_SENT)
        finish(pcb, error);
}

/* --- Timers --------------------------------------------------------------------- */

/* Returns false if the connection was finished (the pcb may be gone). */
static bool retransmit(tcp_pcb_t *pcb, uint64_t now)
{
    if (pcb->state == TCP_SYN_SENT || pcb->state == TCP_SYN_RECEIVED) {
        if (++pcb->retries > TCP_SYN_RETRIES) {
            finish(pcb, STATUS_TIMEOUT);
            return false;
        }
        send_segment(pcb, pcb->iss, pcb->state == TCP_SYN_SENT ? FLAG_SYN : FLAG_SYN | FLAG_ACK, 0, 0);
    } else if (pcb->snd_wnd == 0 && pcb->send_length) {
        /* Zero-window probe: one byte beyond the window; never gives up while the peer answers. */
        pcb->snd_nxt = pcb->snd_una;
        output(pcb, 1);
    } else if (pcb->snd_max != pcb->snd_una) {
        if (++pcb->retries > TCP_MAX_RETRIES) {
            abort_connection(pcb, STATUS_TIMEOUT);
            return false;
        }
        uint32_t in_flight = pcb->snd_max - pcb->snd_una;
        pcb->ssthresh = in_flight / 2 > 2 * pcb->mss ? in_flight / 2 : 2 * pcb->mss;
        pcb->cwnd = pcb->mss;
        pcb->dup_acks = 0;
        pcb->rtt_measuring = false; /* Karn: no samples from retransmitted data */
        pcb->snd_nxt = pcb->snd_una; /* go back N */
        output(pcb, 0);
    } else {
        pcb->retransmit_at = 0;
        return true;
    }
    pcb->rto = pcb->rto * 2 > TCP_MAX_RTO ? TCP_MAX_RTO : pcb->rto * 2;
    pcb->retransmit_at = now + pcb->rto;
    return true;
}

void tcp_timer(uint64_t now)
{
    uint64_t next = WAIT_FOREVER;
    list_node_t *node, *following;

    for (node = pcbs.head.next; node != &pcbs.head; node = following) {
        following = node->next;
        tcp_pcb_t *pcb = container_of(node, tcp_pcb_t, node);

        if (pcb->close_at && now >= pcb->close_at &&
            (pcb->state == TCP_TIME_WAIT || (pcb->state == TCP_FIN_WAIT_2 && !pcb->socket))) {
            finish(pcb, STATUS_SUCCESS);
            continue;
        }
        if (pcb->retransmit_at && now >= pcb->retransmit_at && !retransmit(pcb, now))
            continue; /* finished, maybe freed */
        if (pcb->retransmit_at && pcb->retransmit_at < next)
            next = pcb->retransmit_at;
        if (pcb->close_at && pcb->close_at < next)
            next = pcb->close_at;
    }
    if (next != WAIT_FOREVER)
        net_timer_request(next > now ? next - now : 0);
}
