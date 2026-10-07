/*
 * Compositor: decorations, damage tracking and composition.
 */

#include "graphics/compositor/compositor.h"

#include <string.h>

/* JellyOS palette */
#define DESKTOP_TOP      RGB(0x2A, 0x1F, 0x5C)
#define DESKTOP_BOTTOM   RGB(0x0E, 0x14, 0x2B)
#define DESKTOP_TEXT     RGBA(0xFF, 0xFF, 0xFF, 0x60)
#define TITLE_FOCUSED    RGB(0x7C, 0x3A, 0xED)
#define TITLE_UNFOCUSED  RGB(0x3B, 0x3B, 0x4F)
#define TITLE_TEXT       RGB(0xFF, 0xFF, 0xFF)
#define TITLE_TEXT_DIM   RGB(0xB8, 0xB8, 0xC8)
#define BORDER_FOCUSED   RGB(0x9F, 0x6B, 0xFF)
#define BORDER_UNFOCUSED RGB(0x55, 0x55, 0x6A)
#define CLOSE_COLOR      RGB(0xEF, 0x44, 0x6C)
#define CLOSE_HOVER      RGB(0xFF, 0x6B, 0x8A)
#define CLOSE_SIZE       16

/* Arrow pointer, 12x19: outline (X) and fill (.) masks, generated from this picture:
 *   X
 *   XX
 *   X.X
 *   X..X
 *   X...X
 *   X....X
 *   X.....X
 *   X......X
 *   X.......X
 *   X........X
 *   X.........X
 *   X......XXXXX
 *   X...X..X
 *   X..XX..X
 *   X.X  X..X
 *   XX   X..X
 *   X     X..X
 *         X..X
 *          XX
 */
static const uint8_t pointer_outline[19 * 2] = {
    0x80, 0x00, 0xC0, 0x00, 0xA0, 0x00, 0x90, 0x00, 0x88, 0x00, 0x84, 0x00, 0x82, 0x00, 0x81, 0x00,
    0x80, 0x80, 0x80, 0x40, 0x80, 0x20, 0x81, 0xF0, 0x89, 0x00, 0x99, 0x00, 0xA4, 0x80, 0xC4, 0x80,
    0x82, 0x40, 0x02, 0x40, 0x01, 0x80,
};
static const uint8_t pointer_fill[19 * 2] = {
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x60, 0x00, 0x70, 0x00, 0x78, 0x00, 0x7C, 0x00, 0x7E, 0x00,
    0x7F, 0x00, 0x7F, 0x80, 0x7F, 0xC0, 0x7E, 0x00, 0x76, 0x00, 0x66, 0x00, 0x43, 0x00, 0x03, 0x00,
    0x01, 0x80, 0x01, 0x80, 0x00, 0x00,
};
#define POINTER_WIDTH  12
#define POINTER_HEIGHT 19

static bool decorated(const comp_window_t *w)
{
    return !(w->flags & WM_WINDOW_UNDECORATED);
}

rect_t compositor_content_rect(const comp_window_t *w)
{
    return rect_make(w->x, w->y, w->content.width, w->content.height);
}

rect_t compositor_frame_rect(const comp_window_t *w)
{
    if (!decorated(w))
        return compositor_content_rect(w);
    return rect_make(w->x - BORDER_WIDTH, w->y - TITLE_BAR_HEIGHT, w->content.width + 2 * BORDER_WIDTH,
                     w->content.height + TITLE_BAR_HEIGHT + BORDER_WIDTH);
}

/* Everything a window paints, shadow included. */
static rect_t paint_rect(const comp_window_t *w)
{
    rect_t r = compositor_frame_rect(w);
    return rect_make(r.x - SHADOW_SIZE, r.y - SHADOW_SIZE / 2, r.w + 2 * SHADOW_SIZE, r.h + SHADOW_SIZE * 3 / 2);
}

static rect_t close_rect(const comp_window_t *w)
{
    rect_t frame = compositor_frame_rect(w);
    return rect_make(frame.x + frame.w - CLOSE_SIZE - 10, frame.y + (TITLE_BAR_HEIGHT - CLOSE_SIZE) / 2, CLOSE_SIZE,
                     CLOSE_SIZE);
}

