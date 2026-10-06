#include "tests/kernel/ktest.h"

#include "core/log.h"

extern const ktest_t __ktests_start[], __ktests_end[];

static unsigned current_failures;

void ktest_fail(const char *file, int line, const char *expression)
{
    current_failures++;
    klog_error("ktest:   %s:%d: expected %s", file, line, expression);
}

bool ktest_run_all(void)
{
    unsigned passed = 0, failed = 0;

    klog_info("ktest: running %u tests", (unsigned)(__ktests_end - __ktests_start));
    for (const ktest_t *t = __ktests_start; t < __ktests_end; t++) {
        current_failures = 0;
        t->run();
        if (current_failures) {
            failed++;
            klog_error("ktest: FAIL %s", t->name);
        } else {
            passed++;
            klog_info("ktest: ok   %s", t->name);
        }
    }
    klog_info("ktest: %u passed, %u failed", passed, failed);
    return failed == 0;
}
