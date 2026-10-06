/*
 * Kernel entry point (x86_64).
 *
 * Entered from the JellyOS Boot Manager with the state described in
 * docs/architecture/boot-protocol.md: rdi = boot_info_t *, interrupts disabled,
 * running on the boot manager's stack. Switches to the kernel's own stack
 * right away so the boot stack can be reclaimed later.
 */

#define KERNEL_STACK_SIZE 65536

    .section .text.entry, "ax"
    .global _start
    .type _start, @function

_start:
    movq    $kernel_stack_top, %rsp
    xorl    %ebp, %ebp              /* end of the stack trace */
    call    kernel_main             /* boot_info_t * is still in rdi */
1:  cli
    hlt
    jmp     1b

    .size _start, . - _start

    .section .bss
    .balign 16
kernel_stack:
    .skip   KERNEL_STACK_SIZE
kernel_stack_top:

    .section .note.GNU-stack, "", @progbits
