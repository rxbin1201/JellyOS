#include "scheduler/mutex.h"

#include "scheduler/thread.h"

#include "core/arch.h"
#include "core/export.h"
#include "core/panic.h"

void mutex_init(mutex_t *mutex)
{
    mutex->owner = NULL;
    wait_queue_init(&mutex->waiters);
}

void mutex_lock(mutex_t *mutex)
{
    thread_t *self = thread_current();
    uint64_t flags = arch_interrupts_save();

    if (mutex->owner == self)
        panic("mutex: recursive lock by thread %s", self->name);

    /* A killed thread must still get the lock: its caller expects to hold it. */
    while (mutex->owner)
        wait_queue_block_uninterruptible(&mutex->waiters, WAIT_FOREVER);
    mutex->owner = self;
    arch_interrupts_restore(flags);
}

void mutex_unlock(mutex_t *mutex)
{
    uint64_t flags = arch_interrupts_save();

    ASSERT(mutex->owner == thread_current());
    mutex->owner = NULL;
    wait_queue_wake_one(&mutex->waiters, STATUS_SUCCESS);
    arch_interrupts_restore(flags);
}

bool mutex_held(const mutex_t *mutex)
{
    return mutex->owner == thread_current();
}

EXPORT_SYMBOL(mutex_init);
EXPORT_SYMBOL(mutex_lock);
EXPORT_SYMBOL(mutex_unlock);
