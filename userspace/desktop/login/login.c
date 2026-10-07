/*
 * login: the graphical login and session manager (Phase 10: login/session).
 *
 * Started by displayd as root. It shows a full-screen login, checks the
 * name and password against /etc/passwd, and starts the desktop session
 * (/bin/desktop) with the user's uid and gid, home directory and
 * environment. The session gets a control channel as startup handle 3 and
 * asks through it to "logout", "poweroff" or "reboot" (only root may switch
 * the machine off). When the session ends, the login appears again.
 */

#include <errno.h>
#include <process.h>
#include <pwd.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jelly/os.h>

#include "graphics/gui/gui.h"

#define SESSION   "/bin/desktop"
#define LOGGED_IN 2 /* gui_run result; 1 means the display server is gone */

static gui_app_t *app;
static gui_window_t *window;
static widget_t *user_input, *password_input, *message;
static struct passwd account;
static char account_name[64], account_home[256];

static void logged_in(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    const char *name = gui_input_text(user_input), *password = gui_input_text(password_input);
    struct passwd *entry = getpwnam(name);

    if (!entry || !password_check(entry->pw_passwd, password)) {
        gui_label_set_text(message, "Wrong name or password.");
        gui_input_set_text(password_input, "");
        gui_window_focus(window, password_input);
        printf("login: failed for '%s'\n", name);
        fflush(stdout);
        return;
    }
    account = *entry;
    snprintf(account_name, sizeof(account_name), "%s", entry->pw_name);
    snprintf(account_home, sizeof(account_home), "%s", entry->pw_dir);
    account.pw_name = account_name;
    account.pw_dir = account_home;
    gui_window_destroy(window);
    gui_quit(app, LOGGED_IN);
}

static void next_field(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    gui_window_focus(window, password_input);
}

static void power(uint32_t action)
{
    jelly_sync();
    printf("login: %s\n", action == JELLY_POWER_OFF ? "powering off" : "restarting");
    fflush(stdout);
    jelly_system_power(action);
}

static void power_off(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    power(JELLY_POWER_OFF);
}

static void restart(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    power(JELLY_POWER_REBOOT);
}

static widget_t *spacer(void)
{
    widget_t *w = gui_label("");
    gui_set_expand(w, true);
    return w;
}

static void show_login(void)
{
    int32_t width, height;
    wm_screen_size(gui_connection(app), &width, &height);
    window = gui_window_create_ex(app, "Login", 0, 0, width, height, WM_WINDOW_POSITIONED | WM_WINDOW_UNDECORATED);
    if (!window) {
        fprintf(stderr, "login: cannot create the login window\n");
        exit(1);
    }
    gui_window_set_theme(window, gui_theme_dark(1));
    gui_window_keep_theme(window, true);

    widget_t *card = gui_vbox(10);
    gui_box_set_padding(card, 24);
    gui_box_set_background(card, true);
    gui_set_min_size(card, 340, 0);
    gui_add(card, gui_icon(ICON_JELLY, 64));
    widget_t *title = gui_label("JellyOS");
    gui_label_set_large(title, true);
    gui_label_set_center(title, true);
    gui_add(card, title);
    widget_t *hint = gui_label("Log in to your desktop");
    gui_label_set_dim(hint, true);
    gui_label_set_center(hint, true);
    gui_add(card, hint);
    user_input = gui_input("Name", next_field, NULL);
    gui_add(card, user_input);
    password_input = gui_input("Password", logged_in, NULL);
    gui_input_set_password(password_input, true);
    gui_add(card, password_input);
    widget_t *button = gui_button("Log in", logged_in, NULL);
    gui_button_set_primary(button, true);
    gui_add(card, button);
    message = gui_label("");
    gui_label_set_center(message, true);
    gui_add(card, message);

    widget_t *middle = gui_hbox(0);
    gui_add(middle, spacer());
    gui_add(middle, card);
    gui_add(middle, spacer());

    widget_t *bottom = gui_hbox(8);
    gui_box_set_padding(bottom, 12);
    gui_add(bottom, spacer());
    widget_t *restart_button = gui_button("Restart", restart, NULL);
    gui_button_set_flat(restart_button, true);
    gui_button_set_icon(restart_button, ICON_RESTART);
    gui_add(bottom, restart_button);
    widget_t *off_button = gui_button("Power off", power_off, NULL);
    gui_button_set_flat(off_button, true);
    gui_button_set_icon(off_button, ICON_POWER);
    gui_add(bottom, off_button);

    widget_t *root = gui_vbox(0);
    gui_add(root, spacer());
    gui_add(root, middle);
    gui_add(root, spacer());
    gui_add(root, bottom);
    gui_window_set_root(window, root);
    gui_window_focus(window, user_input);
    printf("login: ready\n");
    fflush(stdout);
}

/* Run the desktop session of `account`; returns when it ends. */
static void run_session(void)
{
    char home[300], user[80], cmdline_path[] = "PATH=/bin:/sbin";
    snprintf(home, sizeof(home), "HOME=%s", account.pw_dir);
    snprintf(user, sizeof(user), "USER=%s", account.pw_name);
    char *envp[] = { home, user, cmdline_path, "SHELL=/bin/sh", "JELLY_SESSION=desktop", NULL };
    char *argv[] = { "desktop", NULL };
    jelly_handle_t control, session_end, process;

    if (STATUS_IS_ERROR(jelly_channel_create(&control, &session_end)))
        return;
    process_options_t options = { .envp = envp, .as_user = 1, .uid = account.pw_uid, .gid = account.pw_gid };
    for (int i = 0; i < 3; i++)
        options.stdio[i] = jelly_startup_handle((unsigned)i);
    options.extra[0] = session_end;
    for (int i = 1; i < 5; i++)
        options.extra[i] = JELLY_HANDLE_INVALID;

    /* The session starts in the home directory. */
    if (chdir(account.pw_dir))
        chdir("/");
    int result = process_spawn(SESSION, argv, &options, &process);
    chdir("/");
    jelly_handle_close(session_end);
    if (result) {
        printf("login: cannot start %s: %s\n", SESSION, strerror(errno));
        jelly_handle_close(control);
        return;
    }
    printf("login: session of %s started (uid %u)\n", account.pw_name, account.pw_uid);
    fflush(stdout);

    for (;;) {
        jelly_handle_t handles[2] = { process, control };
        uint32_t index;
        if (STATUS_IS_ERROR(jelly_wait_many(handles, 2, JELLY_WAIT_FOREVER, &index)))
            break;
        if (index == 0)
            break; /* the session ended */
        char request[32];
        size_t length;
        status_t status = jelly_channel_receive(control, request, sizeof(request) - 1, &length);
        if (status == STATUS_PEER_CLOSED) {
            jelly_wait(process, JELLY_WAIT_FOREVER);
            break;
        }
        if (STATUS_IS_ERROR(status))
            continue;
        request[length] = '\0';
        if (!strcmp(request, "logout")) {
            jelly_process_kill(process, 0);
            break;
        }
        if (!strcmp(request, "poweroff"))
            power(JELLY_POWER_OFF);
        else if (!strcmp(request, "reboot"))
            power(JELLY_POWER_REBOOT);
    }
    jelly_handle_close(process);
    jelly_handle_close(control);
    printf("login: session of %s ended\n", account.pw_name);
    fflush(stdout);
}

int main(void)
{
    app = gui_init();
    if (!app) {
        fprintf(stderr, "login: no display server\n");
        return 1;
    }
    for (;;) {
        show_login();
        if (gui_run(app) != LOGGED_IN)
            return 1; /* the display server is gone */
        run_session();
    }
}
