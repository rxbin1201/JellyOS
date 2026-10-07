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

#define BUTTON_SIZE      14
#define BUTTON_GAP       8
#define MAXIMIZE_COLOR   RGB(0xA7, 0x8B, 0xFA)
#define MINIMIZE_COLOR   RGB(0x2D, 0xD4, 0xBF)
#define OUTLINE_COLOR    RGBA(0xA7, 0x8B, 0xFA, 0xD0)

enum { LAYER_NORMAL, LAYER_PANEL, LAYER_POPUP };

static int layer_of(const comp_window_t *w)
{
    if (w->flags & WM_WINDOW_POPUP)
        return LAYER_POPUP;
    if (w->flags & WM_WINDOW_PANEL)
        return LAYER_PANEL;
    return LAYER_NORMAL;
}

static bool decorated(const comp_window_t *w)
{
    return !(w->flags & (WM_WINDOW_UNDECORATED | WM_WINDOW_PANEL | WM_WINDOW_POPUP));
}

static bool resizable(const comp_window_t *w)
{
    return decorated(w) && (w->flags & WM_WINDOW_RESIZABLE);
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
    if (layer_of(w) == LAYER_PANEL)
        return r;
    return rect_make(r.x - SHADOW_SIZE, r.y - SHADOW_SIZE / 2, r.w + 2 * SHADOW_SIZE, r.h + SHADOW_SIZE * 3 / 2);
}

/* Title bar buttons from the right: close, maximize (resizable windows), minimize. */
static rect_t button_rect(const comp_window_t *w, window_part_t part)
{
    rect_t frame = compositor_frame_rect(w);
    int index = part == PART_CLOSE ? 0 : part == PART_MAXIMIZE ? 1 : resizable(w) ? 2 : 1;
    return rect_make(frame.x + frame.w - 12 - (index + 1) * BUTTON_SIZE - index * BUTTON_GAP,
                     frame.y + (TITLE_BAR_HEIGHT - BUTTON_SIZE) / 2, BUTTON_SIZE, BUTTON_SIZE);
}

