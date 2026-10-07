/*
 * viewer FILE: shows a text file (read-only) in a resizable window.
 *
 * Scrolling with the wheel, the arrow keys, Page Up/Down, Home and End.
 * Up to 1 MiB is loaded; bytes that are not printable show as '.'.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/input.h>

#include "graphics/gui/gui.h"

#define MAX_SIZE (1024 * 1024)
#define PADDING  8

static gui_app_t *app;
static widget_t *view;
static char *text;
static char **lines;
static int line_count, first_line;

static int visible_lines(void)
{
    rect_t r = gui_widget_rect(view);
    int n = (r.h - 2 * PADDING) / TEXT_CELL_HEIGHT;
    return n > 0 ? n : 1;
}

static void scroll_to(int line)
{
    int last = line_count - visible_lines();
    if (line > last)
        line = last;
    if (line < 0)
        line = 0;
    if (line != first_line) {
        first_line = line;
        gui_custom_redraw(view);
    }
}

static void draw(widget_t *widget, canvas_t *canvas, rect_t area, void *user)
{
    (void)user;
    const gui_theme_t *t = gui_window_theme(gui_widget_window(widget));
    canvas_fill(canvas, area, t->surface);
    for (int i = 0; i < visible_lines() && first_line + i < line_count; i++)
        canvas_text(canvas, area.x + PADDING, area.y + PADDING + i * TEXT_CELL_HEIGHT, lines[first_line + i], t->text, 1);
    if (line_count > visible_lines()) {
        int32_t thumb = area.h * visible_lines() / line_count;
        if (thumb < 16)
            thumb = 16;
        int32_t y = area.y + (area.h - thumb) * first_line / (line_count - visible_lines());
        canvas_fill_rounded(canvas, rect_make(area.x + area.w - 6, y, 5, thumb), 3, t->border);
    }
}

static bool event(widget_t *widget, const wm_event_t *e, void *user)
{
    (void)widget;
    (void)user;
    if (e->type == WM_EVENT_MOUSE_WHEEL) {
        scroll_to(first_line - e->wheel * 3);
        return true;
    }
    if (e->type != WM_EVENT_KEY_DOWN)
        return false;
    switch (e->key) {
    case JELLY_KEY_UP:       scroll_to(first_line - 1); return true;
    case JELLY_KEY_DOWN:     scroll_to(first_line + 1); return true;
    case JELLY_KEY_PAGEUP:   scroll_to(first_line - visible_lines() + 1); return true;
    case JELLY_KEY_PAGEDOWN: scroll_to(first_line + visible_lines() - 1); return true;
    case JELLY_KEY_HOME:     scroll_to(0); return true;
    case JELLY_KEY_END:      scroll_to(line_count); return true;
    case JELLY_KEY_ESCAPE:   gui_quit(app, 0); return true;
    }
    return false;
}

static int load(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    text = malloc(MAX_SIZE + 1);
    size_t size = text ? fread(text, 1, MAX_SIZE, file) : 0;
    fclose(file);
    if (!text)
        return -1;
    text[size] = '\0';

    int capacity = 64;
    lines = malloc((size_t)capacity * sizeof(char *));
    char *start = text;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || text[i] == '\n') {
            if (i == size && start == text + size && line_count)
                break;
            text[i] = '\0';
            if (line_count == capacity) {
                capacity *= 2;
                lines = realloc(lines, (size_t)capacity * sizeof(char *));
            }
            lines[line_count++] = start;
            start = text + i + 1;
        } else if ((unsigned char)text[i] < 0x20 && text[i] != '\t') {
            text[i] = '.';
        } else if (text[i] == '\t') {
            text[i] = ' ';
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: viewer FILE\n");
        return 2;
    }
    if (load(argv[1])) {
        fprintf(stderr, "viewer: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    app = gui_init();
    if (!app) {
        fprintf(stderr, "viewer: no display server\n");
        return 1;
    }
    const char *name = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    gui_window_t *window = gui_window_create_ex(app, name, 0, 0, 640, 420, WM_WINDOW_RESIZABLE);
    if (!window)
        return 1;
    gui_custom_t custom = { .draw = draw, .event = event, .min_width = 1, .min_height = 1 };
    view = gui_custom(&custom);
    gui_set_expand(view, true);
    widget_t *root = gui_vbox(0);
    gui_add(root, view);
    gui_window_set_root(window, root);
    gui_window_focus(window, view);
    printf("viewer: %s (%d lines)\n", argv[1], line_count);
    fflush(stdout);
    return gui_run(app);
}
