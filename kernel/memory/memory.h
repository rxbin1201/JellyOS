/*
 * Memory subsystem bring-up: PMM, then the kernel address space (VMM).
 * The heap needs no initialization of its own.
 */

#ifndef MEMORY_MEMORY_H
#define MEMORY_MEMORY_H

#include <jelly/boot_info.h>
#include <jelly/status.h>
#include <stddef.h>

status_t memory_init(const boot_info_t *info, const boot_memory_entry_t *entries, size_t count);

/* Free boot manager memory once nothing references it (own tables, stack, copied boot data). */
void     memory_reclaim_boot(void);

#endif
