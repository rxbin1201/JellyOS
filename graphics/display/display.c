/*
 * Display abstraction: framebuffers with and without a graphics driver.
 * See display.h.
 */

#include "graphics/display/display.h"

#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

#define VBLANK_TIMEOUT_NS 100000000ull /* a tenth of a second: if no frame comes, go on without it */

static int map_buffer(jelly_handle_t memory, volatile uint32_t **pixels)
{
    void *address;
    status_t status = jelly_shm_map(memory, JELLY_MEMORY_WRITE, &address);
    jelly_handle_close(memory); /* the mapping keeps the framebuffer */
    *pixels = address;
    return (int)status;
}

int display_open(uint32_t index, display_t *d)
{
    jelly_handle_t memory;

    memset(d, 0, sizeof(*d));
    status_t status = jelly_display_info(index, &d->info);
    if (!STATUS_IS_ERROR(status))
        status = jelly_display_acquire(index, &memory);
    if (STATUS_IS_ERROR(status))
        return (int)status;
    if (map_buffer(memory, &d->framebuffer))
        return STATUS_OUT_OF_MEMORY;

    uint32_t *back = malloc((size_t)d->info.width * d->info.height * 4);
    if (!back)
        return STATUS_OUT_OF_MEMORY;
    canvas_init(&d->back, back, (int32_t)d->info.width, (int32_t)d->info.height, (int32_t)d->info.width);
    d->framebuffer_stride = d->info.pitch / 4;
    d->native_format = d->info.red_shift == 16 && d->info.green_shift == 8 && d->info.blue_shift == 0 &&
                       d->info.red_size == 8 && d->info.green_size == 8 && d->info.blue_size == 8;

    /* What the driver adds; each part is used only if it is there and works. */
    d->vblank = d->info.flags & JELLY_DISPLAY_VBLANK;
    d->hardware_pointer = d->info.flags & JELLY_DISPLAY_CURSOR;
    d->buffers[0] = d->framebuffer;
    if ((d->info.flags & JELLY_DISPLAY_FLIP) && d->vblank &&
        jelly_display_buffer(index, 1, &memory) == STATUS_SUCCESS && map_buffer(memory, &d->buffers[1]) == 0) {
        d->flip = true;
        /* Neither framebuffer holds our picture yet: the first two frames copy everything. */
        d->previous.full = true;
        d->current.full = true;
    }
    if (jelly_display_watch(index, &d->watch) != STATUS_SUCCESS)
        d->watch = JELLY_HANDLE_INVALID;
    return 0;
}

bool display_changed(display_t *d)
{
    jelly_display_info_t info;

    if (d->watch != JELLY_HANDLE_INVALID)
        jelly_event_reset(d->watch); /* before reading: a change after this signals again */
    if (jelly_display_info(d->info.index, &info) != STATUS_SUCCESS)
        return false;
    bool resized = info.width != d->info.width || info.height != d->info.height || info.pitch != d->info.pitch;
    bool touched = info.generation != d->info.generation;
    d->info = info;
    if (!resized) {
        if (touched) {
            /*
             * Possibly a mode switch at the same size: the first framebuffer is shown and both are empty.
             * Without this, flipping would alternate between the picture and a black one.
             */
            d->front = 0;
            d->previous.count = d->current.count = 0;
            d->previous.full = d->current.full = true;
            d->stale = true;
        }
        return false;
    }

    uint32_t *back = malloc((size_t)info.width * info.height * 4);
    if (!back)
        return false; /* the old back buffer stays; the picture is cut off or has a border */
    free(d->back.pixels);
    canvas_init(&d->back, back, (int32_t)info.width, (int32_t)info.height, (int32_t)info.width);
    d->framebuffer_stride = info.pitch / 4;
    /* After a mode switch the first framebuffer is shown, and neither holds a picture of the new size. */
    d->front = 0;
    d->previous.count = d->current.count = 0;
    d->previous.full = d->current.full = true;
    return true;
}