static rect_t grip_rect(const comp_window_t *w)
{
    rect_t frame = compositor_frame_rect(w);
    return rect_make(frame.x + frame.w - RESIZE_GRIP, frame.y + frame.h - RESIZE_GRIP, RESIZE_GRIP, RESIZE_GRIP);
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

rect_t compositor_work_area(compositor_t *c)
{
    int32_t width = c->display->back.width, height = c->display->back.height;
    int32_t top = 0, bottom = height;
    for (int i = 0; i < c->count; i++) {
        comp_window_t *w = c->windows[i];
        if (!(w->flags & WM_WINDOW_PANEL) || !(w->flags & WM_WINDOW_RESERVE))
            continue;
        if (w->y + w->content.height / 2 > height / 2) {
            if (w->y < bottom)
                bottom = w->y;
        } else if (w->y + w->content.height > top) {
            top = w->y + w->content.height;
        }
    }
    return rect_make(0, top, width, bottom - top);
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
    if (w->minimized)
        return;
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
    if (c->hardware_pointer) { /* nothing to repaint: the display moves its pointer plane */
        c->pointer_x = x;
        c->pointer_y = y;
        return;
    }
    compositor_damage(c, pointer_rect(c));
    c->pointer_x = x;
    c->pointer_y = y;
    compositor_damage(c, pointer_rect(c));
}

static void damage_outline(compositor_t *c, rect_t r)
{
    if (rect_empty(r))
        return;
    compositor_damage(c, rect_make(r.x - 1, r.y - 1, r.w + 2, 3));
    compositor_damage(c, rect_make(r.x - 1, r.y + r.h - 2, r.w + 2, 3));
    compositor_damage(c, rect_make(r.x - 1, r.y - 1, 3, r.h + 2));
    compositor_damage(c, rect_make(r.x + r.w - 2, r.y - 1, 3, r.h + 2));
}

void compositor_set_outline(compositor_t *c, rect_t outline)
{
    damage_outline(c, c->outline);
    c->outline = outline;
    damage_outline(c, outline);
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

comp_window_t *compositor_add(compositor_t *c, canvas_t content, const char *title, uint32_t flags, int32_t x,
                              int32_t y, void *owner)
{
    static comp_window_t pool[COMPOSITOR_MAX_WINDOWS];
    static int cascade;
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

    if (flags & WM_WINDOW_POSITIONED) {
        w->x = x;
        w->y = y;
    } else {
        /* Cascade from the top-left of the work area, wrapping before leaving it. */
        rect_t area = compositor_work_area(c);
        int32_t step = 32 * (cascade++ % 8);
        w->x = area.x + 60 + step;
        w->y = area.y + 50 + TITLE_BAR_HEIGHT + step;
        if (w->x + content.width > area.x + area.w)
            w->x = area.x + (area.w - content.width) / 2;
        if (w->y + content.height > area.y + area.h)
            w->y = area.y + TITLE_BAR_HEIGHT;
        if (w->x < BORDER_WIDTH)
            w->x = BORDER_WIDTH;
    }

    /* Insert at the top of its layer. */
    int position = c->count;
    while (position > 0 && layer_of(c->windows[position - 1]) > layer_of(w))
        position--;
    memmove(&c->windows[position + 1], &c->windows[position], (size_t)(c->count - position) * sizeof(c->windows[0]));
    c->windows[position] = w;
    c->count++;
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
    if (i < 0)
        return;
    int top = i;
    while (top + 1 < c->count && layer_of(c->windows[top + 1]) == layer_of(w))
        top++;
    if (top == i)
        return;
    memmove(&c->windows[i], &c->windows[i + 1], (size_t)(top - i) * sizeof(c->windows[0]));
    c->windows[top] = w;
    compositor_damage(c, paint_rect(w));
}

void compositor_move(compositor_t *c, comp_window_t *w, int32_t x, int32_t y)
{
    /* Keep the title bar reachable. */
    int32_t screen_w = c->display->back.width, screen_h = c->display->back.height;
    if (decorated(w)) {
        if (y < TITLE_BAR_HEIGHT)
            y = TITLE_BAR_HEIGHT;
        if (y > screen_h - 8)
            y = screen_h - 8;
        if (x + w->content.width < 40)
            x = 40 - w->content.width;
        if (x > screen_w - 40)
            x = screen_w - 40;
    }
    compositor_damage(c, paint_rect(w));
    w->x = x;
    w->y = y;
    compositor_damage(c, paint_rect(w));
}

void compositor_set_content(compositor_t *c, comp_window_t *w, canvas_t content)
{
    compositor_damage(c, paint_rect(w));
    w->content = content;
    compositor_damage(c, paint_rect(w));
}

void compositor_set_minimized(compositor_t *c, comp_window_t *w, bool minimized)
{
    if (w->minimized == minimized)
        return;
    compositor_damage(c, paint_rect(w));
    w->minimized = minimized;
    if (minimized && w->focused)
        w->focused = false;
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

comp_window_t *compositor_topmost(compositor_t *c)
{
    for (int i = c->count - 1; i >= 0; i--) {
        comp_window_t *w = c->windows[i];
        if (layer_of(w) == LAYER_NORMAL && !w->minimized)
            return w;
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

void compositor_set_hover(compositor_t *c, comp_window_t *w, window_part_t part)
{
    if (part != PART_CLOSE && part != PART_MAXIMIZE && part != PART_MINIMIZE)
        part = PART_NONE;
    if (w->hover != part) {
        rect_t frame = compositor_frame_rect(w);
        w->hover = part;
        compositor_damage(c, rect_make(frame.x, frame.y, frame.w, TITLE_BAR_HEIGHT));
    }
}

comp_window_t *compositor_hit(compositor_t *c, int32_t x, int32_t y, window_part_t *part)
{
    for (int i = c->count - 1; i >= 0; i--) {
        comp_window_t *w = c->windows[i];
        if (w->minimized || !rect_contains(compositor_frame_rect(w), x, y))
            continue;
        if (resizable(w) && !w->maximized && rect_contains(grip_rect(w), x, y))
            *part = PART_RESIZE;
        else if (rect_contains(compositor_content_rect(w), x, y))
            *part = PART_CONTENT;
        else if (decorated(w) && rect_contains(rect_inset(button_rect(w, PART_CLOSE), -3), x, y))
            *part = PART_CLOSE;
        else if (resizable(w) && rect_contains(rect_inset(button_rect(w, PART_MAXIMIZE), -3), x, y))
            *part = PART_MAXIMIZE;
        else if (decorated(w) && rect_contains(rect_inset(button_rect(w, PART_MINIMIZE), -3), x, y))
            *part = PART_MINIMIZE;
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
    rect_t area = compositor_work_area(c);
    const char *brand = "JellyOS";
    int32_t scale = 3;
    canvas_text(canvas, area.x + area.w - text_width(brand, scale) - 32,
                area.y + area.h - TEXT_CELL_HEIGHT * scale - 40, brand, DESKTOP_TEXT, scale);
    if (c->status_text)
        canvas_text(canvas, area.x + area.w - text_width(c->status_text, 1) - 32, area.y + area.h - 32,
                    c->status_text, DESKTOP_TEXT, 1);
}

static void paint_shadow(canvas_t *canvas, rect_t frame)
{
    /* Layered translucent rounded rectangles, darker towards the window. */
    for (int i = SHADOW_SIZE; i > 0; i -= 2) {
        rect_t r = rect_make(frame.x - i / 2, frame.y - i / 4, frame.w + i, frame.h + i);
        canvas_fill_rounded(canvas, r, CORNER_RADIUS + i / 2, RGBA(0, 0, 0, 10));
    }
}

static void paint_button(canvas_t *canvas, const comp_window_t *w, window_part_t part, color_t color)
{
    rect_t r = button_rect(w, part);
    bool hover = w->hover == part;
    canvas_fill_rounded(canvas, r, BUTTON_SIZE / 2, hover ? color_mix(color, RGB(255, 255, 255), 60) : color);
    if (!hover)
        return;
    color_t mark = RGB(0x30, 0x10, 0x30);
    int32_t cx = r.x + BUTTON_SIZE / 2, cy = r.y + BUTTON_SIZE / 2;
    if (part == PART_CLOSE) {
        canvas_line(canvas, cx - 3, cy - 3, cx + 3, cy + 3, mark);
        canvas_line(canvas, cx + 3, cy - 3, cx - 3, cy + 3, mark);
    } else if (part == PART_MAXIMIZE) {
        canvas_outline(canvas, rect_make(cx - 3, cy - 3, 7, 7), mark);
    } else {
        canvas_fill(canvas, rect_make(cx - 3, cy, 7, 1), mark);
    }
}

static void paint_window(canvas_t *canvas, comp_window_t *w)
{
    rect_t frame = compositor_frame_rect(w);

    if (layer_of(w) == LAYER_POPUP)
        paint_shadow(canvas, frame);
    if (decorated(w)) {
        if (!w->maximized)
            paint_shadow(canvas, frame);
        color_t title_color = w->focused ? TITLE_FOCUSED : TITLE_UNFOCUSED;
        /* Title bar with rounded top corners: a rounded rectangle cut by the content below. */
        canvas_fill_rounded(canvas, rect_make(frame.x, frame.y, frame.w, TITLE_BAR_HEIGHT + CORNER_RADIUS),
                            w->maximized ? 0 : CORNER_RADIUS, title_color);
        color_t border = w->focused ? BORDER_FOCUSED : BORDER_UNFOCUSED;
        canvas_fill(canvas, rect_make(frame.x, w->y, BORDER_WIDTH, w->content.height + BORDER_WIDTH), border);
        canvas_fill(canvas, rect_make(frame.x + frame.w - BORDER_WIDTH, w->y, BORDER_WIDTH,
                                      w->content.height + BORDER_WIDTH),
                    border);
        canvas_fill(canvas, rect_make(frame.x, w->y + w->content.height, frame.w, BORDER_WIDTH), border);

        /* Title, clipped so it never runs under the buttons */
        rect_t leftmost = button_rect(w, PART_MINIMIZE);
        rect_t previous = canvas_clip(canvas, rect_make(frame.x, frame.y, leftmost.x - frame.x - 8, TITLE_BAR_HEIGHT));
        canvas_text(canvas, frame.x + 14, frame.y + (TITLE_BAR_HEIGHT - TEXT_CELL_HEIGHT) / 2, w->title,
                    w->focused ? TITLE_TEXT : TITLE_TEXT_DIM, 1);
        canvas->clip = previous;

        paint_button(canvas, w, PART_CLOSE, CLOSE_COLOR);
        if (resizable(w))
            paint_button(canvas, w, PART_MAXIMIZE, MAXIMIZE_COLOR);
        paint_button(canvas, w, PART_MINIMIZE, MINIMIZE_COLOR);
    }
    canvas_blit(canvas, w->x, w->y, &w->content, rect_make(0, 0, w->content.width, w->content.height), false);

    if (resizable(w) && !w->maximized) {
        /* grip: three short diagonal lines in the corner */
        rect_t g = grip_rect(w);
        for (int i = 0; i < 3; i++) {
            int32_t d = 4 + i * 4;
            canvas_line(canvas, g.x + g.w - 2 - d, g.y + g.h - 2, g.x + g.w - 2, g.y + g.h - 2 - d,
                        RGBA(0x80, 0x80, 0x90, 0xA0));
        }
    }
}

void compositor_pointer_image(uint32_t *pixels, int32_t size)
{
    memset(pixels, 0, (size_t)size * (size_t)size * sizeof(uint32_t)); /* transparent */
    for (int32_t y = 0; y < POINTER_HEIGHT && y < size; y++) {
        for (int32_t x = 0; x < POINTER_WIDTH && x < size; x++) {
            uint8_t bit = (uint8_t)(0x80 >> (x % 8));
            if (pointer_outline[y * 2 + x / 8] & bit)
                pixels[y * size + x] = 0xFF000000;
            else if (pointer_fill[y * 2 + x / 8] & bit)
                pixels[y * size + x] = 0xFFFFFFFF;
        }
    }
}

static void paint_pointer(compositor_t *c, canvas_t *canvas)
{
    if (!c->pointer_visible || c->hardware_pointer)
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
        for (int layer = LAYER_NORMAL; layer <= LAYER_POPUP; layer++) {
            for (int i = 0; i < c->count; i++) {
                comp_window_t *w = c->windows[i];
                if (layer_of(w) == layer && !w->minimized && !rect_empty(rect_intersect(paint_rect(w), area)))
                    paint_window(canvas, w);
            }
        }
        if (!rect_empty(c->outline)) {
            rect_t o = c->outline;
            canvas_outline(canvas, o, OUTLINE_COLOR);
            canvas_outline(canvas, rect_inset(o, 1), OUTLINE_COLOR);
        }
        paint_pointer(c, canvas);
        display_present(c->display, area);
    }
    canvas_set_clip(canvas, rect_make(0, 0, canvas->width, canvas->height));
    c->damage_count = 0;
    display_commit(c->display); /* the whole frame at once; with a graphics driver in step with the screen */
    return true;
}
