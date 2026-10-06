#include "core/object.h"

#include "core/arch.h"
#include "core/panic.h"

void object_init(object_t *object, object_type_t type, const object_ops_t *ops)
{
    object->type = type;
    object->refs = 1;
    object->ops = ops;
    wait_queue_init(&object->waiters);
}

void object_retain(object_t *object)
{
    ASSERT(object->refs > 0);
    object->refs++;
}

void object_release(object_t *object)
{
    ASSERT(object->refs > 0);
    if (--object->refs == 0)
        object->ops->destroy(object);
}

void object_notify(object_t *object)
{
    uint64_t flags = arch_interrupts_save();
    wait_queue_wake_all(&object->waiters, STATUS_SUCCESS);
    arch_interrupts_restore(flags);
}

status_t object_wait(object_t *object, uint64_t timeout_ns)
{
    if (!object->ops->signaled)
        return STATUS_NOT_SUPPORTED;

    uint64_t deadline = wait_deadline(timeout_ns);
    status_t status = STATUS_SUCCESS;
    uint64_t flags = arch_interrupts_save();

    while (!object->ops->signaled(object)) {
        if (timeout_ns == 0) {
            status = STATUS_TIMEOUT;
            break;
        }
        status = wait_queue_block(&object->waiters, deadline);
        if (status != STATUS_SUCCESS)
            break;
    }
    if (status == STATUS_SUCCESS && object->ops->consume)
        object->ops->consume(object);

    arch_interrupts_restore(flags);
    return status;
}

const char *object_type_name(object_type_t type)
{
    switch (type) {
    case OBJECT_PROCESS:       return "process";
    case OBJECT_THREAD:        return "thread";
    case OBJECT_EVENT:         return "event";
    case OBJECT_CHANNEL:       return "channel";
    case OBJECT_SHARED_MEMORY: return "shared memory";
    case OBJECT_FILE:          return "file";
    case OBJECT_SOCKET:        return "socket";
    }
    return "unknown";
}
