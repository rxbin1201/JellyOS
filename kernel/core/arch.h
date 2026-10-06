/*
 * Interface between generic kernel code and the architecture layer.
 * Implemented in kernel/arch/<arch>/.
 */

#ifndef CORE_ARCH_H
#define CORE_ARCH_H

void arch_early_console_init(void);
void arch_early_console_write(const char *s);

__attribute__((noreturn)) void arch_halt(void);

#endif
