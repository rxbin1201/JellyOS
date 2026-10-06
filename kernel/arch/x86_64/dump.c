/*
 * Register dump and stack trace for panics (README section 45).
 */

#include "cpu.h"
#include "interrupt.h"

#include "core/arch.h"
#include "core/ksyms.h"
#include "core/log.h"

#define MAX_FRAMES       32
#define KERNEL_HALF_BASE 0xFFFF800000000000ULL

static void print_location(const char *prefix, uint64_t address)
{
    uint64_t offset;
    const char *name = ksym_lookup(address, &offset);

    if (name)
        klog_raw("%s%p  %s+0x%lx\n", prefix, (void *)address, name, offset);
    else
        klog_raw("%s%p  ?\n", prefix, (void *)address);
}

/* Follow the RBP chain (kernel is built with frame pointers). */
static void stack_trace(uint64_t rbp)
{
    for (int depth = 0; depth < MAX_FRAMES; depth++) {
        if (rbp < KERNEL_HALF_BASE || (rbp & 7))
            return;
        const uint64_t *frame = (const uint64_t *)(uintptr_t)rbp;
        uint64_t return_address = frame[1];
        if (!return_address)
            return;
        /* Return addresses point after the call; step back into it for the symbol. */
        print_location("  ", return_address - 1);
        rbp = frame[0];
    }
    klog_raw("  ...\n");
}

void arch_dump_state(const struct arch_interrupt_frame *f)
{
    if (f) {
        print_location("RIP: ", f->rip);
        klog_raw("CS: 0x%lx  RFLAGS: 0x%lx  RSP: %p  SS: 0x%lx\n", f->cs, f->rflags, (void *)f->rsp, f->ss);
        klog_raw("RAX: %016lx  RBX: %016lx  RCX: %016lx\n", f->rax, f->rbx, f->rcx);
        klog_raw("RDX: %016lx  RSI: %016lx  RDI: %016lx\n", f->rdx, f->rsi, f->rdi);
        klog_raw("RBP: %016lx  R8:  %016lx  R9:  %016lx\n", f->rbp, f->r8, f->r9);
        klog_raw("R10: %016lx  R11: %016lx  R12: %016lx\n", f->r10, f->r11, f->r12);
        klog_raw("R13: %016lx  R14: %016lx  R15: %016lx\n", f->r13, f->r14, f->r15);
    }
    klog_raw("CR0: %016lx  CR2: %016lx  CR3: %016lx  CR4: %016lx\n",
             cpu_read_cr0(), cpu_read_cr2(), cpu_read_cr3(), cpu_read_cr4());

    klog_raw("stack trace:\n");
    if (f) {
        print_location("  ", f->rip);
        stack_trace(f->rbp);
    } else {
        stack_trace((uint64_t)(uintptr_t)__builtin_frame_address(0));
    }
}
