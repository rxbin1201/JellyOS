/*
 * Intel integrated graphics, generation 9: the part of the GPU that
 * executes commands (Intel calls it the GT), as opposed to the display
 * engine that intel_gpu.c drives. See intel_gt.c.
 *
 * The display driver starts it once it owns the screen and lends it what
 * the two share: the registers and room in the graphics address space.
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
} intel_gt_host_t;

typedef struct intel_gt intel_gt_t;

/*
 * Wake the GT, bring the engines up and let each execute a few commands to see that it does. *gt is set if at
 * least one engine works. Thread context; takes a few milliseconds.
 */
status_t intel_gt_start(const intel_gt_host_t *host, intel_gt_t **gt);

bool     intel_gt_engine_works(const intel_gt_t *gt, uint32_t engine);

/*
 * Execute `count` dwords of commands on an engine (count may be 0) and return when the engine is through with
 * them. One caller at a time per engine; the others wait. TIMEOUT: the engine did not get through in time and
 * is not used any more.
 */
status_t intel_gt_run(intel_gt_t *gt, uint32_t engine, const uint32_t *commands, uint32_t count, uint64_t timeout_ns);

#endif
