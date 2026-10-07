/*
 * Early screen console: kernel messages on the boot framebuffer from the
 * first line on.
 *
 * On a real PC there is usually no serial port to read, and the regular
 * framebuffer console (fb_console.c) needs the heap and the kernel's own
 * mappings. Until it takes over, this console draws the log directly into
 * the framebuffer through the boot loader's direct map, without allocating
 * anything: a failure during CPU or memory initialization is visible
 * instead of a black screen. vmm_init() keeps the framebuffer in the direct
 * map so the pointer survives the switch to the kernel's page tables.
 *
 * No scrolling: at the bottom the output continues at the top over a
 * cleared screen.
 */

#include "drivers/graphics/display.h"
#include "graphics/core/font.h"

#include "core/boot.h"
#include "core/log.h"

static volatile uint32_t *pixels;
static uint32_t stride, width, height, columns, rows;
static uint32_t cursor_x, cursor_y;
static uint32_t foreground, background;
static bool in_escape;

static uint32_t color(const boot_framebuffer_t *fb, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)(r >> (8 - fb->red_size)) << fb->red_shift) |
           ((uint32_t)(g >> (8 - fb->green_size)) << fb->green_shift) |
           ((uint32_t)(b >> (8 - fb->blue_size)) << fb->blue_shift);
}

static void clear(void)
{
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++)
            pixels[(uint64_t)y * stride + x] = background;
    }
    cursor_x = cursor_y = 0;
}

static void new_line(void)
{
    cursor_x = 0;
    if (++cursor_y == rows)
        clear();
}

static void put(uint8_t c)
{
    /* ANSI escape sequences are skipped: ESC [ parameters final-byte */
    if (in_escape) {
        if (c >= 0x40 && c <= 0x7E && c != '[')
            in_escape = false;
        return;
    }
    if (c == 0x1B) {
        in_escape = true;
    } else if (c == '\n') {
        new_line();
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c >= 0x20) {
        if (cursor_x == columns)
            new_line();
        const uint8_t *glyph = font8x16[c];
        volatile uint32_t *row = pixels + (uint64_t)cursor_y * FONT_HEIGHT * stride + cursor_x * FONT_WIDTH;
        for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++, row += stride) {
            for (uint32_t gx = 0; gx < FONT_WIDTH; gx++)
                row[gx] = (glyph[gy] & (0x80 >> gx)) ? foreground : background;
        }
        cursor_x++;
    }
}

static void early_fb_write(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++)
        put((uint8_t)text[i]);
}

void early_fb_init(const boot_info_t *info)
{
    const boot_framebuffer_t *fb = &info->framebuffer;

    if (!fb->phys_base || fb->bpp != 32 || fb->width < FONT_WIDTH || fb->height < FONT_HEIGHT || !fb->red_size ||
        fb->red_size > 8 || !fb->green_size || fb->green_size > 8 || !fb->blue_size || fb->blue_size > 8)
        return;
    pixels = (volatile uint32_t *)(uintptr_t)(info->hhdm_base + fb->phys_base);
    stride = fb->pitch / 4;
    width = fb->width;
    height = fb->height;
    columns = width / FONT_WIDTH;
    rows = height / FONT_HEIGHT;
    foreground = color(fb, 0xD8, 0xDE, 0xE9);
    background = color(fb, 0x14, 0x1A, 0x26);
    clear();
    kconsole_set_mirror(early_fb_write);
}
