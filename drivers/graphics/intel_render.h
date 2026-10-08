/*
 * Intel graphics, generation 9: copying and blending rectangles on the
 * render engine's execution units, for putting a desktop together on the
 * GPU. See intel_render.c.
 */

#ifndef DRIVERS_GRAPHICS_INTEL_RENDER_H
#define DRIVERS_GRAPHICS_INTEL_RENDER_H

#include "drivers/graphics/intel_gt.h"

/* A surface of 32-bit pixels in the engines' address space (intel_gt_map()). */
typedef struct {
    uint64_t address;         /* page aligned */
    uint32_t width, height;   /* pixels: at most 4096 x 16384 */
    uint32_t pitch;           /* bytes per line: a multiple of 64 */
    bool     scanout;         /* a display reads it: written past the GPU's caches */
} intel_surface_t;

#define INTEL_COMPOSE_MAX 32

/* One rectangle copied, or blended with the source's alpha times `opacity` (0 to 256) over the destination. */
typedef struct {
    bool                   blend;
    uint32_t               opacity;
    const intel_surface_t *to, *from;
    uint32_t               to_x, to_y, from_x, from_y;
    uint32_t               width, height;
} intel_compose_t;

typedef struct intel_render intel_render_t;

/*
 * Make the programs, set up the render engine's memory for them and try copying and blending on surfaces of its
 * own, pixel by pixel against the CPU. *render is set only if all of it is right. Thread context.
 */
status_t intel_render_start(intel_gt_t *gt, intel_render_t **render);

/*
 * Do these, in order (each sees what the ones before it wrote), and return when they are done. Every rectangle
 * must lie inside both surfaces; a copy or blend within one surface must not lie on itself. INVALID_ARGUMENT
 * if one does not: then nothing is done.
 */
status_t intel_render_compose(intel_render_t *render, const intel_compose_t *operations, uint32_t count);

#endif
