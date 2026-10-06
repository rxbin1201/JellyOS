/*
 * User-mode test program for the kernel's process tests
 * (tests/kernel/process_tests.c).
 *
 * arg0 selects the scenario, arg1/arg2 carry startup handles. Exit code 0
 * means success; any other code is the source line of the failed check.
 * Fault scenarios must be killed by the kernel before they return.
 */

#include <jelly/os.h>

#include "usertest.h"

#define CHECK(cond)              \
    do {                         \
        if (!(cond))             \
            return __LINE__;     \
    } while (0)

#define NOT_REACHED 1000

int main(uint64_t scenario, uint64_t arg1, uint64_t arg2);

static int bytes_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return 0;
    }
    return 1;
}

/* --- Basics and isolation ---------------------------------------------------- */

static int hello(void)
{
    jelly_print("hello from user mode");
    CHECK(jelly_abi_version() == JELLY_SYSCALL_ABI_VERSION);
    CHECK(jelly_clock_ns() > 0);
    return 0;
}

static int bad_arguments(void)
{
    void *p;
    const uint64_t kernel = 0xFFFFFFFF80000000ULL;

    CHECK(jelly_syscall(SYS_ABI_VERSION, kernel, 0, 0, 0, 0, 0) == STATUS_INVALID_ARGUMENT);
    CHECK(jelly_syscall(SYS_DEBUG_WRITE, 0xFFFF800000000000ULL, 16, 0, 0, 0, 0) == STATUS_INVALID_ARGUMENT);
    CHECK(jelly_syscall(SYS_COUNT + 7, 0, 0, 0, 0, 0, 0) == STATUS_NOT_SUPPORTED);
    CHECK(jelly_handle_close(0x12345) == STATUS_BAD_HANDLE);
    CHECK(jelly_wait(JELLY_HANDLE_INVALID, 0) == STATUS_BAD_HANDLE);
    CHECK(jelly_memory_allocate(4096, JELLY_MEMORY_WRITE | JELLY_MEMORY_EXEC, &p) == STATUS_INVALID_ARGUMENT);
    CHECK(jelly_memory_allocate(4096, JELLY_MEMORY_WRITE, (void **)kernel) == STATUS_INVALID_ARGUMENT);

    /* Output pointer into our own read-only code. */
    jelly_handle_t *read_only = (jelly_handle_t *)(uintptr_t)&main;
    CHECK(jelly_event_create(0, read_only) == STATUS_INVALID_ARGUMENT);
    return 0;
}

static int read_address(uint64_t address)
{
    volatile uint32_t value = *(volatile uint32_t *)(uintptr_t)address;
    (void)value;
    return NOT_REACHED;
}

static int fault(uint64_t kind)
{
    switch (kind) {
    case SCENARIO_FAULT_KERNEL_READ:
        return read_address(0xFFFFFFFF80000000ULL);
    case SCENARIO_FAULT_PRIVILEGED:
        __asm__ volatile("cli");
        break;
    case SCENARIO_FAULT_WRITE_TEXT:
        *(volatile uint8_t *)(uintptr_t)&main = 0xC3;
        break;
    case SCENARIO_FAULT_NULL_CALL: {
        void (*volatile function)(void) = 0;
        function();
        break;
    }
    }
    return NOT_REACHED;
}

/* --- IPC --------------------------------------------------------------------- */

static int ping(jelly_handle_t channel)
{
    char reply[8];
    size_t size;

    CHECK(jelly_channel_send(channel, "ping", 4) == STATUS_SUCCESS);
    CHECK(jelly_wait(channel, JELLY_WAIT_FOREVER) == STATUS_SUCCESS);
    CHECK(jelly_channel_receive(channel, reply, 2, &size) == STATUS_BUFFER_TOO_SMALL && size == 4);
    CHECK(jelly_channel_receive(channel, reply, sizeof(reply), &size) == STATUS_SUCCESS);
    CHECK(size == 4 && bytes_equal(reply, "pong", 4));
    return 0;
}

static int pong(jelly_handle_t channel)
{
    char message[8];
    size_t size;

    CHECK(jelly_wait(channel, JELLY_WAIT_FOREVER) == STATUS_SUCCESS);
    CHECK(jelly_channel_receive(channel, message, sizeof(message), &size) == STATUS_SUCCESS);
    CHECK(size == 4 && bytes_equal(message, "ping", 4));
    CHECK(jelly_channel_receive(channel, message, sizeof(message), &size) == STATUS_WOULD_BLOCK);
    CHECK(jelly_channel_send(channel, "pong", 4) == STATUS_SUCCESS);
    return 0;
}

