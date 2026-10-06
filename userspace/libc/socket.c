/*
 * libc: BSD sockets and the POSIX descriptor calls on JellyOS handles.
 */

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "internal.h"

/* STDIN_FILENO..STDERR_FILENO are the startup handles; real handles are always larger. */
static jelly_handle_t handle_of(int fd)
{
    if (fd >= 0 && fd <= 2)
        return jelly_startup_handle((unsigned)fd);
    return (jelly_handle_t)fd;
}

static int check_address(const struct sockaddr *address, socklen_t length)
{
    if (!address || length < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    if (address->sa_family != AF_INET) {
        errno = ENOSYS;
        return -1;
    }
    return 0;
}

static void put_address(const jelly_sockaddr_in_t *in, struct sockaddr *address, socklen_t *length)
{
    if (!address || !length)
        return;
    size_t n = *length < sizeof(*in) ? *length : sizeof(*in);
    memcpy(address, in, n);
    *length = sizeof(*in);
}

int socket(int domain, int type, int protocol)
{
    jelly_handle_t handle;
    status_t status = jelly_socket((uint32_t)domain, (uint32_t)type, (uint32_t)protocol, &handle);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : (int)handle;
}

int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    if (check_address(address, length))
        return -1;
    status_t status = jelly_bind(handle_of(fd), (const jelly_sockaddr_in_t *)address);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
    if (check_address(address, length))
        return -1;
    status_t status = jelly_connect(handle_of(fd), (const jelly_sockaddr_in_t *)address);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int listen(int fd, int backlog)
{
    status_t status = jelly_listen(handle_of(fd), backlog < 0 ? 0 : (uint32_t)backlog);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int accept(int fd, struct sockaddr *address, socklen_t *length)
{
    jelly_handle_t connection;
    jelly_sockaddr_in_t peer;
    status_t status = jelly_accept(handle_of(fd), &connection, &peer);
    if (STATUS_IS_ERROR(status))
        return __libc_fail(status);
    put_address(&peer, address, length);
    return (int)connection;
}

ssize_t sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t length)
{
    size_t done;
    if (to && check_address(to, length))
        return -1;
    status_t status = jelly_send(handle_of(fd), data, size, (const jelly_sockaddr_in_t *)to, (uint32_t)flags, &done);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : (ssize_t)done;
}

ssize_t send(int fd, const void *data, size_t size, int flags)
{
    return sendto(fd, data, size, flags, NULL, 0);
}

ssize_t recvfrom(int fd, void *buffer, size_t size, int flags, struct sockaddr *from, socklen_t *length)
{
    jelly_sockaddr_in_t source;
    size_t done;
    status_t status = jelly_receive(handle_of(fd), buffer, size, from ? &source : NULL, (uint32_t)flags, &done);
    if (STATUS_IS_ERROR(status))
        return __libc_fail(status);
    if (from)
        put_address(&source, from, length);
    return (ssize_t)done;
}

ssize_t recv(int fd, void *buffer, size_t size, int flags)
{
    return recvfrom(fd, buffer, size, flags, NULL, NULL);
}

int shutdown(int fd, int how)
{
    status_t status = jelly_shutdown(handle_of(fd), (uint32_t)how);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{
    uint64_t setting;

    if (level != SOL_SOCKET || !value) {
        errno = ENOSYS;
        return -1;
    }
    if (option == SO_RCVTIMEO || option == SO_SNDTIMEO) {
        if (length < sizeof(struct timeval)) {
            errno = EINVAL;
            return -1;
        }
        const struct timeval *tv = value;
        setting = (uint64_t)tv->tv_sec * 1000000000ULL + (uint64_t)tv->tv_usec * 1000ULL;
    } else {
        if (length < sizeof(int)) {
            errno = EINVAL;
            return -1;
        }
        setting = (uint64_t)*(const int *)value;
    }
    status_t status = jelly_socket_option(handle_of(fd), (uint32_t)option, setting);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

static int socket_name(int fd, struct sockaddr *address, socklen_t *length, int remote)
{
    jelly_socket_info_t info;
    status_t status = jelly_socket_info(handle_of(fd), &info);
    if (STATUS_IS_ERROR(status))
        return __libc_fail(status);
    if (remote && info.state != JELLY_SOCKET_STATE_CONNECTED && info.state != JELLY_SOCKET_STATE_CLOSING) {
        errno = STATUS_NOT_CONNECTED;
        return -1;
    }
    put_address(remote ? &info.remote : &info.local, address, length);
    return 0;
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length)
{
    return socket_name(fd, address, length, 0);
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length)
{
    return socket_name(fd, address, length, 1);
}

/* --- unistd ------------------------------------------------------------------------ */

ssize_t read(int fd, void *buffer, size_t size)
{
    size_t done;
    status_t status = jelly_read(handle_of(fd), buffer, size, &done);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : (ssize_t)done;
}

ssize_t write(int fd, const void *data, size_t size)
{
    size_t done;
    status_t status = jelly_write(handle_of(fd), data, size, &done);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : (ssize_t)done;
}

int close(int fd)
{
    status_t status = jelly_handle_close(handle_of(fd));
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

unsigned sleep(unsigned seconds)
{
    jelly_thread_sleep((uint64_t)seconds * 1000000000ULL);
    return 0;
}

int usleep(uint64_t microseconds)
{
    jelly_thread_sleep(microseconds * 1000ULL);
    return 0;
}

char *getcwd(char *buffer, size_t size)
{
    size_t length;
    status_t status = jelly_getcwd(buffer, size, &length);
    if (STATUS_IS_ERROR(status)) {
        errno = (int)status;
        return NULL;
    }
    return buffer;
}

int chdir(const char *path)
{
    status_t status = jelly_chdir(path);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int unlink(const char *path)
{
    status_t status = jelly_unlink(path);
    return STATUS_IS_ERROR(status) ? __libc_fail(status) : 0;
}

int rmdir(const char *path)
{
    return unlink(path);
}
