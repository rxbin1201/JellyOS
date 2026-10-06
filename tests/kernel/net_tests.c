/*
 * Kernel tests for the network stack (Phase 8).
 *
 * Most tests run over the loopback interface, so they need no hardware.
 * net_virtio_reaches_the_gateway uses the VirtIO NIC that `make test`
 * attaches with QEMU user networking (gateway 10.0.2.2), if present.
 */

#include "tests/kernel/ktest.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "net/arp/arp.h"
#include "net/dns/dns.h"
#include "net/net.h"
#include "net/sockets/socket.h"
#include "net/tcp/tcp.h"
#include "scheduler/thread.h"
#include "time/clock.h"

#define SECOND 1000000000ULL

static net_endpoint_t endpoint(uint32_t address, uint16_t port)
{
    net_endpoint_t e = { address, port };
    return e;
}

static socket_t *open_socket(uint32_t type, uint32_t protocol)
{
    socket_t *s = NULL;
    if (STATUS_IS_ERROR(socket_create(type, protocol, &s)))
        return NULL;
    socket_set_option(s, JELLY_SO_RECEIVE_TIMEOUT, 5 * SECOND);
    socket_set_option(s, JELLY_SO_SEND_TIMEOUT, 5 * SECOND);
    return s;
}

static void close_socket(socket_t *s)
{
    if (s)
        object_release(&s->object);
}

/* Run fn(arg) in a kernel thread; returns the thread (wait with object_wait). */
static thread_t *run_thread(const char *name, void (*fn)(void *), void *arg)
{
    thread_t *thread;
    if (STATUS_IS_ERROR(thread_create_kernel(name, fn, arg, THREAD_PRIORITY_KERNEL, &thread)))
        return NULL;
    thread_start(thread);
    return thread;
}

static bool join(thread_t *thread)
{
    bool done = thread && object_wait(&thread->object, 30 * SECOND) == STATUS_SUCCESS;
    if (thread)
        object_release(&thread->object);
    return done;
}

/* --- Basics -------------------------------------------------------------------------- */

KTEST(net_checksum_and_addresses)
{
    /* RFC 1071 example: the sum of 0001 f203 f4f5 f6f7 is ddf2. */
    static const uint8_t data[] = { 0x00, 0x01, 0xF2, 0x03, 0xF4, 0xF5, 0xF6, 0xF7 };
    KEXPECT(checksum_add(0, data, sizeof(data)) == 0xDDF2);
    KEXPECT(checksum_finish(checksum_add(0, data, sizeof(data))) == 0x220D);
    /* Odd lengths pad with a zero byte. */
    KEXPECT(checksum_add(0, data, 3) == 0x0001 + 0xF200);

    uint32_t address;
    char text[16];
    KEXPECT(ipv4_parse("10.0.2.15", 9, &address) && address == IPV4(10, 0, 2, 15));
    KEXPECT(!strcmp(ipv4_format(IPV4(192, 168, 100, 1), text), "192.168.100.1"));
    KEXPECT(!ipv4_parse("10.0.2", 6, &address));
    KEXPECT(!ipv4_parse("256.1.1.1", 9, &address));
    KEXPECT(!ipv4_parse("1.2.3.4x", 8, &address));
    KEXPECT(!ipv4_parse("example.com", 11, &address));
}

/* --- ICMP and UDP over loopback ----------------------------------------------------- */

