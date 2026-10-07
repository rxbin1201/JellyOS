/*
 * desktop: the desktop shell of a session (Phase 10: desktop shell, taskbar,
 * launcher, notifications, system status).
 *
 * Started by login with the user's rights. It
 *   - applies the user's settings (keyboard layout),
 *   - shows the taskbar at the bottom of the screen: the JellyOS button with
 *     the launcher menu, a button per open window (click: focus, again:
 *     minimize), the network and system status, and the clock,
 *   - starts applications described in the .app files of /etc/apps,
 *   - shows notifications that programs send (wm_notify, /bin/notify) in
 *     the top-right corner for a few seconds,
 *   - asks login (control channel, startup handle 3) to log out, restart or
 *     power off; on logout it ends the programs it started.
 */

#include <dirent.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <jelly/os.h>

#include "graphics/gui/gui.h"

#define TASKBAR_HEIGHT    40
#define MAX_APPS          16
#define MAX_PROCESSES     64
#define MAX_NOTIFICATIONS 4
#define NOTIFICATION_W    380
#define NOTIFICATION_H    76
#define NOTIFICATION_NS   6000000000ULL
#define CONTROL_HANDLE    3

typedef struct {
    char   name[48];
    char   exec[128];
    icon_t icon;
} app_entry_t;

typedef struct {
    gui_window_t *window;
    uint64_t      expires;
} notification_t;

static gui_app_t *app;
static gui_window_t *taskbar, *menu, *status_popup;
static widget_t *tasks, *clock_label, *status_button;
static int32_t screen_w, screen_h;
static app_entry_t apps[MAX_APPS];
static int app_count;
static jelly_handle_t processes[MAX_PROCESSES];
static notification_t notifications[MAX_NOTIFICATIONS];
static jelly_handle_t control = JELLY_HANDLE_INVALID;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("desktop: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

/* --- Applications ---------------------------------------------------------------- */

static void load_apps(void)
{
    DIR *directory = opendir("/etc/apps");
    struct dirent *entry;
    if (!directory)
        return;
    while ((entry = readdir(directory)) && app_count < MAX_APPS) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".app"))
            continue;
        char path[300], line[200];
        snprintf(path, sizeof(path), "/etc/apps/%s", entry->d_name);
        FILE *file = fopen(path, "r");
        if (!file)
            continue;
        app_entry_t *a = &apps[app_count];
        memset(a, 0, sizeof(*a));
        a->icon = ICON_PROGRAM;
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!strncmp(line, "name=", 5))
                snprintf(a->name, sizeof(a->name), "%s", line + 5);
            else if (!strncmp(line, "exec=", 5))
                snprintf(a->exec, sizeof(a->exec), "%s", line + 5);
            else if (!strncmp(line, "icon=", 5))
                a->icon = icon_by_name(line + 5);
        }
        fclose(file);
        if (a->name[0] && a->exec[0])
            app_count++;
    }
    closedir(directory);
    /* alphabetical */
    for (int i = 1; i < app_count; i++)
        for (int j = i; j > 0 && strcmp(apps[j - 1].name, apps[j].name) > 0; j--) {
            app_entry_t t = apps[j];
            apps[j] = apps[j - 1];
            apps[j - 1] = t;
        }
}

static void start_program(const char *command)
{
    char copy[128], *argv[9] = { NULL }, *state;
    int argc = 0;
    snprintf(copy, sizeof(copy), "%s", command);
    for (char *word = strtok_r(copy, " \t", &state); word && argc < 8; word = strtok_r(NULL, " \t", &state))
        argv[argc++] = word;
    if (!argc)
        return;
    jelly_handle_t process;
    if (process_spawn(argv[0], argv, NULL, &process)) {
        say("cannot start %s: %s", argv[0], strerror(errno));
        return;
    }
    say("started %s", argv[0]);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        /* Reuse slots of programs that ended. */
        if (processes[i] && jelly_wait(processes[i], 0) == STATUS_SUCCESS) {
            jelly_handle_close(processes[i]);
            processes[i] = 0;
        }
        if (!processes[i]) {
            processes[i] = process;
            return;
        }
    }
    jelly_handle_close(process);
}

static void end_programs(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i]) {
            jelly_process_kill(processes[i], 0);
            jelly_handle_close(processes[i]);
            processes[i] = 0;
        }
    }
}

static void ask_login(const char *request)
{
    if (control == JELLY_HANDLE_INVALID) {
        say("no session manager: cannot %s", request);
        return;
    }
    if (!strcmp(request, "logout"))
        end_programs();
    say("%s", request);
    jelly_channel_send(control, request, strlen(request));
}

/* --- Launcher --------------------------------------------------------------------- */

static void launcher_chosen(int index, void *user)
{
    (void)user;
    menu = NULL;
    if (index < 0)
        return;
    if (index < app_count) {
        start_program(apps[index].exec);
        return;
    }
    index -= app_count + 1; /* separator */
    static const char *const actions[] = { "logout", "reboot", "poweroff" };
    if (index >= 0 && index < 3)
        ask_login(actions[index]);
}

