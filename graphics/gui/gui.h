/*
 * JellyOS GUI toolkit (README section 36).
 *
 * Controls: window, box layout (vertical/horizontal), label (text), button,
 * text input (also for passwords), checkbox, list, table, icon, separator,
 * scroll view, menu (popup) and dialogs. Themes: light and dark, with an
 * integer scale for accessibility; both come from the desktop settings
 * (~/.config/desktop.conf) and follow changes while the program runs.
 * Keyboard navigation: Tab / Shift+Tab move the focus, Enter and Space
 * activate, arrows move in lists, tables and menus, Escape closes menus and
 * dialogs.
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
 */

#ifndef GRAPHICS_GUI_GUI_H
#define GRAPHICS_GUI_GUI_H

#include "graphics/core/canvas.h"
#include "graphics/core/icons.h"
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

/* Desktop settings (~/.config/desktop.conf, or /etc/desktop.conf as the default). */
typedef struct {
    bool    dark;
    int32_t scale;
    char    keymap[16];
} gui_settings_t;

void        gui_load_settings(gui_settings_t *settings);
int         gui_save_settings(const gui_settings_t *settings);
gui_theme_t gui_theme_from_settings(const gui_settings_t *settings);

typedef void (*gui_callback_t)(widget_t *widget, void *user);

/* --- Application and windows ------------------------------------------------- */

/* Connect to the display server; NULL if there is none. */
gui_app_t    *gui_init(void);
/* Run until gui_quit() or the last window is closed; returns the exit code. */
int           gui_run(gui_app_t *app);
void          gui_quit(gui_app_t *app, int code);
wm_connection_t *gui_connection(gui_app_t *app);
/* Watch another handle in the event loop (e.g. an event signaled by a thread). */
void          gui_watch(gui_app_t *app, jelly_handle_t handle, void (*ready)(void *user), void *user);
/* Call `fn` every interval_ns; returns a timer number for gui_remove_timer, or -1. */
int           gui_add_timer(gui_app_t *app, uint64_t interval_ns, void (*fn)(void *user), void *user);
void          gui_remove_timer(gui_app_t *app, int timer);
/* Desktop shell hooks: the window list changed, a notification arrived. */
void          gui_on_windows(gui_app_t *app, void (*fn)(void *user), void *user);
void          gui_on_notification(gui_app_t *app, void (*fn)(const char *title, const char *text, void *user),
                                  void *user);
/* Settings changed (after the toolkit applied the new theme). */
void          gui_on_settings(gui_app_t *app, void (*fn)(void *user), void *user);
/* The screen changed its size (wm_screen_size() has the new one). */
void          gui_on_screen(gui_app_t *app, void (*fn)(void *user), void *user);

gui_window_t *gui_window_create(gui_app_t *app, const char *title, int32_t width, int32_t height);
/* Resizable, panel, popup, fixed position: WM_WINDOW_* flags; x, y with WM_WINDOW_POSITIONED. */
gui_window_t *gui_window_create_ex(gui_app_t *app, const char *title, int32_t x, int32_t y, int32_t width,
                                   int32_t height, uint32_t flags);
void          gui_window_destroy(gui_window_t *window);
void          gui_window_set_root(gui_window_t *window, widget_t *root);
void          gui_window_set_title(gui_window_t *window, const char *title);
void          gui_window_set_theme(gui_window_t *window, gui_theme_t theme);
/* Keep a theme of the program's own choice when the settings change. */
void          gui_window_keep_theme(gui_window_t *window, bool keep);
const gui_theme_t *gui_window_theme(gui_window_t *window);
/* Default: destroy the window (and quit with the last one). */
void          gui_window_on_close(gui_window_t *window, gui_callback_t callback, void *user);
/* Keys the focused widget did not use (shortcuts); return true if handled. */
void          gui_window_on_key(gui_window_t *window, bool (*fn)(gui_window_t *, const wm_event_t *, void *), void *user);
/* WM_EVENT_GAMEPAD_BUTTON and WM_EVENT_GAMEPAD_AXIS while the window has the focus */
void          gui_window_on_gamepad(gui_window_t *window, void (*fn)(gui_window_t *, const wm_event_t *, void *),
                                    void *user);
/* Mark the window for repainting (widgets do this themselves when they change). */
void          gui_window_invalidate(gui_window_t *window);
void          gui_window_focus(gui_window_t *window, widget_t *widget);
window_t     *gui_window_surface(gui_window_t *window);
gui_app_t    *gui_window_app(gui_window_t *window);

/* A widget that draws itself (terminal, viewer): called with the canvas and its rectangle. */
typedef struct {
    void (*draw)(widget_t *widget, canvas_t *canvas, rect_t area, void *user);
    bool (*event)(widget_t *widget, const wm_event_t *event, void *user); /* widget coordinates */
    void *user;
    int32_t min_width, min_height;
} gui_custom_t;

