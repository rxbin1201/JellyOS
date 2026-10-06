/*
 * TCP (RFC 793, RFC 1122, RFC 5681, RFC 6298). All calls need net_lock.
 */

#ifndef NET_TCP_H
#define NET_TCP_H

#include "net/net.h"

#define TCP_BUFFER_SIZE 65536 /* send and receive buffer per connection (the window stays below 64 KiB) */

typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

struct socket;

typedef struct tcp_pcb {
    list_node_t      node;          /* all connections and listeners */
    struct socket   *socket;        /* NULL: not accepted yet, or closed by the application */
    struct tcp_pcb  *listener;      /* not accepted yet: the listening pcb */
    list_node_t      accept_node;   /* in the listener's accept queue */
    tcp_state_t      state;
    bool             listed;
    status_t         error;         /* why the connection ended abnormally */

    uint32_t         local_address, remote_address;
    uint16_t         local_port, remote_port;
    uint32_t         interface;

    /* send side: the buffer holds the bytes from snd_una on (sent and unsent) */
    uint32_t         iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2; /* snd_max: highest sent */
    uint8_t         *send_buffer;
    uint32_t         send_start, send_length;
    bool             fin_queued;     /* FIN follows the last byte in the buffer */
    bool             fin_sent;
    uint32_t         fin_seq;        /* sequence number of our FIN once sent */
    uint32_t         mss;
    uint32_t         cwnd, ssthresh;
    uint32_t         dup_acks;

    /* retransmission */
    uint64_t         rto, srtt, rttvar;
    bool             rtt_measuring;
    uint32_t         rtt_seq;
    uint64_t         rtt_start;
    uint64_t         retransmit_at;  /* 0: timer off */
    uint32_t         retries;
    uint64_t         close_at;       /* TIME_WAIT end, or orphaned FIN_WAIT_2 timeout */

    /* receive side */
    uint32_t         irs, rcv_nxt;
    uint8_t         *receive_buffer;
    uint32_t         receive_start, receive_length;
    uint32_t         advertised_window;
    bool             fin_received;

    /* listener */
    list_t           accept_queue;
    uint32_t         backlog, children;
} tcp_pcb_t;

tcp_pcb_t *tcp_pcb_create(struct socket *socket);
status_t   tcp_bind(tcp_pcb_t *pcb, uint32_t address, uint16_t port, bool reuse);
status_t   tcp_connect(tcp_pcb_t *pcb, uint32_t address, uint16_t port);
status_t   tcp_listen(tcp_pcb_t *pcb, uint32_t backlog);
tcp_pcb_t *tcp_accept(tcp_pcb_t *listener);

/* Copy into the send buffer and transmit; returns the bytes taken (0 when full). */
size_t     tcp_write(tcp_pcb_t *pcb, const void *data, size_t length);
size_t     tcp_read(tcp_pcb_t *pcb, void *buffer, size_t length, bool peek);
size_t     tcp_send_space(const tcp_pcb_t *pcb);
bool       tcp_can_send(const tcp_pcb_t *pcb);
void       tcp_shutdown_write(tcp_pcb_t *pcb);
/* The application closed its socket: finish (FIN) or reset the connection in the background. */
void       tcp_close(tcp_pcb_t *pcb);

void       tcp_input(netif_t *netif, netbuf_t *packet, uint32_t source, uint32_t destination);
void       tcp_timer(uint64_t now);
void       tcp_error(uint32_t local_address, uint16_t local_port, uint32_t remote_address, uint16_t remote_port,
                     status_t error);

const char *tcp_state_name(tcp_state_t state);
/* Number of live connection blocks (tests). */
uint32_t   tcp_pcb_count(void);

#endif
