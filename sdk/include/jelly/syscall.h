/*
 * JellyOS system call ABI (README section 20).
 *
 * Shared by the kernel and userspace. See docs/abi/syscalls.md for the
 * calling convention and the full table of calls.
 *
 * Rules:
 *  - Every call returns status_t in RAX. Results are written through output
 *    pointers, which the kernel validates.
 *  - Numbers, argument meanings and constants never change; new calls are
 *    appended and JELLY_SYSCALL_ABI_VERSION is incremented.
 */

#ifndef JELLY_SYSCALL_H
#define JELLY_SYSCALL_H

#include <stdint.h>

#define JELLY_SYSCALL_ABI_VERSION 2

typedef uint32_t jelly_handle_t;
#define JELLY_HANDLE_INVALID 0u

enum {
    SYS_ABI_VERSION      = 0,  /* (uint32_t *version) */
    SYS_DEBUG_WRITE      = 1,  /* (const char *text, size_t length) */
    SYS_CLOCK_MONOTONIC  = 2,  /* (uint64_t *ns) */
    SYS_PROCESS_EXIT     = 3,  /* (int32_t code)                                  never returns */
    SYS_THREAD_CREATE    = 4,  /* (entry, stack_top, arg0, arg1, jelly_handle_t *thread) */
    SYS_THREAD_EXIT      = 5,  /* ()                                              never returns */
    SYS_THREAD_YIELD     = 6,  /* () */
    SYS_THREAD_SLEEP     = 7,  /* (uint64_t ns) */
    SYS_MEMORY_ALLOCATE  = 8,  /* (size_t size, uint32_t flags, uintptr_t *address) */
    SYS_MEMORY_UNMAP     = 9,  /* (uintptr_t address, size_t size) */
    SYS_HANDLE_CLOSE     = 10, /* (handle) */
    SYS_HANDLE_DUPLICATE = 11, /* (handle, uint32_t rights, jelly_handle_t *copy) */
    SYS_OBJECT_WAIT      = 12, /* (handle, uint64_t timeout_ns) */
    SYS_EVENT_CREATE     = 13, /* (uint32_t flags, jelly_handle_t *event) */
    SYS_EVENT_SIGNAL     = 14, /* (handle) */
    SYS_EVENT_RESET      = 15, /* (handle) */
    SYS_CHANNEL_CREATE   = 16, /* (jelly_handle_t *end0, jelly_handle_t *end1) */
    SYS_CHANNEL_SEND     = 17, /* (handle, const void *data, size_t size) */
    SYS_CHANNEL_RECEIVE  = 18, /* (handle, void *buffer, size_t size, size_t *actual) */
    SYS_SHM_CREATE       = 19, /* (size_t size, jelly_handle_t *shm) */
    SYS_SHM_MAP          = 20, /* (handle, uint32_t flags, uintptr_t *address) */
    SYS_FUTEX_WAIT       = 21, /* (const uint32_t *word, uint32_t expected, uint64_t timeout_ns) */
    SYS_FUTEX_WAKE       = 22, /* (const uint32_t *word, uint32_t count) */
    /* ABI version 2: files (paths are byte strings with explicit length) */
    SYS_FILE_OPEN        = 23, /* (path, length, uint32_t flags, uint32_t mode, jelly_handle_t *file) */
    SYS_FILE_READ        = 24, /* (handle, void *buffer, size_t size, size_t *done) */
    SYS_FILE_WRITE       = 25, /* (handle, const void *buffer, size_t size, size_t *done) */
    SYS_FILE_SEEK        = 26, /* (handle, int64_t offset, uint32_t whence, uint64_t *position) */
    SYS_FILE_TRUNCATE    = 27, /* (handle, uint64_t size) */
    SYS_FILE_STAT        = 28, /* (handle, jelly_stat_t *stat) */
    SYS_DIRECTORY_READ   = 29, /* (handle, jelly_dirent_t *entry)           NOT_FOUND at the end */
    SYS_PATH_STAT        = 30, /* (path, length, uint32_t flags, jelly_stat_t *stat) */
    SYS_PATH_MKDIR       = 31, /* (path, length, uint32_t mode) */
    SYS_PATH_UNLINK      = 32, /* (path, length) */
    SYS_PATH_RENAME      = 33, /* (from, from_length, to, to_length) */
    SYS_PATH_SYMLINK     = 34, /* (target, target_length, path, length) */
    SYS_PATH_READLINK    = 35, /* (path, length, char *buffer, size_t size, size_t *link_length) */
    SYS_CHDIR            = 36, /* (path, length) */
    SYS_GETCWD           = 37, /* (char *buffer, size_t size, size_t *length) */
    SYS_FS_SYNC          = 38, /* () */
    SYS_MOUNT            = 39, /* (path, length, device, device_length, type, type_length)   root only */
    SYS_UNMOUNT          = 40, /* (path, length)                                             root only */
    SYS_COUNT
};