KTEST(net_loopback_ping)
{
    socket_t *s = open_socket(JELLY_SOCK_DGRAM, JELLY_IPPROTO_ICMP);
    uint8_t request[40], reply[64];
    net_endpoint_t to = endpoint(IPV4_LOOPBACK, 0), from;
    size_t done;
    KASSERT(s != NULL);

    memset(request, 0, sizeof(request));
    request[0] = 8; /* echo request */
    put_be16(request + 6, 7);
    memcpy(request + 8, "JellyOS ping payload", 20);
    KEXPECT(socket_send(s, request, 28, &to, 0, &done) == STATUS_SUCCESS && done == 28);
    KASSERT(socket_receive(s, reply, sizeof(reply), &from, 0, &done) == STATUS_SUCCESS);
    KEXPECT(done == 28 && reply[0] == 0 /* echo reply */ && get_be16(reply + 6) == 7);
    KEXPECT(!memcmp(reply + 8, "JellyOS ping payload", 20));
    KEXPECT(from.address == IPV4_LOOPBACK);
    /* The kernel sets the identifier to the socket's port. */
    KEXPECT(get_be16(reply + 4) == s->local.port);

    /* Only echo requests may be sent. */
    request[0] = 0;
    KEXPECT(socket_send(s, request, 28, &to, 0, &done) == STATUS_INVALID_ARGUMENT);
    close_socket(s);
}

KTEST(net_udp_loopback)
{
    socket_t *a = open_socket(JELLY_SOCK_DGRAM, 0), *b = open_socket(JELLY_SOCK_DGRAM, 0);
    net_endpoint_t a_address = endpoint(IPV4_LOOPBACK, 7000), from;
    char buffer[32];
    size_t done;
    KASSERT(a && b);

    KEXPECT(socket_bind(a, &a_address) == STATUS_SUCCESS);
    KEXPECT(socket_bind(b, &a_address) == STATUS_ADDRESS_IN_USE);
    KEXPECT(socket_send(b, "hello", 5, &a_address, 0, &done) == STATUS_SUCCESS && done == 5);
    KASSERT(socket_receive(a, buffer, sizeof(buffer), &from, 0, &done) == STATUS_SUCCESS);
    KEXPECT(done == 5 && !memcmp(buffer, "hello", 5));
    KEXPECT(from.address == IPV4_LOOPBACK && from.port == b->local.port && from.port >= 49152);

    /* Reply to the sender, peek first. */
    KEXPECT(socket_send(a, "world!", 6, &from, 0, &done) == STATUS_SUCCESS);
    KEXPECT(socket_receive(b, buffer, sizeof(buffer), NULL, JELLY_MSG_PEEK, &done) == STATUS_SUCCESS && done == 6);
    KEXPECT(socket_receive(b, buffer, 3, NULL, 0, &done) == STATUS_SUCCESS && done == 3 && !memcmp(buffer, "wor", 3));
    /* The rest of the datagram was discarded. */
    KEXPECT(socket_receive(b, buffer, sizeof(buffer), NULL, JELLY_MSG_DONTWAIT, &done) == STATUS_WOULD_BLOCK);

    /* Broadcasts need the option. */
    net_endpoint_t broadcast = endpoint(IPV4_BROADCAST, 7000);
    KEXPECT(socket_send(b, "x", 1, &broadcast, 0, &done) == STATUS_ACCESS_DENIED);

    /* A connected socket learns about a closed port (ICMP port unreachable). */
    net_endpoint_t closed = endpoint(IPV4_LOOPBACK, 7001);
    KEXPECT(socket_connect(b, &closed) == STATUS_SUCCESS);
    KEXPECT(socket_send(b, "anyone?", 7, NULL, 0, &done) == STATUS_SUCCESS);
    KEXPECT(socket_receive(b, buffer, sizeof(buffer), NULL, 0, &done) == STATUS_CONNECTION_REFUSED);

    /* Receive timeout */
    socket_set_option(a, JELLY_SO_RECEIVE_TIMEOUT, SECOND / 10);
    KEXPECT(socket_receive(a, buffer, sizeof(buffer), NULL, 0, &done) == STATUS_TIMEOUT);
    close_socket(a);
    close_socket(b);
}

/* --- TCP over loopback ------------------------------------------------------------- */

