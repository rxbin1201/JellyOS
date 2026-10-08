/*
 * Unit test of drivers/graphics/intel_kernels.c: the assembler for Intel's execution units makes the fill program
 * of Intel's test suite bit for bit, the copy and blend programs end their thread and fit, rectangles are cut into
 * pieces whose blocks cover them exactly once, and the CPU's blending formula.
 */

#include "drivers/graphics/intel_kernels.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

/* IGT's lib/gpgpu_fill.c, the Gen9 kernel */
static const uint32_t igt_fill[10][4] = {
    { 0x00400001, 0x20202288, 0x00000020, 0x00000000 }, { 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
    { 0x00000001, 0x20440208, 0x00000018, 0x00000000 }, { 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
    { 0x00200001, 0x20800208, 0x00450040, 0x00000000 }, { 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
    { 0x00800001, 0x20a00208, 0x00000020, 0x00000000 }, { 0x0c800031, 0x24000a40, 0x0e000080, 0x060a8000 },
    { 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 }, { 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 },
};

static uint32_t code[INTEL_KERNEL_MAX + 64][4];
static uint8_t cover[200][300];

/* Cut and check: every pixel once, the source moved by the same amount as the destination. */
static void pieces_cover(uint32_t to_x, uint32_t to_y, uint32_t from_x, uint32_t from_y, uint32_t w, uint32_t h)
{
    intel_piece_t pieces[4];
    uint32_t n = intel_pieces(to_x, to_y, from_x, from_y, w, h, pieces), twice = 0, shifted = 0;

    memset(cover, 0, sizeof(cover));
    for (uint32_t p = 0; p < n; p++) {
        uint32_t bw, bh;
        intel_shape_size(pieces[p].shape, &bw, &bh);
        if (pieces[p].from_x / 4 - pieces[p].to_x / 4 != from_x - to_x || pieces[p].from_y - pieces[p].to_y != from_y - to_y ||
            pieces[p].to_x % 4)
            shifted++;
        for (uint32_t y = 0; y < pieces[p].rows * bh; y++)
            for (uint32_t x = 0; x < pieces[p].columns * bw; x++)
                if (cover[pieces[p].to_y + y][pieces[p].to_x / 4 + x]++)
                    twice++;
    }
    uint32_t missing = 0, outside = 0;
    for (uint32_t y = 0; y < 200; y++)
        for (uint32_t x = 0; x < 300; x++) {
            bool in = x >= to_x && x < to_x + w && y >= to_y && y < to_y + h;
            missing += in && !cover[y][x];
            outside += !in && cover[y][x];
        }
    CHECK(n >= 1 && n <= 4 && !twice && !shifted && !missing && !outside);
}

int main(void)
{
    /* The assembler: IGT's program, bit for bit */
    uint32_t n = intel_kernel_igt_fill(code);
    CHECK(n == 10 && memcmp(code, igt_fill, sizeof(igt_fill)) == 0);

    /* Every program fits and ends with "end of thread" (a send to the thread spawner) */
    for (int blend = 0; blend < 2; blend++) {
        for (uint32_t shape = 0; shape < INTEL_SHAPES; shape++) {
            n = intel_kernel(blend, shape, code);
            CHECK(n > 8 && n <= INTEL_KERNEL_MAX);
            CHECK((code[n - 1][0] & 0x7F) == 0x31 && ((code[n - 1][0] >> 24) & 0xF) == 0x7 && code[n - 1][3] == 0x82000010);
        }
    }
    CHECK(intel_kernel(true, INTEL_SHAPE_8X8, code) > intel_kernel(true, INTEL_SHAPE_1X1, code));

    /* Shapes */
    uint32_t w, h;
    intel_shape_size(INTEL_SHAPE_8X8, &w, &h);
    CHECK(w == 8 && h == 8);
    intel_shape_size(INTEL_SHAPE_1X8, &w, &h);
    CHECK(w == 1 && h == 8);
    intel_shape_size(INTEL_SHAPE_8X1, &w, &h);
    CHECK(w == 8 && h == 1);
    intel_shape_size(INTEL_SHAPE_1X1, &w, &h);
    CHECK(w == 1 && h == 1);

    /* Rectangles: whole blocks, a strip on the right, at the bottom, both, tiny ones */
    intel_piece_t pieces[4];
    CHECK(intel_pieces(0, 0, 0, 0, 64, 32, pieces) == 1 && pieces[0].shape == INTEL_SHAPE_8X8 &&
          pieces[0].columns == 8 && pieces[0].rows == 4);
    CHECK(intel_pieces(3, 5, 7, 9, 101, 29, pieces) == 4);
    pieces_cover(0, 0, 0, 0, 64, 32);
    pieces_cover(37, 11, 5, 3, 101, 29);
    pieces_cover(1, 2, 100, 50, 7, 7);
    pieces_cover(10, 10, 10, 10, 1, 1);
    pieces_cover(16, 3, 0, 0, 200, 9);
    pieces_cover(250, 190, 0, 0, 50, 10);

    /* Blending: opaque is the source, transparent the destination, half way in between, rounded */
    CHECK(intel_blend_pixel(0xFF102030, 0x80405060, 256) == 0xFF102030);
    CHECK(intel_blend_pixel(0x00102030, 0x80405060, 256) == 0x80405060);
    CHECK(intel_blend_pixel(0xFF102030, 0x80405060, 0) == 0x80405060);
    uint32_t half = intel_blend_pixel(0x80FF0000, 0x0000FF00, 256);
    CHECK(((half >> 16) & 0xFF) == 128 && ((half >> 8) & 0xFF) == 127 && (half & 0xFF) == 0);
    CHECK(intel_blend_pixel(0xFFFFFFFF, 0x00000000, 128) == 0x7F7F7F7F);

    printf("unit: intel kernels: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
