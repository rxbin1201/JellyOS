/*
 * JellyOS GUI toolkit (README section 36).
 *
 * Controls: window, box layout (vertical/horizontal), label (text), button,
 * text input, checkbox and list. Themes: light and dark, with an integer
 * scale for accessibility. Keyboard navigation: Tab / Shift+Tab move the
 * focus, Enter and Space activate, arrows move in lists, the text input
 * edits with the usual keys.
 *
 * A program builds a widget tree, puts it into a window and runs the event
 * loop:
 *
 *     gui_app_t *app = gui_init();
 *     gui_window_t *win = gui_window_create(app, "Hello", 320, 200);
 *     widget_t *box = gui_vbox(8);
 *     gui_add(box, gui_label("Hello, JellyOS"));
 *     gui_add(box, gui_button("Quit", on_quit, app));
 *     gui_window_set_root(win, box);
 *     return gui_run(app);
 *
 * Not yet: tables, menus, dialogs, scroll views and icons (README section 36
 * lists them; they arrive with the desktop in Phase 10).
 */

#ifndef GRAPHICS_GUI_GUI_H
#define GRAPHICS_GUI_GUI_H

#include "graphics/core/canvas.h"
#include "graphics/window/window.h"

typedef struct gui_app gui_app_t;
typedef struct gui_window gui_window_t;
typedef struct widget widget_t;

typedef struct {
    bool    dark;
    int32_t scale;       /* 1 = normal, 2 = large */
    color_t background;  /* window */
    color_t surface;     /* controls */
    color_t surface_hover;
    color_t surface_pressed;
    color_t text;
    color_t text_dim;
    color_t accent;      /* JellyOS violet */
    color_t accent_text;
    color_t border;
    color_t focus;
    color_t selection;
} gui_theme_t;

gui_theme_t gui_theme_light(int32_t scale);
gui_theme_t gui_theme_dark(int32_t scale);

typedef void (*gui_callback_t)(widget_t *widget, void *user);

/* --- Application and windows ------------------------------------------------- */

/* Connect to the display server; NULL if there is none. */
gui_app_t    *gui_init(void);
/* Run until gui_quit() or the last window is closed; returns the exit code. */
int           gui_run(gui_app_t *app);
void          gui_quit(gui_app_t *app, int code);
/* Watch another handle in the event loop (e.g. an event signaled by a thread). */
void          gui_watch(gui_app_t *app, jelly_handle_t handle, void (*ready)(void *user), void *user);

gui_window_t *gui_window_create(gui_app_t *app, const char *title, int32_t width, int32_t height);
void          gui_window_destroy(gui_window_t *window);
void          gui_window_set_root(gui_window_t *window, widget_t *root);
void          gui_window_set_title(gui_window_t *window, const char *title);
void          gui_window_set_theme(gui_window_t *window, gui_theme_t theme);
const gui_theme_t *gui_window_theme(gui_window_t *window);
/* Default: destroy the window (and quit with the last one). */
void          gui_window_on_close(gui_window_t *window, gui_callback_t callback, void *user);
/* Mark the window for repainting (widgets do this themselves when they change). */
void          gui_window_invalidate(gui_window_t *window);
void          gui_window_focus(gui_window_t *window, widget_t *widget);

/* A widget that draws itself (terminal, canvas apps): called with the canvas and its rectangle. */
typedef struct {
    void (*draw)(widget_t *widget, canvas_t *canvas, rect_t area, void *user);
    bool (*event)(widget_t *widget, const wm_event_t *event, void *user); /* content coordinates */
    void *user;
    int32_t min_width, min_height;
} gui_custom_t;

/* --- Widgets ------------------------------------------------------------------ */

widget_t *gui_vbox(int32_t spacing);
widget_t *gui_hbox(int32_t spacing);
void      gui_box_set_padding(widget_t *box, int32_t padding);
void      gui_add(widget_t *box, widget_t *child);
/* Give the widget the extra space of its box. */
void      gui_set_expand(widget_t *widget, bool expand);

widget_t *gui_label(const char *text);
void      gui_label_set_text(widget_t *label, const char *text);
void      gui_label_set_large(widget_t *label, bool large); /* headline: double size */

widget_t *gui_button(const char *text, gui_callback_t on_click, void *user);
void      gui_button_set_primary(widget_t *button, bool primary);

widget_t *gui_input(const char *placeholder, gui_callback_t on_submit, void *user);
const char *gui_input_text(widget_t *input);
void      gui_input_set_text(widget_t *input, const char *text);

widget_t *gui_checkbox(const char *text, bool checked, gui_callback_t on_toggle, void *user);
bool      gui_checkbox_checked(widget_t *checkbox);

widget_t *gui_list(gui_callback_t on_select, void *user);
void      gui_list_add(widget_t *list, const char *item);
void      gui_list_clear(widget_t *list);
int       gui_list_selected(widget_t *list); /* -1 if none */
const char *gui_list_item(widget_t *list, int index);
void      gui_list_set_rows(widget_t *list, int rows); /* visible rows (preferred height) */

widget_t *gui_custom(const gui_custom_t *custom);
void      gui_custom_redraw(widget_t *widget);
rect_t    gui_widget_rect(widget_t *widget);
gui_window_t *gui_widget_window(widget_t *widget);

#endif