static void open_launcher(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    if (menu) {
        gui_window_destroy(menu);
        menu = NULL;
        return;
    }
    gui_menu_item_t items[MAX_APPS + 4];
    int count = 0;
    for (int i = 0; i < app_count; i++)
        items[count++] = (gui_menu_item_t){ apps[i].name, apps[i].icon };
    items[count++] = (gui_menu_item_t){ "-", ICON_NONE };
    items[count++] = (gui_menu_item_t){ "Log out", ICON_LOGOUT };
    items[count++] = (gui_menu_item_t){ "Restart", ICON_RESTART };
    items[count++] = (gui_menu_item_t){ "Power off", ICON_POWER };
    menu = gui_menu_show(app, 4, screen_h - TASKBAR_HEIGHT - 4, true, items, count, launcher_chosen, NULL);
    say("launcher open");
}

/* --- Window buttons ------------------------------------------------------------- */

static void task_clicked(widget_t *button, void *user)
{
    (void)user;
    uint32_t id = (uint32_t)(uintptr_t)gui_get_user(button);
    const wm_window_info_t *list;
    int count = wm_window_list(gui_connection(app), &list);
    for (int i = 0; i < count; i++) {
        if (list[i].id != id)
            continue;
        bool active = (list[i].state & WM_STATE_FOCUSED) && !(list[i].state & WM_STATE_MINIMIZED);
        if (active)
            wm_minimize_window(gui_connection(app), id);
        else
            wm_activate_window(gui_connection(app), id);
    }
}

static void windows_changed(void *user)
{
    (void)user;
    const wm_window_info_t *stack;
    wm_window_info_t list[64];
    int count = wm_window_list(gui_connection(app), &stack);
    if (count > 64)
        count = 64;
    /* Buttons in the order the windows were opened, not in stacking order (they would jump around). */
    for (int i = 0; i < count; i++) {
        int j = i;
        for (; j > 0 && list[j - 1].id > stack[i].id; j--)
            list[j] = list[j - 1];
        list[j] = stack[i];
    }
    gui_box_clear(tasks);
    for (int i = 0; i < count; i++) {
        widget_t *b = gui_button(list[i].title[0] ? list[i].title : "(untitled)", task_clicked, NULL);
        gui_set_user(b, (void *)(uintptr_t)list[i].id);
        gui_button_set_flat(b, true);
        gui_button_set_selected(b, (list[i].state & WM_STATE_FOCUSED) && !(list[i].state & WM_STATE_MINIMIZED));
        gui_set_min_size(b, 0, 0);
        gui_add(tasks, b);
    }
}

/* --- Status and clock ----------------------------------------------------------- */

