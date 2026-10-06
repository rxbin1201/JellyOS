/*
 * libc: IPv4 text conversion and host name resolution.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

int h_errno;

int inet_aton(const char *text, struct in_addr *address)
{
    uint32_t result = 0;
    const char *p = text;

    for (int part = 0; part < 4; part++) {
        uint32_t value = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            value = value * 10 + (uint32_t)(*p++ - '0');
            digits++;
        }
        if (digits == 0 || value > 255)
            return 0;
        result = result << 8 | value;
        if (part < 3 && *p++ != '.')
            return 0;
    }
    if (*p)
        return 0;
    address->s_addr = htonl(result);
    return 1;
}

in_addr_t inet_addr(const char *text)
{
    struct in_addr address;
    return inet_aton(text, &address) ? address.s_addr : INADDR_NONE;
}

const char *inet_ntop(int family, const void *address, char *buffer, socklen_t size)
{
    if (family != AF_INET) {
        errno = ENOSYS;
        return NULL;
    }
    uint32_t a = ntohl(((const struct in_addr *)address)->s_addr);
    if (snprintf(buffer, size, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF) >= (int)size) {
        errno = ERANGE;
        return NULL;
    }
    return buffer;
}

int inet_pton(int family, const char *text, void *address)
{
    if (family != AF_INET) {
        errno = ENOSYS;
        return -1;
    }
    return inet_aton(text, address);
}

char *inet_ntoa(struct in_addr address)
{
    static char buffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address, buffer, sizeof(buffer));
    return buffer;
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent entry;
    static char name_copy[256];
    static in_addr_t address;
    static char *addresses[2];
    static char *aliases[1];

    status_t status = jelly_net_resolve(name, &address);
    if (STATUS_IS_ERROR(status)) {
        h_errno = (int)status;
        errno = (int)status;
        return NULL;
    }
    snprintf(name_copy, sizeof(name_copy), "%s", name);
    addresses[0] = (char *)&address;
    addresses[1] = NULL;
    aliases[0] = NULL;
    entry.h_name = name_copy;
    entry.h_aliases = aliases;
    entry.h_addrtype = AF_INET;
    entry.h_length = 4;
    entry.h_addr_list = addresses;
    return &entry;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int flags = hints ? hints->ai_flags : 0;
    in_addr_t address = INADDR_ANY;
    long port = 0;

    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;
    if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
        return EAI_SOCKTYPE;
    if (!node && !service)
        return EAI_NONAME;
    if (service) {
        char *end;
        port = strtol(service, &end, 10);
        if (*end || port < 0 || port > 65535)
            return EAI_SERVICE; /* no services database */
    }
    if (node) {
        struct in_addr numeric;
        if (inet_aton(node, &numeric)) {
            address = numeric.s_addr;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else {
            status_t status = jelly_net_resolve(node, &address);
            if (status == STATUS_NOT_FOUND || status == STATUS_INVALID_ARGUMENT)
                return EAI_NONAME;
            if (status == STATUS_TIMEOUT || status == STATUS_UNREACHABLE)
                return EAI_AGAIN;
            if (STATUS_IS_ERROR(status))
                return EAI_FAIL;
        }
    } else if (!(flags & AI_PASSIVE)) {
        address = htonl(INADDR_LOOPBACK);
    }

    struct addrinfo *info = calloc(1, sizeof(*info) + sizeof(struct sockaddr_in));
    if (!info)
        return EAI_MEMORY;
    struct sockaddr_in *in = (struct sockaddr_in *)(info + 1);
    in->sin_family = AF_INET;
    in->sin_port = htons((uint16_t)port);
    in->sin_addr.s_addr = address;
    info->ai_family = AF_INET;
    info->ai_socktype = socktype ? socktype : SOCK_STREAM;
    info->ai_protocol = info->ai_socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    info->ai_addrlen = sizeof(*in);
    info->ai_addr = (struct sockaddr *)in;
    *result = info;
    return 0;
}

void freeaddrinfo(struct addrinfo *list)
{
    while (list) {
        struct addrinfo *next = list->ai_next;
        free(list);
        list = next;
    }
}

const char *gai_strerror(int error)
{
    switch (error) {
    case 0:            return "Success";
    case EAI_NONAME:   return "Name or service not known";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_FAIL:     return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:   return "Address family not supported";
    case EAI_SOCKTYPE: return "Socket type not supported";
    case EAI_SERVICE:  return "Service not supported";
    case EAI_MEMORY:   return "Out of memory";
    default:           return "Unknown error";
    }
}
