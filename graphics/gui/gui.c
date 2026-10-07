/*
 * JellyOS GUI toolkit (README section 36).
 *
 * One widget structure serves all control types. A window keeps its widget
 * tree, lays it out when its size or content changes and repaints the whole
 * window into its surface; WM_PRESENTED (WM_EVENT_FRAME) paces the frames.
 */

#include "graphics/gui/gui.h"

#include <stdlib.h>
#include <string.h>

#include <jelly/input.h>
#include <jelly/os.h>

#define INPUT_MAX     256
#define MAX_WATCHES   8

typedef enum { W_BOX, W_LABEL, W_BUTTON, W_INPUT, W_CHECKBOX, W_LIST, W_CUSTOM } widget_type_t;

struct widget {
    widget_type_t  type;
    gui_window_t  *window;
    widget_t      *parent;
    widget_t     **children;
    int            child_count, child_capacity;
    rect_t         rect;
    bool           expand;
    bool           focusable;

    /* box */
    bool           horizontal;
    int32_t        spacing, padding;

    /* label, button, checkbox, input placeholder */
    char          *text;
    bool           large, primary;

    /* button, checkbox, input, list */
    gui_callback_t callback;
    void          *user;
    bool           checked;

    /* input */
    char           value[INPUT_MAX];
    int32_t        cursor;      /* in code points */
    int32_t        scroll;      /* first visible code point */

    /* list */
    char         **items;
    int            item_count, item_capacity;
    int            selected, first_visible, rows;

    /* custom */
    gui_custom_t   custom;
};

typedef struct {
    jelly_handle_t handle;
    void         (*ready)(void *user);
    void          *user;
} watch_t;

struct gui_app {
    wm_connection_t *connection;
    gui_window_t    *windows;
    int              window_count;
    bool             quit;
    int              exit_code;
    watch_t          watches[MAX_WATCHES];
    int              watch_count;
};

struct gui_window {
    gui_app_t     *app;
    gui_window_t  *next;
    window_t      *surface;
    widget_t      *root;
    gui_theme_t    theme;
    widget_t      *focus, *hover, *pressed;
    bool           dirty, layout_dirty, active;
    gui_callback_t on_close;
    void          *on_close_user;
};

/* --- Themes ----------------------------------------------------------------------- */

gui_theme_t gui_theme_light(int32_t scale)
{
    gui_theme_t t = {
        .dark = false, .scale = scale < 1 ? 1 : scale,
        .background = RGB(0xF6, 0xF4, 0xFB), .surface = RGB(0xFF, 0xFF, 0xFF),
        .surface_hover = RGB(0xEE, 0xE9, 0xFA), .surface_pressed = RGB(0xDD, 0xD3, 0xF7),
        .text = RGB(0x1E, 0x1B, 0x2E), .text_dim = RGB(0x7A, 0x76, 0x8C),
        .accent = RGB(0x7C, 0x3A, 0xED), .accent_text = RGB(0xFF, 0xFF, 0xFF),
        .border = RGB(0xCF, 0xC9, 0xDE), .focus = RGB(0xA7, 0x8B, 0xFA), .selection = RGB(0xDD, 0xD3, 0xF7),
    };
    return t;
}

gui_theme_t gui_theme_dark(int32_t scale)
{
    gui_theme_t t = {
        .dark = true, .scale = scale < 1 ? 1 : scale,
        .background = RGB(0x1B, 0x1A, 0x26), .surface = RGB(0x29, 0x27, 0x38),
        .surface_hover = RGB(0x34, 0x31, 0x48), .surface_pressed = RGB(0x44, 0x3D, 0x63),
        .text = RGB(0xEC, 0xEA, 0xF4), .text_dim = RGB(0x9B, 0x97, 0xAE),
        .accent = RGB(0x8B, 0x5C, 0xF6), .accent_text = RGB(0xFF, 0xFF, 0xFF),
        .border = RGB(0x45, 0x41, 0x5C), .focus = RGB(0xA7, 0x8B, 0xFA), .selection = RGB(0x4C, 0x3A, 0x86),
    };
    return t;
}

