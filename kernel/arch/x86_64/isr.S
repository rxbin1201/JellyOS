/*
 * Interrupt, exception and system call entry (x86_64).
 *
 * Every vector gets a stub that pushes a uniform frame:
 *   [error code or 0] [vector] [rax ... r15]
 * and calls interrupt_dispatch(struct arch_interrupt_frame *).
 * The CPU aligns RSP to 16 bytes before pushing its frame; together with the
 * 22 quadwords below, RSP is 16-byte aligned at the call.
 *
 * GS: while in the kernel, GS_BASE points to the per-CPU block. Entries from
 * ring 3 (CS RPL = 3) and returns to ring 3 swap it with SWAPGS.
 */

#include "syscall_layout.h"

    .altmacro

/* Vectors for which the CPU pushes an error code. */
.macro isr_stub vector
isr_stub_\vector:
    .if (\vector == 8) || (\vector == 10) || (\vector == 11) || (\vector == 12) || (\vector == 13) || (\vector == 14) || (\vector == 17) || (\vector == 21) || (\vector == 29) || (\vector == 30)
    .else
    pushq   $0
    .endif
    pushq   $\vector
    jmp     isr_common
.endm

.macro push_registers
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
.endm

    .section .text
    .balign 16
isr_common:
    testb   $3, 24(%rsp)        /* CS of the interrupted context */
    jz      1f
    swapgs
1:  push_registers
    cld
    movq    %rsp, %rdi
    call    interrupt_dispatch

/* Restore a frame on the current stack and return (to ring 0 or ring 3). */
    .global interrupt_return
interrupt_return:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    addq    $16, %rsp           /* vector and error code */
    testb   $3, 8(%rsp)         /* CS we return to */
    jz      2f
    swapgs
2:  iretq

/*
 * SYSCALL entry. The CPU saved RIP in RCX and RFLAGS in R11 and cleared IF
 * (SFMASK). Build the same frame as an interrupt from ring 3, then run the
 * handler with interrupts enabled. Returns through interrupt_return (IRETQ).
 */
    .global syscall_entry
    .balign 16
syscall_entry:
    swapgs
    movq    %rsp, %gs:CPU_LOCAL_USER_RSP
    movq    %gs:CPU_LOCAL_KERNEL_RSP, %rsp
    pushq   $USER_DATA_SELECTOR             /* SS */
    pushq   %gs:CPU_LOCAL_USER_RSP          /* RSP */
    pushq   %r11                            /* RFLAGS */
    pushq   $USER_CODE_SELECTOR             /* CS */
    pushq   %rcx                            /* RIP */
    pushq   $0                              /* error code */
    pushq   $SYSCALL_FRAME_VECTOR           /* vector */
    push_registers
    cld
    sti
    movq    %rsp, %rdi
    call    syscall_handler
    cli
    jmp     interrupt_return

.set i, 0
.rept 256
    isr_stub %i
    .set i, i + 1
.endr

.macro stub_address vector
    .quad isr_stub_\vector
.endm

    .section .rodata
    .balign 8
    .global isr_stub_table
isr_stub_table:
.set i, 0
.rept 256
    stub_address %i
    .set i, i + 1
.endr

    .section .note.GNU-stack, "", @progbits
