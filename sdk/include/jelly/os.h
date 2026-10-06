/*
 * libos: the native JellyOS system interface for userspace.
 *
 * Thin wrappers around the system calls in <jelly/syscall.h>. Every function
 * that can fail returns status_t; results come back through pointers.
 * Programs built on libc get main(argc, argv, envp) from the libc entry code.
 * Bare libos programs (the kernel test images) link userspace/libos/start.c and provide
 *     int main(uint64_t arg0, uint64_t arg1, uint64_t arg2);
 */

#ifndef JELLY_OS_H
#define JELLY_OS_H

#include <jelly/status.h>
#include <jelly/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Basics */
uint32_t jelly_abi_version(void);
status_t jelly_debug_write(const char *text, size_t length);
void     jelly_print(const char *text);
uint64_t jelly_clock_ns(void);

/* Processes and threads */
__attribute__((noreturn)) void jelly_process_exit(int32_t code);
status_t jelly_thread_create(void (*entry)(void *), void *arg, void *stack_top, jelly_handle_t *thread);
__attribute__((noreturn)) void jelly_thread_exit(void);
void     jelly_thread_yield(void);
status_t jelly_thread_sleep(uint64_t ns);

/* Memory */
status_t jelly_memory_allocate(size_t size, uint32_t flags, void **address);
status_t jelly_memory_unmap(void *address, size_t size);

/* Handles and waiting */
status_t jelly_handle_close(jelly_handle_t handle);
status_t jelly_handle_duplicate(jelly_handle_t handle, uint32_t rights, jelly_handle_t *copy);
status_t jelly_wait(jelly_handle_t handle, uint64_t timeout_ns);

/* Events */
status_t jelly_event_create(uint32_t flags, jelly_handle_t *event);
status_t jelly_event_signal(jelly_handle_t event);
status_t jelly_event_reset(jelly_handle_t event);

/* Channels */
status_t jelly_channel_create(jelly_handle_t *end0, jelly_handle_t *end1);
status_t jelly_channel_send(jelly_handle_t channel, const void *data, size_t size);
status_t jelly_channel_receive(jelly_handle_t channel, void *buffer, size_t size, size_t *actual);

/* Shared memory */
status_t jelly_shm_create(size_t size, jelly_handle_t *shm);
status_t jelly_shm_map(jelly_handle_t shm, uint32_t flags, void **address);

/* Futex */
status_t jelly_futex_wait(const uint32_t *word, uint32_t expected, uint64_t timeout_ns);
status_t jelly_futex_wake(const uint32_t *word, uint32_t count);

/* Files (paths are NUL-terminated here; relative paths use the working directory) */
status_t jelly_open(const char *path, uint32_t flags, uint32_t mode, jelly_handle_t *file);
status_t jelly_read(jelly_handle_t file, void *buffer, size_t size, size_t *done);
status_t jelly_write(jelly_handle_t file, const void *buffer, size_t size, size_t *done);
status_t jelly_seek(jelly_handle_t file, int64_t offset, uint32_t whence, uint64_t *position);
status_t jelly_truncate(jelly_handle_t file, uint64_t size);
status_t jelly_fstat(jelly_handle_t file, jelly_stat_t *stat);
status_t jelly_readdir(jelly_handle_t directory, jelly_dirent_t *entry);
status_t jelly_stat(const char *path, uint32_t flags, jelly_stat_t *stat);
status_t jelly_mkdir(const char *path, uint32_t mode);
status_t jelly_unlink(const char *path);
status_t jelly_rename(const char *from, const char *to);
status_t jelly_symlink(const char *target, const char *path);
status_t jelly_readlink(const char *path, char *buffer, size_t size, size_t *length);
status_t jelly_chdir(const char *path);
status_t jelly_getcwd(char *buffer, size_t size, size_t *length);
status_t jelly_sync(void);
status_t jelly_mount(const char *path, const char *device, const char *type);
status_t jelly_unmount(const char *path);

/* Programs, pipes and power (ABI version 3) */
status_t jelly_spawn(const jelly_spawn_t *request, jelly_handle_t *process);
status_t jelly_process_info(jelly_handle_t process, jelly_process_info_t *info);
status_t jelly_process_kill(jelly_handle_t process, int32_t code);
status_t jelly_pipe_create(jelly_handle_t *read_end, jelly_handle_t *write_end);
status_t jelly_system_power(uint32_t action);

/* Networking (ABI version 4); addresses in network byte order */
status_t jelly_socket(uint32_t domain, uint32_t type, uint32_t protocol, jelly_handle_t *socket);
status_t jelly_bind(jelly_handle_t socket, const jelly_sockaddr_in_t *address);
status_t jelly_connect(jelly_handle_t socket, const jelly_sockaddr_in_t *address);
status_t jelly_listen(jelly_handle_t socket, uint32_t backlog);
status_t jelly_accept(jelly_handle_t socket, jelly_handle_t *connection, jelly_sockaddr_in_t *peer);
status_t jelly_send(jelly_handle_t socket, const void *data, size_t size, const jelly_sockaddr_in_t *to,
                    uint32_t flags, size_t *done);
status_t jelly_receive(jelly_handle_t socket, void *buffer, size_t size, jelly_sockaddr_in_t *from, uint32_t flags,
                       size_t *done);
status_t jelly_shutdown(jelly_handle_t socket, uint32_t how);
status_t jelly_socket_option(jelly_handle_t socket, uint32_t option, uint64_t value);
status_t jelly_socket_info(jelly_handle_t socket, jelly_socket_info_t *info);
status_t jelly_net_interface_info(uint32_t index, jelly_netif_info_t *info);
status_t jelly_net_configure(uint32_t index, const jelly_netif_config_t *config);
status_t jelly_net_resolve(const char *name, uint32_t *address);

/* Startup handle slot `index` of a libc program (JELLY_HANDLE_INVALID if absent).
   Provided by the libc entry code, not by libos. */
jelly_handle_t jelly_startup_handle(unsigned index);

/* Raw system call */
uint64_t jelly_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5);

#endif
