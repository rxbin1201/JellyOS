/*
 * libc: C11 threads on JellyOS threads, mutexes on futexes.
 *
 * Without thread-local storage a thread finds its own record by its stack
 * pointer (thrd_exit). Each thread gets a 64 KiB stack mapping, released by
 * thrd_join.
 */

#include <stdlib.h>
#include <threads.h>

#include <jelly/os.h>

#define THREAD_STACK_SIZE (64 * 1024)

struct jelly_thread {
    jelly_handle_t       handle;
    thrd_start_t         function;
    void                *arg;
    int                  result;
    unsigned char       *stack;
    struct jelly_thread *next;
};

static struct jelly_thread *threads;
static mtx_t threads_lock;

static void thread_main(void *context)
{
    struct jelly_thread *thread = context;
    thread->result = thread->function(thread->arg);
}

int thrd_create(thrd_t *result, thrd_start_t function, void *arg)
{
    struct jelly_thread *thread = calloc(1, sizeof(*thread));
    if (!thread)
        return thrd_nomem;
    void *stack;
    if (STATUS_IS_ERROR(jelly_memory_allocate(THREAD_STACK_SIZE, JELLY_MEMORY_WRITE, &stack))) {
        free(thread);
        return thrd_nomem;
    }
    thread->function = function;
    thread->arg = arg;
    thread->stack = stack;

    mtx_lock(&threads_lock);
    thread->next = threads;
    threads = thread;
    mtx_unlock(&threads_lock);

    if (STATUS_IS_ERROR(jelly_thread_create(thread_main, thread, thread->stack + THREAD_STACK_SIZE,
                                            &thread->handle))) {
        mtx_lock(&threads_lock);
        threads = thread->next;
        mtx_unlock(&threads_lock);
        jelly_memory_unmap(stack, THREAD_STACK_SIZE);
        free(thread);
        return thrd_error;
    }
    *result = thread;
    return thrd_success;
}

int thrd_join(thrd_t thread, int *result)
{
    if (STATUS_IS_ERROR(jelly_wait(thread->handle, JELLY_WAIT_FOREVER)))
        return thrd_error;
    if (result)
        *result = thread->result;

    mtx_lock(&threads_lock);
    for (struct jelly_thread **link = &threads; *link; link = &(*link)->next) {
        if (*link == thread) {
            *link = thread->next;
            break;
        }
    }
    mtx_unlock(&threads_lock);
    jelly_handle_close(thread->handle);
    jelly_memory_unmap(thread->stack, THREAD_STACK_SIZE);
    free(thread);
    return thrd_success;
}

void thrd_exit(int result)
{
    unsigned char *sp = __builtin_frame_address(0);

    mtx_lock(&threads_lock);
    for (struct jelly_thread *t = threads; t; t = t->next) {
        if (sp >= t->stack && sp < t->stack + THREAD_STACK_SIZE) {
            t->result = result;
            break;
        }
    }
    mtx_unlock(&threads_lock);
    jelly_thread_exit();
}

void thrd_yield(void)
{
    jelly_thread_yield();
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
    if (remaining)
        remaining->tv_sec = remaining->tv_nsec = 0;
    uint64_t ns = (uint64_t)duration->tv_sec * 1000000000ULL + (uint64_t)duration->tv_nsec;
    return STATUS_IS_ERROR(jelly_thread_sleep(ns)) ? -2 : 0;
}

/* --- Mutexes (states: 0 unlocked, 1 locked, 2 locked with waiters) -------------- */

int mtx_init(mtx_t *mutex, int type)
{
    if (type != mtx_plain)
        return thrd_error;
    mutex->state = 0;
    return thrd_success;
}

int mtx_lock(mtx_t *mutex)
{
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return thrd_success;
    while (__atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE) != 0)
        jelly_futex_wait(&mutex->state, 2, JELLY_WAIT_FOREVER);
    return thrd_success;
}

int mtx_trylock(mtx_t *mutex)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&mutex->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
               ? thrd_success
               : thrd_busy;
}

int mtx_unlock(mtx_t *mutex)
{
    if (__atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE) == 2)
        jelly_futex_wake(&mutex->state, 1);
    return thrd_success;
}

void mtx_destroy(mtx_t *mutex)
{
    (void)mutex;
}
