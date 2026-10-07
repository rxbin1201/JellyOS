/*
 * Helpers and handlers shared between the system call source files.
 */

#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include "core/handle.h"

#include <jelly/status.h>
#include <stdint.h>

handle_table_t *syscall_handles(void);

/* Install object into the caller's table and report the handle; consumes the caller's reference. */
status_t syscall_give_handle(object_t *object, uint32_t rights, uint64_t user_out);

/* File system calls (fs_syscalls.c) */
status_t sys_file_open(const uint64_t *a);
status_t sys_file_read(const uint64_t *a);
status_t sys_file_write(const uint64_t *a);
status_t sys_file_seek(const uint64_t *a);
status_t sys_file_truncate(const uint64_t *a);
status_t sys_file_stat(const uint64_t *a);
status_t sys_directory_read(const uint64_t *a);
status_t sys_path_stat(const uint64_t *a);
status_t sys_path_mkdir(const uint64_t *a);
status_t sys_path_unlink(const uint64_t *a);
status_t sys_path_rename(const uint64_t *a);
status_t sys_path_symlink(const uint64_t *a);
status_t sys_path_readlink(const uint64_t *a);
status_t sys_chdir(const uint64_t *a);
status_t sys_getcwd(const uint64_t *a);
status_t sys_fs_sync(const uint64_t *a);
status_t sys_mount(const uint64_t *a);
status_t sys_unmount(const uint64_t *a);

/* Program, pipe and power calls (process_syscalls.c) */
status_t sys_process_spawn(const uint64_t *a);
status_t sys_process_spawn_as(const uint64_t *a);
status_t sys_clock_realtime(const uint64_t *a);
status_t sys_system_info(const uint64_t *a);
status_t sys_process_info(const uint64_t *a);
status_t sys_process_kill(const uint64_t *a);
status_t sys_pipe_create(const uint64_t *a);
status_t sys_system_power(const uint64_t *a);

/* Networking (net_syscalls.c) */
status_t sys_socket_create(const uint64_t *a);
status_t sys_socket_bind(const uint64_t *a);
status_t sys_socket_connect(const uint64_t *a);
status_t sys_socket_listen(const uint64_t *a);
status_t sys_socket_accept(const uint64_t *a);
status_t sys_socket_send(const uint64_t *a);
status_t sys_socket_receive(const uint64_t *a);
status_t sys_socket_shutdown(const uint64_t *a);
status_t sys_socket_set_option(const uint64_t *a);
status_t sys_socket_info(const uint64_t *a);
status_t sys_net_interface_info(const uint64_t *a);
status_t sys_net_configure(const uint64_t *a);
status_t sys_net_resolve(const uint64_t *a);
/* Graphics and input, ABI 5 (graphics_syscalls.c) */
status_t sys_object_wait_many(const uint64_t *a);
status_t sys_channel_send_handles(const uint64_t *a);
status_t sys_channel_receive_handles(const uint64_t *a);
status_t sys_service_register(const uint64_t *a);
status_t sys_service_connect(const uint64_t *a);
status_t sys_display_info(const uint64_t *a);
status_t sys_display_acquire(const uint64_t *a);
status_t sys_input_open(const uint64_t *a);
status_t sys_input_read(const uint64_t *a);
/* Audio devices, ABI 7 (audio_syscalls.c) */
status_t sys_audio_info(const uint64_t *a);
status_t sys_audio_open(const uint64_t *a);
status_t sys_audio_write(const uint64_t *a);
status_t sys_audio_read(const uint64_t *a);
status_t sys_audio_control(const uint64_t *a);
/* SYS_FILE_READ / WRITE on a socket handle */
status_t syscall_socket_file_io(uint64_t handle, bool write, uint64_t buffer, uint64_t size, uint64_t *done);

#endif
