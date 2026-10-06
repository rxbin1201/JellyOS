/*
 * Scheduler (README section 18): priorities, round-robin timeslices,
 * preemption of user mode, sleep/wakeup and an idle thread.
 *
 * The kernel itself is not preemptible: a thread switches only when it
 * blocks, yields, exits, or is about to return to user mode with a pending
 * reschedule. Scheduler state is protected by disabling interrupts
 * (single CPU).
 */

#ifndef SCHEDULER_SCHEDULER_H
#define SCHEDULER_SCHEDULER_H

#include "scheduler/thread.h"

#define SCHEDULER_TIMESLICE_TICKS 10

/* Adopt the running context as the first kernel thread and create the idle thread. */
void scheduler_init(uint64_t current_stack_top);

bool scheduler_running(void);

/* Timer interrupt: account the timeslice, wake expired sleepers. */
void scheduler_tick(void);

/* Put a thread into its ready queue (interrupt-safe). */
void scheduler_make_ready(thread_t *thread);

void scheduler_yield(void);

/* True if the current thread should give up the CPU. */
bool scheduler_need_resched(void);

/*
 * Switch to the best ready thread. Interrupts must be disabled and the
 * current thread's state already set (RUNNING to stay eligible, BLOCKED or
 * DEAD to leave).
 */
void schedule(void);

/* Bookkeeping right after a switch, also run by new threads. */
void scheduler_finish_switch(void);

/* Wake a blocked thread with result (interrupts disabled). */
void scheduler_wake_thread(thread_t *thread, status_t result);

/* Queue a dead thread for reaping (interrupts disabled). */
void scheduler_add_zombie(thread_t *thread);

/* Counters for diagnostics and tests. */
uint64_t scheduler_switch_count(void);

#endif
