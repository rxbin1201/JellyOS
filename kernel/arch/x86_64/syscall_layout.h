/*
 * Constants shared by C and assembly for the syscall path (x86_64).
 */

#ifndef ARCH_X86_64_SYSCALL_LAYOUT_H
#define ARCH_X86_64_SYSCALL_LAYOUT_H

/* struct cpu_local field offsets (syscall.c) */
#define CPU_LOCAL_KERNEL_RSP 0
#define CPU_LOCAL_USER_RSP   8

/* Ring 3 selectors: GDT_USER_DATA | 3, GDT_USER_CODE | 3 */
#define USER_DATA_SELECTOR   0x1B
#define USER_CODE_SELECTOR   0x23

/* Vector value stored in frames built by syscall_entry (not a real vector) */
#define SYSCALL_FRAME_VECTOR 0x100

#endif
