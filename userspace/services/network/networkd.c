/*
 * networkd: configures the network interfaces (README sections 28 and 29:
 * "Initialize networking", network services).
 *
 * /etc/network.conf (optional) selects the method per interface:
 *
 *     [interface eth0]
 *     method=dhcp            dhcp (default) | static | off
 *     address=10.0.2.15/24   static only
 *     gateway=10.0.2.2
 *     dns=10.0.2.3
 *
 * DHCP (RFC 2131): DISCOVER, OFFER, REQUEST, ACK over a broadcast UDP
 * socket bound to the interface (it has no address yet). The lease is
 * renewed at half its time; if renewing fails until it expires, the
 * address is removed and discovery starts over.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <jelly/os.h>

#define CONFIG_PATH     "/etc/network.conf"
#define MAX_INTERFACES  8
#define DHCP_SERVER     67
#define DHCP_CLIENT     68
#define DHCP_MAGIC      0x63825363u
#define MESSAGE_SIZE    576

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define OPT_SUBNET    1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_REQUESTED 50
#define OPT_LEASE     51
#define OPT_TYPE      53
#define OPT_SERVER    54
#define OPT_PARAMS    55
#define OPT_END       255

typedef enum { METHOD_DHCP, METHOD_STATIC, METHOD_OFF } method_t;

typedef struct {
    char     name[16];
    uint32_t index;
    uint8_t  mac[6];
    method_t method;
    uint32_t address, netmask, gateway, dns; /* network order (static or leased) */

    int      socket;
    uint32_t xid;
    uint32_t server;
    uint32_t lease_seconds;
    time_t   renew_at, expires_at;
    int      bound;
} interface_t;

static interface_t interfaces[MAX_INTERFACES];
static int interface_count;

static void log_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("networkd: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

static const char *text_of(uint32_t address)
{
    static char buffers[4][INET_ADDRSTRLEN];
    static int next;
    struct in_addr a = { address };
    char *buffer = buffers[next++ % 4];
    inet_ntop(AF_INET, &a, buffer, sizeof(buffer[0]) * INET_ADDRSTRLEN);
    return buffer;
}

static int prefix_of(uint32_t netmask)
{
    return __builtin_popcount(ntohl(netmask));
}

/* --- Configuration -------------------------------------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int parse_prefix(const char *text, uint32_t *address, uint32_t *netmask)
{
    char copy[32];
    snprintf(copy, sizeof(copy), "%s", text);
    char *slash = strchr(copy, '/');
    long prefix = 24;
    if (slash) {
        *slash = '\0';
        prefix = strtol(slash + 1, NULL, 10);
    }
    struct in_addr a;
    if (!inet_aton(copy, &a) || prefix < 1 || prefix > 32)
        return -1;
    *address = a.s_addr;
    *netmask = htonl(prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix));
    return 0;
}

static void load_config(void)
{
    FILE *file = fopen(CONFIG_PATH, "r");
    char line[256];
    interface_t *current = NULL;

    if (!file)
        return; /* everything DHCP */
    while (fgets(line, sizeof(line), file)) {
        char *text = trim(line);
        if (!*text || *text == '#')
            continue;
        if (!strncmp(text, "[interface ", 11)) {
            char *end = strchr(text, ']');
            current = NULL;
            if (!end)
                continue;
            *end = '\0';
            for (int i = 0; i < interface_count; i++) {
                if (!strcmp(interfaces[i].name, trim(text + 11)))
                    current = &interfaces[i];
            }
            continue;
        }
        char *equals = strchr(text, '=');
        if (!current || !equals)
            continue;
        *equals = '\0';
        char *key = trim(text), *value = trim(equals + 1);
        struct in_addr a;
        if (!strcmp(key, "method"))
            current->method = !strcmp(value, "static") ? METHOD_STATIC : !strcmp(value, "off") ? METHOD_OFF : METHOD_DHCP;
        else if (!strcmp(key, "address") && parse_prefix(value, &current->address, &current->netmask))
            log_message("%s: bad address '%s'", current->name, value);
        else if (!strcmp(key, "gateway") && inet_aton(value, &a))
            current->gateway = a.s_addr;
        else if (!strcmp(key, "dns") && inet_aton(value, &a))
            current->dns = a.s_addr;
    }
    fclose(file);
}

static int apply(interface_t *i, uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns)
{
    jelly_netif_config_t config = { address, netmask, gateway, dns };
    status_t status = jelly_net_configure(i->index, &config);
    if (STATUS_IS_ERROR(status)) {
        log_message("%s: cannot configure: %s", i->name, strerror((int)status));
        return -1;
    }
    return 0;
}

/* --- DHCP ------------------------------------------------------------------------ */

