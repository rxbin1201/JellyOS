/*
 * JellyOS libc: IPv4 address text conversion.
 */

#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>

in_addr_t   inet_addr(const char *text);                 /* INADDR_NONE on error */
int         inet_aton(const char *text, struct in_addr *address); /* 1 ok, 0 error */
char       *inet_ntoa(struct in_addr address);           /* static buffer */
int         inet_pton(int family, const char *text, void *address);
const char *inet_ntop(int family, const void *address, char *buffer, socklen_t size);

#endif
