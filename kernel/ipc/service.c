/*
 * Named services: how clients find servers (the display server, later
 * others).
 *
 * A server creates a channel, keeps one end and registers the other under a
 * name. SYS_SERVICE_CONNECT creates a fresh channel for each client: the
 * client gets one end, and the other end arrives at the server as a message
 * "connect" carrying that handle. A registration whose server end is gone
 * is stale and may be replaced.
 */

#include "ipc/ipc.h"

#include "core/string.h"
#include "memory/heap.h"

#include <jelly/syscall.h>

#define SERVICE_MAX 32

typedef struct {
    char      name[JELLY_SERVICE_NAME_MAX + 1];
    object_t *channel; /* the registered end; messages go to the server's end */
} service_t;

static service_t services[SERVICE_MAX];

static service_t *find(const char *name)
{
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (services[i].channel && !strcmp(services[i].name, name))
            return &services[i];
    }
    return NULL;
}

static void drop(service_t *s)
{
    object_release(s->channel);
    s->channel = NULL;
}

status_t service_register(const char *name, object_t *channel)
{
    if (!name[0] || strlen(name) > JELLY_SERVICE_NAME_MAX)
        return STATUS_INVALID_ARGUMENT;
    service_t *s = find(name);
    if (s) {
        if (channel_peer_alive(s->channel))
            return STATUS_ALREADY_EXISTS;
        drop(s); /* the previous server is gone */
    }
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services[i].channel) {
            strcpy(services[i].name, name);
            object_retain(channel);
            services[i].channel = channel;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_LIMIT_EXCEEDED;
}

status_t service_connect(const char *name, object_t **out)
{
    static const char message[] = "connect";
    service_t *s = find(name);
    object_t *client, *server;

    if (!s)
        return STATUS_NOT_FOUND;
    status_t status = channel_create(&client, &server);
    if (STATUS_IS_ERROR(status))
        return status;
    char *data = kmalloc(sizeof(message));
    if (!data) {
        object_release(client);
        object_release(server);
        return STATUS_OUT_OF_MEMORY;
    }
    memcpy(data, message, sizeof(message));

    uint32_t rights = JELLY_RIGHT_READ | JELLY_RIGHT_WRITE | JELLY_RIGHT_WAIT | JELLY_RIGHT_DUPLICATE;
    status = channel_send_objects(s->channel, data, sizeof(message), &server, &rights, 1);
    if (STATUS_IS_ERROR(status)) {
        kfree(data);
        object_release(server);
        object_release(client);
        if (status == STATUS_PEER_CLOSED) {
            drop(s);
            return STATUS_NOT_FOUND;
        }
        return status == STATUS_WOULD_BLOCK ? STATUS_BUSY : status;
    }
    *out = client;
    return STATUS_SUCCESS;
}
