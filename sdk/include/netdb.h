/*
 * JellyOS libc: host name resolution (through the kernel's DNS resolver,
 * SYS_NET_RESOLVE). IPv4 only.
 */

#ifndef _NETDB_H
#define _NETDB_H

#include <netinet/in.h>

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     1
#define AI_NUMERICHOST 4

#define EAI_NONAME  -2
#define EAI_AGAIN   -3
#define EAI_FAIL    -4
#define EAI_FAMILY  -6
#define EAI_SOCKTYPE -7
#define EAI_SERVICE -8
#define EAI_MEMORY  -10
#define EAI_SYSTEM  -11

extern int h_errno;

struct hostent *gethostbyname(const char *name); /* static result */
int             getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                            struct addrinfo **result);
void            freeaddrinfo(struct addrinfo *list);
const char     *gai_strerror(int error);

#endif
