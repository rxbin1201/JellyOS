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
 *
 * Version 2 (desktop): window kinds (panel, popup, fixed position), resizing
 * (the server proposes a size with WM_EVENT_RESIZE, the client asks for a
 * new surface with WM_RESIZE_WINDOW), minimize/maximize, the window list for
 * a taskbar, settings broadcasts and notifications relayed to the desktop.
 *
 * Version 3 (display modes): the screen can change its size while clients
 * run. The server tells every client the new size with WM_SCREEN and moves
 * and resizes what was laid out along the screen's edges (panels, full
 * screen windows, maximized windows). A client asks for another mode with
 * WM_SET_DISPLAY_MODE; the modes themselves come from SYS_DISPLAY_MODES.
 */

#ifndef GRAPHICS_WINDOW_PROTOCOL_H
#define GRAPHICS_WINDOW_PROTOCOL_H

#include <stdint.h>

#define WM_SERVICE_NAME     "display"
#define WM_PROTOCOL_VERSION 3
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
    /* version 2 */
    WM_RESIZE_WINDOW,      /* client: window, width, height                   -> WM_WINDOW_RESIZED */
    WM_WINDOW_RESIZED,     /* server: window, width, height, stride, status; handle: new surface memory */
    WM_SET_STATE,          /* client: window, flags = WM_STATE_* wanted (minimize, maximize) */
    WM_SUBSCRIBE_WINDOWS,  /* client: wants the window list now and after every change */
    WM_WINDOW_LIST,        /* server: window (global id), title, flags = WM_STATE_*; one per window */
    WM_WINDOW_LIST_END,    /* server: the list above is complete */
    WM_ACTIVATE_WINDOW,    /* client: window (global id): restore, raise, focus */
    WM_MINIMIZE_WINDOW,    /* client: window (global id) */
    WM_SETTINGS_CHANGED,   /* client: settings files changed -> WM_EVENT_SETTINGS to every client */
    WM_SET_KEYMAP,         /* client: title = layout name */
    WM_NOTIFY,             /* client: title, text -> relayed to the desktop shell (server: same fields) */
    /* version 3 */
    WM_SCREEN,             /* server: width, height = the screen's new size */
    WM_SET_DISPLAY_MODE,   /* client: width, height, flags = refresh rate in mHz (0: the fastest) -> WM_SCREEN */
};

/* WM_CREATE_WINDOW flags */
#define WM_WINDOW_UNDECORATED (1u << 0) /* no title bar or border */
#define WM_WINDOW_RESIZABLE   (1u << 1) /* resize grip and maximize button */
#define WM_WINDOW_POSITIONED  (1u << 2) /* place the content at x, y */
#define WM_WINDOW_PANEL       (1u << 3) /* undecorated, above windows, not in the window list, no focus on click */
#define WM_WINDOW_POPUP       (1u << 4) /* undecorated, above everything; gets the focus */
#define WM_WINDOW_RESERVE     (1u << 5) /* panel: maximized windows leave its area free */

/* WM_SET_STATE / WM_WINDOW_LIST flags */
#define WM_STATE_MINIMIZED    (1u << 0)
#define WM_STATE_MAXIMIZED    (1u << 1)
#define WM_STATE_FOCUSED      (1u << 2)

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
    /* version 2 */
    WM_EVENT_RESIZE,       /* the server proposes width = x, height = y (resize grip, maximize) */
    WM_EVENT_SETTINGS,     /* desktop settings changed: reload them */
    WM_EVENT_WINDOWS,      /* client library: the window list changed (wm_window_list()) */
    WM_EVENT_NOTIFICATION, /* desktop shell: wm_last_notification() has title and text */
    /* Gamepads (to the window with the keyboard focus): key = device, button = number or axis, x = value */
    WM_EVENT_GAMEPAD_BUTTON, /* button = 0-based number, x = 1 pressed / 0 released */
    WM_EVENT_GAMEPAD_AXIS,   /* button = JELLY_AXIS_*, x = -32768..32767 */
    /* version 3 */
    WM_EVENT_SCREEN,       /* client library: the screen has another size (x = width, y = height; wm_screen_size()) */
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

#define WM_TEXT_MAX 160

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
    char       text[WM_TEXT_MAX]; /* WM_NOTIFY body */
} wm_message_t;

#endif
