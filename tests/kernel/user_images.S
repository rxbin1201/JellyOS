/*
 * User programs embedded into the kernel for the process tests.
 * Phase 7 replaces this with loading programs from the initramfs.
 */

    .section .rodata
    .balign 16
    .global usertest_image_start
    .global usertest_image_end
usertest_image_start:
    .incbin "build/tests/userspace/usertest.elf"
usertest_image_end:

    .section .note.GNU-stack, "", @progbits
