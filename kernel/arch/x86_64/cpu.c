#include "core/arch.h"

void arch_halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