void compositor_init(compositor_t *c, display_t *display)
{
    memset(c, 0, sizeof(*c));
    c->display = display;
    c->pointer_x = display->back.width / 2;
    c->pointer_y = display->back.height / 2;
    c->pointer_visible = true;
    c->next_id = 1;
    compositor_damage(c, rect_make(0, 0, display->back.width, display->back.height));
}

/* --- Damage ------------------------------------------------------------------------ */

void compositor_damage(compositor_t *c, rect_t area)
{
    area = rect_intersect(area, rect_make(0, 0, c->display->back.width, c->display->back.height));
    if (rect_empty(area))
        return;
    /* Merge with an overlapping rectangle, else append; when full, collapse into one. */
    for (int i = 0; i < c->damage_count; i++) {
        if (!rect_empty(rect_intersect(c->damage[i], area))) {
            c->damage[i] = rect_union(c->damage[i], area);
            return;
        }
    }
    if (c->damage_count == COMPOSITOR_MAX_DAMAGE) {
        for (int i = 1; i < c->damage_count; i++)
            c->damage[0] = rect_union(c->damage[0], c->damage[i]);
        c->damage_count = 1;
        c->damage[0] = rect_union(c->damage[0], area);
        return;
    }
    c->damage[c->damage_count++] = area;
}

void compositor_damage_content(compositor_t *c, comp_window_t *w, rect_t area)
{
    area = rect_intersect(area, rect_make(0, 0, w->content.width, w->content.height));
    area.x += w->x;
    area.y += w->y;
    compositor_damage(c, area);
}

static rect_t pointer_rect(const compositor_t *c)
{
    return rect_make(c->pointer_x, c->pointer_y, POINTER_WIDTH, POINTER_HEIGHT);
}

void compositor_move_pointer(compositor_t *c, int32_t x, int32_t y)
{
    if (x == c->pointer_x && y == c->pointer_y)
        return;
    compositor_damage(c, pointer_rect(c));
    c->pointer_x = x;
    c->pointer_y = y;
    compositor_damage(c, pointer_rect(c));
}

/* --- Window list ---------------------------------------------------------------- */

static int index_of(compositor_t *c, comp_window_t *w)
{
    for (int i = 0; i < c->count; i++) {
        if (c->windows[i] == w)
            return i;
    }
    return -1;
}

comp_window_t *compositor_add(compositor_t *c, canvas_t content, const char *title, uint32_t flags, void *owner)
{
    static comp_window_t pool[COMPOSITOR_MAX_WINDOWS];
    comp_window_t *w = NULL;

    if (c->count == COMPOSITOR_MAX_WINDOWS)
        return NULL;
    for (int i = 0; i < COMPOSITOR_MAX_WINDOWS && !w; i++) {
        if (pool[i].id == 0)
            w = &pool[i];
    }
    if (!w)
        return NULL;
    memset(w, 0, sizeof(*w));
    w->id = c->next_id++;
    w->owner = owner;
    w->content = content;
    w->flags = flags;
    strncpy(w->title, title, WM_TITLE_MAX - 1);

    /* Cascade from the top-left, wrapping before leaving the screen. */
    static int cascade;
    int32_t step = 32 * (cascade++ % 8);
    w->x = 60 + step;
    w->y = 50 + TITLE_BAR_HEIGHT + step;
    if (w->x + content.width > c->display->back.width)
        w->x = (c->display->back.width - content.width) / 2;
    if (w->y + content.height > c->display->back.height)
        w->y = TITLE_BAR_HEIGHT;
    if (w->x < BORDER_WIDTH)
        w->x = BORDER_WIDTH;

    c->windows[c->count++] = w;
    compositor_damage(c, paint_rect(w));
    return w;
}

