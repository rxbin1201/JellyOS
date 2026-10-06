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

#define JELLY_SYSCALL_ABI_VERSION 1

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
    SYS_COUNT
};

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