static socket_t *tcp_listener(uint16_t port)
{
    socket_t *s = open_socket(JELLY_SOCK_STREAM, 0);
    net_endpoint_t local = endpoint(IPV4_LOOPBACK, port);
    if (!s)
        return NULL;
    socket_set_option(s, JELLY_SO_REUSE_ADDRESS, 1);
    if (STATUS_IS_ERROR(socket_bind(s, &local)) || STATUS_IS_ERROR(socket_listen(s, 4))) {
        close_socket(s);
        return NULL;
    }
    return s;
}

static size_t receive_exactly(socket_t *s, void *buffer, size_t size)
{
    size_t total = 0, done;
    while (total < size && socket_receive(s, (uint8_t *)buffer + total, size - total, NULL, 0, &done) == STATUS_SUCCESS &&
           done)
        total += done;
    return total;
}

KTEST(net_tcp_loopback_connection)
{
    socket_t *listener = tcp_listener(8000), *client = open_socket(JELLY_SOCK_STREAM, 0), *server = NULL;
    net_endpoint_t remote = endpoint(IPV4_LOOPBACK, 8000), peer;
    jelly_socket_info_t info;
    char buffer[64];
    size_t done;
    KASSERT(listener && client);

    KASSERT(socket_connect(client, &remote) == STATUS_SUCCESS);
    socket_info(client, &info);
    KEXPECT(info.state == JELLY_SOCKET_STATE_CONNECTED);
    KASSERT(socket_accept(listener, &server, &peer) == STATUS_SUCCESS);
    KEXPECT(peer.port == client->local.port && peer.address == IPV4_LOOPBACK);

    KEXPECT(socket_send(client, "ping over tcp", 13, NULL, 0, &done) == STATUS_SUCCESS && done == 13);
    KEXPECT(receive_exactly(server, buffer, 13) == 13 && !memcmp(buffer, "ping over tcp", 13));
    KEXPECT(socket_send(server, "pong", 4, NULL, 0, &done) == STATUS_SUCCESS);
    KEXPECT(receive_exactly(client, buffer, 4) == 4 && !memcmp(buffer, "pong", 4));

    /* Half close: the server reads end of stream but can still send. */
    KEXPECT(socket_shutdown(client, JELLY_SHUT_WRITE) == STATUS_SUCCESS);
    KEXPECT(socket_receive(server, buffer, sizeof(buffer), NULL, 0, &done) == STATUS_SUCCESS && done == 0);
    KEXPECT(socket_send(server, "late", 4, NULL, 0, &done) == STATUS_SUCCESS);
    KEXPECT(receive_exactly(client, buffer, 4) == 4 && !memcmp(buffer, "late", 4));
    KEXPECT(socket_send(client, "x", 1, NULL, 0, &done) == STATUS_PEER_CLOSED);

    /* Server closes: the client sees end of stream. */
    close_socket(server);
    KEXPECT(socket_receive(client, buffer, sizeof(buffer), NULL, 0, &done) == STATUS_SUCCESS && done == 0);
    close_socket(client);
    close_socket(listener);
}

KTEST(net_tcp_connection_refused)
{
    socket_t *s = open_socket(JELLY_SOCK_STREAM, 0);
    net_endpoint_t nobody = endpoint(IPV4_LOOPBACK, 9);
    char c;
    size_t done;
    KASSERT(s != NULL);
    KEXPECT(socket_connect(s, &nobody) == STATUS_CONNECTION_REFUSED);
    KEXPECT(socket_receive(s, &c, 1, NULL, 0, &done) == STATUS_NOT_CONNECTED);
    close_socket(s);
}

#define BULK_SIZE (1024 * 1024)

typedef struct {
    socket_t *listener;
    size_t    received;
    bool      pattern_ok;
} bulk_server_t;

static uint8_t bulk_byte(size_t i)
{
    return (uint8_t)(i * 31 + (i >> 11));
}

