/*
 * Channels: a pair of endpoints, each with a queue of incoming messages.
 * An endpoint is signaled (readable) when it has a message or its peer is
 * gone. Endpoints reference each other weakly; destroying one detaches it.
 */

#include "ipc/ipc.h"

#include "core/arch.h"
#include "memory/heap.h"

typedef struct message {
    list_node_t node;
    size_t      size;
    void       *data;
} message_t;

typedef struct endpoint {
    object_t         object;
    struct endpoint *peer;      /* NULL once the peer is destroyed */
    list_t           incoming;
    uint32_t         queued;
} endpoint_t;

static endpoint_t *endpoint_of(object_t *object)
{
    return container_of(object, endpoint_t, object);
}

static void endpoint_destroy(object_t *object)
{
    endpoint_t *ep = endpoint_of(object);
    uint64_t flags = arch_interrupts_save();

    if (ep->peer) {
        endpoint_t *peer = ep->peer;
        peer->peer = NULL;
        ep->peer = NULL;
        object_notify(&peer->object); /* waiters now see PEER_CLOSED */
    }
    arch_interrupts_restore(flags);

    list_node_t *node;
    while ((node = list_pop_front(&ep->incoming))) {
        message_t *m = container_of(node, message_t, node);
        kfree(m->data);
        kfree(m);
    }
    kfree(ep);
}

static bool endpoint_signaled(object_t *object)
{
    endpoint_t *ep = endpoint_of(object);
    return !list_empty(&ep->incoming) || !ep->peer;
}

static const object_ops_t endpoint_ops = {
    .destroy = endpoint_destroy,
    .signaled = endpoint_signaled,
};

status_t channel_create(object_t **end0, object_t **end1)
{
    endpoint_t *a = kcalloc(1, sizeof(*a));
    endpoint_t *b = kcalloc(1, sizeof(*b));

    if (!a || !b) {
        kfree(a);
        kfree(b);
        return STATUS_OUT_OF_MEMORY;
    }
    object_init(&a->object, OBJECT_CHANNEL, &endpoint_ops);
    object_init(&b->object, OBJECT_CHANNEL, &endpoint_ops);
    list_init(&a->incoming);
    list_init(&b->incoming);
    a->peer = b;
    b->peer = a;
    *end0 = &a->object;
    *end1 = &b->object;
    return STATUS_SUCCESS;
}

status_t channel_send(object_t *endpoint, void *data, size_t size)
{
    endpoint_t *ep = endpoint_of(endpoint);

    if (size > CHANNEL_MESSAGE_MAX)
        return STATUS_INVALID_ARGUMENT;

    message_t *m = kmalloc(sizeof(*m));
    if (!m)
        return STATUS_OUT_OF_MEMORY;

    status_t status = STATUS_SUCCESS;
    uint64_t flags = arch_interrupts_save();
    endpoint_t *peer = ep->peer;
    if (!peer) {
        status = STATUS_PEER_CLOSED;
    } else if (peer->queued >= CHANNEL_QUEUE_MAX) {
        status = STATUS_WOULD_BLOCK;
    } else {
        m->size = size;
        m->data = data;
        list_push_back(&peer->incoming, &m->node);
        peer->queued++;
        object_notify(&peer->object);
    }
    arch_interrupts_restore(flags);

    if (STATUS_IS_ERROR(status))
        kfree(m);
    return status;
}

status_t channel_peek(object_t *endpoint, size_t *size)
{
    endpoint_t *ep = endpoint_of(endpoint);
    list_node_t *node = list_front(&ep->incoming);

    if (!node)
        return ep->peer ? STATUS_WOULD_BLOCK : STATUS_PEER_CLOSED;
    *size = container_of(node, message_t, node)->size;
    return STATUS_SUCCESS;
}

status_t channel_take(object_t *endpoint, void **data, size_t *size)
{
    endpoint_t *ep = endpoint_of(endpoint);
    list_node_t *node = list_pop_front(&ep->incoming);

    if (!node)
        return ep->peer ? STATUS_WOULD_BLOCK : STATUS_PEER_CLOSED;

    message_t *m = container_of(node, message_t, node);
    ep->queued--;
    *data = m->data;
    *size = m->size;
    kfree(m);
    return STATUS_SUCCESS;
}
