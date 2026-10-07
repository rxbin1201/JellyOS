/*
 * Channels: a pair of endpoints, each with a queue of incoming messages.
 * An endpoint is signaled (readable) when it has a message or its peer is
 * gone. Endpoints reference each other weakly; destroying one detaches it.
 *
 * A message may carry up to CHANNEL_MAX_OBJECTS kernel objects (handles in
 * transit) with their rights. The queue holds a reference to each; objects
 * of messages that are dropped unread are released.
 */

#include "ipc/ipc.h"

#include "core/arch.h"
#include "memory/heap.h"

typedef struct message {
    list_node_t node;
    size_t      size;
    void       *data;
    uint32_t    object_count;
    object_t   *objects[CHANNEL_MAX_OBJECTS];
    uint32_t    rights[CHANNEL_MAX_OBJECTS];
} message_t;

static void free_message(message_t *m)
{
    for (uint32_t i = 0; i < m->object_count; i++)
        object_release(m->objects[i]);
    kfree(m->data);
    kfree(m);
}

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
    while ((node = list_pop_front(&ep->incoming)))
        free_message(container_of(node, message_t, node));
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
    return channel_send_objects(endpoint, data, size, NULL, NULL, 0);
}

status_t channel_send_objects(object_t *endpoint, void *data, size_t size, object_t *const *objects,
                              const uint32_t *rights, uint32_t count)
{
    endpoint_t *ep = endpoint_of(endpoint);

    if (size > CHANNEL_MESSAGE_MAX || count > CHANNEL_MAX_OBJECTS)
        return STATUS_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < count; i++) {
        /* An endpoint inside its own channel could never be closed. */
        if (objects[i] == endpoint || (ep->peer && objects[i] == &ep->peer->object))
            return STATUS_INVALID_ARGUMENT;
    }

    message_t *m = kmalloc(sizeof(*m));
    if (!m)
        return STATUS_OUT_OF_MEMORY;
    m->object_count = count;
    for (uint32_t i = 0; i < count; i++) {
        m->objects[i] = objects[i];
        m->rights[i] = rights[i];
    }

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

bool channel_peer_alive(object_t *endpoint)
{
    return endpoint_of(endpoint)->peer != NULL;
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

status_t channel_peek_objects(object_t *endpoint, size_t *size, uint32_t *count)
{
    endpoint_t *ep = endpoint_of(endpoint);
    list_node_t *node = list_front(&ep->incoming);

    if (!node)
        return ep->peer ? STATUS_WOULD_BLOCK : STATUS_PEER_CLOSED;
    *size = container_of(node, message_t, node)->size;
    *count = container_of(node, message_t, node)->object_count;
    return STATUS_SUCCESS;
}

status_t channel_take_objects(object_t *endpoint, void **data, size_t *size, object_t **objects, uint32_t *rights,
                              uint32_t *count)
{
    endpoint_t *ep = endpoint_of(endpoint);
    list_node_t *node = list_pop_front(&ep->incoming);

    if (!node)
        return ep->peer ? STATUS_WOULD_BLOCK : STATUS_PEER_CLOSED;

    message_t *m = container_of(node, message_t, node);
    ep->queued--;
    *data = m->data;
    *size = m->size;
    *count = m->object_count;
    for (uint32_t i = 0; i < m->object_count; i++) {
        objects[i] = m->objects[i]; /* the caller takes over the references */
        rights[i] = m->rights[i];
    }
    kfree(m);
    return STATUS_SUCCESS;
}

status_t channel_take(object_t *endpoint, void **data, size_t *size)
{
    object_t *objects[CHANNEL_MAX_OBJECTS];
    uint32_t rights[CHANNEL_MAX_OBJECTS], count;
    status_t status = channel_take_objects(endpoint, data, size, objects, rights, &count);
    /* A receiver that cannot accept handles closes them. */
    for (uint32_t i = 0; !STATUS_IS_ERROR(status) && i < count; i++)
        object_release(objects[i]);
    return status;
}
