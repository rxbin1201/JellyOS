/*
 * Host unit tests for graphics/core (drawing, blending, clipping, text).
 *
 * Built and run on the development machine by `make unit` (part of
 * `make test`): the drawing code is plain C without OS dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/core/canvas.h"
#include "graphics/core/font.h"

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("unit: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

#define W 64
#define H 48
static uint32_t pixels[W * H];
static canvas_t canvas;

static void reset(color_t color)
{
    for (int i = 0; i < W * H; i++)
        pixels[i] = color;
    canvas_init(&canvas, pixels, W, H, W);
}

static uint32_t at(int x, int y)
{
    return pixels[y * W + x];
}

static void test_rects(void)
{
    rect_t a = rect_make(0, 0, 10, 10), b = rect_make(5, 5, 10, 10);
    rect_t i = rect_intersect(a, b);
    CHECK(i.x == 5 && i.y == 5 && i.w == 5 && i.h == 5);
    CHECK(rect_empty(rect_intersect(a, rect_make(20, 20, 5, 5))));
    rect_t u = rect_union(a, b);
    CHECK(u.x == 0 && u.y == 0 && u.w == 15 && u.h == 15);
    CHECK(rect_union(rect_make(0, 0, 0, 0), b).x == 5);
    CHECK(rect_contains(a, 9, 9) && !rect_contains(a, 10, 0));
    rect_t in = rect_inset(a, 2);
    CHECK(in.x == 2 && in.w == 6);
}

static void test_blend(void)
{
    CHECK(color_blend(RGB(0, 0, 0), RGB(255, 0, 0)) == RGB(255, 0, 0));       /* opaque wins */
    CHECK(color_blend(RGB(10, 20, 30), RGBA(255, 0, 0, 0)) == RGB(10, 20, 30)); /* transparent */
    color_t half = color_blend(RGB(0, 0, 0), RGBA(255, 255, 255, 128));
    CHECK(((half >> 16) & 0xFF) >= 126 && ((half >> 16) & 0xFF) <= 129);
    CHECK(COLOR_ALPHA(half) == 255);
    CHECK(color_mix(RGB(0, 0, 0), RGB(200, 100, 50), 255) == RGB(199, 99, 49) ||
          color_mix(RGB(0, 0, 0), RGB(200, 100, 50), 255) == RGB(200, 100, 50));
    CHECK(color_mix(RGB(0, 0, 0), RGB(200, 100, 50), 0) == RGB(0, 0, 0));
}

static void test_fill_and_clip(void)
{
    reset(RGB(0, 0, 0));
    canvas_fill(&canvas, rect_make(-5, -5, 10, 10), RGB(255, 0, 0));
    CHECK(at(0, 0) == RGB(255, 0, 0) && at(4, 4) == RGB(255, 0, 0) && at(5, 5) == RGB(0, 0, 0));

    rect_t previous = canvas_clip(&canvas, rect_make(10, 10, 4, 4));
    canvas_fill(&canvas, rect_make(0, 0, W, H), RGB(0, 255, 0));
    CHECK(at(10, 10) == RGB(0, 255, 0) && at(13, 13) == RGB(0, 255, 0));
    CHECK(at(9, 10) == RGB(0, 0, 0) && at(14, 14) == RGB(0, 0, 0));
    canvas.clip = previous;

    canvas_fill(&canvas, rect_make(W - 2, H - 2, 10, 10), RGB(0, 0, 255)); /* clipped at the edge */
    CHECK(at(W - 1, H - 1) == RGB(0, 0, 255));

    reset(RGB(0, 0, 0));
    canvas_outline(&canvas, rect_make(2, 2, 6, 4), RGB(255, 255, 255));
    CHECK(at(2, 2) == RGB(255, 255, 255) && at(7, 5) == RGB(255, 255, 255));
    CHECK(at(4, 3) == RGB(0, 0, 0)); /* inside stays */
}

static void test_rounded(void)
{
    reset(RGB(0, 0, 0));
    canvas_fill_rounded(&canvas, rect_make(0, 0, 40, 30), 8, RGB(255, 255, 255));
    CHECK(at(0, 0) == RGB(0, 0, 0));           /* the corner is cut */
    CHECK(at(39, 29) == RGB(0, 0, 0));
    CHECK(at(20, 15) == RGB(255, 255, 255));   /* middle */
    CHECK(at(20, 0) == RGB(255, 255, 255));    /* top edge between the corners */
    CHECK(at(0, 15) == RGB(255, 255, 255));    /* left edge between the corners */
    CHECK(at(8, 8) == RGB(255, 255, 255));     /* corner center */
    /* bottom corners are rounded like the top ones (symmetry) */
    CHECK(at(1, 1) == at(1, 28) && at(38, 1) == at(38, 28));
}

static void test_blit(void)
{
    uint32_t source_pixels[4 * 4];
    canvas_t source;
    for (int i = 0; i < 16; i++)
        source_pixels[i] = RGB(i, i, i);
    canvas_init(&source, source_pixels, 4, 4, 4);
    reset(RGB(0, 0, 0));
    canvas_blit(&canvas, 10, 10, &source, rect_make(1, 1, 2, 2), false);
    CHECK(at(10, 10) == RGB(5, 5, 5) && at(11, 11) == RGB(10, 10, 10));
    CHECK(at(12, 12) == RGB(0, 0, 0));
    /* partly outside: no overflow */
    canvas_blit(&canvas, W - 2, H - 2, &source, rect_make(0, 0, 4, 4), false);
    CHECK(at(W - 1, H - 1) == RGB(5, 5, 5));
    /* blending with alpha */
    source_pixels[0] = RGBA(255, 255, 255, 0);
    canvas_blit(&canvas, 0, 0, &source, rect_make(0, 0, 1, 1), true);
    CHECK(at(0, 0) == RGB(0, 0, 0));
}

static void test_text(void)
{
    const char *text = "Grüße";
    const char *p = text;
    CHECK(utf8_next(&p) == 'G');
    CHECK(utf8_next(&p) == 'r');
    CHECK(utf8_next(&p) == 0xFC); /* ü */
    CHECK(utf8_next(&p) == 0xDF); /* ß */
    CHECK(text_length(text, (int32_t)strlen(text)) == 5);
    CHECK(text_width(text, 1) == 5 * 8 && text_width(text, 2) == 5 * 16);
    CHECK(text_width("ab\ncd", 1) == 16); /* one line only */

    char encoded[4];
    CHECK(utf8_encode('A', encoded) == 1 && encoded[0] == 'A');
    CHECK(utf8_encode(0xE4, encoded) == 2 && (uint8_t)encoded[0] == 0xC3 && (uint8_t)encoded[1] == 0xA4);
    CHECK(utf8_encode(0x20AC, encoded) == 3);

    reset(RGB(0, 0, 0));
    CHECK(canvas_text(&canvas, 0, 0, "H", RGB(255, 255, 255), 1) == 8);
    int lit = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            lit += at(x, y) == RGB(255, 255, 255);
    CHECK(lit > 10 && lit < 64);          /* a glyph, not a block */
    CHECK(at(8, 8) == RGB(0, 0, 0));      /* nothing beyond the cell */

    /* the font has the Latin-1 letters JellyOS needs */
    int umlaut = 0;
    for (int i = 0; i < 16; i++)
        umlaut |= font8x16[0xE4][i];
    CHECK(umlaut != 0);
}

int main(void)
{
    test_rects();
    test_blend();
    test_fill_and_clip();
    test_rounded();
    test_blit();
    test_text();
    printf("unit: graphics/core %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
