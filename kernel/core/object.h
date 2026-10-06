/*
 * Kernel objects: everything a handle can refer to (README section 17).
 *
 * Objects are reference counted. Handles, threads and in-kernel users each
 * hold a reference. Waitable objects report their signaled state through
 * ops->signaled; state changes call object_notify().
 *
 * Reference counts are plain integers: the kernel is not preemptible and only
 * process context (never interrupt handlers) changes them. SMP will need
 * atomics here.
 */

#ifndef CORE_OBJECT_H
#define CORE_OBJECT_H

#include "scheduler/wait.h"

#include <jelly/status.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OBJECT_PROCESS       = 1,
    OBJECT_THREAD        = 2,
    OBJECT_EVENT         = 3,
    OBJECT_CHANNEL       = 4,
    OBJECT_SHARED_MEMORY = 5,
} object_type_t;

struct object;

typedef struct {
    void (*destroy)(struct object *object);
    bool (*signaled)(struct object *object); /* NULL: not waitable */
    void (*consume)(struct object *object);  /* optional: a wait was satisfied */
} object_ops_t;

typedef struct object {
    object_type_t       type;
    uint32_t            refs;
    const object_ops_t *ops;
    wait_queue_t        waiters;
} object_t;

void     object_init(object_t *object, object_type_t type, const object_ops_t *ops);
void     object_retain(object_t *object);
void     object_release(object_t *object);

/* Wake every waiter so it re-checks the signaled state. */
void     object_notify(object_t *object);

/* Wait until signaled. timeout 0 polls, WAIT_FOREVER blocks indefinitely. */
status_t object_wait(object_t *object, uint64_t timeout_ns);

const char *object_type_name(object_type_t type);

#endif
