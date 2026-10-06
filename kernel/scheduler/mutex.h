/*
 * Sleeping mutex for kernel code that blocks while holding a lock
 * (device I/O, file systems). Not usable from interrupt handlers.
 */

#ifndef SCHEDULER_MUTEX_H
#define SCHEDULER_MUTEX_H

#include "scheduler/wait.h"

#include <stdbool.h>

struct thread;

typedef struct {
    struct thread *owner;
    wait_queue_t   waiters;
} mutex_t;

void mutex_init(mutex_t *mutex);
void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);
bool mutex_held(const mutex_t *mutex);

#endif
