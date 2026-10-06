/*
 * libos: the native JellyOS system interface for userspace.
 *
 * Thin wrappers around the system calls in <jelly/syscall.h>. Every function
 * that can fail returns status_t; results come back through pointers.
 * A program provides
 *     int main(uint64_t arg0, uint64_t arg1, uint64_t arg2);
 * and receives the startup arguments (often handles) its creator passed.
 */

#ifndef JELLY_OS_H
#define JELLY_OS_H

#include <jelly/status.h>
#include <jelly/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Basics */
uint32_t jelly_abi_version(void);
status_t jelly_debug_write(const char *text, size_t length);
void     jelly_print(const char *text);
uint64_t jelly_clock_ns(void);

/* Processes and threads */
__attribute__((noreturn)) void jelly_process_exit(int32_t code);
status_t jelly_thread_create(void (*entry)(void *), void *arg, void *stack_top, jelly_handle_t *thread);
__attribute__((noreturn)) void jelly_thread_exit(void);
void     jelly_thread_yield(void);
status_t jelly_thread_sleep(uint64_t ns);

/* Memory */
status_t jelly_memory_allocate(size_t size, uint32_t flags, void **address);
status_t jelly_memory_unmap(void *address, size_t size);

/* Handles and waiting */
status_t jelly_handle_close(jelly_handle_t handle);
status_t jelly_handle_duplicate(jelly_handle_t handle, uint32_t rights, jelly_handle_t *copy);
status_t jelly_wait(jelly_handle_t handle, uint64_t timeout_ns);

/* Events */
status_t jelly_event_create(uint32_t flags, jelly_handle_t *event);
status_t jelly_event_signal(jelly_handle_t event);
status_t jelly_event_reset(jelly_handle_t event);

/* Channels */
status_t jelly_channel_create(jelly_handle_t *end0, jelly_handle_t *end1);
status_t jelly_channel_send(jelly_handle_t channel, const void *data, size_t size);
status_t jelly_channel_receive(jelly_handle_t channel, void *buffer, size_t size, size_t *actual);

/* Shared memory */
status_t jelly_shm_create(size_t size, jelly_handle_t *shm);
status_t jelly_shm_map(jelly_handle_t shm, uint32_t flags, void **address);

/* Futex */
status_t jelly_futex_wait(const uint32_t *word, uint32_t expected, uint64_t timeout_ns);
status_t jelly_futex_wake(const uint32_t *word, uint32_t count);

/* Raw system call */
uint64_t jelly_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5);

#endif
