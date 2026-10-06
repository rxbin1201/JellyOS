/*
 * User programs embedded into the kernel for the tests:
 *   usertest  - bare libos program, started directly (process_tests.c)
 *   spawntest - libc program, written to the ramfs and spawned (userspace_tests.c)
 */

    .section .rodata
    .balign 16
    .global usertest_image_start
    .global usertest_image_end
usertest_image_start:
    .incbin "build/tests/userspace/usertest.elf"
usertest_image_end:

    .balign 16
    .global spawntest_image_start
    .global spawntest_image_end
spawntest_image_start:
    .incbin "build/userspace/programs/spawntest.elf"
spawntest_image_end:

    .section .note.GNU-stack, "", @progbits
