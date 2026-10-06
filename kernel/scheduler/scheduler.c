#include "scheduler/scheduler.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/panic.h"
#include "memory/vmm.h"
#include "process/process.h"
#include "time/clock.h"

static thread_t *current;
static thread_t *idle_thread;
static list_t ready_queues[THREAD_PRIORITY_COUNT];
static uint32_t ready_mask;
static list_t sleepers; /* sorted by wake_time */
static list_t zombies;
static bool need_resched;
static bool running;
static uint64_t switches;

thread_t *thread_current(void)
{
    return current;
}

bool scheduler_running(void)
{
    return running;
}

uint64_t scheduler_switch_count(void)
{
    return switches;
}

bool scheduler_need_resched(void)
{
    return need_resched;
}

/* --- Ready queues ---------------------------------------------------------- */

static void enqueue(thread_t *t)
{
    list_push_back(&ready_queues[t->priority], &t->run_node);
    ready_mask |= 1u << t->priority;
}

static thread_t *dequeue_best(void)
{
    if (!ready_mask)
        return NULL;

    unsigned priority = 31 - (unsigned)__builtin_clz(ready_mask);
    list_node_t *node = list_pop_front(&ready_queues[priority]);
    if (list_empty(&ready_queues[priority]))
        ready_mask &= ~(1u << priority);
    return container_of(node, thread_t, run_node);
}

static void make_ready_locked(thread_t *t)
{
    t->state = THREAD_READY;
    enqueue(t);
    if (current == idle_thread || t->priority > current->priority)
        need_resched = true;
}

void scheduler_make_ready(thread_t *t)
{
    uint64_t flags = arch_interrupts_save();
    make_ready_locked(t);
    arch_interrupts_restore(flags);
}

/* --- Switching ------------------------------------------------------------- */

void scheduler_finish_switch(void)
{
    list_for_each_safe(node, &zombies) {
        thread_t *t = container_of(node, thread_t, run_node);
        if (t == current)
            continue;
        list_remove(node);
        thread_reap(t);
    }
}

void schedule(void)
{
    thread_t *prev = current;

    if (prev->state == THREAD_RUNNING && prev != idle_thread) {
        prev->state = THREAD_READY;
        enqueue(prev);
    }

    thread_t *next = dequeue_best();
    if (!next)
        next = idle_thread;

    need_resched = false;
    next->state = THREAD_RUNNING;
    next->slice = SCHEDULER_TIMESLICE_TICKS;

    if (next != prev) {
        switches++;
        current = next;
        /* Kernel threads run in whatever address space is active (the kernel half is shared). */
        if (next->process && arch_mmu_current() != next->process->space.root)
            vmm_space_activate(&next->process->space);
        arch_thread_switch(prev->arch, next->arch, next->kernel_stack_top);
    }
    scheduler_finish_switch();
}

void scheduler_yield(void)
{
    uint64_t flags = arch_interrupts_save();
    if (current->state == THREAD_RUNNING)
        schedule();
    arch_interrupts_restore(flags);
}

void scheduler_add_zombie(thread_t *t)
{
    list_push_back(&zombies, &t->run_node);
}

/* --- Waiting --------------------------------------------------------------- */

void wait_queue_init(wait_queue_t *queue)
{
    list_init(&queue->threads);
}

uint64_t wait_deadline(uint64_t timeout_ns)
{
    if (timeout_ns == WAIT_FOREVER)
        return WAIT_FOREVER;
    uint64_t now = clock_monotonic_ns();
    return now + timeout_ns < now ? WAIT_FOREVER : now + timeout_ns;
}

static void add_sleeper(thread_t *t)
{
    list_for_each(node, &sleepers) {
        thread_t *other = container_of(node, thread_t, sleep_node);
        if (other->wake_time > t->wake_time) {
            list_insert_before(node, &t->sleep_node);
            return;
        }
    }
    list_push_back(&sleepers, &t->sleep_node);
}

