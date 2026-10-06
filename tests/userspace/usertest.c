/*
 * User-mode test program for the kernel's process tests
 * (tests/kernel/process_tests.c).
 *
 * arg0 selects the scenario, arg1/arg2 carry startup handles. Exit code 0
 * means success; any other code is the source line of the failed check.
 * Fault scenarios must be killed by the kernel before they return.
 */

#include <jelly/os.h>
#include <stdbool.h>

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

/* --- Files (syscall ABI version 2) -------------------------------------------- */

static size_t text_length(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static bool text_is(const char *buffer, size_t size, const char *expected)
{
    return size == text_length(expected) && bytes_equal(buffer, expected, size);
}

static int files(void)
{
    char buffer[128];
    size_t size;
    uint64_t position;
    jelly_handle_t file, dir;
    jelly_stat_t stat;
    jelly_dirent_t entry;

    /* Working directory and relative paths */
    CHECK(jelly_chdir(TEST_VOLUME) == STATUS_SUCCESS);
    CHECK(jelly_getcwd(buffer, sizeof(buffer), &size) == STATUS_SUCCESS && text_is(buffer, size, TEST_VOLUME));
    CHECK(jelly_getcwd(buffer, 4, &size) == STATUS_BUFFER_TOO_SMALL && size == text_length(TEST_VOLUME));
    CHECK(jelly_open("hello.txt", JELLY_OPEN_READ, 0, &file) == STATUS_SUCCESS);
    CHECK(jelly_read(file, buffer, sizeof(buffer), &size) == STATUS_SUCCESS);
    CHECK(text_is(buffer, size, "Hello from the JellyOS test disk!\n"));
    CHECK(jelly_read(file, buffer, sizeof(buffer), &size) == STATUS_SUCCESS && size == 0); /* end of file */
    CHECK(jelly_write(file, "x", 1, &size) == STATUS_ACCESS_DENIED);                     /* read-only handle */
    CHECK(jelly_handle_close(file) == STATUS_SUCCESS);

    /* Create, write, seek, read back */
    CHECK(jelly_mkdir("user", 0755) == STATUS_SUCCESS);
    CHECK(jelly_open("user/data.txt", JELLY_OPEN_READ | JELLY_OPEN_WRITE | JELLY_OPEN_CREATE, 0644, &file) ==
          STATUS_SUCCESS);
    CHECK(jelly_write(file, "user mode file", 14, &size) == STATUS_SUCCESS && size == 14);
    CHECK(jelly_seek(file, 5, JELLY_SEEK_SET, &position) == STATUS_SUCCESS && position == 5);
    CHECK(jelly_read(file, buffer, 4, &size) == STATUS_SUCCESS && text_is(buffer, size, "mode"));
    CHECK(jelly_fstat(file, &stat) == STATUS_SUCCESS && stat.size == 14 && stat.type == JELLY_FILE_TYPE_FILE);
    CHECK(jelly_truncate(file, 4) == STATUS_SUCCESS);
    CHECK(jelly_handle_close(file) == STATUS_SUCCESS);

    /* Listing, renaming, removing */
    CHECK(jelly_open("user", JELLY_OPEN_READ | JELLY_OPEN_DIRECTORY, 0, &dir) == STATUS_SUCCESS);
    CHECK(jelly_readdir(dir, &entry) == STATUS_SUCCESS && text_is(entry.name, entry.name_length, "data.txt"));
    CHECK(jelly_readdir(dir, &entry) == STATUS_NOT_FOUND);
    CHECK(jelly_handle_close(dir) == STATUS_SUCCESS);
    CHECK(jelly_rename("user/data.txt", "user/renamed.txt") == STATUS_SUCCESS);
    CHECK(jelly_stat(TEST_VOLUME "/user/../user/renamed.txt", 0, &stat) == STATUS_SUCCESS && stat.size == 4);
    CHECK(jelly_unlink("user") == STATUS_NOT_EMPTY);
    CHECK(jelly_unlink("user/renamed.txt") == STATUS_SUCCESS);
    CHECK(jelly_unlink("user") == STATUS_SUCCESS);

    /* Errors */
    CHECK(jelly_open("missing", JELLY_OPEN_READ, 0, &file) == STATUS_NOT_FOUND);
    CHECK(jelly_open("/volumes", JELLY_OPEN_WRITE, 0, &file) == STATUS_IS_DIRECTORY);
    CHECK(jelly_syscall(SYS_FILE_OPEN, 0xFFFF800000000000ULL, 5, JELLY_OPEN_READ, 0, (uint64_t)&file, 0) ==
          STATUS_INVALID_ARGUMENT);
    CHECK(jelly_syscall(SYS_FILE_OPEN, (uint64_t)"/tmp", 0, JELLY_OPEN_READ, 0, (uint64_t)&file, 0) ==
          STATUS_INVALID_ARGUMENT);

    /* Symbolic links (ramfs) */
    const char *target = TEST_VOLUME "/hello.txt";
    CHECK(jelly_symlink(target, "/tmp/hello-link") == STATUS_SUCCESS);
    CHECK(jelly_readlink("/tmp/hello-link", buffer, sizeof(buffer), &size) == STATUS_SUCCESS &&
          text_is(buffer, size, target));
    CHECK(jelly_open("/tmp/hello-link", JELLY_OPEN_READ, 0, &file) == STATUS_SUCCESS);
    CHECK(jelly_handle_close(file) == STATUS_SUCCESS);
    CHECK(jelly_unlink("/tmp/hello-link") == STATUS_SUCCESS);

    CHECK(jelly_sync() == STATUS_SUCCESS);
    return 0;
}

/* Started with uid/gid 1000: the kernel test prepared ROOT_ONLY_FILE (0600, root). */
static int unprivileged(void)
{
    jelly_handle_t file;
    jelly_stat_t stat;

    CHECK(jelly_open(ROOT_ONLY_FILE, JELLY_OPEN_READ, 0, &file) == STATUS_ACCESS_DENIED);
    CHECK(jelly_stat(ROOT_ONLY_FILE, 0, &stat) == STATUS_SUCCESS && stat.uid == 0 && stat.mode == 0600);
    CHECK(jelly_mkdir("/volumes/mine", 0755) == STATUS_ACCESS_DENIED); /* /volumes is root's, 0755 */
    CHECK(jelly_unmount(TEST_VOLUME) == STATUS_ACCESS_DENIED);
    CHECK(jelly_mount("/tmp", "virtio0p1", "fat32") == STATUS_ACCESS_DENIED);

    CHECK(jelly_open("/tmp/user-file", JELLY_OPEN_WRITE | JELLY_OPEN_CREATE, 0600, &file) == STATUS_SUCCESS);
    CHECK(jelly_fstat(file, &stat) == STATUS_SUCCESS && stat.uid == 1000);
    CHECK(jelly_handle_close(file) == STATUS_SUCCESS);
    CHECK(jelly_unlink("/tmp/user-file") == STATUS_SUCCESS);
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
    case SCENARIO_FILES:              return files();
    case SCENARIO_UNPRIVILEGED:       return unprivileged();
    }
    return NOT_REACHED;
}
