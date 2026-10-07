/*
 * Window protocol between applications and the display server
 * (README sections 33 and 35: surfaces, windows, input routing).
 *
 * Transport: a channel obtained with SYS_SERVICE_CONNECT("display"). Every
 * message is one wm_message_t; the server attaches a shared memory handle
 * to WM_WINDOW_CREATED. That memory is the window's surface: width x height
 * pixels, 0xAARRGGBB, `stride` pixels per row. The client draws into it and
 * sends WM_PRESENT with the changed rectangle; the server composites and
 * answers WM_PRESENTED once the frame is on screen, after which the client
 * may draw the next one (no tearing, natural pacing).
 *
 * Input arrives as WM_EVENT for the window with keyboard focus (keys) or
 * under the pointer (mouse, in content coordinates). Characters are already
 * translated with the keyboard layout.
 */

#ifndef GRAPHICS_WINDOW_PROTOCOL_H
#define GRAPHICS_WINDOW_PROTOCOL_H

#include <stdint.h>

#define WM_SERVICE_NAME     "display"
#define WM_PROTOCOL_VERSION 1
#define WM_TITLE_MAX        64
#define WM_WINDOW_MAX_SIZE  4096

enum {
    WM_HELLO = 1,          /* client: version                                 -> WM_WELCOME */
    WM_WELCOME,            /* server: version, width/height = screen size */
    WM_CREATE_WINDOW,      /* client: width, height, flags, title             -> WM_WINDOW_CREATED */
    WM_WINDOW_CREATED,     /* server: window, x, y, width, height, stride, status; handle: surface memory */
    WM_PRESENT,            /* client: window, x/y/width/height = changed area -> WM_PRESENTED */
    WM_PRESENTED,          /* server: window */
    WM_SET_TITLE,          /* client: window, title */
    WM_DESTROY_WINDOW,     /* client: window */
    WM_EVENT,              /* server: window, event */
};

/* WM_CREATE_WINDOW flags */
#define WM_WINDOW_UNDECORATED (1u << 0) /* no title bar or border */

/* Events */
enum {
    WM_EVENT_KEY_DOWN = 1,
    WM_EVENT_KEY_UP,
    WM_EVENT_MOUSE_MOVE,
    WM_EVENT_MOUSE_DOWN,
    WM_EVENT_MOUSE_UP,
    WM_EVENT_MOUSE_WHEEL,
    WM_EVENT_MOUSE_LEAVE,
    WM_EVENT_FOCUS_IN,
    WM_EVENT_FOCUS_OUT,
    WM_EVENT_CLOSE,        /* the user asked to close the window */
    WM_EVENT_FRAME,        /* client library: a WM_PRESENTED arrived */
};

#define WM_MOD_SHIFT (1u << 0)
#define WM_MOD_CTRL  (1u << 1)
#define WM_MOD_ALT   (1u << 2)
#define WM_MOD_META  (1u << 3)
#define WM_MOD_CAPS  (1u << 4)
#define WM_MOD_ALTGR (1u << 5)

typedef struct {
    uint32_t type;       /* WM_EVENT_* */
    uint32_t key;        /* JELLY_KEY_* */
    uint32_t character;  /* Unicode code point after the keyboard layout, 0 if none */
    uint32_t modifiers;  /* WM_MOD_* */
    int32_t  x, y;       /* pointer in content coordinates */
    uint32_t button;     /* JELLY_BUTTON_* of a press or release */
    uint32_t buttons;    /* all pressed buttons, bit (button - 1) */
    int32_t  wheel;      /* steps, positive = up */
    uint32_t repeat;     /* key auto-repeat */
} wm_event_t;

typedef struct {
    uint32_t   type;     /* WM_* */
    uint32_t   window;
    int32_t    x, y, width, height;
    uint32_t   flags;
    uint32_t   stride;
    int32_t    status;   /* replies: 0 or a status code */
    uint32_t   version;
    char       title[WM_TITLE_MAX];
    wm_event_t event;
} wm_message_t;

#endif
