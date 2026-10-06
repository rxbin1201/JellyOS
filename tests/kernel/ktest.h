/*
 * In-kernel test framework (README section 46: kernel tests).
 *
 * Tests are compiled into the kernel and run with "selftest=1" on the kernel
 * command line, or "selftest=exit" to end a QEMU run with the result
 * (used by `make test`).
 *
 *   KTEST(heap_alignment) {
 *       void *p = kmalloc(1);
 *       KASSERT(p != NULL);           // stops this test on failure
 *       KEXPECT(((uintptr_t)p & 15) == 0);  // records failure, continues
 *       kfree(p);
 *   }
 */

#ifndef TESTS_KERNEL_KTEST_H
#define TESTS_KERNEL_KTEST_H

#include <stdbool.h>

typedef struct {
    const char *name;
    void      (*run)(void);
} ktest_t;

#define KTEST(name)                                                                   \
    static void ktest_fn_##name(void);                                                \
    static const ktest_t ktest_entry_##name                                           \
        __attribute__((used, section(".ktests"), aligned(8))) = { #name, ktest_fn_##name }; \
    static void ktest_fn_##name(void)

void ktest_fail(const char *file, int line, const char *expression);

#define KEXPECT(cond)                                                                 \
    do {                                                                              \
        if (!(cond))                                                                  \
            ktest_fail(__FILE__, __LINE__, #cond);                                    \
    } while (0)

#define KASSERT(cond)                                                                 \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ktest_fail(__FILE__, __LINE__, #cond);                                    \
            return;                                                                   \
        }                                                                             \
    } while (0)

/* Run every registered test. Returns true if all passed. */
bool ktest_run_all(void);

#endif