void compositor_remove(compositor_t *c, comp_window_t *w)
{
    int i = index_of(c, w);
    if (i < 0)
        return;
    compositor_damage(c, paint_rect(w));
    memmove(&c->windows[i], &c->windows[i + 1], (size_t)(c->count - i - 1) * sizeof(c->windows[0]));
    c->count--;
    w->id = 0;
}

void compositor_raise(compositor_t *c, comp_window_t *w)
{
    int i = index_of(c, w);
    if (i < 0 || i == c->count - 1)
        return;
    memmove(&c->windows[i], &c->windows[i + 1], (size_t)(c->count - i - 1) * sizeof(c->windows[0]));
    c->windows[c->count - 1] = w;
    compositor_damage(c, paint_rect(w));
}

void compositor_move(compositor_t *c, comp_window_t *w, int32_t x, int32_t y)
{
    /* Keep the title bar reachable. */
    int32_t screen_w = c->display->back.width, screen_h = c->display->back.height;
    if (y < (decorated(w) ? TITLE_BAR_HEIGHT : 0))
        y = decorated(w) ? TITLE_BAR_HEIGHT : 0;
    if (y > screen_h - 8)
        y = screen_h - 8;
    if (x + w->content.width < 40)
        x = 40 - w->content.width;
    if (x > screen_w - 40)
        x = screen_w - 40;
    compositor_damage(c, paint_rect(w));
    w->x = x;
    w->y = y;
    compositor_damage(c, paint_rect(w));
}

void compositor_focus(compositor_t *c, comp_window_t *w)
{
    for (int i = 0; i < c->count; i++) {
        bool focused = c->windows[i] == w;
        if (c->windows[i]->focused != focused) {
            c->windows[i]->focused = focused;
            compositor_damage(c, paint_rect(c->windows[i]));
        }
    }
}

comp_window_t *compositor_focused(compositor_t *c)
{
    for (int i = 0; i < c->count; i++) {
        if (c->windows[i]->focused)
            return c->windows[i];
    }
    return NULL;
}

void compositor_set_title(compositor_t *c, comp_window_t *w, const char *title)
{
    strncpy(w->title, title, WM_TITLE_MAX - 1);
    w->title[WM_TITLE_MAX - 1] = '\0';
    rect_t frame = compositor_frame_rect(w);
    compositor_damage(c, rect_make(frame.x, frame.y, frame.w, TITLE_BAR_HEIGHT));
}

void compositor_set_close_hover(compositor_t *c, comp_window_t *w, bool hover)
{
    if (w->close_hover != hover) {
        w->close_hover = hover;
        compositor_damage(c, close_rect(w));
    }
}

comp_window_t *compositor_hit(compositor_t *c, int32_t x, int32_t y, window_part_t *part)
{
    for (int i = c->count - 1; i >= 0; i--) {
        comp_window_t *w = c->windows[i];
        if (!rect_contains(compositor_frame_rect(w), x, y))
            continue;
        if (rect_contains(compositor_content_rect(w), x, y))
            *part = PART_CONTENT;
        else if (decorated(w) && rect_contains(rect_inset(close_rect(w), -3), x, y))
            *part = PART_CLOSE;
        else if (decorated(w) && y < w->y)
            *part = PART_TITLE;
        else
            *part = PART_BORDER;
        return w;
    }
    *part = PART_NONE;
    return NULL;
}

/* --- Painting ------------------------------------------------------------------- */

static void paint_background(compositor_t *c, canvas_t *canvas)
{
    rect_t screen = rect_make(0, 0, canvas->width, canvas->height);
    canvas_gradient(canvas, screen, DESKTOP_TOP, DESKTOP_BOTTOM);
    const char *brand = "JellyOS";
    int32_t scale = 3;
    canvas_text(canvas, canvas->width - text_width(brand, scale) - 32,
                canvas->height - TEXT_CELL_HEIGHT * scale - 40, brand, DESKTOP_TEXT, scale);
    if (c->status_text)
        canvas_text(canvas, canvas->width - text_width(c->status_text, 1) - 32, canvas->height - 32, c->status_text,
                    DESKTOP_TEXT, 1);
}

