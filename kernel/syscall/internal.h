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

#endif