void scheduler_wake_thread(thread_t *t, status_t result)
{
    if (t->state != THREAD_BLOCKED)
        return;
    if (list_linked(&t->wait_node))
        list_remove(&t->wait_node);
    if (list_linked(&t->sleep_node))
        list_remove(&t->sleep_node);
    t->waiting_on = NULL;
    t->wait_status = result;
    make_ready_locked(t);
}

static status_t block(wait_queue_t *queue, uint64_t deadline_ns, bool interruptible)
{
    thread_t *t = current;

    ASSERT(t != idle_thread);
    if (interruptible && t->kill_pending)
        return STATUS_INTERRUPTED;
    if (deadline_ns != WAIT_FOREVER && deadline_ns <= clock_monotonic_ns())
        return STATUS_TIMEOUT;

    t->state = THREAD_BLOCKED;
    t->wait_status = STATUS_SUCCESS;
    t->wait_interruptible = interruptible;
    t->waiting_on = queue;
    if (queue)
        list_push_back(&queue->threads, &t->wait_node);
    if (deadline_ns != WAIT_FOREVER) {
        t->wake_time = deadline_ns;
        add_sleeper(t);
    }

    schedule();
    return t->wait_status;
}

status_t wait_queue_block(wait_queue_t *queue, uint64_t deadline_ns)
{
    return block(queue, deadline_ns, true);
}

status_t wait_queue_block_uninterruptible(wait_queue_t *queue, uint64_t deadline_ns)
{
    return block(queue, deadline_ns, false);
}

void wait_queue_wake_all(wait_queue_t *queue, status_t result)
{
    uint64_t flags = arch_interrupts_save();
    list_node_t *node;
    while ((node = list_front(&queue->threads)))
        scheduler_wake_thread(container_of(node, thread_t, wait_node), result);
    arch_interrupts_restore(flags);
}

bool wait_queue_wake_one(wait_queue_t *queue, status_t result)
{
    uint64_t flags = arch_interrupts_save();
    list_node_t *node = list_front(&queue->threads);
    if (node)
        scheduler_wake_thread(container_of(node, thread_t, wait_node), result);
    arch_interrupts_restore(flags);
    return node != NULL;
}

uint32_t wait_queue_wake_key(wait_queue_t *queue, uint64_t key, uint32_t max, status_t result)
{
    uint64_t flags = arch_interrupts_save();
    uint32_t woken = 0;

    list_for_each_safe(node, &queue->threads) {
        if (woken == max)
            break;
        thread_t *t = container_of(node, thread_t, wait_node);
        if (t->wait_key == key) {
            scheduler_wake_thread(t, result);
            woken++;
        }
    }
    arch_interrupts_restore(flags);
    return woken;
}

/* --- Timer and idle -------------------------------------------------------- */

void scheduler_tick(void)
{
    if (!running)
        return;

    uint64_t now = clock_monotonic_ns();
    list_node_t *node;
    while ((node = list_front(&sleepers)) && container_of(node, thread_t, sleep_node)->wake_time <= now)
        scheduler_wake_thread(container_of(node, thread_t, sleep_node), STATUS_TIMEOUT);

    if (current != idle_thread && current->slice && --current->slice == 0)
        need_resched = true;
}

static void idle_main(void *arg)
{
    (void)arg;
    for (;;) {
        arch_interrupts_disable();
        if (ready_mask)
            schedule();
        else
            arch_idle(); /* enables interrupts and halts atomically */
    }
}

void scheduler_init(uint64_t current_stack_top)
{
    for (unsigned i = 0; i < THREAD_PRIORITY_COUNT; i++)
        list_init(&ready_queues[i]);
    list_init(&sleepers);
    list_init(&zombies);

    current = thread_adopt_current("kernel-main", THREAD_PRIORITY_KERNEL, current_stack_top);
    if (!current)
        panic("scheduler: cannot create the initial thread");

    if (STATUS_IS_ERROR(thread_create_kernel("idle", idle_main, NULL, 0, &idle_thread)))
        panic("scheduler: cannot create the idle thread");

    running = true;
    klog_info("scheduler: running, %u priorities, timeslice %u ms", THREAD_PRIORITY_COUNT,
              SCHEDULER_TIMESLICE_TICKS);
}