static int shm_writer(jelly_handle_t shm, jelly_handle_t done)
{
    uint8_t *memory;

    CHECK(jelly_shm_map(shm, JELLY_MEMORY_WRITE, (void **)&memory) == STATUS_SUCCESS);
    for (int i = 0; i < SHM_TEST_SIZE; i++)
        memory[i] = (uint8_t)(i * 7);
    CHECK(jelly_event_signal(done) == STATUS_SUCCESS);
    return 0;
}

static int shm_reader(jelly_handle_t shm, jelly_handle_t done)
{
    uint8_t *memory;

    CHECK(jelly_wait(done, JELLY_WAIT_FOREVER) == STATUS_SUCCESS);
    CHECK(jelly_shm_map(shm, JELLY_MEMORY_WRITE, (void **)&memory) == STATUS_ACCESS_DENIED);
    CHECK(jelly_shm_map(shm, 0, (void **)&memory) == STATUS_SUCCESS);
    for (int i = 0; i < SHM_TEST_SIZE; i++)
        CHECK(memory[i] == (uint8_t)(i * 7));
    CHECK(jelly_memory_unmap(memory, SHM_TEST_SIZE) == STATUS_SUCCESS);
    return 0;
}

/* --- Threads, futex, FPU ----------------------------------------------------- */

#define WORKERS      4
#define INCREMENTS   2000
#define WORKER_STACK 16384

static uint32_t lock_word;
static volatile uint64_t counter;

static void lock(void)
{
    while (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE))
        jelly_futex_wait(&lock_word, 1, JELLY_WAIT_FOREVER);
}

static void unlock(void)
{
    __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE);
    jelly_futex_wake(&lock_word, 1);
}

static void worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < INCREMENTS; i++) {
        lock();
        uint64_t value = counter;
        if (i % 100 == 0)
            jelly_thread_yield(); /* force contention inside the critical section */
        counter = value + 1;
        unlock();
    }
}

static int threads(void)
{
    jelly_handle_t handles[WORKERS];
    uint8_t *stacks;

    CHECK(jelly_memory_allocate(WORKERS * WORKER_STACK, JELLY_MEMORY_WRITE, (void **)&stacks) == STATUS_SUCCESS);
    for (int i = 0; i < WORKERS; i++)
        CHECK(jelly_thread_create(worker, 0, stacks + (i + 1) * WORKER_STACK, &handles[i]) == STATUS_SUCCESS);
    for (int i = 0; i < WORKERS; i++) {
        CHECK(jelly_wait(handles[i], JELLY_WAIT_FOREVER) == STATUS_SUCCESS);
        CHECK(jelly_handle_close(handles[i]) == STATUS_SUCCESS);
    }
    CHECK(counter == WORKERS * INCREMENTS);
    return 0;
}

static double basel_sum(int terms)
{
    double sum = 0.0;
    for (int i = 1; i <= terms; i++)
        sum += 1.0 / ((double)i * (double)i);
    return sum;
}

static int fpu(void)
{
    /*
     * Accumulate for a fixed time instead of a fixed count, so the loop is
     * preempted many times on any CPU while another process also uses SSE.
     * A lost or mixed-up register state would make the two sums differ.
     */
    uint64_t start = jelly_clock_ns();
    double first = 0.0;
    int terms = 0;

    while (jelly_clock_ns() - start < FPU_DURATION_NS) {
        for (int end = terms + FPU_CHUNK; terms < end;) {
            terms++;
            first += 1.0 / ((double)terms * (double)terms);
        }
    }
    double second = basel_sum(terms);

    CHECK(first == second);
    CHECK(first > 1.6449 && first < 1.6450); /* pi^2 / 6 = 1.644934... */
    return 0;
}

/* --- Time, memory, handles, exit --------------------------------------------- */

static int sleep_test(void)
{
    uint64_t start = jelly_clock_ns();
    CHECK(jelly_thread_sleep(50000000) == STATUS_SUCCESS);
    CHECK(jelly_clock_ns() - start >= 50000000);
    return 0;
}

static int memory(void)
{
    uint8_t *block;

    CHECK(jelly_memory_allocate(1 << 20, JELLY_MEMORY_WRITE, (void **)&block) == STATUS_SUCCESS);
    for (int i = 0; i < (1 << 20); i += 4096)
        block[i] = (uint8_t)i;
    CHECK(block[4096 * 3] == (uint8_t)(4096 * 3));
    CHECK(jelly_memory_unmap(block + 1, 4096) == STATUS_INVALID_ARGUMENT);
    CHECK(jelly_memory_unmap(block, 1 << 20) == STATUS_SUCCESS);

    void *huge;
    CHECK(jelly_memory_allocate(256u << 20, JELLY_MEMORY_WRITE, &huge) == STATUS_LIMIT_EXCEEDED);
    return 0;
}

