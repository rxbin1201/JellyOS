/*
 * Threads: independently schedulable execution units (README section 16).
 *
 * Kernel threads run only in ring 0 and belong to no process. User threads
 * belong to a process and enter ring 3 through their kernel stack.
 */

#ifndef SCHEDULER_THREAD_H
#define SCHEDULER_THREAD_H

#include "core/list.h"
#include "core/object.h"
#include "scheduler/wait.h"

#include <stdbool.h>
#include <stdint.h>

#define THREAD_PRIORITY_COUNT  32 /* 0 lowest ... 31 highest */
#define THREAD_PRIORITY_USER   16
#define THREAD_PRIORITY_KERNEL 20
#define THREAD_NAME_MAX        32

struct process;
struct arch_thread;

typedef enum {
    THREAD_NEW,      /* created, not started */
    THREAD_READY,    /* in a ready queue */
    THREAD_RUNNING,
    THREAD_BLOCKED,  /* on a wait queue and/or the sleep list */
    THREAD_DEAD,     /* exited, waiting to be reaped */
} thread_state_t;

typedef struct thread {
    object_t            object;
    uint64_t            tid;
    char                name[THREAD_NAME_MAX];
    struct process     *process;          /* NULL for kernel threads */
    thread_state_t      state;
    uint8_t             priority;
    uint32_t            slice;            /* remaining ticks of the timeslice */
    struct arch_thread *arch;
    uint64_t            kernel_stack_top;

    list_node_t         run_node;         /* ready queue or zombie list */
    list_node_t         wait_node;        /* wait queue */
    list_node_t         sleep_node;       /* sleep list (deadline) */
    list_node_t         process_node;     /* process thread list */

    wait_queue_t       *waiting_on;
    uint64_t            wake_time;
    uint64_t            wait_key;         /* futex key */
    status_t            wait_status;
    bool                wait_interruptible;
    bool                kill_pending;
} thread_t;

thread_t *thread_current(void);

status_t thread_create_kernel(const char *name, void (*entry)(void *), void *arg, uint8_t priority,
                              thread_t **thread);

/* User thread starting at ip with stack pointer sp and arguments in RDI, RSI, RDX. */
status_t thread_create_user(struct process *process, const char *name, uint64_t ip, uint64_t sp,
                            uint64_t arg0, uint64_t arg1, uint64_t arg2, thread_t **thread);

/* Make a THREAD_NEW thread runnable. */
void     thread_start(thread_t *thread);

__attribute__((noreturn)) void thread_exit(void);

/* Ask a thread to exit; a blocked thread is woken with STATUS_INTERRUPTED. */
void     thread_kill(thread_t *thread);

status_t thread_sleep(uint64_t ns);

/* Free a dead thread's resources (called by the scheduler, never on that thread). */
void     thread_reap(thread_t *thread);

/* Wrap the already running boot context into a thread (scheduler start). */
thread_t *thread_adopt_current(const char *name, uint8_t priority, uint64_t stack_top);

/* --- Hooks used by the architecture layer --- */

/* Before returning to ring 3: handle pending kills and preemption. */
void     thread_return_to_user(void);

/* First code a new kernel thread runs. */
__attribute__((noreturn)) void thread_kernel_start(void (*entry)(void *), void *arg);

/* First code a new user thread runs before entering ring 3. */
void     thread_user_start(void);

#endif
