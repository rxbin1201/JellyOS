/*
 * Display abstraction (README sections 33-35) for the display server.
 *
 * A display is opened once (SYS_DISPLAY_ACQUIRE makes the server its
 * owner). Drawing goes to a back buffer in normal memory (fast to read for
 * blending); display_present() copies changed rectangles to the hardware
 * framebuffer and converts the pixel format on the way. A GPU backend will
 * implement the same interface later.
 */

#ifndef GRAPHICS_DISPLAY_DISPLAY_H
#define GRAPHICS_DISPLAY_DISPLAY_H

#include "graphics/core/canvas.h"

#include <jelly/syscall.h>

typedef struct {
    jelly_display_info_t info;
    canvas_t             back;          /* draw here (0xAARRGGBB) */
    volatile uint32_t   *framebuffer;
    uint32_t             framebuffer_stride;
    bool                 native_format; /* the framebuffer is already XRGB 8888 */
} display_t;

/* Open display `index`; returns 0 or a status code. */
int  display_open(uint32_t index, display_t *display);
/* Copy a rectangle of the back buffer to the screen. */
void display_present(display_t *display, rect_t area);

#endif
