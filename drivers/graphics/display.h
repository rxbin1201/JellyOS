/*
 * Displays (README sections 33 and 35: the kernel side of the display
 * abstraction).
 *
 * A display is a linear framebuffer: today the UEFI GOP framebuffer the boot
 * manager hands over, later GPU drivers. The kernel draws its console on
 * display 0 until a display server acquires it; the server then owns the
 * framebuffer (mapped into its address space) until it releases it or exits.
 */

#ifndef DRIVERS_GRAPHICS_DISPLAY_H
#define DRIVERS_GRAPHICS_DISPLAY_H

#include "core/object.h"

#include <jelly/syscall.h>

#define DISPLAY_MAX 4

typedef struct display {
    jelly_display_info_t info;
    uint64_t             phys;      /* framebuffer physical address */
    volatile uint32_t   *pixels;    /* kernel mapping (write-combining) */
    bool                 acquired;
} display_t;

/* Register the boot framebuffer as display 0 (no-op without one). */
void       display_init_boot_framebuffer(void);
display_t *display_get(uint32_t index);
uint32_t   display_count(void);

/* Give the framebuffer to a display server: a memory object to map (shared memory handle). */
status_t   display_acquire(uint32_t index, object_t **memory);

/* Pack 8-bit RGB into the display's pixel format. */
static inline uint32_t display_color(const display_t *d, uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)(r >> (8 - d->info.red_size)) << d->info.red_shift |
           (uint32_t)(g >> (8 - d->info.green_size)) << d->info.green_shift |
           (uint32_t)(b >> (8 - d->info.blue_size)) << d->info.blue_shift;
}

/* Framebuffer console (drivers/graphics/fb_console.c) */
void       fb_console_init(display_t *display);
/* The display was acquired (false) or released (true): stop or resume drawing. */
void       fb_console_set_active(bool active);

#endif