static void bulk_server(void *arg)
{
    bulk_server_t *b = arg;
    socket_t *connection;
    uint8_t *buffer = kmalloc(8192);
    size_t done;

    b->pattern_ok = true;
    if (buffer && socket_accept(b->listener, &connection, NULL) == STATUS_SUCCESS) {
        /* Read slowly at first so the sender meets a full window. */
        while (socket_receive(connection, buffer, b->received < 200000 ? 1000 : 8192, NULL, 0, &done) ==
                   STATUS_SUCCESS &&
               done) {
            for (size_t i = 0; i < done; i++) {
                if (buffer[i] != bulk_byte(b->received + i))
                    b->pattern_ok = false;
            }
            b->received += done;
        }
        socket_send(connection, "done", 4, NULL, 0, &done);
        close_socket(connection);
    }
    kfree(buffer);
    thread_exit();
}

KTEST(net_tcp_bulk_transfer)
{
    bulk_server_t b = { tcp_listener(8001), 0, false };
    socket_t *client = open_socket(JELLY_SOCK_STREAM, 0);
    net_endpoint_t remote = endpoint(IPV4_LOOPBACK, 8001);
    uint8_t *data = kmalloc(BULK_SIZE);
    char reply[8];
    size_t done;
    KASSERT(b.listener && client && data);

    for (size_t i = 0; i < BULK_SIZE; i++)
        data[i] = bulk_byte(i);
    thread_t *server = run_thread("bulk-server", bulk_server, &b);
    KASSERT(server != NULL);

    uint64_t start = clock_monotonic_ns();
    KASSERT(socket_connect(client, &remote) == STATUS_SUCCESS);
    KEXPECT(socket_send(client, data, BULK_SIZE, NULL, 0, &done) == STATUS_SUCCESS && done == BULK_SIZE);
    KEXPECT(socket_shutdown(client, JELLY_SHUT_WRITE) == STATUS_SUCCESS);
    KEXPECT(receive_exactly(client, reply, 4) == 4 && !memcmp(reply, "done", 4));
    KEXPECT(join(server));
    uint64_t ms = (clock_monotonic_ns() - start) / 1000000;

    KEXPECT(b.received == BULK_SIZE);
    KEXPECT(b.pattern_ok);
    klog_info("ktest: 1 MiB over loopback TCP in %lu ms", ms);
    close_socket(client);
    close_socket(b.listener);
    kfree(data);
}

/* --- DNS ------------------------------------------------------------------------------ */

typedef struct {
    socket_t *socket;
    unsigned  queries;
} dns_server_t;

/* Answers A queries: "nx.test" with NXDOMAIN, everything else with 10.1.2.3. */
static void fake_dns_server(void *arg)
{
    dns_server_t *d = arg;
    uint8_t message[512];
    net_endpoint_t from;
    size_t length, done;

    while (socket_receive(d->socket, message, sizeof(message), &from, 0, &length) == STATUS_SUCCESS &&
           length > 12) {
        d->queries++;
        bool nx = length >= 12 + 9 && !memcmp(message + 12, "\x02nx\x04test", 9);
        message[2] = 0x81;               /* response, RD */
        message[3] = nx ? 0x83 : 0x80;   /* RA, rcode */
        put_be16(message + 6, nx ? 0 : 1);
        if (!nx) {
            static const uint8_t answer[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 1, 2, 3 };
            memcpy(message + length, answer, sizeof(answer));
            length += sizeof(answer);
        }
        socket_send(d->socket, message, length, &from, 0, &done);
    }
    thread_exit();
}

