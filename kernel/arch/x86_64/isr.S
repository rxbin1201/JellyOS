/*
 * Interrupt and exception entry stubs (x86_64).
 *
 * Every vector gets a stub that pushes a uniform frame:
 *   [error code or 0] [vector] [rax ... r15]
 * and calls interrupt_dispatch(struct arch_interrupt_frame *).
 * The CPU aligns RSP to 16 bytes before pushing its frame; together with the
 * 22 quadwords below, RSP is 16-byte aligned at the call.
 */

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

    .section .text
    .balign 16
isr_common:
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
    cld
    movq    %rsp, %rdi
    call    interrupt_dispatch
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
    iretq

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
