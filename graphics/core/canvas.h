/*
 * Basic drawing (README section 33): a canvas is a rectangle of 32-bit
 * pixels in memory, 0xAARRGGBB (alpha, red, green, blue; little endian
 * bytes B, G, R, A). Every operation clips to the canvas' clip rectangle.
 *
 * Colors with alpha < 255 are blended over the destination ("source over").
 * Text uses the built-in 8x16 font (Latin-1; UTF-8 input) at an integer
 * scale for accessibility.
 */

#ifndef GRAPHICS_CORE_CANVAS_H
#define GRAPHICS_CORE_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t color_t;

#define RGB(r, g, b)      ((color_t)(0xFF000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b)))
#define RGBA(r, g, b, a)  ((color_t)((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b)))
#define COLOR_ALPHA(c)    ((uint8_t)((c) >> 24))

typedef struct {
    int32_t x, y, w, h;
} rect_t;

typedef struct {
    uint32_t *pixels;
    int32_t   width, height;
    int32_t   stride;  /* pixels per row */
    rect_t    clip;
} canvas_t;

/* --- Rectangles -------------------------------------------------------------------- */

static inline rect_t rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    rect_t r = { x, y, w, h };
    return r;
}

static inline bool rect_empty(rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

static inline bool rect_contains(rect_t r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

rect_t rect_intersect(rect_t a, rect_t b);
/* Smallest rectangle containing both (an empty one is ignored). */
rect_t rect_union(rect_t a, rect_t b);
rect_t rect_inset(rect_t r, int32_t amount);

/* --- Canvas -------------------------------------------------------------------- */

void    canvas_init(canvas_t *canvas, uint32_t *pixels, int32_t width, int32_t height, int32_t stride);
/* Restrict drawing to clip ∩ bounds; returns the previous clip. */
rect_t  canvas_clip(canvas_t *canvas, rect_t clip);
void    canvas_set_clip(canvas_t *canvas, rect_t clip);

color_t color_blend(color_t destination, color_t source);
/* Mix two opaque colors: amount 0 = a, 255 = b. */
color_t color_mix(color_t a, color_t b, uint8_t amount);

void    canvas_fill(canvas_t *canvas, rect_t r, color_t color);
void    canvas_fill_rounded(canvas_t *canvas, rect_t r, int32_t radius, color_t color);
void    canvas_outline(canvas_t *canvas, rect_t r, color_t color);
void    canvas_outline_rounded(canvas_t *canvas, rect_t r, int32_t radius, color_t color);
void    canvas_gradient(canvas_t *canvas, rect_t r, color_t top, color_t bottom);
void    canvas_line(canvas_t *canvas, int32_t x0, int32_t y0, int32_t x1, int32_t y1, color_t color);
void    canvas_pixel(canvas_t *canvas, int32_t x, int32_t y, color_t color);

/* Copy `from` (in source coordinates) to (x, y); blend when the source has alpha. */
void    canvas_blit(canvas_t *canvas, int32_t x, int32_t y, const canvas_t *source, rect_t from, bool blend);

/* A 1-bit mask (rows of `width` bits, MSB first, (width + 7) / 8 bytes per row) in one color. */
void    canvas_mask(canvas_t *canvas, int32_t x, int32_t y, const uint8_t *bits, int32_t width, int32_t height,
                    color_t color);

/* --- Text ---------------------------------------------------------------------- */

#define TEXT_CELL_WIDTH  8
#define TEXT_CELL_HEIGHT 16

/* Draw UTF-8 text (one line; stops at '\n'); returns the width drawn. */
int32_t canvas_text(canvas_t *canvas, int32_t x, int32_t y, const char *text, color_t color, int32_t scale);
int32_t text_width(const char *text, int32_t scale);
/* Number of characters (code points) in UTF-8 text up to `bytes`. */
int32_t text_length(const char *text, int32_t bytes);
/* Next code point from UTF-8; advances *text. */
uint32_t utf8_next(const char **text);
/* Encode a code point (< 0x10000) as UTF-8; returns the byte count. */
int     utf8_encode(uint32_t code_point, char *out);

#endif