/* --- Widgets ------------------------------------------------------------------ */

widget_t *gui_vbox(int32_t spacing);
widget_t *gui_hbox(int32_t spacing);
void      gui_box_set_padding(widget_t *box, int32_t padding);
void      gui_box_set_background(widget_t *box, bool surface); /* draw the theme's surface color behind it */
void      gui_add(widget_t *box, widget_t *child);
/* Remove and free all children. */
void      gui_box_clear(widget_t *box);
/* Give the widget the extra space of its box. */
void      gui_set_expand(widget_t *widget, bool expand);
void      gui_set_visible(widget_t *widget, bool visible);
void      gui_set_min_size(widget_t *widget, int32_t width, int32_t height);

widget_t *gui_label(const char *text);
void      gui_label_set_text(widget_t *label, const char *text);
void      gui_label_set_large(widget_t *label, bool large); /* headline: double size */
void      gui_label_set_dim(widget_t *label, bool dim);     /* secondary text */
void      gui_label_set_center(widget_t *label, bool center);

widget_t *gui_button(const char *text, gui_callback_t on_click, void *user);
void      gui_button_set_primary(widget_t *button, bool primary);
void      gui_button_set_flat(widget_t *button, bool flat);       /* no frame (taskbar, toolbars) */
void      gui_button_set_icon(widget_t *button, icon_t icon);
void      gui_button_set_selected(widget_t *button, bool selected); /* highlighted (active window) */
void      gui_set_text(widget_t *widget, const char *text);       /* label, button, checkbox */
void      gui_set_user(widget_t *widget, void *user);
void     *gui_get_user(widget_t *widget);

widget_t *gui_input(const char *placeholder, gui_callback_t on_submit, void *user);
const char *gui_input_text(widget_t *input);
void      gui_input_set_text(widget_t *input, const char *text);
void      gui_input_set_password(widget_t *input, bool password);
/* Mark the whole text: the next typed character replaces it (dialogs with a suggestion). */
void      gui_input_select_all(widget_t *input);

widget_t *gui_checkbox(const char *text, bool checked, gui_callback_t on_toggle, void *user);
bool      gui_checkbox_checked(widget_t *checkbox);
void      gui_checkbox_set_checked(widget_t *checkbox, bool checked);

widget_t *gui_list(gui_callback_t on_select, void *user);
void      gui_list_add(widget_t *list, const char *item);
void      gui_list_clear(widget_t *list);
int       gui_list_selected(widget_t *list); /* -1 if none */
void      gui_list_select(widget_t *list, int index);
const char *gui_list_item(widget_t *list, int index);
void      gui_list_set_rows(widget_t *list, int rows); /* visible rows (preferred height) */
void      gui_list_on_select(widget_t *list, gui_callback_t fn, void *user);

/* Table: columns with titles and widths (the last one takes the rest), rows with an icon. */
widget_t *gui_table(int columns, const char *const *titles, const int32_t *widths);
void      gui_table_clear(widget_t *table);
int       gui_table_add(widget_t *table, icon_t icon, const char *const *cells);
int       gui_table_selected(widget_t *table);
void      gui_table_select(widget_t *table, int row);
const char *gui_table_cell(widget_t *table, int row, int column);
int       gui_table_rows(widget_t *table);
void      gui_table_on_select(widget_t *table, gui_callback_t fn, void *user);
/* Double-click or Enter on a row */
void      gui_table_on_activate(widget_t *table, gui_callback_t fn, void *user);

widget_t *gui_icon(icon_t icon, int32_t size);
widget_t *gui_separator(void);
/* Scrolls its child vertically when the child is taller than the view. */
widget_t *gui_scroll(widget_t *child);

widget_t *gui_custom(const gui_custom_t *custom);
void      gui_custom_redraw(widget_t *widget);
rect_t    gui_widget_rect(widget_t *widget);
gui_window_t *gui_widget_window(widget_t *widget);

/* --- Menus and dialogs ---------------------------------------------------------- */

typedef struct {
    const char *text;    /* NULL or "-" draws a separator */
    icon_t      icon;
} gui_menu_item_t;

/* A popup menu at screen position x, y (bottom-left corner if `above`); chosen(-1) when dismissed. */
gui_window_t *gui_menu_show(gui_app_t *app, int32_t x, int32_t y, bool above, const gui_menu_item_t *items,
                            int count, void (*chosen)(int index, void *user), void *user);

/* Message box with up to 3 buttons; done(index of the button, -1 if closed). */
gui_window_t *gui_dialog_message(gui_app_t *app, const char *title, const char *text, const char *const *buttons,
                                 int count, void (*done)(int button, void *user), void *user);
/* Text question; done(text, or NULL when cancelled). */
gui_window_t *gui_dialog_input(gui_app_t *app, const char *title, const char *prompt, const char *initial,
                               void (*done)(const char *text, void *user), void *user);

#endif