KTEST(net_dns_resolver)
{
    dns_server_t d = { open_socket(JELLY_SOCK_DGRAM, 0), 0 };
    net_endpoint_t local = endpoint(IPV4_LOOPBACK, 5353);
    uint32_t address = 0;
    KASSERT(d.socket != NULL);
    KASSERT(socket_bind(d.socket, &local) == STATUS_SUCCESS);
    socket_set_option(d.socket, JELLY_SO_RECEIVE_TIMEOUT, SECOND);
    thread_t *server = run_thread("fake-dns", fake_dns_server, &d);
    KASSERT(server != NULL);

    dns_set_server_override(IPV4_LOOPBACK, 5353);
    dns_cache_clear();
    KEXPECT(dns_resolve("Host.Example.TEST.", 18, &address) == STATUS_SUCCESS && address == IPV4(10, 1, 2, 3));
    address = 0;
    KEXPECT(dns_resolve("host.example.test", 17, &address) == STATUS_SUCCESS && address == IPV4(10, 1, 2, 3));
    KEXPECT(d.queries == 1); /* the second answer came from the cache */
    KEXPECT(dns_resolve("nx.test", 7, &address) == STATUS_NOT_FOUND);
    KEXPECT(dns_resolve("nx.test", 7, &address) == STATUS_NOT_FOUND && d.queries == 2);
    KEXPECT(dns_resolve("192.0.2.7", 9, &address) == STATUS_SUCCESS && address == IPV4(192, 0, 2, 7));
    KEXPECT(dns_resolve("localhost", 9, &address) == STATUS_SUCCESS && address == IPV4_LOOPBACK);
    KEXPECT(dns_resolve("bad..name", 9, &address) == STATUS_INVALID_ARGUMENT);
    KEXPECT(dns_resolve("under_score-ok.test", 19, &address) == STATUS_SUCCESS);

    uint8_t query[64];
    size_t n = dns_build_query(0x1234, "a.bc", 4, query, sizeof(query));
    KEXPECT(n == 12 + 6 + 4 && !memcmp(query + 12, "\x01" "a\x02" "bc\x00", 6));

    dns_set_server_override(0, 0);
    dns_cache_clear();
    KEXPECT(join(server));
    close_socket(d.socket);
}

/* --- VirtIO NIC against QEMU's user network ------------------------------------------ */

KTEST(net_virtio_reaches_the_gateway)
{
    netif_t *eth = NULL;
    mutex_lock(&net_lock);
    for (uint32_t i = 0; i < netif_count(); i++) {
        if (!netif_get(i)->loopback)
            eth = netif_get(i);
    }
    if (eth)
        netif_configure(eth, IPV4(10, 0, 2, 15), IPV4(255, 255, 255, 0), IPV4(10, 0, 2, 2), IPV4(10, 0, 2, 3));
    mutex_unlock(&net_lock);
    if (!eth) {
        klog_info("ktest: no Ethernet interface, skipping");
        return;
    }

    /* Ping the gateway: resolves its MAC with ARP first. */
    socket_t *s = open_socket(JELLY_SOCK_DGRAM, JELLY_IPPROTO_ICMP);
    uint8_t request[16] = { 8 }, reply[64];
    net_endpoint_t gateway = endpoint(IPV4(10, 0, 2, 2), 0);
    uint8_t mac[6];
    size_t done;
    KASSERT(s != NULL);
    KEXPECT(socket_send(s, request, sizeof(request), &gateway, 0, &done) == STATUS_SUCCESS);
    KEXPECT(socket_receive(s, reply, sizeof(reply), NULL, 0, &done) == STATUS_SUCCESS && reply[0] == 0);
    mutex_lock(&net_lock);
    KEXPECT(arp_lookup(eth, IPV4(10, 0, 2, 2), mac));
    mutex_unlock(&net_lock);
    close_socket(s);

    /* TCP to a host port without a server: the user network answers with a reset. */
    socket_t *t = open_socket(JELLY_SOCK_STREAM, 0);
    net_endpoint_t closed = endpoint(IPV4(10, 0, 2, 2), 1);
    KASSERT(t != NULL);
    status_t status = socket_connect(t, &closed);
    KEXPECT(status == STATUS_CONNECTION_REFUSED);
    close_socket(t);

    KEXPECT(eth->rx_packets > 0 && eth->tx_packets > 0);
    mutex_lock(&net_lock);
    netif_configure(eth, 0, 0, 0, 0);
    mutex_unlock(&net_lock);
}
