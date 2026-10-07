/*
 * settings: desktop settings and system information (Phase 10).
 *
 * Sections: Appearance (dark mode, large text), Keyboard (layout), Network
 * (interfaces), System (version, uptime, memory, time). Changes are saved
 * to ~/.config/desktop.conf and announced with WM_SETTINGS_CHANGED, so
 * every program applies them at once.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <jelly/os.h>

#include "graphics/gui/gui.h"

static gui_app_t *app;
static gui_window_t *window;
static widget_t *sections, *content;
static gui_settings_t settings;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("settings: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

static void save(void)
{
    if (gui_save_settings(&settings))
        gui_dialog_message(app, "Settings", "The settings could not be saved.", NULL, 0, NULL, NULL);
    wm_set_keymap(gui_connection(app), settings.keymap);
    wm_settings_changed(gui_connection(app));
}

static widget_t *heading(const char *text)
{
    widget_t *label = gui_label(text);
    gui_label_set_large(label, true);
    return label;
}

static widget_t *dim(const char *text)
{
    widget_t *label = gui_label(text);
    gui_label_set_dim(label, true);
    return label;
}

/* --- Appearance ----------------------------------------------------------------- */

static void toggle_dark(widget_t *box, void *user)
{
    (void)user;
    settings.dark = gui_checkbox_checked(box);
    save();
    say("theme %s", settings.dark ? "dark" : "light");
}

static void toggle_large(widget_t *box, void *user)
{
    (void)user;
    settings.scale = gui_checkbox_checked(box) ? 2 : 1;
    save();
    say("scale %d", settings.scale);
}

static void show_appearance(void)
{
    gui_add(content, heading("Appearance"));
    gui_add(content, gui_checkbox("Dark mode", settings.dark, toggle_dark, NULL));
    gui_add(content, gui_checkbox("Large text (accessibility)", settings.scale > 1, toggle_large, NULL));
    gui_add(content, dim("Changes apply to all programs at once."));
}

/* --- Keyboard ------------------------------------------------------------------- */

static const char *const layouts[][2] = { { "us", "English (US)" }, { "de", "Deutsch" } };

static void layout_chosen(widget_t *list, void *user)
{
    (void)user;
    int index = gui_list_selected(list);
    if (index < 0)
        return;
    snprintf(settings.keymap, sizeof(settings.keymap), "%s", layouts[index][0]);
    save();
    say("keymap %s", settings.keymap);
}

static void show_keyboard(void)
{
    gui_add(content, heading("Keyboard"));
    gui_add(content, gui_label("Layout:"));
    widget_t *list = gui_list(NULL, NULL);
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        gui_list_add(list, layouts[i][1]);
        if (!strcmp(settings.keymap, layouts[i][0]))
            gui_list_select(list, (int)i);
    }
    gui_list_set_rows(list, 3);
    /* connect the callback after the initial selection */
    gui_list_on_select(list, layout_chosen, NULL);
    gui_add(content, list);
    gui_add(content, gui_label("Try it:"));
    gui_add(content, gui_input("Type here", NULL, NULL));
}

/* --- Network -------------------------------------------------------------------- */

static void format_address(uint32_t network_order, char *out, size_t size)
{
    uint32_t a = __builtin_bswap32(network_order);
    snprintf(out, size, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
}

static void show_network(void)
{
    jelly_netif_info_t info;
    char line[160], a[16], g[16], d[16];
    gui_add(content, heading("Network"));
    for (uint32_t i = 0; jelly_net_interface_info(i, &info) == STATUS_SUCCESS; i++) {
        snprintf(line, sizeof(line), "%s%s", info.name, (info.flags & JELLY_NETIF_LINK) ? "" : " (no link)");
        gui_add(content, gui_label(line));
        if (info.address) {
            format_address(info.address, a, sizeof(a));
            format_address(info.gateway, g, sizeof(g));
            format_address(info.dns, d, sizeof(d));
            snprintf(line, sizeof(line), "  %s/%d  gateway %s  dns %s", a,
                     __builtin_popcount(__builtin_bswap32(info.netmask)), info.gateway ? g : "-", info.dns ? d : "-");
        } else {
            snprintf(line, sizeof(line), "  not configured");
        }
        gui_add(content, dim(line));
        snprintf(line, sizeof(line), "  received %lu packets, sent %lu packets", (unsigned long)info.rx_packets,
                 (unsigned long)info.tx_packets);
        gui_add(content, dim(line));
    }
}

/* --- System --------------------------------------------------------------------- */

static void show_system(void)
{
    jelly_system_info_t info;
    char line[128];
    jelly_system_info(&info);
    gui_add(content, heading("System"));
    snprintf(line, sizeof(line), "JellyOS %s, system call ABI %u", info.version, info.abi_version);
    gui_add(content, gui_label(line));
    uint64_t s = info.uptime_ns / 1000000000ULL;
    snprintf(line, sizeof(line), "Running for %lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
             (unsigned long)(s % 60));
    gui_add(content, dim(line));
    snprintf(line, sizeof(line), "Memory: %lu MiB free of %lu MiB", (unsigned long)(info.memory_free >> 20),
             (unsigned long)(info.memory_total >> 20));
    gui_add(content, dim(line));
    snprintf(line, sizeof(line), "%u processes", info.processes);
    gui_add(content, dim(line));
    if (info.realtime_ns) {
        time_t now = (time_t)(info.realtime_ns / 1000000000ULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(line, sizeof(line), "Date: %A, %d %B %Y, %H:%M UTC", &tm);
        gui_add(content, dim(line));
    }
}

/* --- Sections ------------------------------------------------------------------- */

static void (*const pages[])(void) = { show_appearance, show_keyboard, show_network, show_system };

static void section_chosen(widget_t *list, void *user)
{
    (void)user;
    int index = gui_list_selected(list);
    if (index < 0)
        return;
    gui_box_clear(content);
    pages[index]();
}

int main(void)
{
    app = gui_init();
    if (!app) {
        fprintf(stderr, "settings: no display server\n");
        return 1;
    }
    gui_load_settings(&settings);
    window = gui_window_create_ex(app, "Settings", 0, 0, 560, 380, WM_WINDOW_RESIZABLE);
    if (!window)
        return 1;

    widget_t *root = gui_hbox(16);
    gui_box_set_padding(root, 14);
    sections = gui_list(section_chosen, NULL);
    gui_list_add(sections, "Appearance");
    gui_list_add(sections, "Keyboard");
    gui_list_add(sections, "Network");
    gui_list_add(sections, "System");
    gui_set_min_size(sections, 150, 0);
    gui_add(root, sections);
    content = gui_vbox(10);
    gui_set_expand(content, true);
    widget_t *scroll = gui_scroll(content);
    gui_set_expand(scroll, true);
    gui_add(root, scroll);
    gui_window_set_root(window, root);
    gui_list_select(sections, 0);
    gui_window_focus(window, sections);
    say("ready");
    return gui_run(app);
}