/* --- Widget basics --------------------------------------------------------------- */

static widget_t *new_widget(widget_type_t type)
{
    widget_t *w = calloc(1, sizeof(*w));
    if (w) {
        w->type = type;
        w->selected = -1;
        w->rows = 6;
    }
    return w;
}

static char *copy_text(const char *text)
{
    size_t n = strlen(text ? text : "");
    char *copy = malloc(n + 1);
    if (copy)
        memcpy(copy, text ? text : "", n + 1);
    return copy;
}

static void set_window(widget_t *w, gui_window_t *window)
{
    w->window = window;
    for (int i = 0; i < w->child_count; i++)
        set_window(w->children[i], window);
}

static void changed(widget_t *w, bool layout)
{
    if (w->window) {
        w->window->dirty = true;
        if (layout)
            w->window->layout_dirty = true;
    }
}

void gui_set_expand(widget_t *w, bool expand)
{
    w->expand = expand;
    changed(w, true);
}

rect_t gui_widget_rect(widget_t *w)
{
    return w->rect;
}

gui_window_t *gui_widget_window(widget_t *w)
{
    return w->window;
}

/* Theme-scaled metrics */
static int32_t unit(const gui_theme_t *t, int32_t pixels)
{
    return pixels * t->scale;
}

static int32_t line_height(const gui_theme_t *t)
{
    return TEXT_CELL_HEIGHT * t->scale;
}

static int32_t control_height(const gui_theme_t *t)
{
    return line_height(t) + unit(t, 14);
}

/* --- Boxes ------------------------------------------------------------------------- */

static widget_t *new_box(bool horizontal, int32_t spacing)
{
    widget_t *w = new_widget(W_BOX);
    if (w) {
        w->horizontal = horizontal;
        w->spacing = spacing;
    }
    return w;
}

widget_t *gui_vbox(int32_t spacing)
{
    return new_box(false, spacing);
}

widget_t *gui_hbox(int32_t spacing)
{
    return new_box(true, spacing);
}

void gui_box_set_padding(widget_t *box, int32_t padding)
{
    box->padding = padding;
    changed(box, true);
}

void gui_add(widget_t *box, widget_t *child)
{
    if (!box || !child)
        return;
    if (box->child_count == box->child_capacity) {
        int capacity = box->child_capacity ? box->child_capacity * 2 : 4;
        widget_t **grown = realloc(box->children, (size_t)capacity * sizeof(widget_t *));
        if (!grown)
            return;
        box->children = grown;
        box->child_capacity = capacity;
    }
    box->children[box->child_count++] = child;
    child->parent = box;
    set_window(child, box->window);
    changed(box, true);
}

/* --- Measuring and layout ----------------------------------------------------------- */

static void measure(widget_t *w, const gui_theme_t *t, int32_t *width, int32_t *height)
{
    int32_t text_scale = w->large ? 2 * t->scale : t->scale;
    switch (w->type) {
    case W_BOX: {
        int32_t along = 0, across = 0;
        for (int i = 0; i < w->child_count; i++) {
            int32_t cw, ch;
            measure(w->children[i], t, &cw, &ch);
            along += w->horizontal ? cw : ch;
            int32_t cross = w->horizontal ? ch : cw;
            if (cross > across)
                across = cross;
        }
        if (w->child_count > 1)
            along += unit(t, w->spacing) * (w->child_count - 1);
        int32_t pad = unit(t, w->padding) * 2;
        *width = (w->horizontal ? along : across) + pad;
        *height = (w->horizontal ? across : along) + pad;
        break;
    }
    case W_LABEL:
        *width = text_width(w->text, text_scale);
        *height = TEXT_CELL_HEIGHT * text_scale;
        break;
    case W_BUTTON:
        *width = text_width(w->text, t->scale) + unit(t, 32);
        *height = control_height(t);
        break;
    case W_CHECKBOX:
        *width = line_height(t) + unit(t, 8) + text_width(w->text, t->scale);
        *height = control_height(t);
        break;
    case W_INPUT:
        *width = unit(t, 200);
        *height = control_height(t);
        break;
    case W_LIST:
        *width = unit(t, 160);
        *height = w->rows * (line_height(t) + unit(t, 8)) + unit(t, 4);
        break;
    case W_CUSTOM:
        *width = w->custom.min_width;
        *height = w->custom.min_height;
        break;
    }
}

