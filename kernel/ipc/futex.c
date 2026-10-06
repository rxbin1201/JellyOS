/*
 * Futexes: block until another thread wakes the same 32-bit word.
 *
 * The key is the word's physical address, so threads of different processes
 * sharing memory meet on the same key. Waiters are spread over a small set
 * of hashed wait queues.
 */

#include "ipc/ipc.h"

#include "process/usercopy.h"
#include "scheduler/thread.h"

#include "core/arch.h"

#define BUCKETS 64

static wait_queue_t buckets[BUCKETS];
static bool initialized;

static wait_queue_t *bucket_for(uint64_t key)
{
    if (!initialized) {
        for (unsigned i = 0; i < BUCKETS; i++)
            wait_queue_init(&buckets[i]);
        initialized = true;
    }
    return &buckets[(key >> 2) % BUCKETS];
}

static status_t key_for(uint64_t user_address, uint64_t *key)
{
    process_t *p = process_current();
    uint64_t phys;
    uint32_t flags;

    if ((user_address & 3) || !user_range_ok(user_address, sizeof(uint32_t), false) ||
        !vmm_query(&p->space, user_address, &phys, &flags))
        return STATUS_INVALID_ARGUMENT;
    *key = phys;
    return STATUS_SUCCESS;
}

status_t futex_wait(uint64_t user_address, uint32_t expected, uint64_t timeout_ns)
{
    uint64_t key;
    uint32_t value;
    status_t status = key_for(user_address, &key);

    if (STATUS_IS_ERROR(status))
        return status;

    uint64_t deadline = wait_deadline(timeout_ns);
    uint64_t flags = arch_interrupts_save();

    /* Check and block without a window for a lost wakeup. */
    status = copy_from_user(&value, user_address, sizeof(value));
    if (!STATUS_IS_ERROR(status)) {
        if (value != expected) {
            status = STATUS_WOULD_BLOCK;
        } else if (timeout_ns == 0) {
            status = STATUS_TIMEOUT;
        } else {
            thread_current()->wait_key = key;
            status = wait_queue_block(bucket_for(key), deadline);
            thread_current()->wait_key = 0;
        }
    }
    arch_interrupts_restore(flags);
    return status;
}

status_t futex_wake(uint64_t user_address, uint32_t count, uint32_t *woken)
{
    uint64_t key;
    status_t status = key_for(user_address, &key);

    if (STATUS_IS_ERROR(status))
        return status;
    *woken = wait_queue_wake_key(bucket_for(key), key, count, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}
