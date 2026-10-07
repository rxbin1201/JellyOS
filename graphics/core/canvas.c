/*
 * Basic drawing (README section 33). Plain C without dependencies, so the
 * host unit tests (tests/unit) compile it as well.
 */

#include "graphics/core/canvas.h"
#include "graphics/core/font.h"

rect_t rect_intersect(rect_t a, rect_t b)
{
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int32_t y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0)
        return rect_make(0, 0, 0, 0);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

rect_t rect_union(rect_t a, rect_t b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int32_t x0 = a.x < b.x ? a.x : b.x;
    int32_t y0 = a.y < b.y ? a.y : b.y;
    int32_t x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int32_t y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

rect_t rect_inset(rect_t r, int32_t amount)
{
    return rect_make(r.x + amount, r.y + amount, r.w - 2 * amount, r.h - 2 * amount);
}

void canvas_init(canvas_t *c, uint32_t *pixels, int32_t width, int32_t height, int32_t stride)
{
    c->pixels = pixels;
    c->width = width;
    c->height = height;
    c->stride = stride;
    c->clip = rect_make(0, 0, width, height);
}

rect_t canvas_clip(canvas_t *c, rect_t clip)
{
    rect_t previous = c->clip;
    c->clip = rect_intersect(c->clip, clip);
    return previous;
}

void canvas_set_clip(canvas_t *c, rect_t clip)
{
    c->clip = rect_intersect(rect_make(0, 0, c->width, c->height), clip);
}

color_t color_blend(color_t d, color_t s)
{
    uint32_t a = s >> 24;
    if (a == 255)
        return s;
    if (a == 0)
        return d;
    uint32_t inverse = 255 - a;
    uint32_t rb = ((s & 0xFF00FF) * a + (d & 0xFF00FF) * inverse) >> 8;
    uint32_t g = ((s & 0x00FF00) * a + (d & 0x00FF00) * inverse) >> 8;
    uint32_t da = d >> 24;
    uint32_t out_alpha = a + (da * inverse) / 255;
    return out_alpha << 24 | (rb & 0xFF00FF) | (g & 0x00FF00);
}

color_t color_mix(color_t a, color_t b, uint8_t amount)
{
    return color_blend(a, (b & 0x00FFFFFF) | (uint32_t)amount << 24) | 0xFF000000u;
}

void canvas_pixel(canvas_t *c, int32_t x, int32_t y, color_t color)
{
    if (!rect_contains(c->clip, x, y))
        return;
    uint32_t *p = &c->pixels[y * c->stride + x];
    *p = color_blend(*p, color);
}

void canvas_fill(canvas_t *c, rect_t r, color_t color)
{
    r = rect_intersect(r, c->clip);
    if (rect_empty(r) || COLOR_ALPHA(color) == 0)
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = &c->pixels[y * c->stride + r.x];
        if (COLOR_ALPHA(color) == 255) {
            for (int32_t x = 0; x < r.w; x++)
                row[x] = color;
        } else {
            for (int32_t x = 0; x < r.w; x++)
                row[x] = color_blend(row[x], color);
        }
    }
}

/* Coverage (0..255) of pixel (px, py) inside a quarter circle with the given radius around (cx, cy). */
static uint32_t corner_coverage(int32_t px, int32_t py, int32_t cx, int32_t cy, int32_t radius)
{
    /* 4x4 supersampling */
    uint32_t inside = 0;
    int64_t r2 = (int64_t)radius * radius * 16;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int64_t dx = (int64_t)(px - cx) * 4 + sx - 1;
            int64_t dy = (int64_t)(py - cy) * 4 + sy - 1;
            /* scale: distances in quarter pixels, so compare against (4r)^2 */
            if (dx * dx + dy * dy <= r2)
                inside++;
        }
    }
    return inside * 255 / 16;
}

/* Draw the part `area` of a rounded rectangle r (filled or as a 1-pixel outline). */
static void fill_rounded(canvas_t *c, rect_t r, int32_t radius, color_t color, bool outline, rect_t area)
{
    if (radius * 2 > r.w)
        radius = r.w / 2;
    if (radius * 2 > r.h)
        radius = r.h / 2;
    rect_t visible = rect_intersect(rect_intersect(r, area), c->clip);
    uint32_t alpha = COLOR_ALPHA(color);

    for (int32_t y = visible.y; y < visible.y + visible.h; y++) {
        for (int32_t x = visible.x; x < visible.x + visible.w; x++) {
            int32_t lx = x - r.x, ly = y - r.y;
            int32_t cx = -1, cy = -1;
            if (lx < radius)
                cx = r.x + radius;
            else if (lx >= r.w - radius)
                cx = r.x + r.w - radius - 1;
            if (ly < radius)
                cy = r.y + radius;
            else if (ly >= r.h - radius)
                cy = r.y + r.h - radius - 1;

            uint32_t coverage = 255;
            if (cx >= 0 && cy >= 0) {
                coverage = corner_coverage(x, y, cx, cy, radius);
                if (outline) {
                    uint32_t inner = radius > 1 ? corner_coverage(x, y, cx, cy, radius - 1) : 0;
                    coverage = coverage > inner ? coverage - inner : 0;
                }
            } else if (outline && lx != 0 && ly != 0 && lx != r.w - 1 && ly != r.h - 1) {
                coverage = 0;
            }
            if (coverage)
                canvas_pixel(c, x, y, (color & 0x00FFFFFF) | (alpha * coverage / 255) << 24);
        }
    }
}

