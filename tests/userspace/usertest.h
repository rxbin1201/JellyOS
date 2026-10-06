/*
 * Scenarios of the user-mode test program, shared with the kernel tests.
 */

#ifndef TESTS_USERSPACE_USERTEST_H
#define TESTS_USERSPACE_USERTEST_H

enum {
    SCENARIO_HELLO              = 0,
    SCENARIO_BAD_ARGUMENTS      = 1,
    SCENARIO_FAULT_KERNEL_READ  = 2,
    SCENARIO_FAULT_PRIVILEGED   = 3,
    SCENARIO_FAULT_WRITE_TEXT   = 4,
    SCENARIO_FAULT_NULL_CALL    = 5,
    SCENARIO_READ_ADDRESS       = 6,  /* arg1: address */
    SCENARIO_PING               = 7,  /* arg1: channel */
    SCENARIO_PONG               = 8,  /* arg1: channel */
    SCENARIO_SHM_WRITER         = 9,  /* arg1: shared memory, arg2: event */
    SCENARIO_SHM_READER         = 10, /* arg1: shared memory (map only), arg2: event (wait only) */
    SCENARIO_THREADS            = 11,
    SCENARIO_FPU                = 12,
    SCENARIO_SLEEP              = 13,
    SCENARIO_MEMORY             = 14,
    SCENARIO_RIGHTS             = 15,
    SCENARIO_EXIT_WITH_THREADS  = 16,
    SCENARIO_FILES              = 17, /* file system calls on the test disk */
    SCENARIO_UNPRIVILEGED       = 18, /* run with uid 1000: permission checks */
};

#define TEST_VOLUME            "/volumes/virtio0p1"
#define ROOT_ONLY_FILE         "/tmp/root-only"

#define SHM_TEST_SIZE          8192
#define FPU_DURATION_NS        100000000ULL /* 100 ms = 10 timeslices */
#define FPU_CHUNK              100000
#define EXIT_WITH_THREADS_CODE 7

#endif
