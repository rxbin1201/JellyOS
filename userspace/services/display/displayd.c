/*
 * displayd: the display server and window manager (README sections 35 and
 * Phase 10).
 *
 * Owns display 0 and the input devices, keeps the windows of all clients
 * and composites them (graphics/compositor). Applications connect through
 * the named service "display" and speak graphics/window/protocol.h.
 *
 * Window management:
 *   - a press raises and focuses a window (panels never take the focus);
 *     dragging the title bar moves it; the buttons minimize, maximize
 *     (resizable windows) and close (WM_EVENT_CLOSE to the client)
 *   - the grip in the bottom-right corner resizes: an outline follows the
 *     pointer, and on release the client gets WM_EVENT_RESIZE and asks for a
 *     surface of the new size (WM_RESIZE_WINDOW)
 *   - maximized windows fill the work area (the screen minus reserved panels)
 *   - Alt+Tab brings the next window to the front, restoring minimized ones
 *   - subscribers (the desktop shell) get the window list after every change
 *     and receive the notifications other clients send
 *
 * Input routing: keys go to the focused window, translated with the keyboard
 * layout; the pointer goes to the window under it, and while a button is
 * held, the window that got the press keeps the pointer (grab).
 *
 * /etc/display.conf: keymap=us|de, autostart=PROGRAM (repeatable).
 */

#include <ctype.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/input.h>
#include <jelly/os.h>

#include "graphics/compositor/compositor.h"
#include "graphics/display/display.h"
#include "graphics/window/protocol.h"
#include "keymap.h"

#define CONFIG_PATH      "/etc/display.conf"
#define MAX_CLIENTS      32
#define MAX_AUTOSTART    8

typedef struct client {
    jelly_handle_t channel;
    bool           used;
    bool           subscribed;   /* gets the window list and notifications */
} client_t;

typedef struct {
    comp_window_t *window;
    client_t      *client;
    size_t         mapping_size;
    bool           present_pending;
} window_record_t;

static display_t display;
static compositor_t compositor;
static client_t clients[MAX_CLIENTS];
static window_record_t records[COMPOSITOR_MAX_WINDOWS];
static jelly_handle_t input_queue, service_channel;
static bool list_changed;

/* Input state */
static int32_t pointer_x, pointer_y;
static uint32_t buttons, modifiers;
static comp_window_t *grab, *hover, *drag, *sizing;
static int32_t drag_dx, drag_dy;
static window_part_t pressed_part;
static comp_window_t *pressed_window;

static void log_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_message(const char *format, ...)
{
    /* Into the kernel log (`dmesg displayd`): a service has no terminal to print to. */
    char text[200];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length > 0)
        jelly_debug_write(text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
}

static bool is_normal(const comp_window_t *w)
{
    return !(w->flags & (WM_WINDOW_PANEL | WM_WINDOW_POPUP));
}

/* --- Records -------------------------------------------------------------------- */

static window_record_t *record_of(comp_window_t *w)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++) {
        if (records[i].window == w)
            return &records[i];
    }
    return NULL;
}

static window_record_t *find_record(client_t *client, uint32_t id)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++) {
        if (records[i].window && records[i].window->id == id && (!client || records[i].client == client))
            return &records[i];
    }
    return NULL;
}

static void send_to(client_t *client, const wm_message_t *m)
{
    /* A client that does not read its events loses them rather than stalling the server. */
    jelly_channel_send(client->channel, m, sizeof(*m));
}

static void send_event(comp_window_t *w, const wm_event_t *event)
{
    window_record_t *r = record_of(w);
    if (!r)
        return;
    wm_message_t m = { .type = WM_EVENT, .window = w->id, .event = *event };
    send_to(r->client, &m);
}

static void send_simple_event(comp_window_t *w, uint32_t type)
{
    wm_event_t e = { .type = type, .modifiers = modifiers, .buttons = buttons };
    send_event(w, &e);
}

static void propose_size(comp_window_t *w, int32_t width, int32_t height)
{
    wm_event_t e = { .type = WM_EVENT_RESIZE, .x = width, .y = height };
    send_event(w, &e);
}

/* --- Window list for subscribers ------------------------------------------------- */

