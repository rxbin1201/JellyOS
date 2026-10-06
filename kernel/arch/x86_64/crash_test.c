/*
 * Deliberate CPU faults to verify exception handling ("crashtest=" on the
 * kernel command line).
 */

#include "core/arch.h"
#include "core/string.h"

#include <stdint.h>

/* An address in the lower half: nothing is mapped there in the kernel space. */
#define UNMAPPED_ADDRESS 0x1000ULL

static const uint32_t read_only_value = 0x4A454C4C;

/* Each frame keeps a buffer alive across the recursive call, so the stack must grow. */
__attribute__((noinline)) static unsigned recurse(unsigned depth)
{
    volatile char buffer[512];

    buffer[0] = (char)depth;
    if (depth == UINT32_MAX)
        return 0;
    return recurse(depth + 1) + buffer[0];
}

bool arch_crash_test(const char *kind)
{
    if (strcmp(kind, "divide") == 0) {
        __asm__ volatile("xor %%ecx, %%ecx; div %%ecx" : : : "rax", "rcx", "rdx");
    } else if (strcmp(kind, "pagefault") == 0) {
        (void)*(volatile uint64_t *)(uintptr_t)UNMAPPED_ADDRESS;
    } else if (strcmp(kind, "invalid-opcode") == 0) {
        __asm__ volatile("ud2");
    } else if (strcmp(kind, "breakpoint") == 0) {
        __asm__ volatile("int3");
    } else if (strcmp(kind, "write-rodata") == 0) {
        *(volatile uint32_t *)&read_only_value = 0;
    } else if (strcmp(kind, "stack-overflow") == 0) {
        recurse(0);
    } else {
        return false;
    }
    return true;
}
