/*
 * JellyOS GUI toolkit (README section 36).
 *
 * One widget structure serves all control types. A window keeps its widget
 * tree, lays it out when its size or content changes and repaints the whole
 * window into its surface; WM_PRESENTED (WM_EVENT_FRAME) paces the frames.
 * Destroying a window is deferred to the end of the current event, so
 * callbacks may close the window that called them.
 */

#include "graphics/gui/gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/input.h>
#include <jelly/os.h>

#define INPUT_MAX       256
#define MAX_WATCHES     8
#define MAX_TIMERS      8
#define MAX_COLUMNS     6
#define DOUBLE_CLICK_NS 450000000ULL
#define SCROLLBAR       6

typedef enum {
    W_BOX, W_LABEL, W_BUTTON, W_INPUT, W_CHECKBOX, W_LIST, W_TABLE, W_ICON, W_SEPARATOR, W_SCROLL, W_CUSTOM
} widget_type_t;

typedef struct {
    icon_t icon;
    char  *cells[MAX_COLUMNS];
} table_row_t;

struct widget {
    widget_type_t  type;
    gui_window_t  *window;
    widget_t      *parent;
    widget_t     **children;
    int            child_count, child_capacity;
    rect_t         rect;
    bool           expand, focusable, visible;
    int32_t        min_w, min_h;

    /* box */
    bool           horizontal, background;
    int32_t        spacing, padding;

    /* label, button, checkbox, input placeholder */
    char          *text;
    bool           large, primary, flat, dim, center, selected_flag;
    icon_t         icon;
    int32_t        icon_size;

    /* callbacks */
    gui_callback_t callback;
    void          *user;
    gui_callback_t activate_callback;
    void          *activate_user;
    bool           checked;

    /* input */
    char           value[INPUT_MAX];
    int32_t        cursor, scroll;
    bool           password, all_selected;

    /* list */
    char         **items;
    int            item_count, item_capacity;
    int            selected, first_visible, rows;

    /* table */
    int            columns;
    char          *titles[MAX_COLUMNS];
    int32_t        widths[MAX_COLUMNS];
    table_row_t   *table_rows;
    int            row_count, row_capacity;
    uint64_t       last_click;
    int            last_click_row;

    /* scroll view */
    int32_t        offset, content_height;

    /* custom */
    gui_custom_t   custom;
};

typedef struct {
    jelly_handle_t handle;
    void         (*ready)(void *user);
    void          *user;
} watch_t;

typedef struct {
    bool     used;
    uint64_t interval, next;
    void   (*fn)(void *user);
    void    *user;
} timer_t_;

struct gui_app {
    wm_connection_t *connection;
    gui_window_t    *windows;
    int              window_count;
    bool             quit;
    int              exit_code;
    watch_t          watches[MAX_WATCHES];
    int              watch_count;
    timer_t_         timers[MAX_TIMERS];
    gui_settings_t   settings;
    void           (*on_windows)(void *user);
    void            *on_windows_user;
    void           (*on_notification)(const char *title, const char *text, void *user);
    void            *on_notification_user;
    void           (*on_settings)(void *user);
    void            *on_settings_user;
};

struct gui_window {
    gui_app_t     *app;
    gui_window_t  *next;
    window_t      *surface;
    widget_t      *root;
    gui_theme_t    theme;
    bool           keep_theme;
    uint32_t       flags;
    widget_t      *focus, *hover, *pressed;
    bool           dirty, layout_dirty, active, closing;
    gui_callback_t on_close;
    void          *on_close_user;
    bool         (*on_key)(gui_window_t *, const wm_event_t *, void *);
    void          *on_key_user;
};

/* --- Themes and settings --------------------------------------------------------- */

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

gui_theme_t gui_theme_from_settings(const gui_settings_t *s)
{
    return s->dark ? gui_theme_dark(s->scale) : gui_theme_light(s->scale);
}

static void read_settings(const char *path, gui_settings_t *s)
{
    FILE *file = fopen(path, "r");
    char line[128];
    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "theme=", 6))
            s->dark = !strcmp(line + 6, "dark");
        else if (!strncmp(line, "scale=", 6))
            s->scale = atoi(line + 6) >= 2 ? 2 : 1;
        else if (!strncmp(line, "keymap=", 7))
            snprintf(s->keymap, sizeof(s->keymap), "%s", line + 7);
    }
    fclose(file);
}

static void settings_path(char *path, size_t size)
{
    const char *home = getenv("HOME");
    snprintf(path, size, "%s/.config/desktop.conf", home && strcmp(home, "/") ? home : "");
}

void gui_load_settings(gui_settings_t *s)
{
    char path[256];
    s->dark = false;
    s->scale = 1;
    strcpy(s->keymap, "us");
    read_settings("/etc/desktop.conf", s);
    settings_path(path, sizeof(path));
    if (path[0] != '/' || strncmp(path, "/.config", 8) != 0)
        read_settings(path, s);
}

int gui_save_settings(const gui_settings_t *s)
{
    char path[256], directory[256];
    const char *home = getenv("HOME");
    if (!home || !strcmp(home, "/"))
        return -1;
    snprintf(directory, sizeof(directory), "%s/.config", home);
    jelly_mkdir(directory, 0755);
    settings_path(path, sizeof(path));
    FILE *file = fopen(path, "w");
    if (!file)
        return -1;
    fprintf(file, "theme=%s\nscale=%d\nkeymap=%s\n", s->dark ? "dark" : "light", s->scale, s->keymap);
    return fclose(file);
}

/* --- Widget basics --------------------------------------------------------------- */

