/*
 * Thread context switch and first entry of new threads (x86_64).
 */

    .section .text

/*
 * void arch_context_switch(uint64_t *from_rsp, uint64_t to_rsp)
 * Saves the callee-saved registers on the current stack, stores RSP, and
 * resumes the other thread where it called this function (or at its
 * start trampoline).
 */
    .global arch_context_switch
    .type arch_context_switch, @function
arch_context_switch:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
    .size arch_context_switch, . - arch_context_switch

/* New kernel thread: r12 = entry, r13 = argument. */
    .global arch_kernel_thread_start
    .type arch_kernel_thread_start, @function
arch_kernel_thread_start:
    movq    %r12, %rdi
    movq    %r13, %rsi
    call    thread_kernel_start         /* never returns */
    ud2
    .size arch_kernel_thread_start, . - arch_kernel_thread_start

/* New user thread: RSP points at a prepared interrupt frame. */
    .global arch_user_thread_start
    .type arch_user_thread_start, @function
arch_user_thread_start:
    call    thread_user_start
    cli
    jmp     interrupt_return
    .size arch_user_thread_start, . - arch_user_thread_start

    .section .note.GNU-stack, "", @progbits