static void layout(widget_t *w, const gui_theme_t *t, rect_t r)
{
    w->rect = r;
    if (w->type != W_BOX || w->child_count == 0)
        return;

    int32_t pad = unit(t, w->padding), spacing = unit(t, w->spacing);
    rect_t inner = rect_inset(r, pad);
    int32_t total = 0, expanders = 0;
    int32_t sizes[64];
    int n = w->child_count < 64 ? w->child_count : 64;
    for (int i = 0; i < n; i++) {
        int32_t cw, ch;
        measure(w->children[i], t, &cw, &ch);
        sizes[i] = w->horizontal ? cw : ch;
        total += sizes[i];
        if (w->children[i]->expand)
            expanders++;
    }
    total += spacing * (n - 1);
    int32_t available = w->horizontal ? inner.w : inner.h;
    int32_t extra = available - total;

    int32_t position = w->horizontal ? inner.x : inner.y;
    for (int i = 0; i < n; i++) {
        int32_t size = sizes[i];
        if (extra > 0 && w->children[i]->expand)
            size += extra / expanders;
        rect_t child = w->horizontal ? rect_make(position, inner.y, size, inner.h)
                                     : rect_make(inner.x, position, inner.w, size);
        /* Labels and buttons in a vertical box keep their natural height; text inputs fill the width. */
        layout(w->children[i], t, child);
        position += size + spacing;
    }
}

/* --- Drawing --------------------------------------------------------------------- */

static bool is_focused(widget_t *w)
{
    return w->window && w->window->focus == w && w->window->active;
}

static void draw_focus_ring(canvas_t *c, const gui_theme_t *t, rect_t r)
{
    canvas_outline_rounded(c, rect_inset(r, -2), unit(t, 6) + 2, t->focus);
}

static void draw_text_centered(canvas_t *c, rect_t r, const char *text, color_t color, int32_t scale)
{
    int32_t x = r.x + (r.w - text_width(text, scale)) / 2;
    int32_t y = r.y + (r.h - TEXT_CELL_HEIGHT * scale) / 2;
    canvas_text(c, x, y, text, color, scale);
}

/* Byte offset of the n-th code point. */
static int32_t byte_offset(const char *text, int32_t n)
{
    const char *p = text;
    while (n-- > 0 && *p)
        utf8_next(&p);
    return (int32_t)(p - text);
}

