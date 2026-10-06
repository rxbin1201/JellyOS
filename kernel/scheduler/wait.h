/*
 * Wait queues: the single blocking primitive behind sleep, object waits and
 * futexes. Implemented in scheduler.c.
 *
 * Usage (interrupts disabled from the condition check until blocking, so no
 * wakeup can be lost):
 *
 *   uint64_t flags = arch_interrupts_save();
 *   while (!condition && status == STATUS_SUCCESS)
 *       status = wait_queue_block(&queue, deadline);
 *   arch_interrupts_restore(flags);
 */

#ifndef SCHEDULER_WAIT_H
#define SCHEDULER_WAIT_H

#include "core/list.h"

#include <jelly/status.h>
#include <stdbool.h>
#include <stdint.h>

#define WAIT_FOREVER UINT64_MAX

struct thread;

typedef struct {
    list_t threads;
} wait_queue_t;

void wait_queue_init(wait_queue_t *queue);

/*
 * Block the current thread on queue (NULL: plain sleep) until woken or until
 * the absolute monotonic deadline. Interrupts must be disabled.
 * Returns the waker's status, STATUS_TIMEOUT or STATUS_INTERRUPTED (thread killed).
 */
status_t wait_queue_block(wait_queue_t *queue, uint64_t deadline_ns);

/* Like wait_queue_block, but a kill does not end the wait (locks the caller must get). */
status_t wait_queue_block_uninterruptible(wait_queue_t *queue, uint64_t deadline_ns);

void     wait_queue_wake_all(wait_queue_t *queue, status_t result);
bool     wait_queue_wake_one(wait_queue_t *queue, status_t result);

/* Wake up to max threads whose wait_key equals key. Returns the number woken. */
uint32_t wait_queue_wake_key(wait_queue_t *queue, uint64_t key, uint32_t max, status_t result);

/* Deadline helper: now + timeout, saturating at WAIT_FOREVER. */
uint64_t wait_deadline(uint64_t timeout_ns);

#endif
