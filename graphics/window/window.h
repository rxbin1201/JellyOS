/*
 * Window client library: connect to the display server, create windows,
 * draw into their surfaces and receive events (graphics/window/protocol.h).
 */

#ifndef GRAPHICS_WINDOW_WINDOW_H
#define GRAPHICS_WINDOW_WINDOW_H

#include "graphics/core/canvas.h"
#include "graphics/window/protocol.h"

#include <jelly/syscall.h>

typedef struct wm_connection wm_connection_t;

typedef struct window {
    wm_connection_t *connection;
    uint32_t         id;
    int32_t          width, height;
    canvas_t         canvas;        /* the surface: draw here, then wm_present() */
    bool             frame_pending; /* a present was sent and not answered yet */
    void            *user;
} window_t;

/* Connect to the display server. Returns 0 or a status code (NOT_FOUND: no server). */
int        wm_connect(wm_connection_t **connection);
void       wm_disconnect(wm_connection_t *connection);
/* Screen size reported by the server. */
void       wm_screen_size(wm_connection_t *connection, int32_t *width, int32_t *height);
/* The channel handle, to wait for it together with other handles. */
jelly_handle_t wm_handle(wm_connection_t *connection);

window_t  *wm_create_window(wm_connection_t *connection, int32_t width, int32_t height, const char *title,
                            uint32_t flags);
/* With WM_WINDOW_POSITIONED (panels, popups, full-screen windows). */
window_t  *wm_create_window_at(wm_connection_t *connection, int32_t x, int32_t y, int32_t width, int32_t height,
                               const char *title, uint32_t flags);
/* Replace the surface with one of a new size (after WM_EVENT_RESIZE, or on its own). */
int        wm_resize_window(window_t *window, int32_t width, int32_t height);
int        wm_set_state(window_t *window, uint32_t state);

/* Desktop shell: the window list (after WM_EVENT_WINDOWS) and control over other windows. */
typedef struct {
    uint32_t id;
    uint32_t state;      /* WM_STATE_* */
    char     title[WM_TITLE_MAX];
} wm_window_info_t;
int        wm_subscribe_windows(wm_connection_t *connection);
int        wm_window_list(wm_connection_t *connection, const wm_window_info_t **list);
int        wm_activate_window(wm_connection_t *connection, uint32_t id);
int        wm_minimize_window(wm_connection_t *connection, uint32_t id);

/* Settings and notifications */
int        wm_settings_changed(wm_connection_t *connection);
int        wm_set_keymap(wm_connection_t *connection, const char *name);
int        wm_notify(wm_connection_t *connection, const char *title, const char *text);
/* After WM_EVENT_NOTIFICATION (desktop shell only). */
void       wm_last_notification(wm_connection_t *connection, const char **title, const char **text);
void       wm_destroy_window(window_t *window);
int        wm_set_title(window_t *window, const char *title);
/* Show the changed rectangle of the surface. */
int        wm_present(window_t *window, rect_t changed);

/*
 * Next event. timeout_ns: 0 polls, JELLY_WAIT_FOREVER blocks. Returns 1 with
 * an event (*window set to its window, or NULL), 0 on timeout, or a negative
 * status when the server is gone. WM_PRESENTED arrives as WM_EVENT_FRAME.
 */
int        wm_next_event(wm_connection_t *connection, wm_event_t *event, window_t **window, uint64_t timeout_ns);

#endif