static size_t build_message(interface_t *i, uint8_t type, uint32_t requested, uint32_t server, uint32_t client,
                            uint8_t *m)
{
    memset(m, 0, MESSAGE_SIZE);
    m[0] = 1; /* BOOTREQUEST */
    m[1] = 1; /* Ethernet */
    m[2] = 6;
    memcpy(m + 4, &(uint32_t){ htonl(i->xid) }, 4);
    m[10] = 0x80; /* broadcast flag: we cannot receive unicast yet */
    memcpy(m + 12, &client, 4); /* ciaddr while renewing */
    memcpy(m + 28, i->mac, 6);
    uint32_t magic = htonl(DHCP_MAGIC);
    memcpy(m + 236, &magic, 4);

    size_t n = 240;
    m[n++] = OPT_TYPE;
    m[n++] = 1;
    m[n++] = type;
    if (requested) {
        m[n++] = OPT_REQUESTED;
        m[n++] = 4;
        memcpy(m + n, &requested, 4);
        n += 4;
    }
    if (server) {
        m[n++] = OPT_SERVER;
        m[n++] = 4;
        memcpy(m + n, &server, 4);
        n += 4;
    }
    static const uint8_t params[] = { OPT_SUBNET, OPT_ROUTER, OPT_DNS, OPT_LEASE };
    m[n++] = OPT_PARAMS;
    m[n++] = sizeof(params);
    memcpy(m + n, params, sizeof(params));
    n += sizeof(params);
    m[n++] = OPT_END;
    return n < 300 ? 300 : n; /* BOOTP minimum size */
}

typedef struct {
    uint8_t  type;
    uint32_t your_address, server, netmask, router, dns, lease;
} reply_t;

static int parse_reply(interface_t *i, const uint8_t *m, size_t length, reply_t *r)
{
    uint32_t xid, magic;
    if (length < 240 || m[0] != 2)
        return -1;
    memcpy(&xid, m + 4, 4);
    memcpy(&magic, m + 236, 4);
    if (ntohl(xid) != i->xid || ntohl(magic) != DHCP_MAGIC || memcmp(m + 28, i->mac, 6))
        return -1;

    memset(r, 0, sizeof(*r));
    memcpy(&r->your_address, m + 16, 4);
    for (size_t n = 240; n < length && m[n] != OPT_END;) {
        if (m[n] == 0) {
            n++;
            continue;
        }
        if (n + 2 > length || n + 2 + m[n + 1] > length)
            break;
        uint8_t option = m[n], size = m[n + 1];
        const uint8_t *value = m + n + 2;
        if (option == OPT_TYPE && size >= 1)
            r->type = value[0];
        else if (size >= 4) {
            uint32_t v;
            memcpy(&v, value, 4);
            if (option == OPT_SERVER) r->server = v;
            else if (option == OPT_SUBNET) r->netmask = v;
            else if (option == OPT_ROUTER) r->router = v;
            else if (option == OPT_DNS) r->dns = v;
            else if (option == OPT_LEASE) r->lease = ntohl(v);
        }
        n += 2 + size;
    }
    return r->type ? 0 : -1;
}

static int open_dhcp_socket(interface_t *i)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1, interface = (int)i->index + 1;
    struct timeval timeout = { 1, 0 };
    struct sockaddr_in local = { AF_INET, htons(DHCP_CLIENT), { INADDR_ANY }, { 0 } };

    if (s < 0)
        return -1;
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) ||
        setsockopt(s, SOL_SOCKET, SO_JELLY_INTERFACE, &interface, sizeof(interface)) ||
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        bind(s, (struct sockaddr *)&local, sizeof(local))) {
        close(s);
        return -1;
    }
    return s;
}

