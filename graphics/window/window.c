/*
 * Window client library.
 *
 * Requests that need an answer (hello, create, resize) wait for it; events
 * that arrive in between are kept in a small queue and handed out later by
 * wm_next_event(). The window list (desktop shell) is collected from
 * WM_WINDOW_LIST messages and announced as one WM_EVENT_WINDOWS.
 */

#include "graphics/window/window.h"

#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

#define MAX_WINDOWS      16
#define QUEUE_SIZE       128
#define MAX_LIST_ENTRIES 64

struct wm_connection {
    jelly_handle_t   channel;
    int32_t          screen_width, screen_height;
    window_t        *windows[MAX_WINDOWS];
    wm_message_t     queue[QUEUE_SIZE];
    uint32_t         queue_head, queue_count;

    wm_window_info_t list[MAX_LIST_ENTRIES];      /* last complete window list */
    int              list_count;
    wm_window_info_t building[MAX_LIST_ENTRIES];  /* the one being received */
    int              building_count;

    char             notification_title[WM_TITLE_MAX];
    char             notification_text[WM_TEXT_MAX];
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

/* Wait for a reply of `type` (for `window` unless 0), queueing everything else. */
static int await_reply(wm_connection_t *c, uint32_t type, uint32_t window, wm_message_t *reply,
                       jelly_handle_t *handle)
{
    for (;;) {
        jelly_handle_t attached = JELLY_HANDLE_INVALID;
        int result = receive_message(c, reply, &attached, JELLY_WAIT_FOREVER);
        if (result < 0)
            return result;
        if (reply->type == type && (!window || reply->window == window)) {
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
        result = await_reply(c, WM_WELCOME, 0, &m, NULL);
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

static size_t mapping_size(const window_t *w)
{
    return ((size_t)w->canvas.stride * w->height * 4 + 4095) & ~(size_t)4095;
}

/* Map a surface handle into the window's canvas. */
static int attach_surface(window_t *w, jelly_handle_t memory, const wm_message_t *m)
{
    void *address;
    status_t status = jelly_shm_map(memory, JELLY_MEMORY_WRITE, &address);
    jelly_handle_close(memory);
    if (STATUS_IS_ERROR(status))
        return -(int)status;
    w->width = m->width;
    w->height = m->height;
    canvas_init(&w->canvas, address, m->width, m->height, (int32_t)m->stride);
    return 0;
}

window_t *wm_create_window_at(wm_connection_t *c, int32_t x, int32_t y, int32_t width, int32_t height,
                              const char *title, uint32_t flags)
{
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS && slot < 0; i++) {
        if (!c->windows[i])
            slot = i;
    }
    if (slot < 0)
        return NULL;

    wm_message_t m = { .type = WM_CREATE_WINDOW, .x = x, .y = y, .width = width, .height = height, .flags = flags };
    strncpy(m.title, title ? title : "", WM_TITLE_MAX - 1);
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    if (send_message(c, &m) || await_reply(c, WM_WINDOW_CREATED, 0, &m, &memory) || m.status || !memory) {
        if (memory)
            jelly_handle_close(memory);
        return NULL;
    }

    window_t *w = calloc(1, sizeof(*w));
    if (!w || attach_surface(w, memory, &m)) {
        free(w);
        return NULL;
    }
    w->connection = c;
    w->id = m.window;
    c->windows[slot] = w;
    return w;
}

window_t *wm_create_window(wm_connection_t *c, int32_t width, int32_t height, const char *title, uint32_t flags)
{
    return wm_create_window_at(c, 0, 0, width, height, title, flags & ~WM_WINDOW_POSITIONED);
}

int wm_resize_window(window_t *w, int32_t width, int32_t height)
{
    wm_message_t m = { .type = WM_RESIZE_WINDOW, .window = w->id, .width = width, .height = height };
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    int result = send_message(w->connection, &m);
    if (!result)
        result = await_reply(w->connection, WM_WINDOW_RESIZED, w->id, &m, &memory);
    if (result)
        return result;
    if (m.status || !memory) {
        if (memory)
            jelly_handle_close(memory);
        return m.status ? m.status : STATUS_OUT_OF_MEMORY;
    }
    void *old = w->canvas.pixels;
    size_t old_size = mapping_size(w);
    result = attach_surface(w, memory, &m);
    if (!result)
        jelly_memory_unmap(old, old_size);
    w->frame_pending = false; /* the old surface's frame no longer matters */
    return result;
}

int wm_set_state(window_t *w, uint32_t state)
{
    wm_message_t m = { .type = WM_SET_STATE, .window = w->id, .flags = state };
    return send_message(w->connection, &m);
}

void wm_destroy_window(window_t *w)
{
    wm_connection_t *c = w->connection;
    wm_message_t m = { .type = WM_DESTROY_WINDOW, .window = w->id };
    send_message(c, &m);
    jelly_memory_unmap(w->canvas.pixels, mapping_size(w));
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

/* --- Desktop shell ------------------------------------------------------------- */

int wm_subscribe_windows(wm_connection_t *c)
{
    wm_message_t m = { .type = WM_SUBSCRIBE_WINDOWS };
    return send_message(c, &m);
}

int wm_window_list(wm_connection_t *c, const wm_window_info_t **list)
{
    *list = c->list;
    return c->list_count;
}

int wm_activate_window(wm_connection_t *c, uint32_t id)
{
    wm_message_t m = { .type = WM_ACTIVATE_WINDOW, .window = id };
    return send_message(c, &m);
}

int wm_minimize_window(wm_connection_t *c, uint32_t id)
{
    wm_message_t m = { .type = WM_MINIMIZE_WINDOW, .window = id };
    return send_message(c, &m);
}

int wm_settings_changed(wm_connection_t *c)
{
    wm_message_t m = { .type = WM_SETTINGS_CHANGED };
    return send_message(c, &m);
}

int wm_set_keymap(wm_connection_t *c, const char *name)
{
    wm_message_t m = { .type = WM_SET_KEYMAP };
    strncpy(m.title, name, WM_TITLE_MAX - 1);
    return send_message(c, &m);
}

int wm_set_display_mode(wm_connection_t *c, int32_t width, int32_t height, uint32_t refresh_mhz)
{
    wm_message_t m = { .type = WM_SET_DISPLAY_MODE, .width = width, .height = height, .flags = refresh_mhz };
    return send_message(c, &m);
}

int wm_set_display_power(wm_connection_t *c, bool on)
{
    wm_message_t m = { .type = WM_SET_DISPLAY_POWER, .flags = on ? 1 : 0 };
    return send_message(c, &m);
}

int wm_notify(wm_connection_t *c, const char *title, const char *text)
{
    wm_message_t m = { .type = WM_NOTIFY };
    strncpy(m.title, title ? title : "", WM_TITLE_MAX - 1);
    strncpy(m.text, text ? text : "", WM_TEXT_MAX - 1);
    return send_message(c, &m);
}

void wm_last_notification(wm_connection_t *c, const char **title, const char **text)
{
    *title = c->notification_title;
    *text = c->notification_text;
}

/* --- Events -------------------------------------------------------------------- */

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
        memset(event, 0, sizeof(*event));
        switch (m.type) {
        case WM_PRESENTED:
            if (w)
                w->frame_pending = false;
            event->type = WM_EVENT_FRAME;
            break;
        case WM_EVENT:
            *event = m.event;
            break;
        case WM_WINDOW_LIST:
            if (c->building_count < MAX_LIST_ENTRIES) {
                wm_window_info_t *info = &c->building[c->building_count++];
                info->id = m.window;
                info->state = m.flags;
                memcpy(info->title, m.title, WM_TITLE_MAX);
                info->title[WM_TITLE_MAX - 1] = '\0';
            }
            continue;
        case WM_WINDOW_LIST_END:
            memcpy(c->list, c->building, sizeof(c->list));
            c->list_count = c->building_count;
            c->building_count = 0;
            event->type = WM_EVENT_WINDOWS;
            w = NULL;
            break;
        case WM_NOTIFY:
            memcpy(c->notification_title, m.title, WM_TITLE_MAX);
            memcpy(c->notification_text, m.text, WM_TEXT_MAX);
            c->notification_title[WM_TITLE_MAX - 1] = '\0';
            c->notification_text[WM_TEXT_MAX - 1] = '\0';
            event->type = WM_EVENT_NOTIFICATION;
            w = NULL;
            break;
        case WM_SCREEN:
            c->screen_width = m.width;
            c->screen_height = m.height;
            event->type = WM_EVENT_SCREEN;
            event->x = m.width;
            event->y = m.height;
            w = NULL;
            break;
        default:
            continue; /* unexpected */
        }
        if (window)
            *window = w;
        return 1;
    }
}