static void draw(widget_t *w, canvas_t *c, const gui_theme_t *t)
{
    rect_t r = w->rect;
    gui_window_t *win = w->window;
    bool hover = win && win->hover == w, pressed = win && win->pressed == w;
    int32_t radius = unit(t, 6);

    switch (w->type) {
    case W_BOX:
        for (int i = 0; i < w->child_count; i++)
            draw(w->children[i], c, t);
        break;
    case W_LABEL:
        canvas_text(c, r.x, r.y + (r.h - TEXT_CELL_HEIGHT * (w->large ? 2 : 1) * t->scale) / 2, w->text, t->text,
                    w->large ? 2 * t->scale : t->scale);
        break;
    case W_BUTTON: {
        int32_t h = control_height(t);
        rect_t b = rect_make(r.x, r.y + (r.h - h) / 2, r.w, h);
        color_t fill = w->primary ? (pressed ? color_mix(t->accent, RGB(0, 0, 0), 40)
                                             : hover ? color_mix(t->accent, RGB(255, 255, 255), 30) : t->accent)
                                  : (pressed ? t->surface_pressed : hover ? t->surface_hover : t->surface);
        canvas_fill_rounded(c, b, radius, fill);
        if (!w->primary)
            canvas_outline_rounded(c, b, radius, t->border);
        draw_text_centered(c, b, w->text, w->primary ? t->accent_text : t->text, t->scale);
        if (is_focused(w))
            draw_focus_ring(c, t, b);
        break;
    }
    case W_CHECKBOX: {
        int32_t box = line_height(t);
        rect_t b = rect_make(r.x, r.y + (r.h - box) / 2, box, box);
        canvas_fill_rounded(c, b, unit(t, 4), w->checked ? t->accent : (hover ? t->surface_hover : t->surface));
        canvas_outline_rounded(c, b, unit(t, 4), w->checked ? t->accent : t->border);
        if (w->checked) {
            /* tick */
            for (int32_t d = 0; d < 2 * t->scale; d++) {
                canvas_line(c, b.x + box / 4, b.y + box / 2 + d, b.x + box / 2 - 1, b.y + box * 3 / 4 + d, t->accent_text);
                canvas_line(c, b.x + box / 2 - 1, b.y + box * 3 / 4 + d, b.x + box * 3 / 4 + 1, b.y + box / 4 + d,
                            t->accent_text);
            }
        }
        canvas_text(c, b.x + box + unit(t, 8), r.y + (r.h - line_height(t)) / 2, w->text, t->text, t->scale);
        if (is_focused(w))
            draw_focus_ring(c, t, b);
        break;
    }
    case W_INPUT: {
        int32_t h = control_height(t);
        rect_t b = rect_make(r.x, r.y + (r.h - h) / 2, r.w, h);
        canvas_fill_rounded(c, b, radius, t->surface);
        canvas_outline_rounded(c, b, radius, is_focused(w) ? t->accent : t->border);
        int32_t pad = unit(t, 8), cell = TEXT_CELL_WIDTH * t->scale;
        int32_t visible = (b.w - 2 * pad) / cell;
        if (visible < 1)
            visible = 1;
        if (w->cursor < w->scroll)
            w->scroll = w->cursor;
        if (w->cursor > w->scroll + visible - 1)
            w->scroll = w->cursor - visible + 1;
        rect_t text_area = rect_make(b.x + pad, b.y, b.w - 2 * pad, b.h);
        rect_t previous = canvas_clip(c, text_area);
        int32_t ty = b.y + (b.h - line_height(t)) / 2;
        if (w->value[0])
            canvas_text(c, b.x + pad, ty, w->value + byte_offset(w->value, w->scroll), t->text, t->scale);
        else if (w->text && !is_focused(w))
            canvas_text(c, b.x + pad, ty, w->text, t->text_dim, t->scale);
        if (is_focused(w))
            canvas_fill(c, rect_make(b.x + pad + (w->cursor - w->scroll) * cell, ty, t->scale, line_height(t)),
                        t->accent);
        c->clip = previous;
        break;
    }
    case W_LIST: {
        canvas_fill_rounded(c, r, radius, t->surface);
        canvas_outline_rounded(c, r, radius, is_focused(w) ? t->accent : t->border);
        int32_t row_h = line_height(t) + unit(t, 8);
        rect_t previous = canvas_clip(c, rect_inset(r, 2));
        for (int i = w->first_visible; i < w->item_count; i++) {
            int32_t y = r.y + unit(t, 2) + (i - w->first_visible) * row_h;
            if (y >= r.y + r.h)
                break;
            rect_t row = rect_make(r.x + unit(t, 3), y, r.w - unit(t, 6), row_h);
            if (i == w->selected)
                canvas_fill_rounded(c, row, unit(t, 4), is_focused(w) ? t->accent : t->selection);
            canvas_text(c, row.x + unit(t, 8), y + unit(t, 4), w->items[i],
                        i == w->selected && is_focused(w) ? t->accent_text : t->text, t->scale);
        }
        c->clip = previous;
        break;
    }
    case W_CUSTOM:
        if (w->custom.draw) {
            rect_t previous = canvas_clip(c, r);
            w->custom.draw(w, c, r, w->custom.user);
            c->clip = previous;
        }
        break;
    }
}

/* --- Widget constructors --------------------------------------------------------- */

