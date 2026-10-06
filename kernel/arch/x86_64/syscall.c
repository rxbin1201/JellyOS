/*
 * SYSCALL/SYSRET setup, per-CPU block and the C side of syscall entry.
 *
 * Calling convention (docs/abi/syscalls.md): RAX = number, arguments in
 * RDI, RSI, RDX, R10, R8, R9; result (status_t) in RAX. RCX and R11 are
 * clobbered by the instruction itself.
 */

#include "cpu.h"
#include "gdt.h"
#include "interrupt.h"
#include "syscall_layout.h"

#include "scheduler/thread.h"
#include "syscall/syscall.h"

#include <stddef.h>

#define MSR_STAR           0xC0000081
#define MSR_LSTAR          0xC0000082
#define MSR_SFMASK         0xC0000084
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define EFER_SCE           (1ULL << 0)

/* RFLAGS bits cleared on entry: TF, IF, DF, AC */
#define SYSCALL_FLAG_MASK  ((1u << 8) | (1u << 9) | (1u << 10) | (1u << 18))

struct cpu_local {
    uint64_t kernel_rsp; /* top of the current thread's kernel stack */
    uint64_t user_rsp;   /* scratch for the user stack pointer */
};

_Static_assert(offsetof(struct cpu_local, kernel_rsp) == CPU_LOCAL_KERNEL_RSP, "layout");
_Static_assert(offsetof(struct cpu_local, user_rsp) == CPU_LOCAL_USER_RSP, "layout");

static struct cpu_local boot_cpu;

extern void syscall_entry(void);

void syscall_init(void)
{
    cpu_write_msr(MSR_EFER, cpu_read_msr(MSR_EFER) | EFER_SCE);
    /* SYSCALL loads CS = STAR[47:32], SS = +8; SYSRET would use STAR[63:48] + 16 / + 8. */
    cpu_write_msr(MSR_STAR, ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    cpu_write_msr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    cpu_write_msr(MSR_SFMASK, SYSCALL_FLAG_MASK);

    /* In the kernel GS points at the per-CPU block; SWAPGS exchanges it on ring changes. */
    cpu_write_msr(MSR_GS_BASE, (uint64_t)(uintptr_t)&boot_cpu);
    cpu_write_msr(MSR_KERNEL_GS_BASE, 0);
}

void cpu_set_kernel_stack(uint64_t top)
{
    boot_cpu.kernel_rsp = top;
    gdt_set_kernel_stack(top);
}

void syscall_handler(struct arch_interrupt_frame *frame)
{
    const uint64_t args[6] = { frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9 };

    frame->rax = syscall_dispatch(frame->rax, args);
    thread_return_to_user();
}
