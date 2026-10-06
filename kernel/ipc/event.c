#include "ipc/ipc.h"

#include "core/arch.h"
#include "memory/heap.h"

#include <jelly/syscall.h>

typedef struct {
    object_t object;
    bool     signaled;
    bool     auto_reset;
} event_t;

static event_t *event_of(object_t *object)
{
    return container_of(object, event_t, object);
}

static void event_destroy(object_t *object)
{
    kfree(event_of(object));
}

static bool event_signaled(object_t *object)
{
    return event_of(object)->signaled;
}

static void event_consume(object_t *object)
{
    event_t *e = event_of(object);
    if (e->auto_reset)
        e->signaled = false;
}

static const object_ops_t event_ops = {
    .destroy = event_destroy,
    .signaled = event_signaled,
    .consume = event_consume,
};

status_t event_create(uint32_t flags, object_t **event)
{
    if (flags & ~JELLY_EVENT_AUTO_RESET)
        return STATUS_INVALID_ARGUMENT;

    event_t *e = kcalloc(1, sizeof(*e));
    if (!e)
        return STATUS_OUT_OF_MEMORY;
    object_init(&e->object, OBJECT_EVENT, &event_ops);
    e->auto_reset = flags & JELLY_EVENT_AUTO_RESET;
    *event = &e->object;
    return STATUS_SUCCESS;
}

void event_signal(object_t *event)
{
    uint64_t flags = arch_interrupts_save();
    event_of(event)->signaled = true;
    object_notify(event);
    arch_interrupts_restore(flags);
}

void event_reset(object_t *event)
{
    event_of(event)->signaled = false;
}