static widget_t *new_widget(widget_type_t type)
{
    widget_t *w = calloc(1, sizeof(*w));
    if (w) {
        w->type = type;
        w->visible = true;
        w->selected = -1;
        w->rows = 6;
        w->last_click_row = -1;
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

void gui_set_visible(widget_t *w, bool visible)
{
    w->visible = visible;
    changed(w, true);
}

void gui_set_min_size(widget_t *w, int32_t width, int32_t height)
{
    w->min_w = width;
    w->min_h = height;
    changed(w, true);
}

void gui_set_user(widget_t *w, void *user)
{
    w->user = user;
}

void *gui_get_user(widget_t *w)
{
    return w->user;
}

rect_t gui_widget_rect(widget_t *w)
{
    return w->rect;
}

gui_window_t *gui_widget_window(widget_t *w)
{
    return w->window;
}

static void free_widget(widget_t *w)
{
    for (int i = 0; i < w->child_count; i++)
        free_widget(w->children[i]);
    free(w->children);
    free(w->text);
    for (int i = 0; i < w->item_count; i++)
        free(w->items[i]);
    free(w->items);
    for (int r = 0; r < w->row_count; r++)
        for (int c = 0; c < w->columns; c++)
            free(w->table_rows[r].cells[c]);
    free(w->table_rows);
    for (int c = 0; c < w->columns; c++)
        free(w->titles[c]);
    free(w);
}

/* Metrics scaled by the theme */
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

static int32_t row_height(const gui_theme_t *t)
{
    return line_height(t) + unit(t, 8);
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

void gui_box_set_background(widget_t *box, bool surface)
{
    box->background = surface;
    changed(box, false);
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

static void forget(gui_window_t *win, widget_t *w)
{
    if (!win)
        return;
    if (win->focus == w)
        win->focus = NULL;
    if (win->hover == w)
        win->hover = NULL;
    if (win->pressed == w)
        win->pressed = NULL;
    for (int i = 0; i < w->child_count; i++)
        forget(win, w->children[i]);
}

void gui_box_clear(widget_t *box)
{
    for (int i = 0; i < box->child_count; i++) {
        forget(box->window, box->children[i]);
        free_widget(box->children[i]);
    }
    box->child_count = 0;
    changed(box, true);
}

/* --- Measuring and layout ----------------------------------------------------------- */

static void measure(widget_t *w, const gui_theme_t *t, int32_t *width, int32_t *height)
{
    int32_t text_scale = w->large ? 2 * t->scale : t->scale;
    *width = *height = 0;
    if (!w->visible)
        return;
    switch (w->type) {
    case W_BOX: {
        int32_t along = 0, across = 0, visible = 0;
        for (int i = 0; i < w->child_count; i++) {
            if (!w->children[i]->visible)
                continue;
            int32_t cw, ch;
            measure(w->children[i], t, &cw, &ch);
            along += w->horizontal ? cw : ch;
            int32_t cross = w->horizontal ? ch : cw;
            if (cross > across)
                across = cross;
            visible++;
        }
        if (visible > 1)
            along += unit(t, w->spacing) * (visible - 1);
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
        *width = text_width(w->text, t->scale) + unit(t, w->flat ? 20 : 32) +
                 (w->icon ? line_height(t) + unit(t, w->text[0] ? 6 : 0) : 0);
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
        *height = w->rows * row_height(t) + unit(t, 4);
        break;
    case W_TABLE:
        *width = unit(t, 240);
        *height = (w->rows + 1) * row_height(t) + unit(t, 4);
        break;
    case W_ICON:
        *width = *height = unit(t, w->icon_size);
        break;
    case W_SEPARATOR:
        *width = *height = unit(t, 1);
        break;
    case W_SCROLL:
        if (w->child_count)
            measure(w->children[0], t, width, height);
        *width += SCROLLBAR;
        if (*height > unit(t, 80))
            *height = unit(t, 80); /* can shrink: the rest scrolls */
        break;
    case W_CUSTOM:
        *width = w->custom.min_width;
        *height = w->custom.min_height;
        break;
    }
    if (*width < w->min_w)
        *width = w->min_w;
    if (*height < w->min_h)
        *height = w->min_h;
}

static void layout(widget_t *w, const gui_theme_t *t, rect_t r)
{
    w->rect = r;
    if (w->type == W_SCROLL && w->child_count) {
        int32_t cw, ch;
        measure(w->children[0], t, &cw, &ch);
        w->content_height = ch;
        int32_t max_offset = ch - r.h > 0 ? ch - r.h : 0;
        if (w->offset > max_offset)
            w->offset = max_offset;
        if (w->offset < 0)
            w->offset = 0;
        layout(w->children[0], t, rect_make(r.x, r.y - w->offset, r.w - SCROLLBAR, ch > r.h ? ch : r.h));
        return;
    }
    if (w->type != W_BOX || w->child_count == 0)
        return;

    int32_t pad = unit(t, w->padding), spacing = unit(t, w->spacing);
    rect_t inner = rect_inset(r, pad);
    int32_t total = 0, expanders = 0, visible = 0;
    int32_t sizes[64];
    int n = w->child_count < 64 ? w->child_count : 64;
    for (int i = 0; i < n; i++) {
        int32_t cw, ch;
        measure(w->children[i], t, &cw, &ch);
        sizes[i] = w->horizontal ? cw : ch;
        if (!w->children[i]->visible)
            continue;
        total += sizes[i];
        visible++;
        if (w->children[i]->expand)
            expanders++;
    }
    if (visible > 1)
        total += spacing * (visible - 1);
    int32_t available = w->horizontal ? inner.w : inner.h;
    int32_t extra = available - total;

    int32_t position = w->horizontal ? inner.x : inner.y;
    for (int i = 0; i < n; i++) {
        if (!w->children[i]->visible) {
            w->children[i]->rect = rect_make(0, 0, 0, 0);
            continue;
        }
        int32_t size = sizes[i];
        if (w->children[i]->expand && expanders)
            size += extra / expanders; /* may shrink below the natural size when space is short */
        if (size < 0)
            size = 0;
        rect_t child = w->horizontal ? rect_make(position, inner.y, size, inner.h)
                                     : rect_make(inner.x, position, inner.w, size);
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

/* Byte offset of the n-th code point. */
static int32_t byte_offset(const char *text, int32_t n)
{
    const char *p = text;
    while (n-- > 0 && *p)
        utf8_next(&p);
    return (int32_t)(p - text);
}

/* Text that ends with "…" (three dots) if it does not fit into width. */
static void draw_text_fitted(canvas_t *c, int32_t x, int32_t y, int32_t width, const char *text, color_t color,
                             int32_t scale)
{
    int32_t cell = TEXT_CELL_WIDTH * scale, fits = width / cell;
    int32_t length = text_length(text, (int32_t)strlen(text));
    if (length <= fits) {
        canvas_text(c, x, y, text, color, scale);
        return;
    }
    if (fits < 4)
        return;
    char buffer[512];
    int32_t bytes = byte_offset(text, fits - 3);
    if (bytes > (int32_t)sizeof(buffer) - 4)
        bytes = (int32_t)sizeof(buffer) - 4;
    memcpy(buffer, text, (size_t)bytes);
    strcpy(buffer + bytes, "...");
    canvas_text(c, x, y, buffer, color, scale);
}

static void draw(widget_t *w, canvas_t *c, const gui_theme_t *t);

static void draw_table(widget_t *w, canvas_t *c, const gui_theme_t *t)
{
    rect_t r = w->rect;
    int32_t radius = unit(t, 6), rh = row_height(t), icon = line_height(t);
    canvas_fill_rounded(c, r, radius, t->surface);
    canvas_outline_rounded(c, r, radius, is_focused(w) ? t->accent : t->border);
    rect_t previous = canvas_clip(c, rect_inset(r, 1));

    /* header */
    int32_t x = r.x + unit(t, 8);
    for (int col = 0; col < w->columns; col++) {
        int32_t width = col == w->columns - 1 ? r.x + r.w - x - unit(t, 6) : unit(t, w->widths[col]);
        draw_text_fitted(c, x + (col == 0 ? icon + unit(t, 6) : 0), r.y + unit(t, 4), width - unit(t, 8),
                         w->titles[col], t->text_dim, t->scale);
        x += width;
    }
    canvas_fill(c, rect_make(r.x + 1, r.y + rh, r.w - 2, 1), t->border);

    int visible_rows = (r.h - rh - unit(t, 2)) / rh;
    if (visible_rows < 1)
        visible_rows = 1;
    if (w->selected >= 0 && w->selected < w->first_visible)
        w->first_visible = w->selected;
    if (w->selected >= w->first_visible + visible_rows)
        w->first_visible = w->selected - visible_rows + 1;
    for (int i = w->first_visible; i < w->row_count && i < w->first_visible + visible_rows; i++) {
        int32_t y = r.y + rh + unit(t, 2) + (i - w->first_visible) * rh;
        bool selected = i == w->selected;
        if (selected)
            canvas_fill_rounded(c, rect_make(r.x + unit(t, 3), y, r.w - unit(t, 6), rh), unit(t, 4),
                                is_focused(w) ? t->accent : t->selection);
        color_t text = selected && is_focused(w) ? t->accent_text : t->text;
        x = r.x + unit(t, 8);
        for (int col = 0; col < w->columns; col++) {
            int32_t width = col == w->columns - 1 ? r.x + r.w - x - unit(t, 6) : unit(t, w->widths[col]);
            int32_t tx = x;
            if (col == 0 && w->table_rows[i].icon) {
                icon_draw(c, w->table_rows[i].icon, x, y + (rh - icon) / 2, icon, t->accent);
                tx += icon + unit(t, 6);
            }
            if (w->table_rows[i].cells[col])
                draw_text_fitted(c, tx, y + unit(t, 4), x + width - tx - unit(t, 8), w->table_rows[i].cells[col],
                                 col == 0 ? text : (selected && is_focused(w) ? text : t->text_dim), t->scale);
            x += width;
        }
    }
    c->clip = previous;
}

static void draw(widget_t *w, canvas_t *c, const gui_theme_t *t)
{
    rect_t r = w->rect;
    gui_window_t *win = w->window;
    bool hover = win && win->hover == w, pressed = win && win->pressed == w;
    int32_t radius = unit(t, 6);
    if (!w->visible || rect_empty(r))
        return;

    switch (w->type) {
    case W_BOX:
        if (w->background)
            canvas_fill_rounded(c, r, unit(t, 12), t->surface);
        for (int i = 0; i < w->child_count; i++)
            draw(w->children[i], c, t);
        break;
    case W_LABEL: {
        int32_t scale = w->large ? 2 * t->scale : t->scale;
        int32_t x = w->center ? r.x + (r.w - text_width(w->text, scale)) / 2 : r.x;
        rect_t previous = canvas_clip(c, r);
        canvas_text(c, x, r.y + (r.h - TEXT_CELL_HEIGHT * scale) / 2, w->text, w->dim ? t->text_dim : t->text, scale);
        c->clip = previous;
        break;
    }
    case W_BUTTON: {
        int32_t h = control_height(t);
        rect_t b = rect_make(r.x, r.y + (r.h - h) / 2, r.w, h);
        color_t fill;
        if (w->primary)
            fill = pressed ? color_mix(t->accent, RGB(0, 0, 0), 40)
                           : hover ? color_mix(t->accent, RGB(255, 255, 255), 30) : t->accent;
        else if (w->flat)
            fill = pressed ? t->surface_pressed : hover || w->selected_flag ? t->surface_hover : RGBA(0, 0, 0, 0);
        else
            fill = pressed ? t->surface_pressed : hover ? t->surface_hover : t->surface;
        canvas_fill_rounded(c, b, radius, fill);
        if (!w->primary && !w->flat)
            canvas_outline_rounded(c, b, radius, t->border);
        if (w->flat && w->selected_flag)
            canvas_fill(c, rect_make(b.x + unit(t, 6), b.y + b.h - unit(t, 3), b.w - unit(t, 12), unit(t, 2)),
                        t->accent);
        int32_t icon = w->icon ? line_height(t) : 0, gap = w->icon && w->text[0] ? unit(t, 6) : 0;
        int32_t content = icon + gap + text_width(w->text, t->scale);
        int32_t x = w->flat && w->text[0] ? b.x + unit(t, 10) : b.x + (b.w - content) / 2;
        if (w->icon)
            icon_draw(c, w->icon, x, b.y + (b.h - icon) / 2, icon, t->accent);
        rect_t previous = canvas_clip(c, rect_inset(b, 2));
        draw_text_fitted(c, x + icon + gap, b.y + (b.h - line_height(t)) / 2, b.x + b.w - (x + icon + gap) - unit(t, 6),
                         w->text, w->primary ? t->accent_text : t->text, t->scale);
        c->clip = previous;
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
        rect_t previous = canvas_clip(c, rect_make(b.x + pad, b.y, b.w - 2 * pad, b.h));
        int32_t ty = b.y + (b.h - line_height(t)) / 2;
        if (w->all_selected && is_focused(w))
            canvas_fill(c, rect_make(b.x + pad, ty, (text_length(w->value, INPUT_MAX) - w->scroll) * cell, line_height(t)),
                        t->selection);
        if (w->value[0]) {
            if (w->password) {
                int32_t length = text_length(w->value, INPUT_MAX);
                for (int32_t i = w->scroll; i < length; i++)
                    canvas_fill_rounded(c, rect_make(b.x + pad + (i - w->scroll) * cell + cell / 4,
                                                     ty + line_height(t) / 2 - cell / 4, cell / 2, cell / 2),
                                        cell / 4, t->text);
            } else {
                canvas_text(c, b.x + pad, ty, w->value + byte_offset(w->value, w->scroll), t->text, t->scale);
            }
        } else if (w->text && !is_focused(w)) {
            canvas_text(c, b.x + pad, ty, w->text, t->text_dim, t->scale);
        }
        if (is_focused(w))
            canvas_fill(c, rect_make(b.x + pad + (w->cursor - w->scroll) * cell, ty, t->scale, line_height(t)),
                        t->accent);
        c->clip = previous;
        break;
    }
    case W_LIST: {
        canvas_fill_rounded(c, r, radius, t->surface);
        canvas_outline_rounded(c, r, radius, is_focused(w) ? t->accent : t->border);
        int32_t rh = row_height(t);
        rect_t previous = canvas_clip(c, rect_inset(r, 2));
        for (int i = w->first_visible; i < w->item_count; i++) {
            int32_t y = r.y + unit(t, 2) + (i - w->first_visible) * rh;
            if (y >= r.y + r.h)
                break;
            rect_t row = rect_make(r.x + unit(t, 3), y, r.w - unit(t, 6), rh);
            if (i == w->selected)
                canvas_fill_rounded(c, row, unit(t, 4), is_focused(w) ? t->accent : t->selection);
            canvas_text(c, row.x + unit(t, 8), y + unit(t, 4), w->items[i],
                        i == w->selected && is_focused(w) ? t->accent_text : t->text, t->scale);
        }
        c->clip = previous;
        break;
    }
    case W_TABLE:
        draw_table(w, c, t);
        break;
    case W_ICON:
        icon_draw(c, w->icon, r.x + (r.w - unit(t, w->icon_size)) / 2, r.y + (r.h - unit(t, w->icon_size)) / 2,
                  unit(t, w->icon_size), t->accent);
        break;
    case W_SEPARATOR:
        canvas_fill(c, r.w > r.h ? rect_make(r.x, r.y + r.h / 2, r.w, 1) : rect_make(r.x + r.w / 2, r.y, 1, r.h),
                    t->border);
        break;
    case W_SCROLL: {
        rect_t previous = canvas_clip(c, r);
        if (w->child_count)
            draw(w->children[0], c, t);
        if (w->content_height > r.h) {
            int32_t thumb = r.h * r.h / w->content_height;
            if (thumb < 16)
                thumb = 16;
            int32_t y = r.y + (int32_t)((int64_t)w->offset * (r.h - thumb) / (w->content_height - r.h));
            canvas_fill_rounded(c, rect_make(r.x + r.w - SCROLLBAR, y, SCROLLBAR - 1, thumb), 3, t->border);
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

void gui_set_text(widget_t *w, const char *text)
{
    free(w->text);
    w->text = copy_text(text);
    changed(w, true);
}

void gui_label_set_text(widget_t *w, const char *text)
{
    gui_set_text(w, text);
}

void gui_label_set_large(widget_t *w, bool large)
{
    w->large = large;
    changed(w, true);
}

void gui_label_set_dim(widget_t *w, bool dim)
{
    w->dim = dim;
    changed(w, false);
}

void gui_label_set_center(widget_t *w, bool center)
{
    w->center = center;
    changed(w, false);
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

void gui_button_set_flat(widget_t *w, bool flat)
{
    w->flat = flat;
    changed(w, true);
}

void gui_button_set_icon(widget_t *w, icon_t icon)
{
    w->icon = icon;
    changed(w, true);
}

void gui_button_set_selected(widget_t *w, bool selected)
{
    w->selected_flag = selected;
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

void gui_input_select_all(widget_t *w)
{
    w->all_selected = w->value[0] != '\0';
    changed(w, false);
}

void gui_input_set_password(widget_t *w, bool password)
{
    w->password = password;
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

void gui_checkbox_set_checked(widget_t *w, bool checked)
{
    w->checked = checked;
    changed(w, false);
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

void gui_list_on_select(widget_t *w, gui_callback_t fn, void *user)
{
    w->callback = fn;
    w->user = user;
}

void gui_list_set_rows(widget_t *w, int rows)
{
    w->rows = rows < 1 ? 1 : rows;
    changed(w, true);
}

widget_t *gui_table(int columns, const char *const *titles, const int32_t *widths)
{
    widget_t *w = new_widget(W_TABLE);
    if (!w)
        return NULL;
    w->columns = columns > MAX_COLUMNS ? MAX_COLUMNS : columns;
    for (int c = 0; c < w->columns; c++) {
        w->titles[c] = copy_text(titles[c]);
        w->widths[c] = widths ? widths[c] : 120;
    }
    w->focusable = true;
    w->rows = 8;
    return w;
}

void gui_table_clear(widget_t *w)
{
    for (int r = 0; r < w->row_count; r++)
        for (int c = 0; c < w->columns; c++)
            free(w->table_rows[r].cells[c]);
    w->row_count = 0;
    w->selected = -1;
    w->first_visible = 0;
    changed(w, false);
}

int gui_table_add(widget_t *w, icon_t icon, const char *const *cells)
{
    if (w->row_count == w->row_capacity) {
        int capacity = w->row_capacity ? w->row_capacity * 2 : 32;
        table_row_t *grown = realloc(w->table_rows, (size_t)capacity * sizeof(table_row_t));
        if (!grown)
            return -1;
        w->table_rows = grown;
        w->row_capacity = capacity;
    }
    table_row_t *row = &w->table_rows[w->row_count];
    memset(row, 0, sizeof(*row));
    row->icon = icon;
    for (int c = 0; c < w->columns; c++)
        row->cells[c] = copy_text(cells[c]);
    changed(w, false);
    return w->row_count++;
}

int gui_table_selected(widget_t *w)
{
    return w->selected;
}

int gui_table_rows(widget_t *w)
{
    return w->row_count;
}

const char *gui_table_cell(widget_t *w, int row, int column)
{
    if (row < 0 || row >= w->row_count || column < 0 || column >= w->columns)
        return NULL;
    return w->table_rows[row].cells[column];
}

void gui_table_on_select(widget_t *w, gui_callback_t fn, void *user)
{
    w->callback = fn;
    w->user = user;
}

void gui_table_on_activate(widget_t *w, gui_callback_t fn, void *user)
{
    w->activate_callback = fn;
    w->activate_user = user;
}

widget_t *gui_icon(icon_t icon, int32_t size)
{
    widget_t *w = new_widget(W_ICON);
    if (w) {
        w->icon = icon;
        w->icon_size = size;
    }
    return w;
}

widget_t *gui_separator(void)
{
    return new_widget(W_SEPARATOR);
}

widget_t *gui_scroll(widget_t *child)
{
    widget_t *w = new_widget(W_SCROLL);
    if (w)
        gui_add(w, child);
    return w;
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

void gui_list_select(widget_t *w, int index)
{
    list_select(w, index);
}

static void table_select(widget_t *w, int index)
{
    if (w->row_count == 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= w->row_count)
        index = w->row_count - 1;
    bool different = index != w->selected;
    w->selected = index;
    changed(w, false);
    if (different && w->callback)
        w->callback(w, w->user);
}

void gui_table_select(widget_t *w, int index)
{
    table_select(w, index);
}

static void table_activate(widget_t *w)
{
    if (w->selected >= 0 && w->activate_callback)
        w->activate_callback(w, w->activate_user);
}

static void input_key(widget_t *w, const wm_event_t *e)
{
    /* A marked text is replaced by typing or deleted by Backspace/Delete; other keys unmark it. */
    if (w->all_selected) {
        w->all_selected = false;
        bool typing = e->character >= 0x20 && e->character != 0x7F && !(e->modifiers & WM_MOD_CTRL);
        if (typing || e->key == JELLY_KEY_BACKSPACE || e->key == JELLY_KEY_DELETE) {
            w->value[0] = '\0';
            w->cursor = w->scroll = 0;
            if (!typing) {
                changed(w, false);
                return;
            }
        }
    }
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
    if (!w->visible)
        return;
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
    if (!w->visible || !rect_contains(w->rect, x, y))
        return NULL;
    for (int i = w->child_count - 1; i >= 0; i--) {
        widget_t *found = hit(w->children[i], x, y);
        if (found)
            return found;
    }
    return w->type == W_BOX || w->type == W_LABEL || w->type == W_SEPARATOR || w->type == W_ICON ? NULL : w;
}

static widget_t *scroll_parent(widget_t *w)
{
    for (; w; w = w->parent) {
        if (w->type == W_SCROLL)
            return w;
    }
    return NULL;
}

static void scroll_by(widget_t *scroll, int32_t delta)
{
    scroll->offset += delta;
    changed(scroll, true);
}

static void key_down(gui_window_t *win, const wm_event_t *e)
{
    widget_t *f = win->focus;
    if (e->key == JELLY_KEY_TAB && !(e->modifiers & (WM_MOD_CTRL | WM_MOD_ALT))) {
        move_focus(win, (e->modifiers & WM_MOD_SHIFT) ? -1 : 1);
        return;
    }
    bool handled = true;
    if (!f) {
        handled = false;
    } else {
        switch (f->type) {
        case W_BUTTON:
        case W_CHECKBOX:
            if (e->key == JELLY_KEY_ENTER || e->key == JELLY_KEY_SPACE || e->key == JELLY_KEY_KPENTER)
                activate(f);
            else
                handled = false;
            break;
        case W_INPUT:
            if (e->key == JELLY_KEY_ESCAPE)
                handled = false;
            else
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
            else
                handled = false;
            break;
        case W_TABLE: {
            int page = (f->rect.h / row_height(&win->theme)) - 2;
            if (e->key == JELLY_KEY_UP)
                table_select(f, f->selected - 1);
            else if (e->key == JELLY_KEY_DOWN)
                table_select(f, f->selected + 1);
            else if (e->key == JELLY_KEY_PAGEUP)
                table_select(f, f->selected - (page > 1 ? page : 1));
            else if (e->key == JELLY_KEY_PAGEDOWN)
                table_select(f, f->selected + (page > 1 ? page : 1));
            else if (e->key == JELLY_KEY_HOME)
                table_select(f, 0);
            else if (e->key == JELLY_KEY_END)
                table_select(f, f->row_count - 1);
            else if (e->key == JELLY_KEY_ENTER || e->key == JELLY_KEY_KPENTER)
                table_activate(f);
            else
                handled = false;
            break;
        }
        case W_CUSTOM:
            handled = f->custom.event && f->custom.event(f, e, f->custom.user);
            break;
        default:
            handled = false;
            break;
        }
    }
    if (!handled && win->on_key)
        win->on_key(win, e, win->on_key_user);
}

static void pointer(gui_window_t *win, const wm_event_t *e)
{
    widget_t *under = win->root ? hit(win->root, e->x, e->y) : NULL;
    widget_t *target = win->pressed ? win->pressed : under;
    const gui_theme_t *t = &win->theme;

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
        if (under && under->type == W_LIST)
            list_select(under, under->first_visible + (e->y - under->rect.y - unit(t, 2)) / row_height(t));
        if (under && under->type == W_TABLE) {
            int32_t y = e->y - under->rect.y - row_height(t) - unit(t, 2);
            if (y >= 0) {
                int row = under->first_visible + y / row_height(t);
                if (row < under->row_count) {
                    uint64_t now = jelly_clock_ns();
                    bool double_click = row == under->last_click_row && now - under->last_click < DOUBLE_CLICK_NS;
                    table_select(under, row);
                    under->last_click = now;
                    under->last_click_row = double_click ? -1 : row;
                    if (double_click)
                        table_activate(under);
                }
            }
        }
        if (under && under->type == W_INPUT) {
            int32_t cell = TEXT_CELL_WIDTH * t->scale;
            int32_t position = under->scroll + (e->x - under->rect.x - unit(t, 8) + cell / 2) / cell;
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
    } else if (e->type == WM_EVENT_MOUSE_WHEEL && under) {
        if (under->type == W_LIST) {
            int32_t first = under->first_visible - e->wheel;
            int32_t last = under->item_count - under->rows;
            under->first_visible = first > last ? (last > 0 ? last : 0) : first < 0 ? 0 : first;
            win->dirty = true;
        } else if (under->type == W_TABLE) {
            int visible = (under->rect.h - row_height(t)) / row_height(t);
            int32_t first = under->first_visible - e->wheel * 3;
            int32_t last = under->row_count - visible;
            under->first_visible = first > last ? (last > 0 ? last : 0) : first < 0 ? 0 : first;
            win->dirty = true;
        } else if (scroll_parent(under)) {
            scroll_by(scroll_parent(under), -e->wheel * row_height(t) * 2);
        }
    }
}

/* --- Windows ------------------------------------------------------------------- */

static void repaint(gui_window_t *win)
{
    window_t *s = win->surface;
    if (!s || s->frame_pending || !win->dirty || win->closing)
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

gui_window_t *gui_window_create_ex(gui_app_t *app, const char *title, int32_t x, int32_t y, int32_t width,
                                   int32_t height, uint32_t flags)
{
    gui_window_t *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;
    win->surface = wm_create_window_at(app->connection, x, y, width, height, title, flags);
    if (!win->surface) {
        free(win);
        return NULL;
    }
    win->surface->user = win;
    win->app = app;
    win->flags = flags;
    win->theme = gui_theme_from_settings(&app->settings);
    win->dirty = win->layout_dirty = true;
    win->next = app->windows;
    app->windows = win;
    app->window_count++;
    return win;
}

gui_window_t *gui_window_create(gui_app_t *app, const char *title, int32_t width, int32_t height)
{
    return gui_window_create_ex(app, title, 0, 0, width, height, 0);
}

void gui_window_destroy(gui_window_t *win)
{
    win->closing = true; /* the event loop frees it after the current event */
}

static void destroy_now(gui_window_t *win)
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
    if (win->root)
        free_widget(win->root);
    free(win);
    if (app->window_count == 0)
        gui_quit(app, app->exit_code);
}

static void reap_windows(gui_app_t *app)
{
    for (gui_window_t *w = app->windows, *next; w; w = next) {
        next = w->next;
        if (w->closing)
            destroy_now(w);
    }
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

void gui_window_keep_theme(gui_window_t *win, bool keep)
{
    win->keep_theme = keep;
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

void gui_window_on_key(gui_window_t *win, bool (*fn)(gui_window_t *, const wm_event_t *, void *), void *user)
{
    win->on_key = fn;
    win->on_key_user = user;
}

void gui_window_invalidate(gui_window_t *win)
{
    win->dirty = true;
}

window_t *gui_window_surface(gui_window_t *win)
{
    return win->surface;
}

gui_app_t *gui_window_app(gui_window_t *win)
{
    return win->app;
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
    gui_load_settings(&app->settings);
    return app;
}

wm_connection_t *gui_connection(gui_app_t *app)
{
    return app->connection;
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

int gui_add_timer(gui_app_t *app, uint64_t interval_ns, void (*fn)(void *user), void *user)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!app->timers[i].used) {
            app->timers[i] = (timer_t_){ true, interval_ns, jelly_clock_ns() + interval_ns, fn, user };
            return i;
        }
    }
    return -1;
}

void gui_remove_timer(gui_app_t *app, int timer)
{
    if (timer >= 0 && timer < MAX_TIMERS)
        app->timers[timer].used = false;
}

void gui_on_windows(gui_app_t *app, void (*fn)(void *user), void *user)
{
    app->on_windows = fn;
    app->on_windows_user = user;
    wm_subscribe_windows(app->connection);
}

void gui_on_notification(gui_app_t *app, void (*fn)(const char *title, const char *text, void *user), void *user)
{
    app->on_notification = fn;
    app->on_notification_user = user;
}

void gui_on_settings(gui_app_t *app, void (*fn)(void *user), void *user)
{
    app->on_settings = fn;
    app->on_settings_user = user;
}

static void apply_settings(gui_app_t *app)
{
    gui_load_settings(&app->settings);
    for (gui_window_t *w = app->windows; w; w = w->next) {
        if (!w->keep_theme)
            gui_window_set_theme(w, gui_theme_from_settings(&app->settings));
    }
    if (app->on_settings)
        app->on_settings(app->on_settings_user);
}

static void dispatch(gui_app_t *app, const wm_event_t *e, window_t *surface)
{
    if (e->type == WM_EVENT_WINDOWS) {
        if (app->on_windows)
            app->on_windows(app->on_windows_user);
        return;
    }
    if (e->type == WM_EVENT_NOTIFICATION) {
        const char *title, *text;
        wm_last_notification(app->connection, &title, &text);
        if (app->on_notification)
            app->on_notification(title, text, app->on_notification_user);
        return;
    }
    if (e->type == WM_EVENT_SETTINGS) {
        apply_settings(app);
        return;
    }

    gui_window_t *win = surface ? surface->user : NULL;
    if (!win || win->closing)
        return;
    switch (e->type) {
    case WM_EVENT_FRAME:
        break; /* repaint() may send the next frame */
    case WM_EVENT_CLOSE:
        if (win->on_close)
            win->on_close(NULL, win->on_close_user);
        else
            gui_window_destroy(win);
        return;
    case WM_EVENT_RESIZE:
        if (!wm_resize_window(win->surface, e->x, e->y))
            win->dirty = win->layout_dirty = true;
        break;
    case WM_EVENT_FOCUS_IN:
    case WM_EVENT_FOCUS_OUT:
        win->active = e->type == WM_EVENT_FOCUS_IN;
        win->dirty = true;
        /* A popup that loses the focus is dismissed. */
        if (!win->active && (win->flags & WM_WINDOW_POPUP)) {
            if (win->on_close)
                win->on_close(NULL, win->on_close_user);
            else
                gui_window_destroy(win);
        }
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
}

static void run_timers(gui_app_t *app, uint64_t *next)
{
    uint64_t now = jelly_clock_ns();
    *next = UINT64_MAX;
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_t_ *t = &app->timers[i];
        if (!t->used)
            continue;
        if (now >= t->next) {
            t->next = now + t->interval;
            t->fn(t->user);
        }
        if (t->used && t->next < *next)
            *next = t->next;
    }
}

int gui_run(gui_app_t *app)
{
    app->quit = false; /* gui_run may be called again after gui_quit (login screen) */
    while (!app->quit) {
        /* Events first: some may already be queued in the client library, where no wait would see them. */
        wm_event_t e;
        window_t *surface;
        int result;
        while (!app->quit && (result = wm_next_event(app->connection, &e, &surface, 0)) > 0) {
            dispatch(app, &e, surface);
            reap_windows(app);
        }
        if (app->quit)
            break;
        if (result < 0)
            return 1; /* the display server is gone */

        uint64_t next_timer;
        run_timers(app, &next_timer);
        reap_windows(app);
        if (app->quit)
            break;
        for (gui_window_t *w = app->windows; w; w = w->next)
            repaint(w);

        jelly_handle_t handles[1 + MAX_WATCHES];
        uint32_t count = 0, index = 0;
        handles[count++] = wm_handle(app->connection);
        for (int i = 0; i < app->watch_count; i++)
            handles[count++] = app->watches[i].handle;
        uint64_t now = jelly_clock_ns();
        uint64_t timeout = next_timer == UINT64_MAX ? JELLY_WAIT_FOREVER : next_timer > now ? next_timer - now : 0;
        status_t status = jelly_wait_many(handles, count, timeout, &index);
        if (status == STATUS_SUCCESS && index > 0)
            app->watches[index - 1].ready(app->watches[index - 1].user);
        else if (STATUS_IS_ERROR(status) && status != STATUS_TIMEOUT)
            return 1;
        reap_windows(app);
    }
    return app->exit_code;
}

/* --- Menus ----------------------------------------------------------------------- */

typedef struct {
    gui_window_t          *window;
    const gui_menu_item_t *items;
    gui_menu_item_t        copy[32];
    int                    count;
    int                    hover;
    bool                   done;
    void                 (*chosen)(int index, void *user);
    void                  *user;
} menu_t;

static bool is_separator(const gui_menu_item_t *item)
{
    return !item->text || !strcmp(item->text, "-");
}

static int32_t menu_item_height(const gui_theme_t *t, const gui_menu_item_t *item)
{
    return is_separator(item) ? unit(t, 9) : row_height(t) + unit(t, 4);
}

static void menu_finish(menu_t *m, int index)
{
    if (m->done)
        return;
    m->done = true;
    gui_window_destroy(m->window);
    if (m->chosen)
        m->chosen(index, m->user);
}

static void menu_draw(widget_t *w, canvas_t *c, rect_t area, void *user)
{
    menu_t *m = user;
    const gui_theme_t *t = gui_window_theme(w->window);
    canvas_fill(c, area, t->surface);
    canvas_outline(c, area, t->border);
    int32_t y = area.y + unit(t, 4);
    for (int i = 0; i < m->count; i++) {
        const gui_menu_item_t *item = &m->copy[i];
        int32_t h = menu_item_height(t, item);
        if (is_separator(item)) {
            canvas_fill(c, rect_make(area.x + unit(t, 8), y + h / 2, area.w - unit(t, 16), 1), t->border);
        } else {
            if (i == m->hover)
                canvas_fill_rounded(c, rect_make(area.x + unit(t, 4), y, area.w - unit(t, 8), h), unit(t, 5),
                                    t->accent);
            int32_t icon = line_height(t);
            if (item->icon)
                icon_draw(c, item->icon, area.x + unit(t, 12), y + (h - icon) / 2, icon,
                          i == m->hover ? t->accent_text : t->accent);
            canvas_text(c, area.x + unit(t, 12) + icon + unit(t, 10), y + (h - line_height(t)) / 2, item->text,
                        i == m->hover ? t->accent_text : t->text, t->scale);
        }
        y += h;
    }
}

static int menu_index_at(menu_t *m, const gui_theme_t *t, int32_t y)
{
    int32_t top = unit(t, 4);
    for (int i = 0; i < m->count; i++) {
        int32_t h = menu_item_height(t, &m->copy[i]);
        if (y >= top && y < top + h)
            return is_separator(&m->copy[i]) ? -1 : i;
        top += h;
    }
    return -1;
}

static bool menu_event(widget_t *w, const wm_event_t *e, void *user)
{
    menu_t *m = user;
    const gui_theme_t *t = gui_window_theme(w->window);
    if (e->type == WM_EVENT_MOUSE_MOVE) {
        int hover = menu_index_at(m, t, e->y);
        if (hover != m->hover) {
            m->hover = hover;
            gui_custom_redraw(w);
        }
    } else if (e->type == WM_EVENT_MOUSE_UP && e->button == JELLY_BUTTON_LEFT) {
        int index = menu_index_at(m, t, e->y);
        if (index >= 0)
            menu_finish(m, index);
    } else if (e->type == WM_EVENT_KEY_DOWN) {
        int step = e->key == JELLY_KEY_DOWN ? 1 : e->key == JELLY_KEY_UP ? -1 : 0;
        if (step) {
            int next = m->hover;
            for (int tries = 0; tries < m->count; tries++) {
                next = (next + step + m->count) % m->count;
                if (!is_separator(&m->copy[next]))
                    break;
            }
            m->hover = next;
            gui_custom_redraw(w);
        } else if ((e->key == JELLY_KEY_ENTER || e->key == JELLY_KEY_SPACE) && m->hover >= 0) {
            menu_finish(m, m->hover);
        } else if (e->key == JELLY_KEY_ESCAPE) {
            menu_finish(m, -1);
        }
    }
    return true;
}

static void menu_dismissed(widget_t *widget, void *user)
{
    (void)widget;
    menu_finish(user, -1);
}

gui_window_t *gui_menu_show(gui_app_t *app, int32_t x, int32_t y, bool above, const gui_menu_item_t *items,
                            int count, void (*chosen)(int index, void *user), void *user)
{
    static menu_t menus[4];
    static int next_menu;
    menu_t *m = &menus[next_menu++ % 4]; /* menus are short-lived; a few slots suffice */
    gui_theme_t theme = gui_theme_from_settings(&app->settings);
    int32_t width = unit(&theme, 120), height = unit(&theme, 8);

    memset(m, 0, sizeof(*m));
    m->count = count > 32 ? 32 : count;
    for (int i = 0; i < m->count; i++) {
        m->copy[i] = items[i];
        height += menu_item_height(&theme, &items[i]);
        if (!is_separator(&items[i])) {
            int32_t w = text_width(items[i].text, theme.scale) + line_height(&theme) + unit(&theme, 40);
            if (w > width)
                width = w;
        }
    }
    m->hover = -1;
    m->chosen = chosen;
    m->user = user;
    if (above)
        y -= height;

    m->window = gui_window_create_ex(app, "menu", x, y, width, height, WM_WINDOW_POPUP | WM_WINDOW_POSITIONED);
    if (!m->window)
        return NULL;
    gui_custom_t custom = { .draw = menu_draw, .event = menu_event, .user = m, .min_width = width, .min_height = height };
    widget_t *view = gui_custom(&custom);
    gui_set_expand(view, true);
    widget_t *root = gui_vbox(0);
    gui_add(root, view);
    gui_window_set_root(m->window, root);
    gui_window_focus(m->window, view);
    gui_window_on_close(m->window, menu_dismissed, m);
    return m->window;
}

/* --- Dialogs -------------------------------------------------------------------- */

typedef struct {
    gui_window_t *window;
    widget_t     *input;
    void        (*message_done)(int button, void *user);
    void        (*input_done)(const char *text, void *user);
    void         *user;
    bool          finished;
} dialog_t;

static void dialog_finish(dialog_t *d, int button)
{
    if (d->finished)
        return;
    d->finished = true;
    if (d->input_done)
        d->input_done(button == 0 ? gui_input_text(d->input) : NULL, d->user);
    else if (d->message_done)
        d->message_done(button, d->user);
    gui_window_destroy(d->window);
}

/* Each dialog button keeps its index in activate_user. */
static void dialog_button(widget_t *button, void *user)
{
    dialog_finish(user, (int)(intptr_t)button->activate_user);
}

static void dialog_closed(widget_t *widget, void *user)
{
    (void)widget;
    dialog_finish(user, -1);
}

static bool dialog_key(gui_window_t *win, const wm_event_t *e, void *user)
{
    (void)win;
    if (e->key == JELLY_KEY_ESCAPE) {
        dialog_finish(user, -1);
        return true;
    }
    return false;
}

static void dialog_submit(widget_t *input, void *user)
{
    (void)input;
    dialog_finish(user, 0);
}

static gui_window_t *dialog_window(gui_app_t *app, dialog_t *d, const char *title, const char *text,
                                   const char *const *buttons, int count, bool with_input, const char *initial)
{
    int32_t screen_w, screen_h;
    gui_theme_t theme = gui_theme_from_settings(&app->settings);
    wm_screen_size(app->connection, &screen_w, &screen_h);
    int32_t width = unit(&theme, 360), height = unit(&theme, with_input ? 150 : 120);
    int32_t text_w = text_width(text, theme.scale) + unit(&theme, 48);
    if (text_w > width)
        width = text_w < screen_w - 40 ? text_w : screen_w - 40;

    d->window = gui_window_create_ex(app, title, (screen_w - width) / 2, (screen_h - height) / 2 - 40, width, height,
                                     WM_WINDOW_POSITIONED);
    if (!d->window)
        return NULL;
    widget_t *root = gui_vbox(12);
    gui_box_set_padding(root, 16);
    gui_add(root, gui_label(text));
    if (with_input) {
        d->input = gui_input("", dialog_submit, d);
        gui_input_set_text(d->input, initial ? initial : "");
        gui_input_select_all(d->input);
        gui_add(root, d->input);
    }
    widget_t *spacer = gui_label("");
    gui_set_expand(spacer, true);
    gui_add(root, spacer);
    widget_t *row = gui_hbox(8);
    widget_t *fill = gui_label("");
    gui_set_expand(fill, true);
    gui_add(row, fill);
    for (int i = 0; i < count; i++) {
        widget_t *b = gui_button(buttons[i], dialog_button, d);
        b->activate_user = (void *)(intptr_t)i;
        if (i == 0)
            gui_button_set_primary(b, true);
        gui_add(row, b);
    }
    gui_add(root, row);
    gui_window_set_root(d->window, root);
    gui_window_on_close(d->window, dialog_closed, d);
    gui_window_on_key(d->window, dialog_key, d);
    if (d->input)
        gui_window_focus(d->window, d->input);
    return d->window;
}

gui_window_t *gui_dialog_message(gui_app_t *app, const char *title, const char *text, const char *const *buttons,
                                 int count, void (*done)(int button, void *user), void *user)
{
    static dialog_t dialogs[4];
    static int next;
    dialog_t *d = &dialogs[next++ % 4];
    memset(d, 0, sizeof(*d));
    d->message_done = done;
    d->user = user;
    static const char *const ok[] = { "OK" };
    return dialog_window(app, d, title, text, count ? buttons : ok, count ? (count > 3 ? 3 : count) : 1, false, NULL);
}

gui_window_t *gui_dialog_input(gui_app_t *app, const char *title, const char *prompt, const char *initial,
                               void (*done)(const char *text, void *user), void *user)
{
    static dialog_t dialogs[4];
    static int next;
    dialog_t *d = &dialogs[next++ % 4];
    static const char *const buttons[] = { "OK", "Cancel" };
    memset(d, 0, sizeof(*d));
    d->input_done = done;
    d->user = user;
    return dialog_window(app, d, title, prompt, buttons, 2, true, initial);
}
