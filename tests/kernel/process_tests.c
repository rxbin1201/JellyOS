/*
 * Kernel tests: processes, threads, system calls, handles and IPC.
 *
 * Each test starts the embedded user program (tests/userspace/usertest.c)
 * with a scenario number and checks its exit code. Together they cover
 * milestone M3: isolated userspace processes.
 */

#include "tests/kernel/ktest.h"
#include "tests/userspace/usertest.h"

#include "core/handle.h"
#include "core/string.h"
#include "ipc/ipc.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "memory/pmm.h"
#include "process/process.h"
#include "scheduler/scheduler.h"

#include <jelly/syscall.h>

#define PROCESS_TIMEOUT_NS 20000000000ULL /* 20 s, generous for emulation */
#define NOT_FINISHED       (-999)

extern const uint8_t usertest_image_start[], usertest_image_end[];

typedef struct {
    object_t *object;
    uint32_t  rights;
} startup_handle_t;

static process_t *spawn(uint64_t scenario, startup_handle_t h1, startup_handle_t h2, uint64_t raw_arg)
{
    process_t *p;
    uint64_t entry;
    handle_t a = 0, b = 0;

    if (STATUS_IS_ERROR(process_create("usertest", &p)))
        return NULL;
    if (STATUS_IS_ERROR(process_load_elf(p, usertest_image_start,
                                         (size_t)(usertest_image_end - usertest_image_start), &entry)) ||
        (h1.object && STATUS_IS_ERROR(handle_install(&p->handles, h1.object, h1.rights, &a))) ||
        (h2.object && STATUS_IS_ERROR(handle_install(&p->handles, h2.object, h2.rights, &b))) ||
        STATUS_IS_ERROR(process_start(p, entry, scenario, h1.object ? a : raw_arg, b))) {
        object_release(&p->object);
        return NULL;
    }
    return p;
}

/* Wait for the process and return its exit code; reason may be NULL. */
static int32_t finish(process_t *p, char *reason, size_t reason_size)
{
    if (!p)
        return NOT_FINISHED;

    int32_t code = NOT_FINISHED;
    if (object_wait(&p->object, PROCESS_TIMEOUT_NS) == STATUS_SUCCESS)
        code = p->exit_code;
    if (reason) {
        size_t n = strlen(p->exit_reason) < reason_size - 1 ? strlen(p->exit_reason) : reason_size - 1;
        memcpy(reason, p->exit_reason, n);
        reason[n] = '\0';
    }
    object_release(&p->object);
    return code;
}

static int32_t run(uint64_t scenario)
{
    startup_handle_t none = { 0, 0 };
    return finish(spawn(scenario, none, none, 0), NULL, 0);
}

static bool contains(const char *text, const char *word)
{
    size_t n = strlen(word);
    for (; *text; text++) {
        if (strncmp(text, word, n) == 0)
            return true;
    }
    return false;
}

static uint64_t free_memory(void)
{
    pmm_stats_t pmm;
    heap_stats_t heap;
    pmm_get_stats(&pmm);
    heap_get_stats(&heap);
    /* Heap growth also consumes frames; count them as available. */
    return pmm.free_bytes + heap.pages * PAGE_SIZE;
}

/* --- Basics -------------------------------------------------------------------- */

KTEST(process_runs_in_user_mode)
{
    KEXPECT(run(SCENARIO_HELLO) == 0);
}

KTEST(process_resources_are_released)
{
    run(SCENARIO_HELLO); /* warm up heap size classes */
    uint64_t before = free_memory();
    KEXPECT(run(SCENARIO_HELLO) == 0);
    KEXPECT(run(SCENARIO_THREADS) == 0);
    KEXPECT(free_memory() == before);
}

KTEST(syscalls_validate_arguments)
{
    KEXPECT(run(SCENARIO_BAD_ARGUMENTS) == 0);
}

/* --- Isolation (README section 41) ---------------------------------------------- */

KTEST(process_faults_kill_only_the_process)
{
    static const struct { uint64_t scenario; const char *cause; } faults[] = {
        { SCENARIO_FAULT_KERNEL_READ, "user access to kernel page" },
        { SCENARIO_FAULT_PRIVILEGED,  "general protection fault" },
        { SCENARIO_FAULT_WRITE_TEXT,  "write to read-only page" },
        { SCENARIO_FAULT_NULL_CALL,   "null pointer" },
    };
    startup_handle_t none = { 0, 0 };
    char reason[PROCESS_REASON_MAX];

    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        int32_t code = finish(spawn(faults[i].scenario, none, none, 0), reason, sizeof(reason));
        KEXPECT(code == JELLY_EXIT_FAULT);
        KEXPECT(contains(reason, faults[i].cause));
    }
    /* The kernel and other processes are unaffected. */
    KEXPECT(run(SCENARIO_HELLO) == 0);
}

