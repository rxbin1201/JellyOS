/*
 * JellyOS libc: BSD sockets on JellyOS socket handles.
 *
 * A socket descriptor is the JellyOS handle (always a positive int), so it
 * also works with read(), write(), close() and the jelly_* functions.
 * Only IPv4 (AF_INET) exists. Functions return -1 and set errno on failure.
 */

#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/types.h>

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

#define AF_UNSPEC    0
#define AF_INET      2
#define PF_INET      AF_INET

#define SOCK_STREAM  1
#define SOCK_DGRAM   2

#define SOL_SOCKET   1
#define SO_RCVTIMEO  1   /* struct timeval */
#define SO_SNDTIMEO  2   /* struct timeval */
#define SO_BROADCAST 4   /* int */
#define SO_REUSEADDR 5   /* int */
#define SO_JELLY_INTERFACE 6 /* int: interface index + 1, 0 = any (JellyOS) */

#define MSG_PEEK     1
#define MSG_DONTWAIT 2

#define SHUT_RD      0
#define SHUT_WR      1
#define SHUT_RDWR    2

int     socket(int domain, int type, int protocol);
int     bind(int socket, const struct sockaddr *address, socklen_t length);
int     connect(int socket, const struct sockaddr *address, socklen_t length);
int     listen(int socket, int backlog);
int     accept(int socket, struct sockaddr *address, socklen_t *length);
ssize_t send(int socket, const void *data, size_t size, int flags);
ssize_t recv(int socket, void *buffer, size_t size, int flags);
ssize_t sendto(int socket, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t length);
ssize_t recvfrom(int socket, void *buffer, size_t size, int flags, struct sockaddr *from, socklen_t *length);
int     shutdown(int socket, int how);
int     setsockopt(int socket, int level, int option, const void *value, socklen_t length);
int     getsockname(int socket, struct sockaddr *address, socklen_t *length);
int     getpeername(int socket, struct sockaddr *address, socklen_t *length);

#endif
