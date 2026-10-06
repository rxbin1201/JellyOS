/*
 * JellyOS libc: a small POSIX subset on handles.
 *
 * A "file descriptor" is a JellyOS handle: files, pipes, the console and
 * sockets all work with read() and write(). 0, 1 and 2 are not handles:
 * use STDIN_FILENO etc., which map to the startup handles.
 */

#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t  read(int fd, void *buffer, size_t size);
ssize_t  write(int fd, const void *data, size_t size);
int      close(int fd);
unsigned sleep(unsigned seconds);
int      usleep(uint64_t microseconds);
char    *getcwd(char *buffer, size_t size);
int      chdir(const char *path);
int      unlink(const char *path);
int      rmdir(const char *path);

#endif