void canvas_fill_rounded(canvas_t *c, rect_t r, int32_t radius, color_t color)
{
    if (radius <= 0) {
        canvas_fill(c, r, color);
        return;
    }
    /* The middle band without corners is a plain fill. */
    int32_t rr = radius * 2 > r.h ? r.h / 2 : radius;
    if (rr * 2 > r.w)
        rr = r.w / 2;
    canvas_fill(c, rect_make(r.x, r.y + rr, r.w, r.h - 2 * rr), color);
    fill_rounded(c, r, radius, color, false, rect_make(r.x, r.y, r.w, rr));
    fill_rounded(c, r, radius, color, false, rect_make(r.x, r.y + r.h - rr, r.w, rr));
}

void canvas_outline(canvas_t *c, rect_t r, color_t color)
{
    canvas_fill(c, rect_make(r.x, r.y, r.w, 1), color);
    canvas_fill(c, rect_make(r.x, r.y + r.h - 1, r.w, 1), color);
    canvas_fill(c, rect_make(r.x, r.y + 1, 1, r.h - 2), color);
    canvas_fill(c, rect_make(r.x + r.w - 1, r.y + 1, 1, r.h - 2), color);
}

void canvas_outline_rounded(canvas_t *c, rect_t r, int32_t radius, color_t color)
{
    if (radius <= 0)
        canvas_outline(c, r, color);
    else
        fill_rounded(c, r, radius, color, true, r);
}

void canvas_gradient(canvas_t *c, rect_t r, color_t top, color_t bottom)
{
    for (int32_t y = 0; y < r.h; y++) {
        uint8_t amount = (uint8_t)(r.h > 1 ? y * 255 / (r.h - 1) : 0);
        canvas_fill(c, rect_make(r.x, r.y + y, r.w, 1), color_mix(top, bottom, amount));
    }
}

void canvas_line(canvas_t *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, color_t color)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    for (;;) {
        canvas_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void canvas_blit(canvas_t *c, int32_t x, int32_t y, const canvas_t *source, rect_t from, bool blend)
{
    from = rect_intersect(from, rect_make(0, 0, source->width, source->height));
    rect_t to = rect_intersect(rect_make(x, y, from.w, from.h), c->clip);
    if (rect_empty(to))
        return;
    int32_t ox = from.x + (to.x - x), oy = from.y + (to.y - y);
    for (int32_t row = 0; row < to.h; row++) {
        uint32_t *d = &c->pixels[(to.y + row) * c->stride + to.x];
        const uint32_t *s = &source->pixels[(oy + row) * source->stride + ox];
        if (blend) {
            for (int32_t i = 0; i < to.w; i++)
                d[i] = color_blend(d[i], s[i]);
        } else {
            for (int32_t i = 0; i < to.w; i++)
                d[i] = s[i];
        }
    }
}

void canvas_mask(canvas_t *c, int32_t x, int32_t y, const uint8_t *bits, int32_t width, int32_t height,
                 color_t color)
{
    int32_t bytes_per_row = (width + 7) / 8;
    for (int32_t row = 0; row < height; row++) {
        for (int32_t col = 0; col < width; col++) {
            if (bits[row * bytes_per_row + col / 8] & (0x80 >> (col % 8)))
                canvas_pixel(c, x + col, y + row, color);
        }
    }
}

/* --- Text ---------------------------------------------------------------------- */

uint32_t utf8_next(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t c = *p++;
    int extra = 0;
    if (c >= 0xF0 && c < 0xF8) {
        c &= 0x07;
        extra = 3;
    } else if (c >= 0xE0) {
        c &= 0x0F;
        extra = 2;
    } else if (c >= 0xC0) {
        c &= 0x1F;
        extra = 1;
    } else if (c >= 0x80) {
        c = '?';
    }
    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) {
            c = '?';
            break;
        }
        c = c << 6 | (*p++ & 0x3F);
    }
    *text = (const char *)p;
    return c;
}

int utf8_encode(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | c >> 12);
    out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

int32_t text_length(const char *text, int32_t bytes)
{
    const char *end = text + bytes;
    int32_t n = 0;
    while (text < end && *text) {
        utf8_next(&text);
        n++;
    }
    return n;
}

int32_t text_width(const char *text, int32_t scale)
{
    int32_t n = 0;
    while (*text && *text != '\n') {
        utf8_next(&text);
        n++;
    }
    return n * TEXT_CELL_WIDTH * scale;
}

int32_t canvas_text(canvas_t *c, int32_t x, int32_t y, const char *text, color_t color, int32_t scale)
{
    int32_t start = x;
    if (scale < 1)
        scale = 1;
    while (*text && *text != '\n') {
        uint32_t code = utf8_next(&text);
        const uint8_t *glyph = font8x16[code < 256 ? code : '?'];
        /* Skip glyphs outside the clip quickly. */
        rect_t cell = rect_make(x, y, FONT_WIDTH * scale, FONT_HEIGHT * scale);
        if (!rect_empty(rect_intersect(cell, c->clip))) {
            for (int32_t gy = 0; gy < FONT_HEIGHT; gy++) {
                uint8_t bits = glyph[gy];
                for (int32_t gx = 0; bits && gx < FONT_WIDTH; gx++) {
                    if (bits & (0x80 >> gx)) {
                        if (scale == 1)
                            canvas_pixel(c, x + gx, y + gy, color);
                        else
                            canvas_fill(c, rect_make(x + gx * scale, y + gy * scale, scale, scale), color);
                    }
                }
            }
        }
        x += FONT_WIDTH * scale;
    }
    return x - start;
}
