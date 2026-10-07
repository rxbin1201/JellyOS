/*
 * Display abstraction (README sections 33-35) for the display server.
 *
 * A display is opened once (SYS_DISPLAY_ACQUIRE makes the server its
 * owner). Drawing goes to a back buffer in normal memory (fast to read for
 * blending). A frame is shown in two steps: display_present() names the
 * rectangles that changed, display_commit() puts them on the screen.
 *
 * How they get there depends on what the display's driver offers; the
 * server does not need to know which GPU, if any, is behind it:
 *
 *   plain framebuffer     the rectangles are copied at once
 *   vertical blank        the copy waits for the next frame
 *   page flipping         the rectangles go into the framebuffer that is
 *                         not shown, then the two swap at the next frame:
 *                         nothing half-drawn is ever visible
 *   hardware pointer      the pointer is a plane of its own and never
 *                         touches the picture (display_pointer_*)
 */

#ifndef GRAPHICS_DISPLAY_DISPLAY_H
#define GRAPHICS_DISPLAY_DISPLAY_H

#include "graphics/core/canvas.h"

#include <jelly/syscall.h>

#define DISPLAY_FRAME_RECTS 64

/* The rectangles of one frame; `full` when there were too many to keep apart. */
typedef struct {
    rect_t rects[DISPLAY_FRAME_RECTS];
    int    count;
    bool   full;
} display_frame_t;

typedef struct {
    jelly_display_info_t info;
    canvas_t             back;           /* draw here (0xAARRGGBB) */
    volatile uint32_t   *framebuffer;    /* the one shown without page flipping */
    uint32_t             framebuffer_stride;
    bool                 native_format;  /* the framebuffer is already XRGB 8888 */

    /* What the driver offers */
    bool                 vblank, flip, hardware_pointer;
    volatile uint32_t   *buffers[2];     /* page flipping: both framebuffers */
    int                  front;          /* the one on the screen */
    display_frame_t      current, previous;
} display_t;

/* Open display `index`; returns 0 or a status code. */
int  display_open(uint32_t index, display_t *display);
/* A rectangle of the back buffer changed and belongs to the frame being built. */
void display_present(display_t *display, rect_t area);
/* Show the frame. With vertical blank timing this returns when the frame is on the screen. */
void display_commit(display_t *display);

/* Hardware pointer (only if display->hardware_pointer): a 64x64 image, 0xAARRGGBB; x, y: its top left corner. */
bool display_pointer_image(display_t *display, const uint32_t *pixels);
void display_pointer_move(display_t *display, int32_t x, int32_t y, bool visible);

/* For tests of the frame bookkeeping (no display needed). */
void display_frame_add(display_frame_t *frame, rect_t area);

#endif
