/*
 * Kernel entry point (x86_64).
 *
 * Entered from the JellyOS Boot Manager with the state described in
 * docs/architecture/boot-protocol.md: rdi = boot_info_t *, valid stack,
 * interrupts disabled.
 */

    .section .text.entry, "ax"
    .global _start
    .type _start, @function

_start:
    call    kernel_main         /* boot_info_t * is already in rdi */
1:  cli
    hlt
    jmp     1b

    .size _start, . - _start
    .section .note.GNU-stack, "", @progbits