static void send_window_list(client_t *client)
{
    for (int i = 0; i < compositor.count; i++) {
        comp_window_t *w = compositor.windows[i];
        if (!is_normal(w))
            continue;
        wm_message_t m = { .type = WM_WINDOW_LIST, .window = w->id };
        memcpy(m.title, w->title, WM_TITLE_MAX);
        m.flags = (w->minimized ? WM_STATE_MINIMIZED : 0) | (w->maximized ? WM_STATE_MAXIMIZED : 0) |
                  (w->focused ? WM_STATE_FOCUSED : 0);
        send_to(client, &m);
    }
    wm_message_t end = { .type = WM_WINDOW_LIST_END };
    send_to(client, &end);
}

static void publish_window_list(void)
{
    if (!list_changed)
        return;
    list_changed = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].subscribed)
            send_window_list(&clients[i]);
    }
}

/* --- Focus and window states ---------------------------------------------------- */

static void focus(comp_window_t *w)
{
    comp_window_t *old = compositor_focused(&compositor);
    if (w && (w->flags & WM_WINDOW_PANEL))
        return; /* panels never take the focus */
    if (old == w)
        return;
    if (old)
        send_simple_event(old, WM_EVENT_FOCUS_OUT);
    compositor_focus(&compositor, w);
    if (w) {
        compositor_raise(&compositor, w);
        send_simple_event(w, WM_EVENT_FOCUS_IN);
    }
    list_changed = true;
}

static void focus_topmost(void)
{
    focus(compositor_topmost(&compositor));
}

static void set_minimized(comp_window_t *w, bool minimized)
{
    if (!is_normal(w) || w->minimized == minimized)
        return;
    compositor_set_minimized(&compositor, w, minimized);
    list_changed = true;
    if (minimized) {
        if (grab == w)
            grab = NULL;
        if (drag == w)
            drag = NULL;
        focus_topmost();
    } else {
        focus(w);
    }
}

static void activate(comp_window_t *w)
{
    set_minimized(w, false);
    compositor_raise(&compositor, w);
    focus(w);
}

static void set_maximized(comp_window_t *w, bool maximized)
{
    if (!(w->flags & WM_WINDOW_RESIZABLE) || w->maximized == maximized)
        return;
    if (maximized) {
        rect_t area = compositor_work_area(&compositor);
        w->restore = compositor_content_rect(w);
        w->maximized = true;
        compositor_move(&compositor, w, area.x + BORDER_WIDTH, area.y + TITLE_BAR_HEIGHT);
        propose_size(w, area.w - 2 * BORDER_WIDTH, area.h - TITLE_BAR_HEIGHT - BORDER_WIDTH);
    } else {
        w->maximized = false;
        compositor_move(&compositor, w, w->restore.x, w->restore.y);
        propose_size(w, w->restore.w, w->restore.h);
    }
    list_changed = true;
}

static void focus_next(void)
{
    /* Alt+Tab: bring the bottom normal window (also a minimized one) to the top. */
    for (int i = 0; i < compositor.count; i++) {
        comp_window_t *w = compositor.windows[i];
        if (is_normal(w) && !w->focused) {
            activate(w);
            return;
        }
    }
}

/* --- Windows -------------------------------------------------------------------- */

static void destroy_window(window_record_t *r)
{
    comp_window_t *w = r->window;
    bool was_focused = w->focused;
    if (grab == w)
        grab = NULL;
    if (hover == w)
        hover = NULL;
    if (drag == w)
        drag = NULL;
    if (sizing == w) {
        sizing = NULL;
        compositor_set_outline(&compositor, rect_make(0, 0, 0, 0));
    }
    if (pressed_window == w)
        pressed_window = NULL;
    void *pixels = w->content.pixels;
    compositor_remove(&compositor, w);
    jelly_memory_unmap(pixels, r->mapping_size);
    memset(r, 0, sizeof(*r));
    list_changed = true;
    if (was_focused)
        focus_topmost();
}

/* A new shared surface of width x height: mapped for us, the handle for the client. */
static status_t make_surface(int32_t width, int32_t height, size_t *size, void **pixels, jelly_handle_t *memory)
{
    if (width <= 0 || height <= 0 || width > WM_WINDOW_MAX_SIZE || height > WM_WINDOW_MAX_SIZE)
        return STATUS_INVALID_ARGUMENT;
    *size = ((size_t)width * height * 4 + 4095) & ~(size_t)4095;
    status_t status = jelly_shm_create(*size, memory);
    if (!STATUS_IS_ERROR(status))
        status = jelly_shm_map(*memory, JELLY_MEMORY_WRITE, pixels);
    if (STATUS_IS_ERROR(status) && *memory) {
        jelly_handle_close(*memory);
        *memory = JELLY_HANDLE_INVALID;
    }
    return status;
}

