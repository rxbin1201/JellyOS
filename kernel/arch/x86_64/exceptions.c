/*
 * CPU exception handling: every exception is fatal except breakpoints.
 */

#include "cpu.h"
#include "interrupt.h"

#include "core/format.h"
#include "core/log.h"
#include "core/panic.h"

#define VECTOR_BREAKPOINT 3
#define VECTOR_PAGE_FAULT 14

static const char *const exception_names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS", "segment not present",
    "stack-segment fault", "general protection fault", "page fault", "reserved (15)",
    "x87 floating-point error", "alignment check", "machine check", "SIMD floating-point error",
    "virtualization exception", "control protection exception", "reserved (22)", "reserved (23)",
    "reserved (24)", "reserved (25)", "reserved (26)", "reserved (27)",
    "hypervisor injection exception", "VMM communication exception", "security exception", "reserved (31)",
};

static void breakpoint(struct arch_interrupt_frame *frame)
{
    klog_warn("breakpoint at %p, continuing", (void *)frame->rip);
}

void exception_handle(struct arch_interrupt_frame *frame)
{
    static char reason[160];
    const char *name = exception_names[frame->vector & 31];

    if (frame->vector == VECTOR_PAGE_FAULT) {
        uint64_t e = frame->error_code;
        format(reason, sizeof(reason), "page fault at %p (%s, %s, %s%s)", (void *)cpu_read_cr2(),
               (e & 1) ? "protection violation" : "page not present",
               (e & 2) ? "write" : "read",
               (e & 4) ? "user" : "kernel",
               (e & 16) ? ", instruction fetch" : "");
    } else {
        format(reason, sizeof(reason), "%s (vector %u, error code 0x%lx)", name,
               (unsigned)frame->vector, frame->error_code);
    }
    panic_with_frame(frame, reason);
}

void exceptions_init(void)
{
    interrupt_set_handler(VECTOR_BREAKPOINT, breakpoint);
}
