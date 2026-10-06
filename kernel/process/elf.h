/*
 * Static ELF64 executables for user processes.
 *
 * Used for the embedded test programs (milestone M3) and by process_spawn()
 * for program files (process/spawn.c).
 */

#ifndef PROCESS_ELF_H
#define PROCESS_ELF_H

#include "memory/vmm.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Validate and map image into space: PT_LOAD segments in user space below
 * USER_IMAGE_END, page-separated, never writable and executable.
 * pages receives the number of frames allocated.
 */
status_t elf_load_user(vm_space_t *space, const void *image, size_t size, uint64_t *entry, uint64_t *pages);

#endif
