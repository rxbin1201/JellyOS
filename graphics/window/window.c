/*
 * Window client library.
 *
 * Requests that need an answer (hello, create window) wait for it; events
 * that arrive in between are kept in a small queue and handed out later by
 * wm_next_event().
 */

#include "graphics/window/window.h"

#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

#define MAX_WINDOWS 16
#define QUEUE_SIZE  128

struct wm_connection {
    jelly_handle_t channel;
    int32_t        screen_width, screen_height;
    window_t      *windows[MAX_WINDOWS];
    wm_message_t   queue[QUEUE_SIZE];
    uint32_t       queue_head, queue_count;
};

static int send_message(wm_connection_t *c, const wm_message_t *m)
{
    status_t status = jelly_channel_send(c->channel, m, sizeof(*m));
    return STATUS_IS_ERROR(status) ? -(int)status : 0;
}

/* Receive one message (blocking up to timeout); returns 1, 0 on timeout, or -status. */
static int receive_message(wm_connection_t *c, wm_message_t *m, jelly_handle_t *handle, uint64_t timeout_ns)
{
    jelly_handle_t handles[JELLY_CHANNEL_MAX_HANDLES];
    uint32_t count = 0;
    size_t size;

    for (;;) {
        status_t status = jelly_channel_receive_handles(c->channel, m, sizeof(*m), &size, handles, &count);
        if (status == STATUS_SUCCESS) {
            for (uint32_t i = 0; i < count; i++) {
                if (i == 0 && handle)
                    *handle = handles[0];
                else
                    jelly_handle_close(handles[i]);
            }
            if (size != sizeof(*m))
                continue; /* not ours: ignore */
            return 1;
        }
        if (status != STATUS_WOULD_BLOCK)
            return -(int)status;
        if (timeout_ns == 0)
            return 0;
        status = jelly_wait(c->channel, timeout_ns);
        if (status == STATUS_TIMEOUT)
            return 0;
        if (STATUS_IS_ERROR(status))
            return -(int)status;
    }
}

static void enqueue(wm_connection_t *c, const wm_message_t *m)
{
    if (c->queue_count == QUEUE_SIZE) {
        c->queue_head = (c->queue_head + 1) % QUEUE_SIZE; /* drop the oldest */
        c->queue_count--;
    }
    c->queue[(c->queue_head + c->queue_count) % QUEUE_SIZE] = *m;
    c->queue_count++;
}

/* Wait for a reply of `type`, queueing everything else. */
static int await_reply(wm_connection_t *c, uint32_t type, wm_message_t *reply, jelly_handle_t *handle)
{
    for (;;) {
        jelly_handle_t attached = JELLY_HANDLE_INVALID;
        int result = receive_message(c, reply, &attached, JELLY_WAIT_FOREVER);
        if (result < 0)
            return result;
        if (reply->type == type) {
            if (handle)
                *handle = attached;
            else if (attached)
                jelly_handle_close(attached);
            return 0;
        }
        if (attached)
            jelly_handle_close(attached);
        enqueue(c, reply);
    }
}

int wm_connect(wm_connection_t **out)
{
    wm_connection_t *c = calloc(1, sizeof(*c));
    if (!c)
        return STATUS_OUT_OF_MEMORY;
    status_t status = jelly_service_connect(WM_SERVICE_NAME, &c->channel);
    if (STATUS_IS_ERROR(status)) {
        free(c);
        return (int)status;
    }
    wm_message_t m = { .type = WM_HELLO, .version = WM_PROTOCOL_VERSION };
    int result = send_message(c, &m);
    if (!result)
        result = await_reply(c, WM_WELCOME, &m, NULL);
    if (result) {
        jelly_handle_close(c->channel);
        free(c);
        return -result;
    }
    c->screen_width = m.width;
    c->screen_height = m.height;
    *out = c;
    return 0;
}

void wm_disconnect(wm_connection_t *c)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (c->windows[i])
            wm_destroy_window(c->windows[i]);
    }
    jelly_handle_close(c->channel);
    free(c);
}

void wm_screen_size(wm_connection_t *c, int32_t *width, int32_t *height)
{
    *width = c->screen_width;
    *height = c->screen_height;
}

jelly_handle_t wm_handle(wm_connection_t *c)
{
    return c->channel;
}

window_t *wm_create_window(wm_connection_t *c, int32_t width, int32_t height, const char *title, uint32_t flags)
{
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS && slot < 0; i++) {
        if (!c->windows[i])
            slot = i;
    }
    if (slot < 0)
        return NULL;

    wm_message_t m = { .type = WM_CREATE_WINDOW, .width = width, .height = height, .flags = flags };
    strncpy(m.title, title ? title : "", WM_TITLE_MAX - 1);
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    if (send_message(c, &m) || await_reply(c, WM_WINDOW_CREATED, &m, &memory) || m.status || !memory) {
        if (memory)
            jelly_handle_close(memory);
        return NULL;
    }

    void *address;
    status_t status = jelly_shm_map(memory, JELLY_MEMORY_WRITE, &address);
    jelly_handle_close(memory);
    window_t *w = calloc(1, sizeof(*w));
    if (STATUS_IS_ERROR(status) || !w) {
        free(w);
        return NULL;
    }
    w->connection = c;
    w->id = m.window;
    w->width = m.width;
    w->height = m.height;
    canvas_init(&w->canvas, address, m.width, m.height, (int32_t)m.stride);
    c->windows[slot] = w;
    return w;
}

void wm_destroy_window(window_t *w)
{
    wm_connection_t *c = w->connection;
    wm_message_t m = { .type = WM_DESTROY_WINDOW, .window = w->id };
    send_message(c, &m);
    jelly_memory_unmap(w->canvas.pixels, ((size_t)w->canvas.stride * w->height * 4 + 4095) & ~(size_t)4095);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (c->windows[i] == w)
            c->windows[i] = NULL;
    }
    free(w);
}

int wm_set_title(window_t *w, const char *title)
{
    wm_message_t m = { .type = WM_SET_TITLE, .window = w->id };
    strncpy(m.title, title, WM_TITLE_MAX - 1);
    return send_message(w->connection, &m);
}

int wm_present(window_t *w, rect_t changed)
{
    wm_message_t m = { .type = WM_PRESENT, .window = w->id, .x = changed.x, .y = changed.y,
                       .width = changed.w, .height = changed.h };
    int result = send_message(w->connection, &m);
    if (!result)
        w->frame_pending = true;
    return result;
}

static window_t *find_window(wm_connection_t *c, uint32_t id)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (c->windows[i] && c->windows[i]->id == id)
            return c->windows[i];
    }
    return NULL;
}

int wm_next_event(wm_connection_t *c, wm_event_t *event, window_t **window, uint64_t timeout_ns)
{
    wm_message_t m;
    for (;;) {
        if (c->queue_count) {
            m = c->queue[c->queue_head];
            c->queue_head = (c->queue_head + 1) % QUEUE_SIZE;
            c->queue_count--;
        } else {
            int result = receive_message(c, &m, NULL, timeout_ns);
            if (result <= 0)
                return result;
        }

        window_t *w = find_window(c, m.window);
        if (m.type == WM_PRESENTED) {
            if (w)
                w->frame_pending = false;
            memset(event, 0, sizeof(*event));
            event->type = WM_EVENT_FRAME;
        } else if (m.type == WM_EVENT) {
            *event = m.event;
        } else {
            continue; /* unexpected */
        }
        if (window)
            *window = w;
        return 1;
    }
}