static void create_window(client_t *client, const wm_message_t *request)
{
    wm_message_t reply = { .type = WM_WINDOW_CREATED };
    window_record_t *r = NULL;
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    void *pixels = NULL;
    size_t size = 0;

    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS && !r; i++) {
        if (!records[i].window)
            r = &records[i];
    }
    status_t status = r ? make_surface(request->width, request->height, &size, &pixels, &memory)
                        : STATUS_LIMIT_EXCEEDED;
    if (STATUS_IS_ERROR(status)) {
        reply.status = (int32_t)status;
        send_to(client, &reply);
        return;
    }

    canvas_t content;
    canvas_init(&content, pixels, request->width, request->height, request->width);
    char title[WM_TITLE_MAX];
    memcpy(title, request->title, WM_TITLE_MAX);
    title[WM_TITLE_MAX - 1] = '\0';
    comp_window_t *w = compositor_add(&compositor, content, title, request->flags, request->x, request->y, client);
    if (!w) {
        jelly_memory_unmap(pixels, size);
        jelly_handle_close(memory);
        reply.status = STATUS_LIMIT_EXCEEDED;
        send_to(client, &reply);
        return;
    }
    r->window = w;
    r->client = client;
    r->mapping_size = size;

    reply.window = w->id;
    reply.x = w->x;
    reply.y = w->y;
    reply.width = request->width;
    reply.height = request->height;
    reply.stride = (uint32_t)request->width;
    /* The surface memory moves to the client; our mapping keeps it alive for us. */
    jelly_channel_send_handles(client->channel, &reply, sizeof(reply), &memory, 1);
    list_changed = true;
    focus(w);
}

static void resize_window(client_t *client, const wm_message_t *request)
{
    wm_message_t reply = { .type = WM_WINDOW_RESIZED, .window = request->window };
    window_record_t *r = find_record(client, request->window);
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    void *pixels = NULL;
    size_t size = 0;
    int32_t width = request->width < MIN_WINDOW_WIDTH ? MIN_WINDOW_WIDTH : request->width;
    int32_t height = request->height < 1 ? 1 : request->height;

    status_t status = r ? make_surface(width, height, &size, &pixels, &memory) : STATUS_NOT_FOUND;
    if (STATUS_IS_ERROR(status)) {
        reply.status = (int32_t)status;
        send_to(client, &reply);
        return;
    }
    comp_window_t *w = r->window;
    void *old = w->content.pixels;
    canvas_t content;
    canvas_init(&content, pixels, width, height, width);
    compositor_set_content(&compositor, w, content);
    jelly_memory_unmap(old, r->mapping_size);
    r->mapping_size = size;
    r->present_pending = false;

    reply.width = width;
    reply.height = height;
    reply.stride = (uint32_t)width;
    jelly_channel_send_handles(client->channel, &reply, sizeof(reply), &memory, 1);
}

static void broadcast_settings(void)
{
    for (int c = 0; c < MAX_CLIENTS; c++) {
        if (!clients[c].used)
            continue;
        /* One event per client, for one of its windows (toolkits apply settings app-wide). */
        for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++) {
            if (records[i].window && records[i].client == &clients[c]) {
                send_simple_event(records[i].window, WM_EVENT_SETTINGS);
                break;
            }
        }
    }
}

