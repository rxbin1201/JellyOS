#include "core/panic.h"
#include "core/export.h"

#include "core/arch.h"
#include "core/format.h"
#include "core/log.h"

#include <stdarg.h>

static int panicking;

__attribute__((noreturn))
static void panic_common(const struct arch_interrupt_frame *frame, const char *reason)
{
    arch_interrupts_disable();

    /* A panic inside the panic handler must not recurse. */
    if (panicking++) {
        /* Possibly the screen console is what failed: say the rest on the serial port only. */
        kconsole_set_mirror(NULL);
        if (panicking == 2) {
            klog_raw("\n*** panic while handling a panic: %s ***\n", reason);
            arch_dump_state(frame);
        }
        kconsole_write("\n*** nested panic, halting ***\n", 31);
        arch_halt();
    }

    klog_raw("\n*** KERNEL PANIC on CPU %u ***\n", arch_cpu_id());
    klog_raw("reason: %s\n", reason);
    arch_dump_state(frame);
    klog_raw("*** system halted ***\n");
    arch_halt();
}

void panic(const char *fmt, ...)
{
    static char reason[256];
    va_list args;

    va_start(args, fmt);
    format_v(reason, sizeof(reason), fmt, args);
    va_end(args);
    panic_common(NULL, reason);
}

void panic_with_frame(const struct arch_interrupt_frame *frame, const char *reason)
{
    panic_common(frame, reason);
}

EXPORT_SYMBOL(panic);