void display_frame_add(display_frame_t *frame, rect_t area)
{
    if (frame->full || rect_empty(area))
        return;
    if (frame->count == DISPLAY_FRAME_RECTS) {
        frame->full = true;
        return;
    }
    frame->rects[frame->count++] = area;
}

static uint32_t convert(const display_t *d, uint32_t pixel)
{
    uint32_t r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
    return (r >> (8 - d->info.red_size)) << d->info.red_shift |
           (g >> (8 - d->info.green_size)) << d->info.green_shift |
           (b >> (8 - d->info.blue_size)) << d->info.blue_shift;
}

static void copy_rect(const display_t *d, volatile uint32_t *framebuffer, rect_t area)
{
    area = rect_intersect(area, rect_make(0, 0, d->back.width, d->back.height));
    for (int32_t y = area.y; y < area.y + area.h; y++) {
        const uint32_t *source = &d->back.pixels[y * d->back.stride + area.x];
        volatile uint32_t *target = &framebuffer[y * d->framebuffer_stride + area.x];
        if (d->native_format) {
            for (int32_t x = 0; x < area.w; x++)
                target[x] = source[x] & 0x00FFFFFF;
        } else {
            for (int32_t x = 0; x < area.w; x++)
                target[x] = convert(d, source[x]);
        }
    }
}

static void copy_frame(const display_t *d, volatile uint32_t *framebuffer, const display_frame_t *frame)
{
    if (frame->full) {
        copy_rect(d, framebuffer, rect_make(0, 0, d->back.width, d->back.height));
        return;
    }
    for (int i = 0; i < frame->count; i++)
        copy_rect(d, framebuffer, frame->rects[i]);
}

void display_present(display_t *d, rect_t area)
{
    display_frame_add(&d->current, rect_intersect(area, rect_make(0, 0, d->back.width, d->back.height)));
}

void display_commit(display_t *d)
{
    if (!d->current.full && d->current.count == 0)
        return;
    if (d->info.flags & JELLY_DISPLAY_OFF)
        return; /* nobody sees it: the frame waits */

    if (d->flip) {
        /*
         * The hidden framebuffer was last complete two frames ago: it lacks what the previous frame
         * changed (that went to the other one) and what this frame changes.
         */
        int hidden = 1 - d->front;
        copy_frame(d, d->buffers[hidden], &d->previous);
        copy_frame(d, d->buffers[hidden], &d->current);
        status_t flipped = jelly_display_flip(d->info.index, (uint32_t)hidden);
        if (flipped == STATUS_SUCCESS) {
            /* Until the swap has happened the old front is still being shown: wait before drawing into it. */
            jelly_display_vblank(d->info.index, VBLANK_TIMEOUT_NS);
            d->front = hidden;
            d->previous = d->current;
        } else if (flipped == STATUS_BUSY) {
            return; /* the screen was switched off this moment (display_changed() will tell): the frame waits */
        } else {
            /* The driver changed its mind: show the frame the plain way from now on. */
            d->flip = false;
            copy_frame(d, d->buffers[d->front], &d->current);
        }
    } else {
        if (d->vblank && jelly_display_vblank(d->info.index, VBLANK_TIMEOUT_NS) == STATUS_NOT_SUPPORTED)
            d->vblank = false;
        copy_frame(d, d->buffers[d->front], &d->current);
    }
    d->current.count = 0;
    d->current.full = false;
}

bool display_pointer_image(display_t *d, const uint32_t *pixels)
{
    jelly_cursor_t cursor = { .flags = JELLY_CURSOR_IMAGE, .pixels = pixels };

    if (!d->hardware_pointer || jelly_display_cursor(d->info.index, &cursor) != STATUS_SUCCESS)
        d->hardware_pointer = false;
    return d->hardware_pointer;
}

void display_pointer_move(display_t *d, int32_t x, int32_t y, bool visible)
{
    jelly_cursor_t cursor = { .flags = visible ? JELLY_CURSOR_VISIBLE : 0, .x = x, .y = y };

    if (d->hardware_pointer)
        jelly_display_cursor(d->info.index, &cursor);
}
