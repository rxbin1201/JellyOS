/*
 * System call dispatch (README section 20). The architecture layer collects
 * the number and six arguments; the result is a status_t in the return
 * register. See docs/abi/syscalls.md.
 */

#ifndef SYSCALL_SYSCALL_H
#define SYSCALL_SYSCALL_H

#include <stdint.h>

uint64_t syscall_dispatch(uint64_t number, const uint64_t args[6]);

#endif