widget_t *gui_label(const char *text)
{
    widget_t *w = new_widget(W_LABEL);
    if (w)
        w->text = copy_text(text);
    return w;
}

void gui_label_set_text(widget_t *w, const char *text)
{
    free(w->text);
    w->text = copy_text(text);
    changed(w, true);
}

void gui_label_set_large(widget_t *w, bool large)
{
    w->large = large;
    changed(w, true);
}

widget_t *gui_button(const char *text, gui_callback_t on_click, void *user)
{
    widget_t *w = new_widget(W_BUTTON);
    if (w) {
        w->text = copy_text(text);
        w->callback = on_click;
        w->user = user;
        w->focusable = true;
    }
    return w;
}

void gui_button_set_primary(widget_t *w, bool primary)
{
    w->primary = primary;
    changed(w, false);
}

widget_t *gui_input(const char *placeholder, gui_callback_t on_submit, void *user)
{
    widget_t *w = new_widget(W_INPUT);
    if (w) {
        w->text = copy_text(placeholder);
        w->callback = on_submit;
        w->user = user;
        w->focusable = true;
    }
    return w;
}

const char *gui_input_text(widget_t *w)
{
    return w->value;
}

void gui_input_set_text(widget_t *w, const char *text)
{
    strncpy(w->value, text, INPUT_MAX - 1);
    w->value[INPUT_MAX - 1] = '\0';
    w->cursor = text_length(w->value, INPUT_MAX);
    changed(w, false);
}

widget_t *gui_checkbox(const char *text, bool checked, gui_callback_t on_toggle, void *user)
{
    widget_t *w = new_widget(W_CHECKBOX);
    if (w) {
        w->text = copy_text(text);
        w->checked = checked;
        w->callback = on_toggle;
        w->user = user;
        w->focusable = true;
    }
    return w;
}

bool gui_checkbox_checked(widget_t *w)
{
    return w->checked;
}

widget_t *gui_list(gui_callback_t on_select, void *user)
{
    widget_t *w = new_widget(W_LIST);
    if (w) {
        w->callback = on_select;
        w->user = user;
        w->focusable = true;
    }
    return w;
}

void gui_list_add(widget_t *w, const char *item)
{
    if (w->item_count == w->item_capacity) {
        int capacity = w->item_capacity ? w->item_capacity * 2 : 8;
        char **grown = realloc(w->items, (size_t)capacity * sizeof(char *));
        if (!grown)
            return;
        w->items = grown;
        w->item_capacity = capacity;
    }
    w->items[w->item_count++] = copy_text(item);
    changed(w, false);
}

void gui_list_clear(widget_t *w)
{
    for (int i = 0; i < w->item_count; i++)
        free(w->items[i]);
    w->item_count = 0;
    w->selected = -1;
    w->first_visible = 0;
    changed(w, false);
}

int gui_list_selected(widget_t *w)
{
    return w->selected;
}

const char *gui_list_item(widget_t *w, int index)
{
    return index >= 0 && index < w->item_count ? w->items[index] : NULL;
}

void gui_list_set_rows(widget_t *w, int rows)
{
    w->rows = rows < 1 ? 1 : rows;
    changed(w, true);
}

widget_t *gui_custom(const gui_custom_t *custom)
{
    widget_t *w = new_widget(W_CUSTOM);
    if (w) {
        w->custom = *custom;
        w->focusable = custom->event != NULL;
    }
    return w;
}

void gui_custom_redraw(widget_t *w)
{
    changed(w, false);
}

/* --- Behavior ------------------------------------------------------------------ */

static void activate(widget_t *w)
{
    if (w->type == W_CHECKBOX) {
        w->checked = !w->checked;
        changed(w, false);
    }
    if (w->callback)
        w->callback(w, w->user);
}

static void list_select(widget_t *w, int index)
{
    if (w->item_count == 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= w->item_count)
        index = w->item_count - 1;
    if (index < w->first_visible)
        w->first_visible = index;
    if (index >= w->first_visible + w->rows)
        w->first_visible = index - w->rows + 1;
    bool different = index != w->selected;
    w->selected = index;
    changed(w, false);
    if (different && w->callback)
        w->callback(w, w->user);
}