KTEST(process_cannot_see_other_address_spaces)
{
    startup_handle_t none = { 0, 0 };
    char reason[PROCESS_REASON_MAX];

    /* USER_MAP_BASE holds other processes' allocations, but nothing in a fresh one. */
    int32_t code = finish(spawn(SCENARIO_READ_ADDRESS, none, none, USER_MAP_BASE), reason, sizeof(reason));
    KEXPECT(code == JELLY_EXIT_FAULT);
    KEXPECT(contains(reason, "unmapped user address"));
}

/* --- Threads and scheduling -------------------------------------------------------- */

KTEST(threads_and_futex_mutex)
{
    KEXPECT(run(SCENARIO_THREADS) == 0);
}

KTEST(preemption_preserves_fpu_state)
{
    startup_handle_t none = { 0, 0 };
    uint64_t switches = scheduler_switch_count();

    process_t *a = spawn(SCENARIO_FPU, none, none, 0);
    process_t *b = spawn(SCENARIO_FPU, none, none, 0);
    KEXPECT(finish(a, NULL, 0) == 0);
    KEXPECT(finish(b, NULL, 0) == 0);
    KEXPECT(scheduler_switch_count() - switches > 10); /* they were really interleaved */
}

KTEST(sleep_and_clock)
{
    KEXPECT(run(SCENARIO_SLEEP) == 0);
}

KTEST(exit_kills_blocked_threads)
{
    KEXPECT(run(SCENARIO_EXIT_WITH_THREADS) == EXIT_WITH_THREADS_CODE);
}

/* --- Memory, handles, IPC ---------------------------------------------------------- */

KTEST(memory_allocation_and_limits)
{
    KEXPECT(run(SCENARIO_MEMORY) == 0);
}

KTEST(handle_rights_and_generations)
{
    KEXPECT(run(SCENARIO_RIGHTS) == 0);
}

KTEST(channel_between_processes)
{
    object_t *end0, *end1;
    const uint32_t rights = JELLY_RIGHT_READ | JELLY_RIGHT_WRITE | JELLY_RIGHT_WAIT;

    KASSERT(channel_create(&end0, &end1) == STATUS_SUCCESS);
    process_t *a = spawn(SCENARIO_PING, (startup_handle_t){ end0, rights }, (startup_handle_t){ 0, 0 }, 0);
    process_t *b = spawn(SCENARIO_PONG, (startup_handle_t){ end1, rights }, (startup_handle_t){ 0, 0 }, 0);
    object_release(end0);
    object_release(end1);
    KEXPECT(finish(a, NULL, 0) == 0);
    KEXPECT(finish(b, NULL, 0) == 0);
}

KTEST(shared_memory_between_processes)
{
    object_t *shm, *event;

    KASSERT(shm_create(SHM_TEST_SIZE, &shm) == STATUS_SUCCESS);
    KASSERT(event_create(0, &event) == STATUS_SUCCESS);
    process_t *writer = spawn(SCENARIO_SHM_WRITER, (startup_handle_t){ shm, JELLY_RIGHT_MAP | JELLY_RIGHT_WRITE },
                              (startup_handle_t){ event, JELLY_RIGHT_SIGNAL }, 0);
    process_t *reader = spawn(SCENARIO_SHM_READER, (startup_handle_t){ shm, JELLY_RIGHT_MAP },
                              (startup_handle_t){ event, JELLY_RIGHT_WAIT }, 0);
    object_release(shm);
    object_release(event);
    KEXPECT(finish(writer, NULL, 0) == 0);
    KEXPECT(finish(reader, NULL, 0) == 0);
}

KTEST(channel_reports_closed_peer)
{
    object_t *end0, *end1;
    size_t size;
    void *data;

    KASSERT(channel_create(&end0, &end1) == STATUS_SUCCESS);
    void *message = kmalloc(5);
    memcpy(message, "jelly", 5);
    KEXPECT(channel_send(end0, message, 5) == STATUS_SUCCESS);
    object_release(end0);

    /* Queued messages survive the peer; afterwards the channel reports PEER_CLOSED. */
    KEXPECT(channel_peek(end1, &size) == STATUS_SUCCESS && size == 5);
    KEXPECT(channel_take(end1, &data, &size) == STATUS_SUCCESS && memcmp(data, "jelly", 5) == 0);
    kfree(data);
    KEXPECT(channel_peek(end1, &size) == STATUS_PEER_CLOSED);
    KEXPECT(object_wait(end1, 0) == STATUS_SUCCESS); /* closed peer counts as readable */
    object_release(end1);
}
