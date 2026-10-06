/*
 * Safe access to user memory from system calls.
 *
 * Every user pointer is checked against the current address space before it
 * is touched: the range must lie in user space and be mapped with VM_USER
 * (and VM_WRITE for writes). The kernel is not preemptible on a single CPU,
 * so the mapping cannot change between check and copy. With SMAP the copy
 * is the only place where the kernel can reach user memory.
 */

#ifndef PROCESS_USERCOPY_H
#define PROCESS_USERCOPY_H

#include <jelly/status.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool     user_range_ok(uint64_t address, size_t size, bool write);

status_t copy_from_user(void *dest, uint64_t user_src, size_t size);
status_t copy_to_user(uint64_t user_dest, const void *src, size_t size);

static inline status_t put_user_u32(uint64_t address, uint32_t value)
{
    return copy_to_user(address, &value, sizeof(value));
}

static inline status_t put_user_u64(uint64_t address, uint64_t value)
{
    return copy_to_user(address, &value, sizeof(value));
}

#endif
