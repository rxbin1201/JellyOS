/*
 * JellyOS Boot Manager - final jump into the kernel (handoff.S).
 *
 * boot_handoff() switches to the kernel page tables, so its own code must be
 * identity mapped in them: [boot_handoff, boot_handoff_end) is that range.
 */

#ifndef BOOT_HANDOFF_H
#define BOOT_HANDOFF_H

#include <stdint.h>

__attribute__((noreturn))
void boot_handoff(uint64_t cr3, uint64_t stack_top, uint64_t boot_info, uint64_t entry);

extern const char boot_handoff_end[];

#endif
