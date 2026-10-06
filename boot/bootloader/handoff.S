/*
 * void boot_handoff(uint64_t cr3, uint64_t stack_top, uint64_t boot_info, uint64_t entry)
 *                   rdi           rsi                 rdx                 rcx
 *
 * Runs after ExitBootServices(). Establishes the kernel entry state described
 * in docs/architecture/boot-protocol.md and never returns.
 */

    .section .text
    .global boot_handoff
    .global boot_handoff_end
    .type boot_handoff, @function

boot_handoff:
    cli
    cld
    mov     %rdi, %cr3          /* this page stays identity mapped */
    mov     %rsi, %rsp          /* 16-byte aligned stack in the direct map */
    mov     %rdx, %rdi          /* first argument: boot_info_t * */
    xor     %ebp, %ebp          /* terminate stack traces */
    push    $0                  /* fake return address: SysV alignment at entry */
    jmp     *%rcx
boot_handoff_end:

    .size boot_handoff, . - boot_handoff
    .section .note.GNU-stack, "", @progbits
