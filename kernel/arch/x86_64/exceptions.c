/*
 * CPU exception handling.
 *
 * In ring 0 every exception is fatal (panic) except breakpoints and page
 * faults the VMM can resolve. In ring 3 an unresolved exception terminates
 * only the faulting process.
 */

#include "cpu.h"
#include "interrupt.h"

#include "core/format.h"
#include "core/log.h"
#include "core/panic.h"
#include "memory/vmm.h"
#include "process/process.h"

#define VECTOR_BREAKPOINT   3
#define VECTOR_DOUBLE_FAULT 8
#define VECTOR_PAGE_FAULT   14

/* Page fault error code bits */
#define PF_PRESENT (1u << 0)
#define PF_WRITE   (1u << 1)
#define PF_USER    (1u << 2)
#define PF_FETCH   (1u << 4)

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

static char reason[192];

/* Kill the process for faults in ring 3, panic for faults in the kernel. */
__attribute__((noreturn)) static void fatal(struct arch_interrupt_frame *frame)
{
    if (interrupt_from_user(frame)) {
        size_t length = 0;
        while (reason[length])
            length++;
        format(reason + length, sizeof(reason) - length, " at rip %p", (void *)frame->rip);
        process_fault(reason);
    }
    panic_with_frame(frame, reason);
}

static void breakpoint(struct arch_interrupt_frame *frame)
{
    klog_warn("breakpoint at %p, continuing", (void *)frame->rip);
}

static void page_fault(struct arch_interrupt_frame *frame)
{
    uint64_t address = cpu_read_cr2();
    uint32_t e = (uint32_t)frame->error_code;
    uint32_t access = ((e & PF_PRESENT) ? VM_FAULT_PRESENT : 0) | ((e & PF_WRITE) ? VM_FAULT_WRITE : 0) |
                      ((e & PF_USER) ? VM_FAULT_USER : 0) | ((e & PF_FETCH) ? VM_FAULT_EXEC : 0);

    if (vmm_page_fault(address, access))
        return;

    format(reason, sizeof(reason), "page fault at %p: %s (%s %s%s)", (void *)address,
           vmm_fault_cause(address, access), (e & PF_USER) ? "user" : "kernel",
           (e & PF_FETCH) ? "instruction fetch" : (e & PF_WRITE) ? "write" : "read",
           (e & PF_PRESENT) ? ", page present" : "");
    fatal(frame);
}

/* A fault while pushing onto an overflowed stack escalates to a double fault. */
static void double_fault(struct arch_interrupt_frame *frame)
{
    uint64_t cr2 = cpu_read_cr2();

    if (vmm_is_stack_guard(cr2) || vmm_is_stack_guard(frame->rsp) || vmm_is_stack_guard(frame->rsp - 1))
        panic_with_frame(frame, "double fault: kernel stack overflow (guard page hit)");
    panic_with_frame(frame, "double fault");
}

void exception_handle(struct arch_interrupt_frame *frame)
{
    format(reason, sizeof(reason), "%s (vector %u, error code 0x%lx)", exception_names[frame->vector & 31],
           (unsigned)frame->vector, frame->error_code);
    fatal(frame);
}

void exceptions_init(void)
{
    interrupt_set_handler(VECTOR_BREAKPOINT, breakpoint);
    interrupt_set_handler(VECTOR_DOUBLE_FAULT, double_fault);
    interrupt_set_handler(VECTOR_PAGE_FAULT, page_fault);
}
