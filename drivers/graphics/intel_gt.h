/*
 * Intel integrated graphics, generation 9: the part of the GPU that
 * executes commands (Intel calls it the GT), as opposed to the display
 * engine that intel_gpu.c drives. See intel_gt.c.
 *
 * The display driver starts it once it owns the screen and lends it what
 * the two share: the registers, room in the graphics address space, and its
 * interrupt.
 */

#ifndef DRIVERS_GRAPHICS_INTEL_GT_H
#define DRIVERS_GRAPHICS_INTEL_GT_H

#include <jelly/status.h>
#include <stdbool.h>
#include <stdint.h>

/* The engines this driver brings up; each executes its own kind of commands. */
#define INTEL_ENGINE_RENDER  0 /* 3D and everything else: the one that draws */
#define INTEL_ENGINE_BLITTER 1 /* copies and fills rectangles */
#define INTEL_ENGINE_COUNT   2

typedef struct {
    volatile uint8_t *regs;   /* BAR 0 */
    void             *context;
    /*
     * `pages` pages of zeroed memory, mapped next to each other in the global graphics address space (GGTT).
     * Returns their address there (0: none to be had) and in *phys their memory, which is contiguous, too.
     */
    uint32_t        (*alloc)(void *context, uint32_t pages, uint64_t *phys);
    /* The host's interrupt handler calls intel_gt_interrupt() when the master register names an engine. */
    bool              interrupts;
} intel_gt_host_t;

typedef struct intel_gt intel_gt_t;

/*
 * Wake the GT, bring the engines up and let each execute a few commands to see that it does. *gt is set early
 * (the interrupt handler needs it) and is NULL again if no engine works. Thread context; takes some milliseconds.
 */
status_t intel_gt_start(const intel_gt_host_t *host, intel_gt_t **gt);

bool     intel_gt_engine_works(const intel_gt_t *gt, uint32_t engine);
/* The rate the GT's clock runs at right now. */
uint32_t intel_gt_clock_mhz(const intel_gt_t *gt);
/* From the host's interrupt handler: an engine has something to say. */
void     intel_gt_interrupt(intel_gt_t *gt);

/*
 * Execute `count` dwords of commands on an engine (count may be 0), straight from its ring, where privileged
 * commands are allowed, and return when the engine is through with them. One caller at a time per engine; the
 * others wait. TIMEOUT: the engine did not get through in time and is not used any more.
 */
status_t intel_gt_run(intel_gt_t *gt, uint32_t engine, const uint32_t *commands, uint32_t count, uint64_t timeout_ns);

/*
 * The address space the engines draw in (one, shared by their contexts): enter `pages` pages of contiguous
 * memory at `phys`. `scanout`: a display reads this memory, so what the GPU writes must not stay in a cache.
 * Returns the address there, 0 if there is no room.
 */
uint64_t intel_gt_map(intel_gt_t *gt, uint64_t phys, uint32_t pages, bool scanout);
void     intel_gt_unmap(intel_gt_t *gt, uint64_t address, uint32_t pages);

/* A rectangle filled with a colour, or copied from another place. Pixels of 32 bits; addresses from intel_gt_map(). */
typedef struct {
    bool     copy;
    uint64_t to;             /* the surface drawn into, and the bytes of one of its lines */
    uint32_t to_pitch;
    uint32_t x, y, width, height;
    uint32_t color;          /* fill */
    uint64_t from;           /* copy: the surface read, its line length, the rectangle's corner there */
    uint32_t from_pitch;
    uint32_t from_x, from_y;
} intel_blit_t;

#define INTEL_BLIT_MAX 64

/*
 * Let the blitter do these, in order, and return when they are done. Source and destination of a copy must
 * not overlap. NOT_SUPPORTED without a working blitter.
 */
status_t intel_gt_blit(intel_gt_t *gt, const intel_blit_t *blits, uint32_t count);

#endif
