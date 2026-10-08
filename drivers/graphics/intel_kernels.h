/*
 * Programs for the execution units (EUs) of Intel graphics, generation 9,
 * and how a rectangle is cut into the blocks they work on. See
 * intel_kernels.c. No hardware here: the render engine (intel_render.c)
 * runs what is made here, and the unit test checks it on the host.
 */

#ifndef DRIVERS_GRAPHICS_INTEL_KERNELS_H
#define DRIVERS_GRAPHICS_INTEL_KERNELS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The block one hardware thread copies or blends: the hardware does not cut a block at the edge of a surface, so
 * a rectangle is cut into pieces whose blocks fit exactly. Inside 8 x 8 pixels; a strip on the right of 1 x 8,
 * one at the bottom of 8 x 1, the corner 1 x 1.
 */
#define INTEL_SHAPE_8X8    0
#define INTEL_SHAPE_1X8    1
#define INTEL_SHAPE_8X1    2
#define INTEL_SHAPE_1X1    3
#define INTEL_SHAPES       4

#define INTEL_KERNEL_MAX   512 /* instructions of the longest program (blending 8 x 8: about 380) */

/* One piece of a rectangle: its shape, how many blocks across and down, its corner in the destination and in the
 * source (x in bytes, as the programs count). */
typedef struct {
    uint32_t shape;
    uint32_t columns, rows;
    uint32_t to_x, to_y, from_x, from_y;
} intel_piece_t;

/* Cut a rectangle of width x height pixels at (to_x, to_y) / (from_x, from_y) into at most four pieces; returns how many. */
uint32_t intel_pieces(uint32_t to_x, uint32_t to_y, uint32_t from_x, uint32_t from_y, uint32_t width, uint32_t height,
                      intel_piece_t pieces[4]);

/* The pixels of one block of a shape: across, down. */
void     intel_shape_size(uint32_t shape, uint32_t *width, uint32_t *height);

/*
 * The program that copies (blend false) or blends (true) one block of a shape. Its input, in register r1 (the
 * constants of a piece): dwords 0, 1 the piece's corner in the destination (x in bytes), 2, 3 in the source, word 8
 * the opacity of blending, 0 to 256 (256: the source's own alpha as it is). The destination is binding table entry 0,
 * the source entry 1. Returns the number of instructions (16 bytes each) written to `code`.
 */
uint32_t intel_kernel(bool blend, uint32_t shape, uint32_t (*code)[4]);

/* The fill program of Intel's own test suite (IGT, gpgpu_fill), made by the same assembler: for checking it. */
uint32_t intel_kernel_igt_fill(uint32_t (*code)[4]);

/*
 * What the blending program computes, for the CPU: per byte (source * a + destination * (255 - a)) / 255,
 * rounded, where a = source alpha (its top byte) * opacity / 256.
 */
uint32_t intel_blend_pixel(uint32_t source, uint32_t destination, uint32_t opacity);

#endif
