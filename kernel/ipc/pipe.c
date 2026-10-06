/*
 * Pipes: a byte stream between two open files (README section 19).
 *
 * Reads block while the pipe is empty and return 0 once every writer is
 * gone; writes block while it is full and fail with PEER_CLOSED once every
 * reader is gone. Ends are ordinary file objects, so handles to them work
 * with SYS_FILE_READ/WRITE and can be given to child processes as stdio.
 */

#include "ipc/ipc.h"

#include "fs/vfs/vfs.h"

#include "core/arch.h"
#include "core/export.h"
#include "memory/heap.h"

#define PIPE_CAPACITY 16384

typedef struct {
    vnode_t      vnode;
    uint8_t     *buffer;
    size_t       head;
    size_t       count;
    uint32_t     readers;
    uint32_t     writers;
    wait_queue_t readable;
    wait_queue_t writable;
} pipe_t;

static pipe_t *pipe_of(vnode_t *v)
{
    return container_of(v, pipe_t, vnode);
}

static status_t pipe_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    pipe_t *p = pipe_of(v);
    uint8_t *out = buffer;
    status_t status = STATUS_SUCCESS;

    (void)offset;
    *done = 0;
    if (size == 0)
        return STATUS_SUCCESS;

    uint64_t flags = arch_interrupts_save();
    while (p->count == 0 && p->writers > 0 && status == STATUS_SUCCESS)
        status = wait_queue_block(&p->readable, WAIT_FOREVER);
    while (status == STATUS_SUCCESS && p->count && *done < size) {
        out[(*done)++] = p->buffer[p->head];
        p->head = (p->head + 1) % PIPE_CAPACITY;
        p->count--;
    }
    if (*done)
        wait_queue_wake_all(&p->writable, STATUS_SUCCESS);
    arch_interrupts_restore(flags);
    return status;
}

static status_t pipe_write(vnode_t *v, uint64_t offset, const void *buffer, size_t size, size_t *done)
{
    pipe_t *p = pipe_of(v);
    const uint8_t *in = buffer;
    status_t status = STATUS_SUCCESS;

    (void)offset;
    *done = 0;
    uint64_t flags = arch_interrupts_save();
    while (status == STATUS_SUCCESS && *done < size) {
        if (p->readers == 0) {
            status = STATUS_PEER_CLOSED;
            break;
        }
        if (p->count == PIPE_CAPACITY) {
            status = wait_queue_block(&p->writable, WAIT_FOREVER);
            continue;
        }
        while (*done < size && p->count < PIPE_CAPACITY) {
            p->buffer[(p->head + p->count) % PIPE_CAPACITY] = in[(*done)++];
            p->count++;
        }
        wait_queue_wake_all(&p->readable, STATUS_SUCCESS);
    }
    arch_interrupts_restore(flags);
    return status;
}

static void pipe_close(vnode_t *v, uint32_t open_flags)
{
    pipe_t *p = pipe_of(v);
    uint64_t flags = arch_interrupts_save();

    if (open_flags & JELLY_OPEN_READ) {
        p->readers--;
        wait_queue_wake_all(&p->writable, STATUS_SUCCESS); /* writers see PEER_CLOSED */
    }
    if (open_flags & JELLY_OPEN_WRITE) {
        p->writers--;
        wait_queue_wake_all(&p->readable, STATUS_SUCCESS); /* readers see end of file */
    }
    arch_interrupts_restore(flags);
}

static void pipe_release(vnode_t *v)
{
    pipe_t *p = pipe_of(v);
    kfree(p->buffer);
    kfree(p);
}

static const vnode_ops_t pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
    .release = pipe_release,
};

status_t pipe_create(file_t **read_end, file_t **write_end)
{
    pipe_t *p = kcalloc(1, sizeof(*p));
    if (!p)
        return STATUS_OUT_OF_MEMORY;
    p->buffer = kmalloc(PIPE_CAPACITY);
    if (!p->buffer) {
        kfree(p);
        return STATUS_OUT_OF_MEMORY;
    }

    vnode_init(&p->vnode, NULL, VNODE_PIPE, &pipe_ops);
    p->vnode.mode = 0600;
    wait_queue_init(&p->readable);
    wait_queue_init(&p->writable);
    p->readers = p->writers = 1;

    status_t status = vfs_file_from_vnode(&p->vnode, JELLY_OPEN_READ, read_end);
    if (!STATUS_IS_ERROR(status)) {
        status = vfs_file_from_vnode(&p->vnode, JELLY_OPEN_WRITE, write_end);
        if (STATUS_IS_ERROR(status))
            object_release(&(*read_end)->object);
    }
    vnode_release(&p->vnode); /* the files hold the pipe now */
    return status;
}