static void handle_request(client_t *client, const wm_message_t *m)
{
    window_record_t *r;

    switch (m->type) {
    case WM_HELLO: {
        wm_message_t reply = { .type = WM_WELCOME, .version = WM_PROTOCOL_VERSION,
                               .width = display.back.width, .height = display.back.height };
        send_to(client, &reply);
        break;
    }
    case WM_CREATE_WINDOW:
        create_window(client, m);
        break;
    case WM_RESIZE_WINDOW:
        resize_window(client, m);
        break;
    case WM_PRESENT:
        if ((r = find_record(client, m->window))) {
            compositor_damage_content(&compositor, r->window, rect_make(m->x, m->y, m->width, m->height));
            r->present_pending = true;
        }
        break;
    case WM_SET_TITLE:
        if ((r = find_record(client, m->window))) {
            char title[WM_TITLE_MAX];
            memcpy(title, m->title, WM_TITLE_MAX);
            title[WM_TITLE_MAX - 1] = '\0';
            compositor_set_title(&compositor, r->window, title);
            list_changed = true;
        }
        break;
    case WM_SET_STATE:
        if ((r = find_record(client, m->window))) {
            set_maximized(r->window, (m->flags & WM_STATE_MAXIMIZED) != 0);
            set_minimized(r->window, (m->flags & WM_STATE_MINIMIZED) != 0);
        }
        break;
    case WM_DESTROY_WINDOW:
        if ((r = find_record(client, m->window)))
            destroy_window(r);
        break;
    case WM_SUBSCRIBE_WINDOWS:
        client->subscribed = true;
        send_window_list(client);
        break;
    case WM_ACTIVATE_WINDOW:
        if ((r = find_record(NULL, m->window)) && is_normal(r->window))
            activate(r->window);
        break;
    case WM_MINIMIZE_WINDOW:
        if ((r = find_record(NULL, m->window)))
            set_minimized(r->window, true);
        break;
    case WM_SETTINGS_CHANGED:
        broadcast_settings();
        break;
    case WM_SET_KEYMAP: {
        char name[WM_TITLE_MAX];
        memcpy(name, m->title, WM_TITLE_MAX);
        name[WM_TITLE_MAX - 1] = '\0';
        if (keymap_select(name))
            log_message("keymap %s", name);
        break;
    }
    case WM_NOTIFY:
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].used && clients[i].subscribed)
                send_to(&clients[i], m);
        }
        break;
    }
}

static void drop_client(client_t *client)
{
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++) {
        if (records[i].window && records[i].client == client)
            destroy_window(&records[i]);
    }
    jelly_handle_close(client->channel);
    client->used = false;
    client->subscribed = false;
}

static void serve_client(client_t *client)
{
    wm_message_t m;
    size_t size;
    for (;;) {
        status_t status = jelly_channel_receive(client->channel, &m, sizeof(m), &size);
        if (status == STATUS_WOULD_BLOCK)
            return;
        if (STATUS_IS_ERROR(status)) {
            /* gone, or not speaking the protocol */
            drop_client(client);
            return;
        }
        if (size == sizeof(m))
            handle_request(client, &m);
    }
}

static void accept_clients(void)
{
    char message[16];
    jelly_handle_t handles[JELLY_CHANNEL_MAX_HANDLES];
    uint32_t count;
    size_t size;

    while (jelly_channel_receive_handles(service_channel, message, sizeof(message), &size, handles, &count) ==
           STATUS_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            client_t *slot = NULL;
            for (int c = 0; c < MAX_CLIENTS && !slot; c++) {
                if (!clients[c].used)
                    slot = &clients[c];
            }
            if (!slot) {
                jelly_handle_close(handles[i]);
                log_message("too many clients");
                continue;
            }
            slot->used = true;
            slot->subscribed = false;
            slot->channel = handles[i];
        }
    }
}

/* --- Input ---------------------------------------------------------------------- */

static void pointer_event(comp_window_t *w, uint32_t type, uint32_t button, int32_t wheel)
{
    wm_event_t e = {
        .type = type,
        .x = pointer_x - w->x,
        .y = pointer_y - w->y,
        .button = button,
        .buttons = buttons,
        .wheel = wheel,
        .modifiers = modifiers,
    };
    send_event(w, &e);
}

static rect_t sizing_rect(void)
{
    rect_t frame = compositor_frame_rect(sizing);
    int32_t width = pointer_x - frame.x + 1, height = pointer_y - frame.y + 1;
    int32_t min_w = MIN_WINDOW_WIDTH + 2 * BORDER_WIDTH, min_h = MIN_WINDOW_HEIGHT + TITLE_BAR_HEIGHT + BORDER_WIDTH;
    return rect_make(frame.x, frame.y, width < min_w ? min_w : width, height < min_h ? min_h : height);
}