static void network_text(char *text, size_t size)
{
    jelly_netif_info_t info;
    snprintf(text, size, "offline");
    for (uint32_t i = 0; jelly_net_interface_info(i, &info) == STATUS_SUCCESS; i++) {
        if (!(info.flags & JELLY_NETIF_LOOPBACK) && info.address) {
            uint32_t a = __builtin_bswap32(info.address);
            snprintf(text, size, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
            return;
        }
    }
}

static void tick(void *user)
{
    (void)user;
    char text[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(text, sizeof(text), "%a %d %b  %H:%M UTC", &tm);
    gui_label_set_text(clock_label, text);
    network_text(text, sizeof(text));
    gui_set_text(status_button, text);

    /* Notifications that have been shown long enough */
    uint64_t ns = jelly_clock_ns();
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (notifications[i].window && ns >= notifications[i].expires) {
            gui_window_destroy(notifications[i].window);
            notifications[i].window = NULL;
        }
    }
}

static void status_closed(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    gui_window_destroy(status_popup);
    status_popup = NULL;
}

static void show_status(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    if (status_popup) {
        status_closed(NULL, NULL);
        return;
    }
    jelly_system_info_t info;
    char lines[6][96], network[32];
    jelly_system_info(&info);
    network_text(network, sizeof(network));
    uint64_t seconds = info.uptime_ns / 1000000000ULL;
    snprintf(lines[0], sizeof(lines[0]), "JellyOS %s (ABI %u)", info.version, info.abi_version);
    snprintf(lines[1], sizeof(lines[1]), "Up %lu:%02lu:%02lu", (unsigned long)(seconds / 3600),
             (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
    snprintf(lines[2], sizeof(lines[2]), "Memory %lu MiB free of %lu MiB", (unsigned long)(info.memory_free >> 20),
             (unsigned long)(info.memory_total >> 20));
    snprintf(lines[3], sizeof(lines[3]), "%u processes", info.processes);
    snprintf(lines[4], sizeof(lines[4]), "Network %s", network);
    snprintf(lines[5], sizeof(lines[5]), "Signed in as %s", getenv("USER") ? getenv("USER") : "?");

    int32_t width = 300, height = 6 * 22 + 32;
    status_popup = gui_window_create_ex(app, "Status", screen_w - width - 8, screen_h - TASKBAR_HEIGHT - height - 8,
                                        width, height, WM_WINDOW_POPUP | WM_WINDOW_POSITIONED);
    if (!status_popup)
        return;
    widget_t *root = gui_vbox(6);
    gui_box_set_padding(root, 14);
    for (int i = 0; i < 6; i++) {
        widget_t *label = gui_label(lines[i]);
        if (i)
            gui_label_set_dim(label, true);
        gui_add(root, label);
    }
    gui_window_set_root(status_popup, root);
    gui_window_on_close(status_popup, status_closed, NULL);
    say("status shown");
}

/* --- Notifications -------------------------------------------------------------- */

static void notification_clicked(widget_t *widget, void *user)
{
    (void)widget;
    notification_t *n = user;
    if (n->window) {
        gui_window_destroy(n->window);
        n->window = NULL;
    }
}

static void notify(const char *title, const char *text, void *user)
{
    (void)user;
    int slot = -1;
    for (int i = 0; i < MAX_NOTIFICATIONS && slot < 0; i++) {
        if (!notifications[i].window)
            slot = i;
    }
    if (slot < 0) {
        /* replace the oldest */
        slot = 0;
        for (int i = 1; i < MAX_NOTIFICATIONS; i++)
            if (notifications[i].expires < notifications[slot].expires)
                slot = i;
        gui_window_destroy(notifications[slot].window);
    }
    notification_t *n = &notifications[slot];
    n->window = gui_window_create_ex(app, "Notification", screen_w - NOTIFICATION_W - 12,
                                     12 + slot * (NOTIFICATION_H + 10), NOTIFICATION_W, NOTIFICATION_H,
                                     WM_WINDOW_PANEL | WM_WINDOW_POSITIONED);
    if (!n->window)
        return;
    n->expires = jelly_clock_ns() + NOTIFICATION_NS;
    widget_t *row = gui_hbox(12);
    gui_box_set_padding(row, 12);
    gui_add(row, gui_icon(ICON_INFO, 32));
    widget_t *texts = gui_vbox(4);
    gui_set_expand(texts, true);
    gui_add(texts, gui_label(title));
    widget_t *body = gui_label(text);
    gui_label_set_dim(body, true);
    gui_add(texts, body);
    gui_add(row, texts);
    widget_t *close = gui_button("x", notification_clicked, n);
    gui_button_set_flat(close, true);
    gui_add(row, close);
    gui_window_set_root(n->window, row);
    say("notification '%s'", title);
}

/* --- Taskbar -------------------------------------------------------------------- */

static void settings_changed(void *user)
{
    (void)user;
    gui_settings_t settings;
    gui_load_settings(&settings);
    wm_set_keymap(gui_connection(app), settings.keymap);
}

static void build_taskbar(void)
{
    taskbar = gui_window_create_ex(app, "Taskbar", 0, screen_h - TASKBAR_HEIGHT, screen_w, TASKBAR_HEIGHT,
                                   WM_WINDOW_PANEL | WM_WINDOW_POSITIONED | WM_WINDOW_RESERVE);
    if (!taskbar) {
        fprintf(stderr, "desktop: cannot create the taskbar\n");
        exit(1);
    }
    gui_window_set_theme(taskbar, gui_theme_dark(1));
    gui_window_keep_theme(taskbar, true);
    gui_window_on_close(taskbar, NULL, NULL);

    widget_t *bar = gui_hbox(4);
    gui_box_set_padding(bar, 2);
    widget_t *launcher = gui_button("JellyOS", open_launcher, NULL);
    gui_button_set_flat(launcher, true);
    gui_button_set_icon(launcher, ICON_JELLY);
    gui_add(bar, launcher);
    gui_add(bar, gui_separator());
    tasks = gui_hbox(2);
    gui_set_expand(tasks, true);
    gui_add(bar, tasks);
    status_button = gui_button("offline", show_status, NULL);
    gui_button_set_flat(status_button, true);
    gui_button_set_icon(status_button, ICON_NETWORK);
    gui_add(bar, status_button);
    clock_label = gui_label("");
    gui_set_min_size(clock_label, 170, 0);
    gui_add(bar, clock_label);
    gui_window_set_root(taskbar, bar);
}

int main(void)
{
    app = gui_init();
    if (!app) {
        fprintf(stderr, "desktop: no display server\n");
        return 1;
    }
    control = jelly_startup_handle(CONTROL_HANDLE);
    wm_screen_size(gui_connection(app), &screen_w, &screen_h);
    settings_changed(NULL);
    load_apps();
    build_taskbar();
    tick(NULL);
    gui_add_timer(app, 1000000000ULL, tick, NULL);
    gui_on_windows(app, windows_changed, NULL);
    gui_on_notification(app, notify, NULL);
    gui_on_settings(app, settings_changed, NULL);

    char welcome[96];
    snprintf(welcome, sizeof(welcome), "Welcome, %s!", getenv("USER") ? getenv("USER") : "user");
    notify(welcome, "Start programs with JellyOS.", NULL);
    say("ready (%d applications)", app_count);
    return gui_run(app);
}
