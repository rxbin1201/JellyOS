/*
 * Processes (README section 16).
 *
 * A process owns an address space, a handle table, credentials, resource
 * limits and its threads. It ends when its last thread is reaped; its object
 * then becomes signaled with the exit code. Environment and working
 * directory arrive with the VFS and the program loader (Phases 6 and 7).
 */

#ifndef PROCESS_PROCESS_H
#define PROCESS_PROCESS_H

#include "core/handle.h"
#include "core/list.h"
#include "core/object.h"
#include "memory/vmm.h"
#include "security/credentials.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROCESS_NAME_MAX   32
#define PROCESS_REASON_MAX 128
#define PROCESS_CWD_MAX    1024

/* User address layout (see docs/architecture/memory-layout.md). */
#define USER_IMAGE_END     0x0000010000000000ULL /* program images below 1 TiB */
#define USER_MAP_BASE      0x0000010000000000ULL /* SYS_MEMORY_ALLOCATE, SYS_SHM_MAP */
#define USER_MAP_END       0x00007F0000000000ULL
#define USER_STACK_TOP     0x00007FFFFFFF0000ULL
#define USER_STACK_SIZE    0x40000ULL            /* 256 KiB, unmapped guard below */

typedef struct {
    uint32_t max_handles;
    uint32_t max_threads;
    uint64_t max_memory_pages;
} process_limits_t;

typedef struct process {
    object_t         object;
    uint64_t         pid;
    char             name[PROCESS_NAME_MAX];
    vm_space_t       space;
    handle_table_t   handles;
    credentials_t    credentials;
    process_limits_t limits;

    list_t           threads;
    uint32_t         live_threads;
    bool             started;

    uint64_t         memory_pages;  /* frames owned through the address space */
    uint64_t         next_map;      /* next free address in the mapping region */
    list_t           mappings;      /* shared memory mappings */

    char             cwd[PROCESS_CWD_MAX]; /* normalized absolute path */

    bool             critical;      /* the system cannot continue without it (init) */
    bool             exiting;
    bool             exited;
    int32_t          exit_code;
    char             exit_reason[PROCESS_REASON_MAX];
    list_node_t      finalize_node;
} process_t;

/* Start the reaper thread that finalizes exited processes. */
status_t   process_init(void);

/* Empty process (address space, handle table). The caller holds one reference. */
status_t   process_create(const char *name, process_t **process);

/* Load a static ELF64 executable into the process. */
status_t   process_load_elf(process_t *process, const void *image, size_t size, uint64_t *entry);

/*
 * Allocate the user stack and start the main thread at entry with
 * RDI/RSI/RDX = arg0..arg2. Startup handles are installed before this.
 */
status_t   process_start(process_t *process, uint64_t entry, uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* The two halves of process_start, for callers that prepare the stack themselves. */
status_t   process_allocate_stack(process_t *process, uint64_t *top);
status_t   process_start_thread(process_t *process, uint64_t entry, uint64_t sp, uint64_t arg0, uint64_t arg1,
                                uint64_t arg2);

/* Write into a (not necessarily active) process's user memory. */
status_t   process_copy_to(process_t *process, uint64_t address, const void *data, size_t size);

process_t *process_current(void);
/* Processes that exist (running or not yet destroyed). */
uint32_t   process_live_count(void);

/* Terminate: record the code, kill all threads. */
void       process_exit(process_t *process, int32_t code, const char *reason);
__attribute__((noreturn)) void process_exit_current(int32_t code, const char *reason);

/* The current thread caused a CPU fault in user mode. */
__attribute__((noreturn)) void process_fault(const char *reason);

/* A thread of the process was reaped (scheduler context). */
void       process_thread_exited(process_t *process);

/* --- Memory (SYS_MEMORY_*, SYS_SHM_MAP) --- */

status_t   process_memory_allocate(process_t *process, uint64_t size, uint32_t vm_flags, uint64_t *address);
status_t   process_memory_unmap(process_t *process, uint64_t address, uint64_t size);

/* Reserve a range in the mapping region. */
status_t   process_reserve_range(process_t *process, uint64_t size, uint64_t *address);

/* Record a shared-memory mapping (takes a reference to shm). */
status_t   process_add_mapping(process_t *process, uint64_t address, uint64_t size, object_t *shm);

#endif