static void input_key(widget_t *w, const wm_event_t *e)
{
    int32_t length = text_length(w->value, INPUT_MAX);
    int32_t at = byte_offset(w->value, w->cursor);
    switch (e->key) {
    case JELLY_KEY_LEFT:
        if (w->cursor > 0)
            w->cursor--;
        break;
    case JELLY_KEY_RIGHT:
        if (w->cursor < length)
            w->cursor++;
        break;
    case JELLY_KEY_HOME:
        w->cursor = 0;
        break;
    case JELLY_KEY_END:
        w->cursor = length;
        break;
    case JELLY_KEY_BACKSPACE:
        if (w->cursor > 0) {
            int32_t before = byte_offset(w->value, w->cursor - 1);
            memmove(w->value + before, w->value + at, strlen(w->value + at) + 1);
            w->cursor--;
        }
        break;
    case JELLY_KEY_DELETE:
        if (w->cursor < length) {
            int32_t next = byte_offset(w->value, w->cursor + 1);
            memmove(w->value + at, w->value + next, strlen(w->value + next) + 1);
        }
        break;
    case JELLY_KEY_ENTER:
    case JELLY_KEY_KPENTER:
        if (w->callback)
            w->callback(w, w->user);
        break;
    default:
        if (e->character >= 0x20 && e->character != 0x7F && !(e->modifiers & WM_MOD_CTRL)) {
            char encoded[4];
            int n = utf8_encode(e->character, encoded);
            size_t used = strlen(w->value);
            if (used + (size_t)n < INPUT_MAX) {
                memmove(w->value + at + n, w->value + at, used - (size_t)at + 1);
                memcpy(w->value + at, encoded, (size_t)n);
                w->cursor++;
            }
        }
        break;
    }
    changed(w, false);
}

/* Focusable widgets in tree order */
static void collect_focusable(widget_t *w, widget_t **out, int *count, int max)
{
    if (w->focusable && *count < max)
        out[(*count)++] = w;
    for (int i = 0; i < w->child_count; i++)
        collect_focusable(w->children[i], out, count, max);
}

void gui_window_focus(gui_window_t *win, widget_t *w)
{
    if (win->focus != w) {
        win->focus = w;
        win->dirty = true;
    }
}

static void move_focus(gui_window_t *win, int direction)
{
    widget_t *list[128];
    int count = 0;
    if (!win->root)
        return;
    collect_focusable(win->root, list, &count, 128);
    if (count == 0)
        return;
    int current = -1;
    for (int i = 0; i < count; i++) {
        if (list[i] == win->focus)
            current = i;
    }
    int next = current < 0 ? (direction > 0 ? 0 : count - 1) : (current + direction + count) % count;
    gui_window_focus(win, list[next]);
}

static widget_t *hit(widget_t *w, int32_t x, int32_t y)
{
    if (!rect_contains(w->rect, x, y))
        return NULL;
    for (int i = w->child_count - 1; i >= 0; i--) {
        widget_t *found = hit(w->children[i], x, y);
        if (found)
            return found;
    }
    return w->type == W_BOX ? NULL : w;
}

static void key_down(gui_window_t *win, const wm_event_t *e)
{
    widget_t *f = win->focus;
    if (e->key == JELLY_KEY_TAB && !(e->modifiers & (WM_MOD_CTRL | WM_MOD_ALT))) {
        move_focus(win, (e->modifiers & WM_MOD_SHIFT) ? -1 : 1);
        return;
    }
    if (!f)
        return;
    switch (f->type) {
    case W_BUTTON:
    case W_CHECKBOX:
        if (e->key == JELLY_KEY_ENTER || e->key == JELLY_KEY_SPACE || e->key == JELLY_KEY_KPENTER)
            activate(f);
        break;
    case W_INPUT:
        input_key(f, e);
        break;
    case W_LIST:
        if (e->key == JELLY_KEY_UP)
            list_select(f, f->selected - 1);
        else if (e->key == JELLY_KEY_DOWN)
            list_select(f, f->selected + 1);
        else if (e->key == JELLY_KEY_HOME)
            list_select(f, 0);
        else if (e->key == JELLY_KEY_END)
            list_select(f, f->item_count - 1);
        break;
    case W_CUSTOM:
        if (f->custom.event)
            f->custom.event(f, e, f->custom.user);
        break;
    default:
        break;
    }
}

