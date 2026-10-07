/*
 * Framebuffer console: the kernel log and /dev/console on the screen.
 *
 * A text grid of 8x16 cells (graphics/core/font8x16.c, Latin-1; UTF-8 is
 * decoded and anything beyond Latin-1 shows as '?'). The characters are kept
 * in a shadow grid, so scrolling redraws from it and never reads the
 * (write-combining) framebuffer. It scrolls a quarter screen at a time to
 * keep redraws rare. ANSI escape sequences are skipped.
 *
 * While a display server owns display 0 the console only updates the
 * shadow grid; when the display is released, the screen is repainted.
 */

#include "drivers/graphics/display.h"
#include "graphics/core/font.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

static display_t *display;
static uint8_t *cells;        /* rows * columns Latin-1 characters */
static uint32_t columns, rows;
static uint32_t cursor_x, cursor_y;
static bool active;
static uint32_t foreground, background;

static enum { PLAIN, ESCAPE, CSI } escape_state;
static uint32_t utf8_value, utf8_remaining;

static void draw_cell(uint32_t x, uint32_t y)
{
    if (!active)
        return;
    const uint8_t *glyph = font8x16[cells[y * columns + x]];
    uint32_t stride = display->info.pitch / 4;
    volatile uint32_t *row = display->pixels + (uint64_t)y * FONT_HEIGHT * stride + x * FONT_WIDTH;
    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++, row += stride) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < FONT_WIDTH; gx++)
            row[gx] = (bits & (0x80 >> gx)) ? foreground : background;
    }
}

static void redraw(void)
{
    if (!active)
        return;
    /* Clear the margins right of and below the text grid. */
    uint32_t stride = display->info.pitch / 4;
    for (uint32_t y = 0; y < display->info.height; y++) {
        uint32_t from = y < rows * FONT_HEIGHT ? columns * FONT_WIDTH : 0;
        for (uint32_t x = from; x < display->info.width; x++)
            display->pixels[(uint64_t)y * stride + x] = background;
    }
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < columns; x++)
            draw_cell(x, y);
    }
}

static void scroll(void)
{
    uint32_t lines = rows / 4 ? rows / 4 : 1;
    memmove(cells, cells + lines * columns, (rows - lines) * columns);
    memset(cells + (rows - lines) * columns, ' ', lines * columns);
    cursor_y -= lines;
    redraw();
}

static void new_line(void)
{
    cursor_x = 0;
    if (++cursor_y == rows)
        scroll();
}

static void put_cell(uint8_t c)
{
    if (cursor_x == columns)
        new_line();
    cells[cursor_y * columns + cursor_x] = c;
    draw_cell(cursor_x, cursor_y);
    cursor_x++;
}

static void put_byte(uint8_t c)
{
    /* ANSI escape sequences: ESC [ parameters final-byte */
    if (escape_state == ESCAPE) {
        escape_state = c == '[' ? CSI : PLAIN;
        return;
    }
    if (escape_state == CSI) {
        if (c >= 0x40 && c <= 0x7E)
            escape_state = PLAIN;
        return;
    }
    /* UTF-8 to Latin-1 */
    if (utf8_remaining) {
        if ((c & 0xC0) != 0x80) {
            utf8_remaining = 0;
        } else {
            utf8_value = utf8_value << 6 | (c & 0x3F);
            if (--utf8_remaining == 0)
                put_cell(utf8_value < 256 ? (uint8_t)utf8_value : '?');
            return;
        }
    }
    if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) {
            utf8_value = c & 0x1F;
            utf8_remaining = 1;
        } else if ((c & 0xF0) == 0xE0) {
            utf8_value = c & 0x0F;
            utf8_remaining = 2;
        } else if ((c & 0xF8) == 0xF0) {
            utf8_value = c & 0x07;
            utf8_remaining = 3;
        } else {
            put_cell('?');
        }
        return;
    }

    switch (c) {
    case '\n':
        new_line();
        break;
    case '\r':
        cursor_x = 0;
        break;
    case '\b':
        if (cursor_x)
            cursor_x--;
        break;
    case '\t':
        do
            put_cell(' ');
        while (cursor_x % 8 && cursor_x < columns);
        break;
    case 0x1B:
        escape_state = ESCAPE;
        break;
    default:
        if (c >= 0x20)
            put_cell(c);
        break;
    }
}

static void console_write(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++)
        put_byte((uint8_t)text[i]);
}

void fb_console_set_active(bool on)
{
    if (!display)
        return;
    uint64_t flags = arch_interrupts_save();
    active = on;
    if (on)
        redraw();
    arch_interrupts_restore(flags);
}

void fb_console_init(display_t *d)
{
    columns = d->info.width / FONT_WIDTH;
    rows = d->info.height / FONT_HEIGHT;
    cells = kmalloc(columns * rows);
    if (!cells || !columns || !rows)
        return;
    memset(cells, ' ', columns * rows);
    display = d;
    foreground = display_color(d, 0xD8, 0xDE, 0xE9);
    background = display_color(d, 0x14, 0x1A, 0x26);
    active = true;
    redraw();

    /* Show what was logged before the screen existed, then follow the console. */
    klog_replay(console_write);
    kconsole_set_mirror(console_write);
    klog_info("console: framebuffer console %ux%u characters", columns, rows);
}
