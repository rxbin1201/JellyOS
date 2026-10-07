/*
 * Inter-process communication (README section 19).
 *
 * One mechanism per problem:
 *   event          - signaling ("something happened"), manual or auto reset
 *   channel        - message queue between two endpoints, datagram semantics
 *   shared memory  - bulk data, mapped into several address spaces
 *   futex          - user-space synchronization (wait/wake on a 32-bit word)
 *   pipe           - byte stream between programs (file handles, below)
 * Sockets live in the network stack (net/sockets).
 */

#ifndef IPC_IPC_H
#define IPC_IPC_H

#include "core/object.h"
#include "process/process.h"

#include <stddef.h>
#include <stdint.h>

#define CHANNEL_MESSAGE_MAX  65536
#define CHANNEL_QUEUE_MAX    64
#define CHANNEL_MAX_OBJECTS  8
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

/* Remove the next message; the caller frees *data with kfree. Objects in it are released. */
status_t channel_take(object_t *endpoint, void **data, size_t *size);

/* With objects: send takes over one reference per object (on success). */
status_t channel_send_objects(object_t *endpoint, void *data, size_t size, object_t *const *objects,
                              const uint32_t *rights, uint32_t count);
status_t channel_peek_objects(object_t *endpoint, size_t *size, uint32_t *count);
/* The caller takes over the references (arrays of CHANNEL_MAX_OBJECTS). */
status_t channel_take_objects(object_t *endpoint, void **data, size_t *size, object_t **objects, uint32_t *rights,
                              uint32_t *count);

/* Named services: a server registers a channel end; connect() hands it the peer of a new channel. */
status_t service_register(const char *name, object_t *channel);
status_t service_connect(const char *name, object_t **channel);

/* Shared memory */
status_t shm_create(uint64_t size, object_t **shm);
status_t shm_map(process_t *process, object_t *shm, uint32_t vm_flags, uint64_t *address);
uint64_t shm_size(object_t *shm);
/* Device memory (framebuffer): mapped write-combining; release(context) runs when the object dies. */
status_t shm_create_device(uint64_t phys, uint64_t size, void (*release)(void *context), void *context,
                           object_t **shm);
/* Is the peer of this channel endpoint still open? */
bool     channel_peer_alive(object_t *endpoint);

/* Pipes: two file objects (read end, write end) on one byte stream. */
struct file;
status_t pipe_create(struct file **read_end, struct file **write_end);

/* Futex: the key is the physical address of the word, so it works across processes. */
status_t futex_wait(uint64_t user_address, uint32_t expected, uint64_t timeout_ns);
status_t futex_wake(uint64_t user_address, uint32_t count, uint32_t *woken);

#endif