static void pointer(gui_window_t *win, const wm_event_t *e)
{
    widget_t *under = win->root ? hit(win->root, e->x, e->y) : NULL;
    widget_t *target = win->pressed ? win->pressed : under;

    if (e->type == WM_EVENT_MOUSE_MOVE || e->type == WM_EVENT_MOUSE_LEAVE) {
        widget_t *hover = e->type == WM_EVENT_MOUSE_LEAVE ? NULL : under;
        if (hover != win->hover) {
            win->hover = hover;
            win->dirty = true;
        }
    }
    if (target && target->type == W_CUSTOM && target->custom.event) {
        wm_event_t local = *e;
        local.x -= target->rect.x;
        local.y -= target->rect.y;
        target->custom.event(target, &local, target->custom.user);
    }
    if (e->type == WM_EVENT_MOUSE_DOWN && e->button == JELLY_BUTTON_LEFT) {
        win->pressed = under;
        if (under && under->focusable)
            gui_window_focus(win, under);
        if (under && under->type == W_LIST) {
            int32_t row_h = line_height(&win->theme) + unit(&win->theme, 8);
            list_select(under, under->first_visible + (e->y - under->rect.y - unit(&win->theme, 2)) / row_h);
        }
        if (under && under->type == W_INPUT) {
            int32_t cell = TEXT_CELL_WIDTH * win->theme.scale;
            int32_t position = under->scroll + (e->x - under->rect.x - unit(&win->theme, 8) + cell / 2) / cell;
            int32_t length = text_length(under->value, INPUT_MAX);
            under->cursor = position < 0 ? 0 : position > length ? length : position;
        }
        win->dirty = true;
    } else if (e->type == WM_EVENT_MOUSE_UP && e->button == JELLY_BUTTON_LEFT) {
        widget_t *pressed = win->pressed;
        win->pressed = NULL;
        win->dirty = true;
        if (pressed && pressed == under && (pressed->type == W_BUTTON || pressed->type == W_CHECKBOX))
            activate(pressed);
    } else if (e->type == WM_EVENT_MOUSE_WHEEL && under && under->type == W_LIST) {
        int32_t first = under->first_visible - e->wheel;
        int32_t last = under->item_count - under->rows;
        under->first_visible = first > last ? (last > 0 ? last : 0) : first < 0 ? 0 : first;
        win->dirty = true;
    }
}

/* --- Windows ------------------------------------------------------------------- */

static void repaint(gui_window_t *win)
{
    window_t *s = win->surface;
    if (!s || s->frame_pending || !win->dirty)
        return;
    if (win->layout_dirty && win->root) {
        layout(win->root, &win->theme, rect_make(0, 0, s->width, s->height));
        win->layout_dirty = false;
    }
    canvas_set_clip(&s->canvas, rect_make(0, 0, s->width, s->height));
    canvas_fill(&s->canvas, rect_make(0, 0, s->width, s->height), win->theme.background);
    if (win->root)
        draw(win->root, &s->canvas, &win->theme);
    win->dirty = false;
    wm_present(s, rect_make(0, 0, s->width, s->height));
}

gui_window_t *gui_window_create(gui_app_t *app, const char *title, int32_t width, int32_t height)
{
    gui_window_t *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;
    win->surface = wm_create_window(app->connection, width, height, title, 0);
    if (!win->surface) {
        free(win);
        return NULL;
    }
    win->surface->user = win;
    win->app = app;
    win->theme = gui_theme_light(1);
    win->dirty = win->layout_dirty = true;
    win->next = app->windows;
    app->windows = win;
    app->window_count++;
    return win;
}

