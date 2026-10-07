/*
 * Display abstraction: the framebuffer backend.
 */

#include "graphics/display/display.h"

#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

int display_open(uint32_t index, display_t *d)
{
    jelly_handle_t memory;
    void *address;

    memset(d, 0, sizeof(*d));
    status_t status = jelly_display_info(index, &d->info);
    if (!STATUS_IS_ERROR(status))
        status = jelly_display_acquire(index, &memory);
    if (STATUS_IS_ERROR(status))
        return (int)status;
    status = jelly_shm_map(memory, JELLY_MEMORY_WRITE, &address);
    jelly_handle_close(memory); /* the mapping keeps the framebuffer */
    if (STATUS_IS_ERROR(status))
        return (int)status;

    uint32_t *back = malloc((size_t)d->info.width * d->info.height * 4);
    if (!back)
        return STATUS_OUT_OF_MEMORY;
    canvas_init(&d->back, back, (int32_t)d->info.width, (int32_t)d->info.height, (int32_t)d->info.width);
    d->framebuffer = address;
    d->framebuffer_stride = d->info.pitch / 4;
    d->native_format = d->info.red_shift == 16 && d->info.green_shift == 8 && d->info.blue_shift == 0 &&
                       d->info.red_size == 8 && d->info.green_size == 8 && d->info.blue_size == 8;
    return 0;
}

static uint32_t convert(const display_t *d, uint32_t pixel)
{
    uint32_t r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
    return (r >> (8 - d->info.red_size)) << d->info.red_shift |
           (g >> (8 - d->info.green_size)) << d->info.green_shift |
           (b >> (8 - d->info.blue_size)) << d->info.blue_shift;
}

void display_present(display_t *d, rect_t area)
{
    area = rect_intersect(area, rect_make(0, 0, d->back.width, d->back.height));
    for (int32_t y = area.y; y < area.y + area.h; y++) {
        const uint32_t *source = &d->back.pixels[y * d->back.stride + area.x];
        volatile uint32_t *target = &d->framebuffer[y * d->framebuffer_stride + area.x];
        if (d->native_format) {
            for (int32_t x = 0; x < area.w; x++)
                target[x] = source[x] & 0x00FFFFFF;
        } else {
            for (int32_t x = 0; x < area.w; x++)
                target[x] = convert(d, source[x]);
        }
    }
}