/* Send a message and wait (with retries) for a reply of one of the wanted types. */
static int exchange(interface_t *i, uint8_t type, uint32_t requested, uint32_t server, uint32_t client, reply_t *r,
                    int attempts)
{
    static uint8_t message[MESSAGE_SIZE];
    struct sockaddr_in to = { AF_INET, htons(DHCP_SERVER), { INADDR_BROADCAST }, { 0 } };

    for (int attempt = 0; attempt < attempts; attempt++) {
        size_t length = build_message(i, type, requested, server, client, message);
        if (sendto(i->socket, message, length, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
            return -1;
        /* Wait up to 1 s, 2 s, 4 s ... for an answer. */
        time_t deadline = time(NULL) + (1 << (attempt < 3 ? attempt : 3));
        while (time(NULL) <= deadline) {
            ssize_t n = recv(i->socket, message, sizeof(message), 0);
            if (n < 0 && errno != ETIMEDOUT)
                return -1;
            if (n > 0 && !parse_reply(i, message, (size_t)n, r)) {
                if ((type == DHCP_DISCOVER && r->type == DHCP_OFFER) ||
                    (type == DHCP_REQUEST && (r->type == DHCP_ACK || r->type == DHCP_NAK)))
                    return 0;
            }
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int bind_lease(interface_t *i, const reply_t *ack)
{
    uint32_t netmask = ack->netmask ? ack->netmask : htonl(0xFFFFFF00u);
    uint32_t gateway = ack->router;
    /* A gateway outside the subnet cannot be used. */
    if (gateway && (gateway & netmask) != (ack->your_address & netmask))
        gateway = 0;
    if (apply(i, ack->your_address, netmask, gateway, ack->dns))
        return -1;
    i->address = ack->your_address;
    i->netmask = netmask;
    i->gateway = gateway;
    i->dns = ack->dns;
    i->server = ack->server;
    i->lease_seconds = ack->lease ? ack->lease : 3600;
    i->renew_at = time(NULL) + i->lease_seconds / 2;
    i->expires_at = time(NULL) + i->lease_seconds;
    i->bound = 1;
    log_message("%s: %s/%d gateway %s dns %s (lease %u s from %s)", i->name, text_of(i->address),
                prefix_of(i->netmask), gateway ? text_of(gateway) : "none", ack->dns ? text_of(ack->dns) : "none",
                i->lease_seconds, text_of(i->server));
    return 0;
}

static int dhcp_acquire(interface_t *i)
{
    reply_t offer, ack;

    i->xid = (uint32_t)(jelly_clock_ns() ^ ((uint64_t)i->mac[5] << 24) ^ i->index);
    if (exchange(i, DHCP_DISCOVER, 0, 0, 0, &offer, 4)) {
        log_message("%s: no DHCP offer", i->name);
        return -1;
    }
    if (exchange(i, DHCP_REQUEST, offer.your_address, offer.server, 0, &ack, 3) || ack.type != DHCP_ACK) {
        log_message("%s: DHCP request for %s %s", i->name, text_of(offer.your_address),
                    ack.type == DHCP_NAK ? "refused" : "unanswered");
        return -1;
    }
    return bind_lease(i, &ack);
}

static void dhcp_renew(interface_t *i)
{
    reply_t ack;
    if (!exchange(i, DHCP_REQUEST, 0, 0, i->address, &ack, 2) && ack.type == DHCP_ACK) {
        bind_lease(i, &ack);
        return;
    }
    if (time(NULL) >= i->expires_at) {
        log_message("%s: lease expired", i->name);
        apply(i, 0, 0, 0, 0);
        i->bound = 0;
    } else {
        i->renew_at = time(NULL) + 30; /* try again later */
    }
}

/* --- Main ------------------------------------------------------------------------ */

static void discover_interfaces(void)
{
    jelly_netif_info_t info;
    for (uint32_t index = 0; interface_count < MAX_INTERFACES && !jelly_net_interface_info(index, &info); index++) {
        if (info.flags & JELLY_NETIF_LOOPBACK)
            continue;
        interface_t *i = &interfaces[interface_count++];
        memset(i, 0, sizeof(*i));
        snprintf(i->name, sizeof(i->name), "%s", info.name);
        i->index = info.index;
        memcpy(i->mac, info.mac, 6);
        i->method = METHOD_DHCP;
        i->socket = -1;
    }
}

int main(void)
{
    discover_interfaces();
    if (interface_count == 0) {
        log_message("no network interfaces");
        return 0;
    }
    load_config();

    int dhcp = 0;
    for (int n = 0; n < interface_count; n++) {
        interface_t *i = &interfaces[n];
        if (i->method == METHOD_OFF) {
            log_message("%s: disabled", i->name);
        } else if (i->method == METHOD_STATIC) {
            if (i->address && !apply(i, i->address, i->netmask, i->gateway, i->dns))
                log_message("%s: %s/%d (static)", i->name, text_of(i->address), prefix_of(i->netmask));
        } else {
            i->socket = open_dhcp_socket(i);
            if (i->socket < 0) {
                log_message("%s: cannot open the DHCP socket: %s", i->name, strerror(errno));
                continue;
            }
            dhcp++;
        }
    }
    if (dhcp == 0)
        return 0;

    /* Keep leases: acquire, renew at half time, start over when lost. */
    for (;;) {
        time_t next = time(NULL) + 3600;
        for (int n = 0; n < interface_count; n++) {
            interface_t *i = &interfaces[n];
            if (i->socket < 0)
                continue;
            if (!i->bound) {
                if (dhcp_acquire(i))
                    i->renew_at = time(NULL) + 10; /* retry discovery */
            } else if (time(NULL) >= i->renew_at) {
                dhcp_renew(i);
            }
            if (i->renew_at < next)
                next = i->renew_at;
        }
        time_t now = time(NULL);
        if (next > now)
            sleep((unsigned)(next - now));
    }
}