void gui_window_destroy(gui_window_t *win)
{
    gui_app_t *app = win->app;
    for (gui_window_t **link = &app->windows; *link; link = &(*link)->next) {
        if (*link == win) {
            *link = win->next;
            break;
        }
    }
    app->window_count--;
    wm_destroy_window(win->surface);
    free(win);
    if (app->window_count == 0)
        gui_quit(app, 0);
}

void gui_window_set_root(gui_window_t *win, widget_t *root)
{
    win->root = root;
    set_window(root, win);
    win->dirty = win->layout_dirty = true;
    if (!win->focus)
        move_focus(win, 1);
}

void gui_window_set_title(gui_window_t *win, const char *title)
{
    wm_set_title(win->surface, title);
}

void gui_window_set_theme(gui_window_t *win, gui_theme_t theme)
{
    win->theme = theme;
    win->dirty = win->layout_dirty = true;
}

const gui_theme_t *gui_window_theme(gui_window_t *win)
{
    return &win->theme;
}

void gui_window_on_close(gui_window_t *win, gui_callback_t callback, void *user)
{
    win->on_close = callback;
    win->on_close_user = user;
}

void gui_window_invalidate(gui_window_t *win)
{
    win->dirty = true;
}

/* --- Application --------------------------------------------------------------- */

gui_app_t *gui_init(void)
{
    gui_app_t *app = calloc(1, sizeof(*app));
    if (!app)
        return NULL;
    if (wm_connect(&app->connection)) {
        free(app);
        return NULL;
    }
    return app;
}

void gui_quit(gui_app_t *app, int code)
{
    app->quit = true;
    app->exit_code = code;
}

void gui_watch(gui_app_t *app, jelly_handle_t handle, void (*ready)(void *user), void *user)
{
    if (app->watch_count < MAX_WATCHES)
        app->watches[app->watch_count++] = (watch_t){ handle, ready, user };
}

static void dispatch(gui_app_t *app, const wm_event_t *e, window_t *surface)
{
    gui_window_t *win = surface ? surface->user : NULL;
    if (!win)
        return;
    switch (e->type) {
    case WM_EVENT_FRAME:
        break; /* repaint() below may send the next frame */
    case WM_EVENT_CLOSE:
        if (win->on_close)
            win->on_close(NULL, win->on_close_user);
        else
            gui_window_destroy(win);
        return;
    case WM_EVENT_FOCUS_IN:
    case WM_EVENT_FOCUS_OUT:
        win->active = e->type == WM_EVENT_FOCUS_IN;
        win->dirty = true;
        break;
    case WM_EVENT_KEY_DOWN:
        key_down(win, e);
        break;
    case WM_EVENT_KEY_UP:
        if (win->focus && win->focus->type == W_CUSTOM && win->focus->custom.event)
            win->focus->custom.event(win->focus, e, win->focus->custom.user);
        break;
    default:
        pointer(win, e);
        break;
    }
    (void)app;
}

int gui_run(gui_app_t *app)
{
    while (!app->quit) {
        /* Events first: some may already be queued in the client library, where no wait would see them. */
        wm_event_t e;
        window_t *surface;
        int result;
        while (!app->quit && (result = wm_next_event(app->connection, &e, &surface, 0)) > 0)
            dispatch(app, &e, surface);
        if (app->quit)
            break;
        if (result < 0)
            return 1; /* the display server is gone */

        for (gui_window_t *w = app->windows; w; w = w->next)
            repaint(w);

        jelly_handle_t handles[1 + MAX_WATCHES];
        uint32_t count = 0, index = 0;
        handles[count++] = wm_handle(app->connection);
        for (int i = 0; i < app->watch_count; i++)
            handles[count++] = app->watches[i].handle;
        status_t status = jelly_wait_many(handles, count, JELLY_WAIT_FOREVER, &index);
        if (STATUS_IS_ERROR(status))
            return 1;
        if (index > 0)
            app->watches[index - 1].ready(app->watches[index - 1].user);
    }
    return app->exit_code;
}
