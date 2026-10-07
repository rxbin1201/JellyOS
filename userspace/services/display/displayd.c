/*
 * displayd: the display server (README section 35).
 *
 * Owns display 0 and the input devices, keeps the windows of all clients
 * and composites them (graphics/compositor). Applications connect through
 * the named service "display" and speak graphics/window/protocol.h.
 *
 * Input routing:
 *   - keys go to the focused window, translated with the keyboard layout
 *   - the pointer goes to the window under it; while a button is held, the
 *     window that got the press keeps receiving the pointer (grab)
 *   - a press raises and focuses a window; dragging the title bar moves it;
 *     the close button sends WM_EVENT_CLOSE; Alt+Tab focuses the next window
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

/* Input state */
static int32_t pointer_x, pointer_y;
static uint32_t buttons, modifiers;
static comp_window_t *grab, *hover, *drag;
static int32_t drag_dx, drag_dy;

static void log_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("displayd: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
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
        if (records[i].window && records[i].client == client && records[i].window->id == id)
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

/* --- Focus ---------------------------------------------------------------------- */

static void focus(comp_window_t *w)
{
    comp_window_t *old = compositor_focused(&compositor);
    if (old == w)
        return;
    if (old)
        send_simple_event(old, WM_EVENT_FOCUS_OUT);
    compositor_focus(&compositor, w);
    if (w) {
        compositor_raise(&compositor, w);
        send_simple_event(w, WM_EVENT_FOCUS_IN);
    }
}

static void focus_topmost(void)
{
    focus(compositor.count ? compositor.windows[compositor.count - 1] : NULL);
}

static void focus_next(void)
{
    /* Alt+Tab: bring the bottom window to the top. */
    if (compositor.count > 1)
        focus(compositor.windows[0]);
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
    void *pixels = w->content.pixels;
    compositor_remove(&compositor, w);
    jelly_memory_unmap(pixels, r->mapping_size);
    memset(r, 0, sizeof(*r));
    if (was_focused)
        focus_topmost();
}

static void create_window(client_t *client, const wm_message_t *request)
{
    wm_message_t reply = { .type = WM_WINDOW_CREATED };
    window_record_t *r = NULL;
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    void *pixels = NULL;
    int32_t width = request->width, height = request->height;

    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS && !r; i++) {
        if (!records[i].window)
            r = &records[i];
    }
    size_t size = ((size_t)width * height * 4 + 4095) & ~(size_t)4095;
    status_t status = STATUS_SUCCESS;
    if (width <= 0 || height <= 0 || width > WM_WINDOW_MAX_SIZE || height > WM_WINDOW_MAX_SIZE)
        status = STATUS_INVALID_ARGUMENT;
    else if (!r)
        status = STATUS_LIMIT_EXCEEDED;
    if (!STATUS_IS_ERROR(status))
        status = jelly_shm_create(size, &memory);
    if (!STATUS_IS_ERROR(status))
        status = jelly_shm_map(memory, JELLY_MEMORY_WRITE, &pixels);
    if (STATUS_IS_ERROR(status)) {
        if (memory)
            jelly_handle_close(memory);
        reply.status = (int32_t)status;
        send_to(client, &reply);
        return;
    }

    canvas_t content;
    canvas_init(&content, pixels, width, height, width);
    char title[WM_TITLE_MAX];
    memcpy(title, request->title, WM_TITLE_MAX);
    title[WM_TITLE_MAX - 1] = '\0';
    comp_window_t *w = compositor_add(&compositor, content, title, request->flags, client);
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
    reply.width = width;
    reply.height = height;
    reply.stride = (uint32_t)width;
    /* The surface memory moves to the client; our mapping keeps it alive for us. */
    jelly_channel_send_handles(client->channel, &reply, sizeof(reply), &memory, 1);
    focus(w);
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
        }
        break;
    case WM_DESTROY_WINDOW:
        if ((r = find_record(client, m->window)))
            destroy_window(r);
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
}

static void serve_client(client_t *client)
{
    wm_message_t m;
    size_t size;
    for (;;) {
        status_t status = jelly_channel_receive(client->channel, &m, sizeof(m), &size);
        if (status == STATUS_WOULD_BLOCK)
            return;
        if (status == STATUS_BUFFER_TOO_SMALL) {
            /* Not a protocol message: the client is broken. */
            drop_client(client);
            return;
        }
        if (STATUS_IS_ERROR(status)) {
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

static void pointer_moved(void)
{
    compositor_move_pointer(&compositor, pointer_x, pointer_y);
    if (drag) {
        compositor_move(&compositor, drag, pointer_x - drag_dx, pointer_y - drag_dy);
        return;
    }

    window_part_t part;
    comp_window_t *under = compositor_hit(&compositor, pointer_x, pointer_y, &part);
    for (int i = 0; i < compositor.count; i++)
        compositor_set_close_hover(&compositor, compositor.windows[i],
                                   compositor.windows[i] == under && part == PART_CLOSE);

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
        if (!w) {
            focus(NULL);
            return;
        }
        focus(w);
        if (part == PART_TITLE && button == JELLY_BUTTON_LEFT) {
            drag = w;
            drag_dx = pointer_x - w->x;
            drag_dy = pointer_y - w->y;
        } else if (part == PART_CLOSE && button == JELLY_BUTTON_LEFT) {
            send_simple_event(w, WM_EVENT_CLOSE);
        } else if (part == PART_CONTENT) {
            grab = w;
            pointer_event(w, WM_EVENT_MOUSE_DOWN, button, 0);
        }
    } else {
        buttons &= ~bit;
        if (drag && button == JELLY_BUTTON_LEFT)
            drag = NULL;
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
                key_event(e->code, true, e->value == 2);
                break;
            case JELLY_INPUT_KEY_UP:
                key_event(e->code, false, false);
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
    compositor.status_text = "Alt+Tab: next window";
    pointer_x = compositor.pointer_x;
    pointer_y = compositor.pointer_y;
    compositor_render(&compositor);
    log_message("%ux%u, keymap %s, waiting for clients", display.info.width, display.info.height, keymap_name());
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
        jelly_wait_many(handles, count, JELLY_WAIT_FOREVER, &index);

        /* Serve everything that is ready, not only what woke us. */
        handle_input();
        accept_clients();
        for (uint32_t i = 2; i < count; i++) {
            if (owners[i]->used)
                serve_client(owners[i]);
        }

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