static void pointer_moved(void)
{
    compositor_move_pointer(&compositor, pointer_x, pointer_y);
    if (compositor.hardware_pointer)
        display_pointer_move(&display, pointer_x, pointer_y, true);
    if (drag) {
        compositor_move(&compositor, drag, pointer_x - drag_dx, pointer_y - drag_dy);
        return;
    }
    if (sizing) {
        compositor_set_outline(&compositor, sizing_rect());
        return;
    }

    window_part_t part;
    comp_window_t *under = compositor_hit(&compositor, pointer_x, pointer_y, &part);
    for (int i = 0; i < compositor.count; i++)
        compositor_set_hover(&compositor, compositor.windows[i], compositor.windows[i] == under ? part : PART_NONE);

    comp_window_t *target = grab ? grab : (part == PART_CONTENT ? under : NULL);
    if (hover && hover != target)
        send_simple_event(hover, WM_EVENT_MOUSE_LEAVE);
    hover = target;
    if (target)
        pointer_event(target, WM_EVENT_MOUSE_MOVE, 0, 0);
}

static void button_event(uint32_t button, bool pressed)
{
    uint32_t bit = 1u << (button - 1);
    if (pressed) {
        buttons |= bit;
        window_part_t part;
        comp_window_t *w = compositor_hit(&compositor, pointer_x, pointer_y, &part);
        pressed_window = w;
        pressed_part = part;
        if (!w) {
            comp_window_t *focused = compositor_focused(&compositor);
            if (focused && (focused->flags & WM_WINDOW_POPUP))
                focus_topmost(); /* a click outside closes menus */
            return;
        }
        /* Panels never take the focus: a click on the taskbar leaves an open menu to the taskbar. */
        if (!(w->flags & WM_WINDOW_PANEL))
            focus(w);
        if (part == PART_TITLE && button == JELLY_BUTTON_LEFT && !w->maximized) {
            drag = w;
            drag_dx = pointer_x - w->x;
            drag_dy = pointer_y - w->y;
        } else if (part == PART_RESIZE && button == JELLY_BUTTON_LEFT) {
            sizing = w;
            compositor_set_outline(&compositor, sizing_rect());
        } else if (part == PART_CONTENT) {
            grab = w;
            pointer_event(w, WM_EVENT_MOUSE_DOWN, button, 0);
        }
    } else {
        buttons &= ~bit;
        if (button == JELLY_BUTTON_LEFT) {
            window_part_t part;
            comp_window_t *w = compositor_hit(&compositor, pointer_x, pointer_y, &part);
            /* Title bar buttons act on release over the same button. */
            if (w && w == pressed_window && part == pressed_part) {
                if (part == PART_CLOSE)
                    send_simple_event(w, WM_EVENT_CLOSE);
                else if (part == PART_MAXIMIZE)
                    set_maximized(w, !w->maximized);
                else if (part == PART_MINIMIZE)
                    set_minimized(w, true);
            }
            if (drag)
                drag = NULL;
            if (sizing) {
                rect_t r = sizing_rect();
                comp_window_t *s = sizing;
                sizing = NULL;
                compositor_set_outline(&compositor, rect_make(0, 0, 0, 0));
                propose_size(s, r.w - 2 * BORDER_WIDTH, r.h - TITLE_BAR_HEIGHT - BORDER_WIDTH);
            }
            pressed_window = NULL;
        }
        if (grab) {
            comp_window_t *w = grab;
            if (!buttons)
                grab = NULL;
            pointer_event(w, WM_EVENT_MOUSE_UP, button, 0);
            pointer_moved(); /* the window under the pointer may differ now */
        }
    }
}

static uint32_t modifier_of(uint32_t key)
{
    switch (key) {
    case JELLY_KEY_LEFTSHIFT:
    case JELLY_KEY_RIGHTSHIFT: return WM_MOD_SHIFT;
    case JELLY_KEY_LEFTCTRL:
    case JELLY_KEY_RIGHTCTRL:  return WM_MOD_CTRL;
    case JELLY_KEY_LEFTALT:    return WM_MOD_ALT;
    case JELLY_KEY_RIGHTALT:   return WM_MOD_ALTGR;
    case JELLY_KEY_LEFTMETA:
    case JELLY_KEY_RIGHTMETA:  return WM_MOD_META;
    default:                   return 0;
    }
}

