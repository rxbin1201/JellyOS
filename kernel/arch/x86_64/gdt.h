/*
 * x86_64 GDT and TSS. See docs/architecture/x86_64.md for the layout.
 */

#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

/* Order matches SYSCALL/SYSRET: user data must directly precede user code. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

/* Interrupt stack table slots (1-based, as used in IDT gates). */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3

#include <stdint.h>

void gdt_init(void);

/* Stack the CPU switches to on interrupts from ring 3 (TSS.RSP0). */
void gdt_set_kernel_stack(uint64_t top);

#endif
