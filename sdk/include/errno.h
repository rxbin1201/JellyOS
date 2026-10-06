/*
 * JellyOS libc: errno.
 *
 * errno holds JellyOS status codes (<jelly/status.h>), so every value has a
 * meaning shared with the kernel. The usual names map onto them.
 * errno is process-wide until thread-local storage exists.
 */

#ifndef _ERRNO_H
#define _ERRNO_H

#include <jelly/status.h>

extern int errno;

#define EINVAL    STATUS_INVALID_ARGUMENT
#define ENOENT    STATUS_NOT_FOUND
#define EACCES    STATUS_ACCESS_DENIED
#define EPERM     STATUS_ACCESS_DENIED
#define ENOMEM    STATUS_OUT_OF_MEMORY
#define EBUSY     STATUS_BUSY
#define ENOSYS    STATUS_NOT_SUPPORTED
#define EIO       STATUS_IO_ERROR
#define ETIMEDOUT STATUS_TIMEOUT
#define EAGAIN    STATUS_WOULD_BLOCK
#define ERANGE    STATUS_BUFFER_TOO_SMALL
#define EPIPE     STATUS_PEER_CLOSED
#define EBADF     STATUS_BAD_HANDLE
#define EMFILE    STATUS_LIMIT_EXCEEDED
#define EINTR     STATUS_INTERRUPTED
#define EEXIST    STATUS_ALREADY_EXISTS
#define ENOTDIR   STATUS_NOT_DIRECTORY
#define EISDIR    STATUS_IS_DIRECTORY
#define ENOTEMPTY STATUS_NOT_EMPTY
#define ENOSPC    STATUS_NO_SPACE
#define EWOULDBLOCK  EAGAIN
#define EINPROGRESS  EAGAIN   /* non-blocking connect: the connection is being set up */
#define ECONNREFUSED STATUS_CONNECTION_REFUSED
#define ECONNRESET   STATUS_CONNECTION_RESET
#define ENOTCONN     STATUS_NOT_CONNECTED
#define EADDRINUSE   STATUS_ADDRESS_IN_USE
#define EHOSTUNREACH STATUS_UNREACHABLE
#define ENETUNREACH  STATUS_UNREACHABLE
#define EMSGSIZE     STATUS_LIMIT_EXCEEDED
#define EISCONN      STATUS_ALREADY_EXISTS

#endif