static void key_event(uint32_t key, bool down, bool repeat)
{
    uint32_t modifier = modifier_of(key);
    if (modifier) {
        if (down)
            modifiers |= modifier;
        else
            modifiers &= ~modifier;
    }
    if (key == JELLY_KEY_CAPSLOCK && down && !repeat)
        modifiers ^= WM_MOD_CAPS;

    if (down && key == JELLY_KEY_TAB && (modifiers & WM_MOD_ALT)) {
        focus_next();
        return;
    }
    comp_window_t *w = compositor_focused(&compositor);
    if (!w)
        return;
    wm_event_t e = {
        .type = down ? WM_EVENT_KEY_DOWN : WM_EVENT_KEY_UP,
        .key = key,
        .character = down ? keymap_translate(key, modifiers) : 0,
        .modifiers = modifiers,
        .repeat = repeat,
        .buttons = buttons,
    };
    send_event(w, &e);
}

/*
 * Key repeat is made here, the same for every keyboard: USB keyboards never
 * repeat by themselves, PS/2 keyboards do (their repeats are ignored).
 */
#define REPEAT_DELAY_NS    400000000ull
#define REPEAT_INTERVAL_NS 33000000ull

static uint32_t repeat_key;
static uint64_t repeat_at;

static void gamepad_event(const jelly_input_event_t *e)
{
    comp_window_t *w = compositor_focused(&compositor);
    if (!w)
        return;
    wm_event_t event = {
        .type = e->type == JELLY_INPUT_GAMEPAD_BUTTON ? WM_EVENT_GAMEPAD_BUTTON : WM_EVENT_GAMEPAD_AXIS,
        .key = e->device,
        .button = e->code,
        .x = e->value,
        .modifiers = modifiers,
        .buttons = buttons,
    };
    send_event(w, &event);
}

static void handle_input(void)
{
    jelly_input_event_t events[32];
    size_t count;
    while (jelly_input_read(input_queue, events, 32, &count) == STATUS_SUCCESS && count) {
        for (size_t i = 0; i < count; i++) {
            jelly_input_event_t *e = &events[i];
            switch (e->type) {
            case JELLY_INPUT_MOUSE_MOVE:
                if (e->flags & JELLY_INPUT_ABSOLUTE) {
                    pointer_x = (int32_t)(((int64_t)e->x * (display.back.width - 1) + 32767) / 65535);
                    pointer_y = (int32_t)(((int64_t)e->y * (display.back.height - 1) + 32767) / 65535);
                } else {
                    pointer_x += e->dx;
                    pointer_y += e->dy;
                }
                if (pointer_x < 0) pointer_x = 0;
                if (pointer_y < 0) pointer_y = 0;
                if (pointer_x >= display.back.width) pointer_x = display.back.width - 1;
                if (pointer_y >= display.back.height) pointer_y = display.back.height - 1;
                pointer_moved();
                break;
            case JELLY_INPUT_MOUSE_BUTTON:
                if (e->code >= 1 && e->code <= 3)
                    button_event(e->code, e->value != 0);
                break;
            case JELLY_INPUT_MOUSE_WHEEL: {
                window_part_t part;
                comp_window_t *w = compositor_hit(&compositor, pointer_x, pointer_y, &part);
                if (w && part == PART_CONTENT)
                    pointer_event(w, WM_EVENT_MOUSE_WHEEL, 0, e->value);
                break;
            }
            case JELLY_INPUT_KEY_DOWN:
                if (e->value == 2)
                    break; /* the keyboard's own repeat */
                key_event(e->code, true, false);
                if (!modifier_of(e->code)) {
                    repeat_key = e->code;
                    repeat_at = jelly_clock_ns() + REPEAT_DELAY_NS;
                }
                break;
            case JELLY_INPUT_KEY_UP:
                if (e->code == repeat_key)
                    repeat_key = 0;
                key_event(e->code, false, false);
                break;
            case JELLY_INPUT_GAMEPAD_BUTTON:
            case JELLY_INPUT_GAMEPAD_AXIS:
                gamepad_event(e);
                break;
            }
        }
    }
}

/* --- Configuration and startup ------------------------------------------------ */

static char autostart[MAX_AUTOSTART][128];
static int autostart_count;

