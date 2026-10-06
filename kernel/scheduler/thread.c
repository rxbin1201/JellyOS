#include "scheduler/thread.h"
#include "core/export.h"

#include "core/arch.h"
#include "core/panic.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "process/process.h"
#include "scheduler/scheduler.h"

static uint64_t next_tid = 1;

static thread_t *thread_of(object_t *object)
{
    return container_of(object, thread_t, object);
}

static bool thread_signaled(object_t *object)
{
    return thread_of(object)->state == THREAD_DEAD;
}

static void release_resources(thread_t *t)
{
    if (t->arch) {
        arch_thread_destroy(t->arch);
        t->arch = NULL;
    }
    if (t->kernel_stack_top) {
        vmm_free_kernel_stack(t->kernel_stack_top);
        t->kernel_stack_top = 0;
    }
    if (t->process) {
        process_t *p = t->process;
        t->process = NULL;
        list_remove(&t->process_node);
        process_thread_exited(p);
        object_release(&p->object);
    }
}

static void thread_destroy(object_t *object)
{
    thread_t *t = thread_of(object);

    /* A thread that never ran still owns its stack and context. */
    release_resources(t);
    kfree(t);
}

static const object_ops_t thread_ops = {
    .destroy = thread_destroy,
    .signaled = thread_signaled,
};

static thread_t *alloc_thread(const char *name, uint8_t priority)
{
    thread_t *t = kcalloc(1, sizeof(*t));
    if (!t)
        return NULL;

    object_init(&t->object, OBJECT_THREAD, &thread_ops);
    t->tid = next_tid++;
    size_t length = strlen(name) < THREAD_NAME_MAX - 1 ? strlen(name) : THREAD_NAME_MAX - 1;
    memcpy(t->name, name, length);
    t->priority = priority < THREAD_PRIORITY_COUNT ? priority : THREAD_PRIORITY_COUNT - 1;
    t->state = THREAD_NEW;
    return t;
}

thread_t *thread_adopt_current(const char *name, uint8_t priority, uint64_t stack_top)
{
    thread_t *t = alloc_thread(name, priority);
    if (!t || STATUS_IS_ERROR(arch_thread_create_current(&t->arch)))
        return NULL;
    t->kernel_stack_top = stack_top;
    t->state = THREAD_RUNNING;
    object_retain(&t->object); /* the scheduler's reference while alive */
    return t;
}

status_t thread_create_kernel(const char *name, void (*entry)(void *), void *arg, uint8_t priority,
                              thread_t **thread)
{
    thread_t *t = alloc_thread(name, priority);
    if (!t)
        return STATUS_OUT_OF_MEMORY;

    status_t status = vmm_alloc_kernel_stack(&t->kernel_stack_top);
    if (!STATUS_IS_ERROR(status))
        status = arch_thread_create_kernel(&t->arch, t->kernel_stack_top, entry, arg);
    if (STATUS_IS_ERROR(status)) {
        object_release(&t->object);
        return status;
    }
    *thread = t;
    return STATUS_SUCCESS;
}

status_t thread_create_user(process_t *process, const char *name, uint64_t ip, uint64_t sp,
                            uint64_t arg0, uint64_t arg1, uint64_t arg2, thread_t **thread)
{
    if (process->live_threads >= process->limits.max_threads)
        return STATUS_LIMIT_EXCEEDED;
    if (process->exiting)
        return STATUS_INTERRUPTED;

    thread_t *t = alloc_thread(name, THREAD_PRIORITY_USER);
    if (!t)
        return STATUS_OUT_OF_MEMORY;

    status_t status = vmm_alloc_kernel_stack(&t->kernel_stack_top);
    if (!STATUS_IS_ERROR(status))
        status = arch_thread_create_user(&t->arch, t->kernel_stack_top, ip, sp, arg0, arg1, arg2);
    if (STATUS_IS_ERROR(status)) {
        object_release(&t->object);
        return status;
    }

    object_retain(&process->object);
    t->process = process;
    list_push_back(&process->threads, &t->process_node);
    process->live_threads++;
    *thread = t;
    return STATUS_SUCCESS;
}

void thread_start(thread_t *t)
{
    ASSERT(t->state == THREAD_NEW);
    object_retain(&t->object); /* the scheduler's reference while alive */
    scheduler_make_ready(t);
}

void thread_exit(void)
{
    arch_interrupts_disable();

    thread_t *t = thread_current();
    t->state = THREAD_DEAD;
    wait_queue_wake_all(&t->object.waiters, STATUS_SUCCESS);
    scheduler_add_zombie(t);
    schedule();
    panic("thread_exit: dead thread %s was scheduled again", t->name);
}

void thread_reap(thread_t *t)
{
    release_resources(t);
    object_release(&t->object); /* the scheduler's reference */
}

void thread_kill(thread_t *t)
{
    uint64_t flags = arch_interrupts_save();
    t->kill_pending = true;
    if (t->state == THREAD_BLOCKED && t->wait_interruptible)
        scheduler_wake_thread(t, STATUS_INTERRUPTED);
    arch_interrupts_restore(flags);
}

status_t thread_sleep(uint64_t ns)
{
    uint64_t flags = arch_interrupts_save();
    status_t status = wait_queue_block(NULL, wait_deadline(ns));
    arch_interrupts_restore(flags);
    return status == STATUS_TIMEOUT ? STATUS_SUCCESS : status;
}

void thread_return_to_user(void)
{
    thread_t *t = thread_current();

    if (t->kill_pending)
        thread_exit();
    if (scheduler_need_resched()) {
        scheduler_yield();
        if (t->kill_pending)
            thread_exit();
    }
}

void thread_kernel_start(void (*entry)(void *), void *arg)
{
    scheduler_finish_switch();
    arch_interrupts_enable();
    entry(arg);
    thread_exit();
}

void thread_user_start(void)
{
    scheduler_finish_switch();
    if (thread_current()->kill_pending)
        thread_exit();
}

EXPORT_SYMBOL(thread_sleep);