static int rights(void)
{
    jelly_handle_t event, waiter, copy, other;

    CHECK(jelly_event_create(0, &event) == STATUS_SUCCESS);
    CHECK(jelly_handle_duplicate(event, JELLY_RIGHT_WAIT, &waiter) == STATUS_SUCCESS);
    CHECK(jelly_event_signal(waiter) == STATUS_ACCESS_DENIED);
    CHECK(jelly_handle_duplicate(waiter, JELLY_RIGHT_WAIT, &copy) == STATUS_ACCESS_DENIED); /* no DUPLICATE right */
    CHECK(jelly_handle_duplicate(event, JELLY_RIGHTS_ALL, &copy) == STATUS_ACCESS_DENIED);  /* rights never grow */
    CHECK(jelly_handle_duplicate(event, JELLY_RIGHT_WAIT | JELLY_RIGHT_SIGNAL, &copy) == STATUS_SUCCESS);

    CHECK(jelly_wait(waiter, JELLY_NO_WAIT) == STATUS_TIMEOUT);
    CHECK(jelly_event_signal(event) == STATUS_SUCCESS);
    CHECK(jelly_wait(waiter, JELLY_NO_WAIT) == STATUS_SUCCESS);
    CHECK(jelly_wait(waiter, JELLY_NO_WAIT) == STATUS_SUCCESS); /* manual reset stays signaled */

    /* A closed handle stays invalid even when its slot is reused. */
    CHECK(jelly_handle_close(waiter) == STATUS_SUCCESS);
    CHECK(jelly_event_create(JELLY_EVENT_AUTO_RESET, &other) == STATUS_SUCCESS);
    CHECK(other != waiter);
    CHECK(jelly_handle_close(waiter) == STATUS_BAD_HANDLE);

    CHECK(jelly_event_signal(other) == STATUS_SUCCESS);
    CHECK(jelly_wait(other, JELLY_NO_WAIT) == STATUS_SUCCESS);
    CHECK(jelly_wait(other, JELLY_NO_WAIT) == STATUS_TIMEOUT); /* auto reset */
    return 0;
}

static void block_forever(void *event)
{
    jelly_wait((jelly_handle_t)(uintptr_t)event, JELLY_WAIT_FOREVER);
}

static int exit_with_threads(void)
{
    jelly_handle_t event, thread;
    uint8_t *stack;

    CHECK(jelly_event_create(0, &event) == STATUS_SUCCESS);
    CHECK(jelly_memory_allocate(WORKER_STACK, JELLY_MEMORY_WRITE, (void **)&stack) == STATUS_SUCCESS);
    CHECK(jelly_thread_create(block_forever, (void *)(uintptr_t)event, stack + WORKER_STACK, &thread) ==
          STATUS_SUCCESS);
    jelly_thread_sleep(10000000);
    jelly_process_exit(EXIT_WITH_THREADS_CODE); /* must also end the blocked thread */
}

int main(uint64_t scenario, uint64_t arg1, uint64_t arg2)
{
    switch (scenario) {
    case SCENARIO_HELLO:              return hello();
    case SCENARIO_BAD_ARGUMENTS:      return bad_arguments();
    case SCENARIO_FAULT_KERNEL_READ:
    case SCENARIO_FAULT_PRIVILEGED:
    case SCENARIO_FAULT_WRITE_TEXT:
    case SCENARIO_FAULT_NULL_CALL:    return fault(scenario);
    case SCENARIO_READ_ADDRESS:       return read_address(arg1);
    case SCENARIO_PING:               return ping((jelly_handle_t)arg1);
    case SCENARIO_PONG:               return pong((jelly_handle_t)arg1);
    case SCENARIO_SHM_WRITER:         return shm_writer((jelly_handle_t)arg1, (jelly_handle_t)arg2);
    case SCENARIO_SHM_READER:         return shm_reader((jelly_handle_t)arg1, (jelly_handle_t)arg2);
    case SCENARIO_THREADS:            return threads();
    case SCENARIO_FPU:                return fpu();
    case SCENARIO_SLEEP:              return sleep_test();
    case SCENARIO_MEMORY:             return memory();
    case SCENARIO_RIGHTS:             return rights();
    case SCENARIO_EXIT_WITH_THREADS:  return exit_with_threads();
    }
    return NOT_REACHED;
}
