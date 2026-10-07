/*
 * Compositor (README sections 33 and 35): windows, decorations, the
 * pointer and composition onto the display.
 *
 * Windows are kept bottom to top. Changes are recorded as damaged screen
 * rectangles; compositor_render() repaints only those (background, then
 * every window from the bottom, then the pointer) into the display's back
 * buffer and presents them.
 *
 * The look belongs to JellyOS's visual identity: a violet-blue desktop,
 * rounded title bars with a "jelly" accent for the focused window, soft
 * shadows and a round close button.
 */

#ifndef GRAPHICS_COMPOSITOR_COMPOSITOR_H
#define GRAPHICS_COMPOSITOR_COMPOSITOR_H

#include "graphics/core/canvas.h"
#include "graphics/display/display.h"
#include "graphics/window/protocol.h"

#define COMPOSITOR_MAX_WINDOWS 64
#define COMPOSITOR_MAX_DAMAGE  16
#define TITLE_BAR_HEIGHT       28
#define BORDER_WIDTH           1
#define CORNER_RADIUS          8
#define SHADOW_SIZE            10

typedef enum {
    PART_NONE,
    PART_TITLE,
    PART_CLOSE,
    PART_CONTENT,
    PART_BORDER,
} window_part_t;

typedef struct comp_window {
    uint32_t id;
    void    *owner;          /* the server's client record */
    canvas_t content;        /* the client's surface */
    int32_t  x, y;           /* top-left of the content area on screen */
    char     title[WM_TITLE_MAX];
    uint32_t flags;          /* WM_WINDOW_* */
    bool     focused;
    bool     close_hover;
} comp_window_t;

typedef struct {
    display_t     *display;
    comp_window_t *windows[COMPOSITOR_MAX_WINDOWS]; /* bottom to top */
    int            count;
    rect_t         damage[COMPOSITOR_MAX_DAMAGE];
    int            damage_count;
    int32_t        pointer_x, pointer_y;
    bool           pointer_visible;
    uint32_t       next_id;
    const char    *status_text;  /* bottom-right of the desktop */
} compositor_t;

void           compositor_init(compositor_t *c, display_t *display);

/* Content rectangle and whole outer rectangle (with decorations, without shadow). */
rect_t         compositor_content_rect(const comp_window_t *w);
rect_t         compositor_frame_rect(const comp_window_t *w);

/* Add a window (positioned in a cascade) above all others; returns NULL when full. */
comp_window_t *compositor_add(compositor_t *c, canvas_t content, const char *title, uint32_t flags, void *owner);
void           compositor_remove(compositor_t *c, comp_window_t *w);
void           compositor_raise(compositor_t *c, comp_window_t *w);
void           compositor_move(compositor_t *c, comp_window_t *w, int32_t x, int32_t y);
void           compositor_focus(compositor_t *c, comp_window_t *w); /* NULL: none */
comp_window_t *compositor_focused(compositor_t *c);
void           compositor_set_title(compositor_t *c, comp_window_t *w, const char *title);
void           compositor_set_close_hover(compositor_t *c, comp_window_t *w, bool hover);

/* The window and part at a screen position (topmost first). */
comp_window_t *compositor_hit(compositor_t *c, int32_t x, int32_t y, window_part_t *part);

void           compositor_damage(compositor_t *c, rect_t area);
/* A rectangle of a window's content changed (content coordinates). */
void           compositor_damage_content(compositor_t *c, comp_window_t *w, rect_t area);
void           compositor_move_pointer(compositor_t *c, int32_t x, int32_t y);

/* Repaint and present everything damaged; returns true if anything was drawn. */
bool           compositor_render(compositor_t *c);

#endif
