/*
 * Thread contexts (x86_64): saved stack pointer plus FPU/SSE state.
 *
 * The kernel is built without SSE, so only user threads use the FPU; its
 * state is saved and restored eagerly on every switch (FXSAVE/FXRSTOR).
 */

#include "cpu.h"
#include "gdt.h"
#include "interrupt.h"
#include "syscall_layout.h"

#include "core/arch.h"
#include "core/string.h"
#include "memory/heap.h"

#define FXSAVE_FCW_OFFSET   0
#define FXSAVE_MXCSR_OFFSET 24
#define DEFAULT_FCW         0x037F /* all x87 exceptions masked, extended precision */
#define DEFAULT_MXCSR       0x1F80 /* all SSE exceptions masked, round to nearest */
#define USER_RFLAGS         0x202  /* IF, reserved bit 1 */

struct arch_thread {
    uint64_t rsp;
    uint8_t  fpu[512] __attribute__((aligned(16)));
};

extern void arch_context_switch(uint64_t *from_rsp, uint64_t to_rsp);
extern void arch_kernel_thread_start(void);
extern void arch_user_thread_start(void);
void cpu_set_kernel_stack(uint64_t top);

/* Callee-saved registers as arch_context_switch pops them. */
struct switch_frame {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t return_address;
};

static struct arch_thread *alloc_context(void)
{
    struct arch_thread *t = kcalloc(1, sizeof(*t));
    if (!t)
        return NULL;
    *(uint16_t *)&t->fpu[FXSAVE_FCW_OFFSET] = DEFAULT_FCW;
    *(uint32_t *)&t->fpu[FXSAVE_MXCSR_OFFSET] = DEFAULT_MXCSR;
    return t;
}

status_t arch_thread_create_kernel(struct arch_thread **thread, uint64_t stack_top,
                                   void (*entry)(void *), void *arg)
{
    struct arch_thread *t = alloc_context();
    if (!t)
        return STATUS_OUT_OF_MEMORY;

    /* After the pops and RET the stack is 16-byte aligned for the trampoline's call. */
    struct switch_frame *f = (struct switch_frame *)(stack_top - sizeof(*f));
    memset(f, 0, sizeof(*f));
    f->r12 = (uint64_t)(uintptr_t)entry;
    f->r13 = (uint64_t)(uintptr_t)arg;
    f->return_address = (uint64_t)(uintptr_t)arch_kernel_thread_start;
    t->rsp = (uint64_t)(uintptr_t)f;
    *thread = t;
    return STATUS_SUCCESS;
}

status_t arch_thread_create_user(struct arch_thread **thread, uint64_t stack_top, uint64_t ip, uint64_t sp,
                                 uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    struct arch_thread *t = alloc_context();
    if (!t)
        return STATUS_OUT_OF_MEMORY;

    /* The interrupt frame interrupt_return will consume, at the top of the kernel stack. */
    struct arch_interrupt_frame *frame = (struct arch_interrupt_frame *)(stack_top - sizeof(*frame));
    memset(frame, 0, sizeof(*frame));
    frame->rip = ip;
    frame->cs = USER_CODE_SELECTOR;
    frame->rflags = USER_RFLAGS;
    frame->rsp = sp;
    frame->ss = USER_DATA_SELECTOR;
    frame->rdi = arg0;
    frame->rsi = arg1;
    frame->rdx = arg2;

    struct switch_frame *f = (struct switch_frame *)((uint64_t)(uintptr_t)frame - sizeof(*f));
    memset(f, 0, sizeof(*f));
    f->return_address = (uint64_t)(uintptr_t)arch_user_thread_start;
    t->rsp = (uint64_t)(uintptr_t)f;
    *thread = t;
    return STATUS_SUCCESS;
}

status_t arch_thread_create_current(struct arch_thread **thread)
{
    struct arch_thread *t = alloc_context();
    if (!t)
        return STATUS_OUT_OF_MEMORY;
    *thread = t; /* rsp is filled in when this thread is first switched away from */
    return STATUS_SUCCESS;
}

void arch_thread_destroy(struct arch_thread *thread)
{
    kfree(thread);
}

void arch_thread_switch(struct arch_thread *from, struct arch_thread *to, uint64_t to_stack_top)
{
    __asm__ volatile("fxsave64 %0" : "=m"(from->fpu));
    __asm__ volatile("fxrstor64 %0" : : "m"(to->fpu));
    cpu_set_kernel_stack(to_stack_top);
    arch_context_switch(&from->rsp, to->rsp);
}