/* SYS_FILE_OPEN flags */
#define JELLY_OPEN_READ       (1u << 0)
#define JELLY_OPEN_WRITE      (1u << 1)
#define JELLY_OPEN_CREATE     (1u << 2) /* create the file if it does not exist */
#define JELLY_OPEN_EXCLUSIVE  (1u << 3) /* with CREATE: fail with ALREADY_EXISTS */
#define JELLY_OPEN_TRUNCATE   (1u << 4)
#define JELLY_OPEN_APPEND     (1u << 5) /* every write goes to the end */
#define JELLY_OPEN_DIRECTORY  (1u << 6) /* must be a directory (for SYS_DIRECTORY_READ) */
#define JELLY_OPEN_NOFOLLOW   (1u << 7) /* do not follow a symbolic link in the last component */

/* SYS_FILE_SEEK whence */
#define JELLY_SEEK_SET        0
#define JELLY_SEEK_CURRENT    1
#define JELLY_SEEK_END        2

/* SYS_PATH_STAT flags */
#define JELLY_STAT_NOFOLLOW   (1u << 0)

#define JELLY_FILE_TYPE_FILE      1
#define JELLY_FILE_TYPE_DIRECTORY 2
#define JELLY_FILE_TYPE_SYMLINK   3

#define JELLY_NAME_MAX        255

typedef struct {
    uint32_t type;   /* JELLY_FILE_TYPE_* */
    uint32_t mode;   /* permission bits, 0777 */
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t inode;
} jelly_stat_t;

typedef struct {
    uint32_t type;
    uint32_t name_length;
    uint64_t inode;
    char     name[JELLY_NAME_MAX + 1]; /* NUL terminated */
} jelly_dirent_t;

/* Handle rights */
#define JELLY_RIGHT_READ      (1u << 0) /* receive from a channel */
#define JELLY_RIGHT_WRITE     (1u << 1) /* send to a channel, write shared memory */
#define JELLY_RIGHT_WAIT      (1u << 2) /* SYS_OBJECT_WAIT */
#define JELLY_RIGHT_SIGNAL    (1u << 3) /* signal or reset an event */
#define JELLY_RIGHT_MAP       (1u << 4) /* map shared memory */
#define JELLY_RIGHT_DUPLICATE (1u << 5) /* SYS_HANDLE_DUPLICATE */
#define JELLY_RIGHTS_ALL      0x3Fu

/* SYS_MEMORY_ALLOCATE / SYS_SHM_MAP flags (memory is always readable) */
#define JELLY_MEMORY_WRITE    (1u << 0)
#define JELLY_MEMORY_EXEC     (1u << 1) /* never together with WRITE */

/* SYS_EVENT_CREATE flags */
#define JELLY_EVENT_AUTO_RESET (1u << 0) /* a satisfied wait resets the event */

/* Timeouts */
#define JELLY_WAIT_FOREVER    UINT64_MAX
#define JELLY_NO_WAIT         0

/* Exit code of a process terminated because of a CPU fault */
#define JELLY_EXIT_FAULT      (-1)

#endif