static void load_config(void)
{
    FILE *file = fopen(CONFIG_PATH, "r");
    char line[160];
    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        char *text = line;
        while (isspace((unsigned char)*text))
            text++;
        text[strcspn(text, "\r\n")] = '\0';
        if (!*text || *text == '#')
            continue;
        if (!strncmp(text, "keymap=", 7)) {
            if (!keymap_select(text + 7))
                log_message("unknown keymap '%s'", text + 7);
        } else if (!strncmp(text, "autostart=", 10) && autostart_count < MAX_AUTOSTART) {
            snprintf(autostart[autostart_count++], sizeof(autostart[0]), "%s", text + 10);
        }
    }
    fclose(file);
}

static void start_programs(void)
{
    for (int i = 0; i < autostart_count; i++) {
        char *argv[9] = { NULL };
        int argc = 0;
        char *state;
        for (char *word = strtok_r(autostart[i], " \t", &state); word && argc < 8;
             word = strtok_r(NULL, " \t", &state))
            argv[argc++] = word;
        jelly_handle_t process;
        if (argc && process_spawn(argv[0], argv, NULL, &process) == 0) {
            log_message("started %s", argv[0]);
            jelly_handle_close(process);
        } else if (argc) {
            log_message("cannot start %s: %s", argv[0], strerror(errno));
        }
    }
}

int main(void)
{
    jelly_handle_t registry_end;

    int status = display_open(0, &display);
    if (status) {
        log_message("no display available (%s)", strerror(status));
        return 0;
    }
    if (STATUS_IS_ERROR(jelly_input_open(&input_queue))) {
        log_message("cannot open the input devices");
        return 1;
    }
    if (STATUS_IS_ERROR(jelly_channel_create(&service_channel, &registry_end)) ||
        STATUS_IS_ERROR(jelly_service_register(WM_SERVICE_NAME, registry_end))) {
        log_message("cannot register the service '%s'", WM_SERVICE_NAME);
        return 1;
    }

    load_config();
    compositor_init(&compositor, &display);
    pointer_x = compositor.pointer_x;
    pointer_y = compositor.pointer_y;
    if (display.hardware_pointer) {
        /* The graphics driver has a pointer plane: the pointer no longer costs a repaint when it moves. */
        static uint32_t image[JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE];
        compositor_pointer_image(image, JELLY_CURSOR_SIZE);
        if (display_pointer_image(&display, image)) {
            compositor.hardware_pointer = true;
            display_pointer_move(&display, pointer_x, pointer_y, true);
        }
    }
    compositor_render(&compositor);
    log_message("%ux%u, keymap %s, %s%s, waiting for clients", display.info.width, display.info.height, keymap_name(),
                display.flip ? "page flipping" : display.vblank ? "vertical blank timing" : "plain framebuffer",
                compositor.hardware_pointer ? ", hardware pointer" : "");
    start_programs();

    for (;;) {
        jelly_handle_t handles[2 + MAX_CLIENTS];
        client_t *owners[2 + MAX_CLIENTS];
        uint32_t count = 0, index;
        handles[count] = input_queue;
        owners[count++] = NULL;
        handles[count] = service_channel;
        owners[count++] = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].used) {
                handles[count] = clients[i].channel;
                owners[count++] = &clients[i];
            }
        }
        uint64_t timeout = JELLY_WAIT_FOREVER, now = jelly_clock_ns();
        if (repeat_key)
            timeout = repeat_at > now ? repeat_at - now : 0;
        jelly_wait_many(handles, count, timeout, &index);

        /* Serve everything that is ready, not only what woke us. */
        handle_input();
        if (repeat_key && jelly_clock_ns() >= repeat_at) { /* after handle_input(): the key may be up by now */
            key_event(repeat_key, true, true);
            repeat_at = jelly_clock_ns() + REPEAT_INTERVAL_NS;
        }
        accept_clients();
        for (uint32_t i = 2; i < count; i++) {
            if (owners[i]->used)
                serve_client(owners[i]);
        }

        publish_window_list();
        compositor_render(&compositor);
        for (int i = 0; i < COMPOSITOR_MAX_WINDOWS; i++) {
            if (records[i].window && records[i].present_pending) {
                records[i].present_pending = false;
                wm_message_t m = { .type = WM_PRESENTED, .window = records[i].window->id };
                send_to(records[i].client, &m);
            }
        }
    }
}
