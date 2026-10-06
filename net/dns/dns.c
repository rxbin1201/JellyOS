/*
 * DNS resolver (README section 32; RFC 1035).
 *
 * A stub resolver: one recursive query (RD set) for an A record to the name
 * server of the first configured interface (learned by DHCP), retried
 * DNS_ATTEMPTS times with DNS_TIMEOUT each. CNAME chains in the answer are
 * followed implicitly by taking the first A record. Answers are cached for
 * their TTL (at least 5 s, at most an hour); "no such name" is cached for 5 s.
 */

#include "net/dns/dns.h"
#include "net/net.h"
#include "net/sockets/socket.h"

#include "core/string.h"
#include "time/clock.h"

#define DNS_PORT          53
#define DNS_ATTEMPTS      3
#define DNS_TIMEOUT       2000000000ULL
#define DNS_CACHE_SIZE    32
#define DNS_NAME_MAX      253
#define DNS_MESSAGE_MAX   512
#define DNS_MIN_TTL       5ULL
#define DNS_MAX_TTL       3600ULL
#define DNS_NEGATIVE_TTL  5ULL

#define TYPE_A   1
#define CLASS_IN 1

typedef struct {
    char     name[DNS_NAME_MAX + 1];
    uint32_t address;            /* 0: negative entry (no such name) */
    uint64_t expires;
} cache_entry_t;

static mutex_t dns_lock;          /* cache and override; never held while waiting for the network */
static bool initialized;
static cache_entry_t cache[DNS_CACHE_SIZE];
static uint32_t override_address;
static uint16_t override_port;

static void init_once(void)
{
    if (!initialized) {
        mutex_init(&dns_lock);
        initialized = true;
    }
}

void dns_set_server_override(uint32_t address, uint16_t port)
{
    init_once();
    mutex_lock(&dns_lock);
    override_address = address;
    override_port = port ? port : DNS_PORT;
    mutex_unlock(&dns_lock);
}

void dns_cache_clear(void)
{
    init_once();
    mutex_lock(&dns_lock);
    memset(cache, 0, sizeof(cache));
    mutex_unlock(&dns_lock);
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* Lower-case copy without a trailing dot; false for invalid names. */
static bool normalize(const char *name, size_t length, char *out)
{
    if (length && name[length - 1] == '.')
        length--;
    if (length == 0 || length > DNS_NAME_MAX)
        return false;
    size_t label = 0;
    for (size_t i = 0; i < length; i++) {
        char c = lower(name[i]);
        if (c == '.') {
            if (label == 0)
                return false;
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            if (++label > 63)
                return false;
        } else {
            return false;
        }
        out[i] = c;
    }
    out[length] = '\0';
    return label != 0;
}

size_t dns_build_query(uint16_t id, const char *name, size_t length, uint8_t *buffer, size_t size)
{
    char normalized[DNS_NAME_MAX + 1];
    if (!normalize(name, length, normalized) || size < 12 + strlen(normalized) + 2 + 4)
        return 0;

    memset(buffer, 0, 12);
    put_be16(buffer, id);
    put_be16(buffer + 2, 0x0100); /* standard query, recursion desired */
    put_be16(buffer + 4, 1);      /* one question */
    size_t n = 12;
    const char *label = normalized;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t label_length = dot ? (size_t)(dot - label) : strlen(label);
        buffer[n++] = (uint8_t)label_length;
        memcpy(buffer + n, label, label_length);
        n += label_length;
        label += label_length + (dot ? 1 : 0);
    }
    buffer[n++] = 0;
    put_be16(buffer + n, TYPE_A);
    put_be16(buffer + n + 2, CLASS_IN);
    return n + 4;
}

/* Skip a possibly compressed name; returns the offset after it or 0. */
static size_t skip_name(const uint8_t *message, size_t length, size_t offset)
{
    while (offset < length) {
        uint8_t c = message[offset];
        if (c == 0)
            return offset + 1;
        if ((c & 0xC0) == 0xC0)
            return offset + 2 <= length ? offset + 2 : 0;
        if (c & 0xC0)
            return 0;
        offset += 1 + c;
    }
    return 0;
}

/* SUCCESS with the address, NOT_FOUND for NXDOMAIN or no A record, IO_ERROR for broken answers. */
static status_t parse_answer(const uint8_t *message, size_t length, uint16_t id, uint32_t *address, uint32_t *ttl)
{
    if (length < 12 || get_be16(message) != id || !(message[2] & 0x80))
        return STATUS_IO_ERROR;
    uint8_t rcode = message[3] & 0x0F;
    if (rcode == 3)
        return STATUS_NOT_FOUND;
    if (rcode != 0)
        return STATUS_IO_ERROR;

    uint16_t questions = get_be16(message + 4), answers = get_be16(message + 6);
    size_t offset = 12;
    for (uint16_t i = 0; i < questions; i++) {
        offset = skip_name(message, length, offset);
        if (!offset || offset + 4 > length)
            return STATUS_IO_ERROR;
        offset += 4;
    }
    for (uint16_t i = 0; i < answers; i++) {
        offset = skip_name(message, length, offset);
        if (!offset || offset + 10 > length)
            return STATUS_IO_ERROR;
        uint16_t type = get_be16(message + offset), class = get_be16(message + offset + 2);
        uint32_t record_ttl = get_be32(message + offset + 4);
        uint16_t data_length = get_be16(message + offset + 8);
        offset += 10;
        if (offset + data_length > length)
            return STATUS_IO_ERROR;
        if (type == TYPE_A && class == CLASS_IN && data_length == 4) {
            *address = get_be32(message + offset);
            *ttl = record_ttl;
            return STATUS_SUCCESS;
        }
        offset += data_length;
    }
    return STATUS_NOT_FOUND;
}

