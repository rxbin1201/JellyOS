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
 *
 * A kernel panic does not wait for that: its text goes into the shadow grid
 * like everything else, and then the grid is drawn, whoever owns the screen
 * (fb_console_panic_prepare() and fb_console_panic_show()).
 */

#include "drivers/graphics/display.h"
#include "graphics/core/font.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

static display_t *display;
static uint8_t *cells;        /* rows * columns Latin-1 characters */
static uint32_t capacity;     /* characters `cells` has room for */
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

void fb_console_resize(display_t *d)
{
    if (display != d)
        return; /* the console is off (fbconsole=0) or on another display */
    uint32_t new_columns = d->info.width / FONT_WIDTH, new_rows = d->info.height / FONT_HEIGHT;
    uint8_t *new_cells = kmalloc(new_columns * new_rows), *old;

    if (!new_cells || !new_columns || !new_rows) {
        kfree(new_cells);
        return; /* the console stays off the screen: its grid does not fit the new size */
    }
    memset(new_cells, ' ', new_columns * new_rows);
    uint64_t flags = arch_interrupts_save();
    old = cells;
    cells = new_cells;
    capacity = new_columns * new_rows;
    columns = new_columns;
    rows = new_rows;
    cursor_x = cursor_y = 0;
    active = true;
    redraw();
    arch_interrupts_restore(flags);
    kfree(old);
    klog_replay(console_write); /* fill the new screen with what was logged so far */
}

/*
 * A panic is about to be written. If the mode changed while a display server had the screen, the grid is
 * still laid out for the old size: it is laid out again for the screen as it is, in the memory that is there
 * (a panic allocates nothing; a grid that would need more gets fewer rows), and filled from the log. Nothing
 * is drawn.
 */
void fb_console_panic_prepare(void)
{
    if (!display || !cells)
        return;
    uint32_t new_columns = display->info.width / FONT_WIDTH, new_rows = display->info.height / FONT_HEIGHT;

    if (new_columns == columns && new_rows == rows)
        return;
    if (new_columns && new_rows > capacity / new_columns)
        new_rows = capacity / new_columns;
    if (!new_columns || !new_rows)
        return; /* (the grid stays as it is; fb_console_panic_show() does not draw one that does not fit) */
    uint64_t flags = arch_interrupts_save();
    bool was_active = active;
    active = false;
    columns = new_columns;
    rows = new_rows;
    cursor_x = cursor_y = 0;
    escape_state = PLAIN;
    utf8_remaining = 0;
    memset(cells, ' ', columns * rows);
    klog_replay(console_write);
    active = was_active;
    arch_interrupts_restore(flags);
}

/* The panic's text is in the grid: onto the screen with it. */
void fb_console_panic_show(void)
{
    if (!display || !cells || columns > display->info.width / FONT_WIDTH || rows > display->info.height / FONT_HEIGHT)
        return;
    uint64_t flags = arch_interrupts_save();
    active = true;
    redraw();
    arch_interrupts_restore(flags);
}

void fb_console_init(display_t *d)
{
    columns = d->info.width / FONT_WIDTH;
    rows = d->info.height / FONT_HEIGHT;
    cells = kmalloc(columns * rows);
    capacity = columns * rows;
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
