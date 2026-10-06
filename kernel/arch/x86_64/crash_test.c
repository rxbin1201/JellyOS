/*
 * Deliberate CPU faults to verify exception handling ("crashtest=" on the
 * kernel command line).
 */

#include "core/arch.h"
#include "core/string.h"

#include <stdint.h>

/* An address in the lower half; only the boot trampoline is mapped there. */
#define UNMAPPED_ADDRESS 0x1000ULL

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
    } else {
        return false;
    }
    return true;
}
