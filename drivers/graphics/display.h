/*
 * Displays (README sections 33 and 35: the kernel side of the display
 * abstraction).
 *
 * A display is a linear framebuffer: at first the UEFI GOP framebuffer the
 * boot manager hands over. The kernel draws its console on display 0 until
 * a display server acquires it; the server then owns the framebuffer
 * (mapped into its address space) until it releases it or exits.
 *
 * A graphics driver can take a display over. Everything it adds goes
 * through this file, so that neither the display server nor the next
 * driver depends on a particular GPU:
 *
 *   display_set_framebuffer()   another mode: new memory, new size
 *   display_set_driver()        display_ops_t with what the hardware can do:
 *                               a hardware pointer, waiting for the vertical
 *                               blank, a second framebuffer to flip to
 *
 * Each operation is optional; what a driver provides shows up as
 * JELLY_DISPLAY_* flags of the display, and the display server falls back
 * to software for the rest (system calls 76-79).
 */

#ifndef DRIVERS_GRAPHICS_DISPLAY_H
#define DRIVERS_GRAPHICS_DISPLAY_H

#include "core/boot.h"
#include "core/object.h"

#include <jelly/syscall.h>

#define DISPLAY_MAX 4

struct display;

/* What a graphics driver can do for a display beyond a framebuffer. Thread context; every entry may be NULL. */
typedef struct {
    /* A new pointer image: JELLY_CURSOR_SIZE x JELLY_CURSOR_SIZE pixels, 0xAARRGGBB, not premultiplied. */
    status_t (*cursor_image)(struct display *display, const uint32_t *pixels);
    /* Put the image's top left corner at x, y (may be off-screen), or hide the pointer. */
    void     (*cursor_move)(struct display *display, int32_t x, int32_t y, bool visible);
    /* Block until the next vertical blank; after a flip, until the flip has happened. TIMEOUT if none comes. */
    status_t (*wait_vblank)(struct display *display, uint64_t timeout_ns);
    /* Show framebuffer 0 or 1 from the next frame on (needs a second framebuffer in display_set_driver()). */
    status_t (*flip)(struct display *display, uint32_t buffer);
} display_ops_t;

typedef struct display {
    jelly_display_info_t info;
    uint64_t             phys;      /* framebuffer physical address */
    volatile uint32_t   *pixels;    /* kernel mapping (write-combining) */
    bool                 acquired;

    /* Set by a graphics driver */
    const display_ops_t *ops;
    void                *driver_data;
    uint64_t             second_phys; /* the second framebuffer (same size), 0 if there is none */
} display_t;

/* Register the boot framebuffer as display 0 (no-op without one). */
void       display_init_boot_framebuffer(void);
display_t *display_get(uint32_t index);
uint32_t   display_count(void);

/*
 * A graphics driver switched modes: display `index` now has this framebuffer (32 bits per pixel, the pixel
 * format stays). BUSY once a display server owns the display. The kernel console moves to the new screen.
 */
status_t   display_set_framebuffer(uint32_t index, uint64_t phys, uint32_t width, uint32_t height, uint32_t pitch);

/*
 * A graphics driver offers its operations for display `index` (after display_set_framebuffer(), if it changed
 * the mode). second_phys: a second framebuffer of the same size for flipping, or 0. The capability flags of
 * the display follow from what is there.
 */
status_t   display_set_driver(uint32_t index, const display_ops_t *ops, void *driver_data, uint64_t second_phys);

/* The operations for the display server (system calls); NOT_SUPPORTED where the driver has nothing. */
status_t   display_cursor(uint32_t index, const jelly_cursor_t *cursor, const uint32_t *pixels);
status_t   display_wait_vblank(uint32_t index, uint64_t timeout_ns);
status_t   display_flip(uint32_t index, uint32_t buffer);
/* The second framebuffer as a memory object to map. */
status_t   display_buffer(uint32_t index, uint32_t buffer, object_t **memory);

/* Give the framebuffer to a display server: a memory object to map (shared memory handle). */
status_t   display_acquire(uint32_t index, object_t **memory);

/* Pack 8-bit RGB into the display's pixel format. */
static inline uint32_t display_color(const display_t *d, uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)(r >> (8 - d->info.red_size)) << d->info.red_shift |
           (uint32_t)(g >> (8 - d->info.green_size)) << d->info.green_shift |
           (uint32_t)(b >> (8 - d->info.blue_size)) << d->info.blue_shift;
}

/* Early screen console on the boot framebuffer until fb_console_init() (drivers/graphics/early_fb.c) */
void       early_fb_init(const boot_info_t *info);

/* Framebuffer console (drivers/graphics/fb_console.c) */
void       fb_console_init(display_t *display);
/* The display's size or framebuffer changed (the console was set inactive before): new text grid, repaint, active. */
void       fb_console_resize(display_t *display);
/* The display was acquired (false) or released (true): stop or resume drawing. */
void       fb_console_set_active(bool active);

#endif
