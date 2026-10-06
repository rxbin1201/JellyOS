/*
 * Inter-process communication (README section 19).
 *
 * One mechanism per problem:
 *   event          - signaling ("something happened"), manual or auto reset
 *   channel        - message queue between two endpoints, datagram semantics
 *   shared memory  - bulk data, mapped into several address spaces
 *   futex          - user-space synchronization (wait/wake on a 32-bit word)
 * Pipes (byte streams for file descriptors) and sockets arrive with the VFS
 * and the network stack.
 */

#ifndef IPC_IPC_H
#define IPC_IPC_H

#include "core/object.h"
#include "process/process.h"

#include <stddef.h>
#include <stdint.h>

#define CHANNEL_MESSAGE_MAX  65536
#define CHANNEL_QUEUE_MAX    64
#define SHM_SIZE_MAX         (64ULL << 20)

/* Events */
status_t event_create(uint32_t flags, object_t **event);
void     event_signal(object_t *event);
void     event_reset(object_t *event);

/* Channels: two connected endpoints. Closing one makes the other PEER_CLOSED. */
status_t channel_create(object_t **end0, object_t **end1);

/* Queue a message for the peer; takes ownership of the kmalloc'ed data on success. */
status_t channel_send(object_t *endpoint, void *data, size_t size);

/* Size of the next message, WOULD_BLOCK if none, PEER_CLOSED if none will come. */
status_t channel_peek(object_t *endpoint, size_t *size);

/* Remove the next message; the caller frees *data with kfree. */
status_t channel_take(object_t *endpoint, void **data, size_t *size);

/* Shared memory */
status_t shm_create(uint64_t size, object_t **shm);
status_t shm_map(process_t *process, object_t *shm, uint32_t vm_flags, uint64_t *address);
uint64_t shm_size(object_t *shm);

/* Futex: the key is the physical address of the word, so it works across processes. */
status_t futex_wait(uint64_t user_address, uint32_t expected, uint64_t timeout_ns);
status_t futex_wake(uint64_t user_address, uint32_t count, uint32_t *woken);

#endif
