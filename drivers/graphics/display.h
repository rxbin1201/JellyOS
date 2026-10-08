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
 *   display_set_framebuffer()   the driver's own framebuffer memory
 *   display_set_driver()        display_ops_t with what the hardware can do:
 *                               a hardware pointer, waiting for the vertical
 *                               blank, a second framebuffer to flip to,
 *                               switching modes
 *   display_set_modes()         the modes the monitor and the connection
 *                               allow (again after another monitor was
 *                               plugged in)
 *   display_set_connected()     a monitor went away or came back
 *
 * Each operation is optional; what a driver provides shows up as
 * JELLY_DISPLAY_* flags of the display, and the display server falls back
 * to software for the rest (system calls 76-82).
 *
 * A kernel panic takes the screen back from a display server: the console
 * draws into framebuffer 0, and the driver's `panic` operation makes that
 * what the monitor shows.
 *
 * Modes change while the display is in use, also while a display server
 * owns it. The framebuffer memory therefore never moves: a driver that can
 * switch modes allocates framebuffers large enough for every mode once, and
 * a mode only changes width, height and pitch. Whoever has the memory
 * mapped keeps a valid mapping; the display's event (display_watch()) tells
 * it to read the new geometry.
 */

#ifndef DRIVERS_GRAPHICS_DISPLAY_H
#define DRIVERS_GRAPHICS_DISPLAY_H

#include "core/boot.h"
#include "core/object.h"
#include "scheduler/mutex.h"

#include <jelly/syscall.h>

#define DISPLAY_MAX     4
#define DISPLAY_NO_MODE 0xFFFFFFFFu

struct display;

/*
 * What a graphics driver can do for a display beyond a framebuffer. Thread context; every entry may be NULL.
 * All but wait_vblank are called with the display's lock held (display_lock()), one at a time.
 */
typedef struct {
    /* A new pointer image: JELLY_CURSOR_SIZE x JELLY_CURSOR_SIZE pixels, 0xAARRGGBB, not premultiplied. */
    status_t (*cursor_image)(struct display *display, const uint32_t *pixels);
    /* Put the image's top left corner at x, y (may be off-screen), or hide the pointer. */
    void     (*cursor_move)(struct display *display, int32_t x, int32_t y, bool visible);
    /* Block until the next vertical blank; after a flip, until the flip has happened. TIMEOUT if none comes. */
    status_t (*wait_vblank)(struct display *display, uint64_t timeout_ns);
    /* Show framebuffer 0 or 1 from the next frame on (needs a second framebuffer in display_set_driver()). */
    status_t (*flip)(struct display *display, uint32_t buffer);
    /*
     * Switch to entry `mode` of the list given with display_set_modes(). The framebuffers stay where they are;
     * *pitch gets the bytes per line of the new mode. Framebuffer 0 is shown afterwards. If the mode does not
     * come up, the driver puts the previous one back and returns an error.
     */
    status_t (*set_mode)(struct display *display, uint32_t mode, uint32_t *pitch);
    /*
     * The kernel has panicked and its last words are in framebuffer 0: put that on the screen, without the
     * pointer, now. Unlike everything above this runs with interrupts off and for the last time, perhaps in the
     * middle of another operation: no locks (whoever holds one will never run again), no sleeping, only
     * polling. A driver whose screen always shows framebuffer 0 as it is needs none.
     */
    void     (*panic)(struct display *display);
} display_ops_t;

typedef struct display {
    jelly_display_info_t info;
    uint64_t             phys;      /* framebuffer physical address */
    volatile uint32_t   *pixels;    /* kernel mapping (write-combining) */
    bool                 acquired;
    bool                 console_stale; /* the mode changed while a display server owned the display */

    /* Set by a graphics driver */
    const display_ops_t *ops;
    void                *driver_data;
    uint64_t             second_phys; /* the second framebuffer (same size), 0 if there is none */
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];
    uint32_t             mode_count;
    uint32_t             current_mode; /* DISPLAY_NO_MODE: what is shown is not in the list */

    mutex_t              lock;      /* driver operations and mode changes */
    object_t            *changed;   /* event: signaled when geometry, modes or the connection change */
} display_t;

/* Register the boot framebuffer as display 0 (no-op without one). */
void       display_init_boot_framebuffer(void);
display_t *display_get(uint32_t index);
uint32_t   display_count(void);

/*
 * A graphics driver gives display `index` its own framebuffer (32 bits per pixel, the pixel format stays):
 * `size` bytes of memory at `phys` (0: just enough for this mode), showing width x height with `pitch` bytes
 * per line. BUSY once a display server owns the display. The kernel console moves to the new screen.
 */
status_t   display_set_framebuffer(uint32_t index, uint64_t phys, uint64_t size, uint32_t width, uint32_t height,
                                   uint32_t pitch);

/*
 * A graphics driver offers its operations for display `index` (after display_set_framebuffer(), if it changed
 * the mode). second_phys: a second framebuffer of the same size for flipping, or 0. The capability flags of
 * the display follow from what is there. ops == NULL: the display is a plain framebuffer again.
 */
status_t   display_set_driver(uint32_t index, const display_ops_t *ops, void *driver_data, uint64_t second_phys);

/*
 * The modes display `index` can show now (at most JELLY_DISPLAY_MODE_MAX; the flags are set here). `current`:
 * the entry being shown, or DISPLAY_NO_MODE. May be called at any time, also while the display is owned.
 */
status_t   display_set_modes(uint32_t index, const jelly_display_mode_t *modes, uint32_t count, uint32_t current);
/* A monitor was unplugged or plugged in. */
void       display_set_connected(uint32_t index, bool connected);

/* For drivers that touch their hardware outside the operations (hot plug): the lock the operations run under. */
void       display_lock(display_t *display);
void       display_unlock(display_t *display);

/* The operations for the display server (system calls); NOT_SUPPORTED where the driver has nothing. */
status_t   display_cursor(uint32_t index, const jelly_cursor_t *cursor, const uint32_t *pixels);
status_t   display_wait_vblank(uint32_t index, uint64_t timeout_ns);
status_t   display_flip(uint32_t index, uint32_t buffer);
/* The second framebuffer as a memory object to map. */
status_t   display_buffer(uint32_t index, uint32_t buffer, object_t **memory);
/* The list of modes; a display without a driver has one: what it shows. */
status_t   display_modes(uint32_t index, jelly_display_mode_t *modes, uint32_t max, uint32_t *count);
/* Switch modes. The console follows if it has the screen; a display server learns of it through the event. */
status_t   display_set_mode(uint32_t index, uint32_t mode);
/* The display's event (a new reference): signaled after every change; the watcher resets it. */
status_t   display_watch(uint32_t index, object_t **event);

/*
 * A kernel panic gets onto display 0 whoever owns it (registered with panic_set_screen()): prepare before its
 * text is written, show afterwards. Interrupts off.
 */
void       display_panic_prepare(void);
void       display_panic_show(void);

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
/* A panic: the text grid fits the screen as it is now (nothing is drawn); then the grid is drawn, whoever owns the screen. */
void       fb_console_panic_prepare(void);
void       fb_console_panic_show(void);

#endif