static bool cache_lookup(const char *name, uint32_t *address, status_t *status)
{
    uint64_t now = clock_monotonic_ns();
    bool found = false;
    mutex_lock(&dns_lock);
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (cache[i].name[0] && now < cache[i].expires && !strcmp(cache[i].name, name)) {
            *address = cache[i].address;
            *status = cache[i].address ? STATUS_SUCCESS : STATUS_NOT_FOUND;
            found = true;
            break;
        }
    }
    mutex_unlock(&dns_lock);
    return found;
}

static void cache_store(const char *name, uint32_t address, uint64_t ttl_seconds)
{
    uint64_t now = clock_monotonic_ns();
    if (ttl_seconds < DNS_MIN_TTL)
        ttl_seconds = DNS_MIN_TTL;
    if (ttl_seconds > DNS_MAX_TTL)
        ttl_seconds = DNS_MAX_TTL;

    mutex_lock(&dns_lock);
    cache_entry_t *slot = &cache[0];
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!strcmp(cache[i].name, name) || !cache[i].name[0] || cache[i].expires <= now) {
            slot = &cache[i];
            break;
        }
        if (cache[i].expires < slot->expires)
            slot = &cache[i];
    }
    strcpy(slot->name, name);
    slot->address = address;
    slot->expires = now + ttl_seconds * 1000000000ULL;
    mutex_unlock(&dns_lock);
}

static bool find_server(net_endpoint_t *server)
{
    mutex_lock(&dns_lock);
    server->address = override_address;
    server->port = override_port;
    mutex_unlock(&dns_lock);
    if (server->address)
        return true;

    mutex_lock(&net_lock);
    for (uint32_t i = 0; i < netif_count() && !server->address; i++) {
        netif_t *netif = netif_get(i);
        if (netif->address && netif->dns)
            server->address = netif->dns;
    }
    mutex_unlock(&net_lock);
    server->port = DNS_PORT;
    return server->address != 0;
}

static status_t query(const char *name, const net_endpoint_t *server, uint32_t *address, uint32_t *ttl)
{
    uint8_t request[DNS_MESSAGE_MAX], reply[DNS_MESSAGE_MAX];
    socket_t *s;

    status_t status = socket_create(JELLY_SOCK_DGRAM, JELLY_IPPROTO_UDP, &s);
    if (STATUS_IS_ERROR(status))
        return status;
    socket_set_option(s, JELLY_SO_RECEIVE_TIMEOUT, DNS_TIMEOUT);
    status = socket_connect(s, server);

    for (int attempt = 0; attempt < DNS_ATTEMPTS && !STATUS_IS_ERROR(status); attempt++) {
        uint16_t id = (uint16_t)(clock_monotonic_ns() >> 10) ^ (uint16_t)attempt;
        size_t length = dns_build_query(id, name, strlen(name), request, sizeof(request)), done;
        status = socket_send(s, request, length, NULL, 0, &done);
        if (STATUS_IS_ERROR(status))
            break;
        /* Skip stray answers (old IDs) until the timeout. */
        for (;;) {
            status = socket_receive(s, reply, sizeof(reply), NULL, 0, &done);
            if (STATUS_IS_ERROR(status))
                break;
            status = parse_answer(reply, done, id, address, ttl);
            if (status != STATUS_IO_ERROR || (done >= 2 && get_be16(reply) == id))
                break;
        }
        if (status != STATUS_TIMEOUT)
            break;
    }
    object_release(&s->object);
    return status;
}

status_t dns_resolve(const char *name, size_t length, uint32_t *address)
{
    char normalized[DNS_NAME_MAX + 1];
    net_endpoint_t server;
    status_t status;
    uint32_t ttl = 0;

    init_once();
    if (ipv4_parse(name, length, address))
        return STATUS_SUCCESS;
    if (!normalize(name, length, normalized))
        return STATUS_INVALID_ARGUMENT;
    if (!strcmp(normalized, "localhost")) {
        *address = IPV4_LOOPBACK;
        return STATUS_SUCCESS;
    }
    if (cache_lookup(normalized, address, &status))
        return status;
    if (!find_server(&server))
        return STATUS_UNREACHABLE;

    status = query(normalized, &server, address, &ttl);
    if (status == STATUS_SUCCESS)
        cache_store(normalized, *address, ttl);
    else if (status == STATUS_NOT_FOUND)
        cache_store(normalized, 0, DNS_NEGATIVE_TTL);
    return status;
}
