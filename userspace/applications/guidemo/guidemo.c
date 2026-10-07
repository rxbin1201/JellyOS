/*
 * guidemo: the GUI toolkit's controls in one window.
 *
 * Every action is also printed on stdout, so tests can follow it.
 */

#include <stdio.h>
#include <string.h>

#include "graphics/gui/gui.h"

static gui_app_t *app;
static gui_window_t *window;
static widget_t *name_input, *greeting, *counter_button, *dark_mode, *large_text;
static int count;

static void greet(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    char text[300];
    const char *name = gui_input_text(name_input);
    snprintf(text, sizeof(text), "Hello, %s!", name[0] ? name : "stranger");
    gui_label_set_text(greeting, text);
    printf("guidemo: greeted '%s'\n", name);
    fflush(stdout);
}

static void count_up(widget_t *widget, void *user)
{
    (void)user;
    char text[32];
    snprintf(text, sizeof(text), "Clicked %d", ++count);
    gui_label_set_text(widget, text); /* a button's text is set like a label's */
    printf("guidemo: clicked %d\n", count);
    fflush(stdout);
}

static void apply_theme(void)
{
    int32_t scale = gui_checkbox_checked(large_text) ? 2 : 1;
    gui_window_set_theme(window, gui_checkbox_checked(dark_mode) ? gui_theme_dark(scale) : gui_theme_light(scale));
}

static void toggle_theme(widget_t *widget, void *user)
{
    (void)user;
    apply_theme();
    printf("guidemo: %s %s\n", widget == dark_mode ? "dark mode" : "large text",
           gui_checkbox_checked(widget) ? "on" : "off");
    fflush(stdout);
}

static void selected(widget_t *list, void *user)
{
    (void)user;
    printf("guidemo: selected '%s'\n", gui_list_item(list, gui_list_selected(list)));
    fflush(stdout);
}

static void quit(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    printf("guidemo: bye\n");
    fflush(stdout);
    gui_quit(app, 0);
}

int main(void)
{
    app = gui_init();
    if (!app) {
        fprintf(stderr, "guidemo: no display server\n");
        return 1;
    }
    window = gui_window_create(app, "JellyOS GUI demo", 440, 440);
    if (!window) {
        fprintf(stderr, "guidemo: cannot create a window\n");
        return 1;
    }

    widget_t *root = gui_vbox(10);
    gui_box_set_padding(root, 18);

    widget_t *title = gui_label("Hello, JellyOS!");
    gui_label_set_large(title, true);
    gui_add(root, title);
    gui_add(root, gui_label("Type your name and press Greet:"));

    widget_t *row = gui_hbox(8);
    name_input = gui_input("Your name", greet, NULL);
    gui_set_expand(name_input, true);
    gui_add(row, name_input);
    widget_t *greet_button = gui_button("Greet", greet, NULL);
    gui_button_set_primary(greet_button, true);
    gui_add(row, greet_button);
    gui_add(root, row);

    greeting = gui_label("");
    gui_add(root, greeting);

    widget_t *options = gui_hbox(16);
    counter_button = gui_button("Click me", count_up, NULL);
    gui_add(options, counter_button);
    dark_mode = gui_checkbox("Dark mode", false, toggle_theme, NULL);
    gui_add(options, dark_mode);
    large_text = gui_checkbox("Large", false, toggle_theme, NULL);
    gui_add(options, large_text);
    gui_add(root, options);

    gui_add(root, gui_label("JellyOS layers:"));
    widget_t *list = gui_list(selected, NULL);
    const char *layers[] = { "Boot manager", "Kernel", "Drivers", "File systems", "Network", "Graphics", "Desktop" };
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); i++)
        gui_list_add(list, layers[i]);
    gui_list_set_rows(list, 4);
    gui_set_expand(list, true);
    gui_add(root, list);

    widget_t *bottom = gui_hbox(8);
    widget_t *spacer = gui_label("");
    gui_set_expand(spacer, true);
    gui_add(bottom, spacer);
    gui_add(bottom, gui_button("Quit", quit, NULL));
    gui_add(root, bottom);

    gui_window_set_root(window, root);
    gui_window_focus(window, name_input);
    printf("guidemo: ready\n");
    fflush(stdout);
    return gui_run(app);
}