static void paint_shadow(canvas_t *canvas, rect_t frame)
{
    /* Layered translucent rounded rectangles, darker towards the window. */
    for (int i = SHADOW_SIZE; i > 0; i -= 2) {
        rect_t r = rect_make(frame.x - i / 2, frame.y - i / 4, frame.w + i, frame.h + i);
        canvas_fill_rounded(canvas, r, CORNER_RADIUS + i / 2, RGBA(0, 0, 0, 10));
    }
}

static void paint_window(compositor_t *c, canvas_t *canvas, comp_window_t *w)
{
    (void)c;
    rect_t frame = compositor_frame_rect(w);

    if (decorated(w)) {
        paint_shadow(canvas, frame);
        color_t title_color = w->focused ? TITLE_FOCUSED : TITLE_UNFOCUSED;
        /* Title bar with rounded top corners: a rounded rectangle cut by the content below. */
        canvas_fill_rounded(canvas, rect_make(frame.x, frame.y, frame.w, TITLE_BAR_HEIGHT + CORNER_RADIUS),
                            CORNER_RADIUS, title_color);
        /* Border around the content */
        color_t border = w->focused ? BORDER_FOCUSED : BORDER_UNFOCUSED;
        canvas_fill(canvas, rect_make(frame.x, w->y, BORDER_WIDTH, w->content.height + BORDER_WIDTH), border);
        canvas_fill(canvas, rect_make(frame.x + frame.w - BORDER_WIDTH, w->y, BORDER_WIDTH,
                                      w->content.height + BORDER_WIDTH),
                    border);
        canvas_fill(canvas, rect_make(frame.x, w->y + w->content.height, frame.w, BORDER_WIDTH), border);

        /* Title, clipped so it never runs under the close button */
        rect_t close = close_rect(w);
        rect_t previous = canvas_clip(canvas, rect_make(frame.x, frame.y, close.x - frame.x - 8, TITLE_BAR_HEIGHT));
        canvas_text(canvas, frame.x + 14, frame.y + (TITLE_BAR_HEIGHT - TEXT_CELL_HEIGHT) / 2, w->title,
                    w->focused ? TITLE_TEXT : TITLE_TEXT_DIM, 1);
        canvas->clip = previous;

        /* Close button: a jelly dot with an x on hover */
        canvas_fill_rounded(canvas, close, CLOSE_SIZE / 2, w->close_hover ? CLOSE_HOVER : CLOSE_COLOR);
        if (w->close_hover) {
            canvas_line(canvas, close.x + 5, close.y + 5, close.x + 10, close.y + 10, RGB(0x40, 0x10, 0x20));
            canvas_line(canvas, close.x + 10, close.y + 5, close.x + 5, close.y + 10, RGB(0x40, 0x10, 0x20));
        }
    }
    canvas_blit(canvas, w->x, w->y, &w->content, rect_make(0, 0, w->content.width, w->content.height), false);
}

static void paint_pointer(compositor_t *c, canvas_t *canvas)
{
    if (!c->pointer_visible)
        return;
    canvas_mask(canvas, c->pointer_x, c->pointer_y, pointer_fill, POINTER_WIDTH, POINTER_HEIGHT, RGB(255, 255, 255));
    canvas_mask(canvas, c->pointer_x, c->pointer_y, pointer_outline, POINTER_WIDTH, POINTER_HEIGHT, RGB(0, 0, 0));
}

bool compositor_render(compositor_t *c)
{
    canvas_t *canvas = &c->display->back;
    if (c->damage_count == 0)
        return false;

    for (int d = 0; d < c->damage_count; d++) {
        rect_t area = c->damage[d];
        canvas_set_clip(canvas, area);
        paint_background(c, canvas);
        for (int i = 0; i < c->count; i++) {
            if (!rect_empty(rect_intersect(paint_rect(c->windows[i]), area)))
                paint_window(c, canvas, c->windows[i]);
        }
        paint_pointer(c, canvas);
        display_present(c->display, area);
    }
    canvas_set_clip(canvas, rect_make(0, 0, canvas->width, canvas->height));
    c->damage_count = 0;
    return true;
}
