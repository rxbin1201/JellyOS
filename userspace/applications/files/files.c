/*
 * files: the file manager (Phase 10).
 *
 * Browses folders in a table (name, size, type), opens folders with a
 * double-click or Enter and files in the text viewer, and creates, renames,
 * copies, pastes and deletes files and folders (also on the FAT volumes
 * under /volumes). Backspace goes up, F5 refreshes, Delete deletes.
 *
 *     files [FOLDER]      starts in FOLDER, else in $HOME
 *
 * Actions are also printed on stdout, so tests can follow them.
 */

#include <dirent.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/input.h>
#include <jelly/os.h>

#include "graphics/gui/gui.h"

#define PATH_MAX_LENGTH 1024

typedef struct {
    char         name[JELLY_NAME_MAX + 1];
    jelly_stat_t stat;
} entry_t;

static gui_app_t *app;
static gui_window_t *window;
static widget_t *path_input, *table, *status;
static char cwd[PATH_MAX_LENGTH];
static char clipboard[PATH_MAX_LENGTH];
static entry_t *entries;
static int entry_count;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("files: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

static void join(char *out, size_t size, const char *directory, const char *name)
{
    snprintf(out, size, "%s%s%s", directory, strcmp(directory, "/") ? "/" : "", name);
}

static void error(const char *what, status_t status)
{
    char text[200];
    snprintf(text, sizeof(text), "%s: %s", what, strerror((int)status));
    gui_dialog_message(app, "Files", text, NULL, 0, NULL, NULL);
}

static const char *type_of(const entry_t *e, icon_t *icon)
{
    switch (e->stat.type) {
    case JELLY_FILE_TYPE_DIRECTORY: *icon = ICON_FOLDER; return "Folder";
    case JELLY_FILE_TYPE_SYMLINK:   *icon = ICON_FILE; return "Link";
    case JELLY_FILE_TYPE_DEVICE:    *icon = ICON_FILE; return "Device";
    case JELLY_FILE_TYPE_PIPE:      *icon = ICON_FILE; return "Pipe";
    }
    const char *dot = strrchr(e->name, '.');
    static const char *const text_types[] = { ".txt", ".conf", ".c", ".h", ".md", ".sh", ".log", ".app", ".cfg" };
    for (size_t i = 0; dot && i < sizeof(text_types) / sizeof(text_types[0]); i++) {
        if (!strcmp(dot, text_types[i])) {
            *icon = ICON_TEXT;
            return "Text file";
        }
    }
    if (e->stat.mode & 0111) {
        *icon = ICON_PROGRAM;
        return "Program";
    }
    *icon = ICON_FILE;
    return "File";
}

static void human_size(uint64_t size, char *out, size_t length)
{
    if (size < 1024)
        snprintf(out, length, "%lu B", (unsigned long)size);
    else if (size < 1024 * 1024)
        snprintf(out, length, "%lu.%lu KiB", (unsigned long)(size / 1024), (unsigned long)(size % 1024 * 10 / 1024));
    else
        snprintf(out, length, "%lu.%lu MiB", (unsigned long)(size >> 20),
                 (unsigned long)((size & 0xFFFFF) * 10 >> 20));
}

static int compare_entries(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    bool xd = x->stat.type == JELLY_FILE_TYPE_DIRECTORY, yd = y->stat.type == JELLY_FILE_TYPE_DIRECTORY;
    if (xd != yd)
        return xd ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void show(const char *directory, const char *select)
{
    DIR *d = opendir(directory);
    if (!d) {
        error(directory, (status_t)errno);
        return;
    }
    snprintf(cwd, sizeof(cwd), "%s", directory);
    gui_input_set_text(path_input, cwd);
    free(entries);
    entries = NULL;
    entry_count = 0;
    int capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry_count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            entry_t *grown = realloc(entries, (size_t)capacity * sizeof(entry_t));
            if (!grown)
                break;
            entries = grown;
        }
        entry_t *e = &entries[entry_count++];
        char full[PATH_MAX_LENGTH];
        snprintf(e->name, sizeof(e->name), "%s", entry->d_name);
        join(full, sizeof(full), cwd, e->name);
        if (STATUS_IS_ERROR(jelly_stat(full, JELLY_STAT_NOFOLLOW, &e->stat))) {
            memset(&e->stat, 0, sizeof(e->stat));
            e->stat.type = entry->d_type;
        }
    }
    closedir(d);
    qsort(entries, (size_t)entry_count, sizeof(entry_t), compare_entries);

    gui_table_clear(table);
    int selected = entry_count ? 0 : -1;
    for (int i = 0; i < entry_count; i++) {
        icon_t icon;
        char size[24] = "";
        const char *type = type_of(&entries[i], &icon);
        if (entries[i].stat.type == JELLY_FILE_TYPE_FILE)
            human_size(entries[i].stat.size, size, sizeof(size));
        const char *cells[] = { entries[i].name, size, type };
        gui_table_add(table, icon, cells);
        if (select && !strcmp(entries[i].name, select))
            selected = i;
    }
    if (selected >= 0)
        gui_table_select(table, selected);
    char text[96];
    snprintf(text, sizeof(text), "%d item%s", entry_count, entry_count == 1 ? "" : "s");
    gui_label_set_text(status, text);
    gui_window_set_title(window, cwd);
    say("showing %s (%d items)", cwd, entry_count);
}

static void refresh(void)
{
    int row = gui_table_selected(table);
    char keep[JELLY_NAME_MAX + 1] = "";
    if (row >= 0 && row < entry_count)
        snprintf(keep, sizeof(keep), "%s", entries[row].name);
    show(cwd, keep);
}

static entry_t *selected_entry(void)
{
    int row = gui_table_selected(table);
    return row >= 0 && row < entry_count ? &entries[row] : NULL;
}

static void go_up(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    if (!strcmp(cwd, "/"))
        return;
    char parent[PATH_MAX_LENGTH], child[JELLY_NAME_MAX + 1];
    snprintf(parent, sizeof(parent), "%s", cwd);
    char *slash = strrchr(parent, '/');
    snprintf(child, sizeof(child), "%s", slash + 1);
    if (slash == parent)
        parent[1] = '\0';
    else
        *slash = '\0';
    show(parent, child);
}

static void go_home(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    show(getenv("HOME") ? getenv("HOME") : "/", NULL);
}

static void go_path(widget_t *widget, void *user)
{
    (void)user;
    const char *path = gui_input_text(widget);
    if (path[0] == '/')
        show(path, NULL);
}

static void open_selected(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    entry_t *e = selected_entry();
    if (!e)
        return;
    char full[PATH_MAX_LENGTH];
    join(full, sizeof(full), cwd, e->name);
    if (e->stat.type == JELLY_FILE_TYPE_DIRECTORY) {
        show(full, NULL);
        return;
    }
    char *argv[] = { "viewer", full, NULL };
    jelly_handle_t process;
    if (process_spawn("/bin/viewer", argv, NULL, &process))
        error("viewer", (status_t)errno);
    else {
        jelly_handle_close(process);
        say("opened %s", full);
    }
}

/* --- New folder, rename, delete, copy, paste ----------------------------------- */

static void folder_named(const char *name, void *user)
{
    (void)user;
    if (!name || !name[0])
        return;
    char full[PATH_MAX_LENGTH];
    join(full, sizeof(full), cwd, name);
    status_t status = jelly_mkdir(full, 0755);
    if (STATUS_IS_ERROR(status)) {
        error(name, status);
        return;
    }
    say("created folder %s", full);
    show(cwd, name);
}

static void new_folder(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    gui_dialog_input(app, "New folder", "Name of the new folder:", "New folder", folder_named, NULL);
}

static char rename_from[JELLY_NAME_MAX + 1];

static void renamed(const char *name, void *user)
{
    (void)user;
    if (!name || !name[0] || !strcmp(name, rename_from))
        return;
    char from[PATH_MAX_LENGTH], to[PATH_MAX_LENGTH];
    join(from, sizeof(from), cwd, rename_from);
    join(to, sizeof(to), cwd, name);
    status_t status = jelly_rename(from, to);
    if (STATUS_IS_ERROR(status)) {
        error(name, status);
        return;
    }
    say("renamed %s to %s", from, to);
    show(cwd, name);
}

static void rename_selected(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    entry_t *e = selected_entry();
    if (!e)
        return;
    snprintf(rename_from, sizeof(rename_from), "%s", e->name);
    gui_dialog_input(app, "Rename", "New name:", e->name, renamed, NULL);
}

static status_t remove_tree(const char *path)
{
    jelly_stat_t stat;
    status_t status = jelly_stat(path, JELLY_STAT_NOFOLLOW, &stat);
    if (STATUS_IS_ERROR(status))
        return status;
    if (stat.type == JELLY_FILE_TYPE_DIRECTORY) {
        for (;;) {
            DIR *d = opendir(path);
            struct dirent *entry;
            char child[PATH_MAX_LENGTH];
            bool found = false;
            if (!d)
                break;
            if ((entry = readdir(d))) {
                join(child, sizeof(child), path, entry->d_name);
                found = true;
            }
            closedir(d);
            if (!found)
                break;
            status = remove_tree(child);
            if (STATUS_IS_ERROR(status))
                return status;
        }
    }
    return jelly_unlink(path);
}

static void delete_confirmed(int button, void *user)
{
    (void)user;
    entry_t *e = selected_entry();
    if (button != 0 || !e)
        return;
    char full[PATH_MAX_LENGTH];
    join(full, sizeof(full), cwd, e->name);
    status_t status = remove_tree(full);
    if (STATUS_IS_ERROR(status)) {
        error(e->name, status);
        return;
    }
    say("deleted %s", full);
    refresh();
}

static void delete_selected(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    entry_t *e = selected_entry();
    if (!e)
        return;
    char text[300];
    static const char *const buttons[] = { "Delete", "Cancel" };
    snprintf(text, sizeof(text), "Delete \"%s\"%s?", e->name,
             e->stat.type == JELLY_FILE_TYPE_DIRECTORY ? " with everything in it" : "");
    gui_dialog_message(app, "Delete", text, buttons, 2, delete_confirmed, NULL);
}

static void copy_selected(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    entry_t *e = selected_entry();
    if (!e)
        return;
    join(clipboard, sizeof(clipboard), cwd, e->name);
    char text[PATH_MAX_LENGTH + 16];
    snprintf(text, sizeof(text), "Copied %s", e->name);
    gui_label_set_text(status, text);
    say("copied %s", clipboard);
}

static status_t copy_file(const char *from, const char *to)
{
    jelly_handle_t in, out;
    status_t status = jelly_open(from, JELLY_OPEN_READ, 0, &in);
    if (STATUS_IS_ERROR(status))
        return status;
    status = jelly_open(to, JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | JELLY_OPEN_EXCLUSIVE, 0644, &out);
    if (STATUS_IS_ERROR(status)) {
        jelly_handle_close(in);
        return status;
    }
    static char buffer[16384];
    for (;;) {
        size_t done = 0, written = 0;
        status = jelly_read(in, buffer, sizeof(buffer), &done);
        if (STATUS_IS_ERROR(status) || done == 0)
            break;
        status = jelly_write(out, buffer, done, &written);
        if (STATUS_IS_ERROR(status))
            break;
    }
    jelly_handle_close(in);
    jelly_handle_close(out);
    return status;
}

static status_t copy_tree(const char *from, const char *to)
{
    jelly_stat_t stat;
    status_t status = jelly_stat(from, 0, &stat);
    if (STATUS_IS_ERROR(status))
        return status;
    if (stat.type != JELLY_FILE_TYPE_DIRECTORY)
        return copy_file(from, to);
    status = jelly_mkdir(to, 0755);
    if (STATUS_IS_ERROR(status))
        return status;
    DIR *d = opendir(from);
    struct dirent *entry;
    while (d && (entry = readdir(d)) && !STATUS_IS_ERROR(status)) {
        char a[PATH_MAX_LENGTH], b[PATH_MAX_LENGTH];
        join(a, sizeof(a), from, entry->d_name);
        join(b, sizeof(b), to, entry->d_name);
        status = copy_tree(a, b);
    }
    if (d)
        closedir(d);
    return status;
}

static void paste(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    if (!clipboard[0])
        return;
    const char *base = strrchr(clipboard, '/') + 1;
    char name[JELLY_NAME_MAX + 1], target[PATH_MAX_LENGTH];
    jelly_stat_t stat;
    snprintf(name, sizeof(name), "%s", base);
    join(target, sizeof(target), cwd, name);
    for (int n = 2; !STATUS_IS_ERROR(jelly_stat(target, JELLY_STAT_NOFOLLOW, &stat)) && n < 100; n++) {
        snprintf(name, sizeof(name), "%s (%d)", base, n);
        join(target, sizeof(target), cwd, name);
    }
    status_t status = copy_tree(clipboard, target);
    if (STATUS_IS_ERROR(status)) {
        error(name, status);
        return;
    }
    say("pasted %s to %s", clipboard, target);
    show(cwd, name);
}

static void refresh_clicked(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    refresh();
}

static bool shortcut(gui_window_t *win, const wm_event_t *e, void *user)
{
    (void)win;
    (void)user;
    switch (e->key) {
    case JELLY_KEY_BACKSPACE: go_up(NULL, NULL); return true;
    case JELLY_KEY_F5:        refresh(); return true;
    case JELLY_KEY_DELETE:    delete_selected(NULL, NULL); return true;
    case JELLY_KEY_F2:        rename_selected(NULL, NULL); return true;
    }
    if (e->modifiers & WM_MOD_CTRL) {
        if (e->key == JELLY_KEY_C) {
            copy_selected(NULL, NULL);
            return true;
        }
        if (e->key == JELLY_KEY_V) {
            paste(NULL, NULL);
            return true;
        }
    }
    return false;
}

static widget_t *tool(const char *text, gui_callback_t fn)
{
    widget_t *b = gui_button(text, fn, NULL);
    gui_button_set_flat(b, true);
    return b;
}

int main(int argc, char **argv)
{
    app = gui_init();
    if (!app) {
        fprintf(stderr, "files: no display server\n");
        return 1;
    }
    window = gui_window_create_ex(app, "Files", 0, 0, 660, 440, WM_WINDOW_RESIZABLE);
    if (!window)
        return 1;

    widget_t *root = gui_vbox(8);
    gui_box_set_padding(root, 10);
    widget_t *bar = gui_hbox(4);
    widget_t *up = tool("Up", go_up);
    gui_button_set_icon(up, ICON_FOLDER);
    gui_add(bar, up);
    gui_add(bar, tool("Home", go_home));
    path_input = gui_input("Path", go_path, NULL);
    gui_set_expand(path_input, true);
    gui_add(bar, path_input);
    gui_add(root, bar);

    widget_t *actions = gui_hbox(4);
    gui_add(actions, tool("New folder", new_folder));
    gui_add(actions, tool("Rename", rename_selected));
    gui_add(actions, tool("Copy", copy_selected));
    gui_add(actions, tool("Paste", paste));
    gui_add(actions, tool("Delete", delete_selected));
    widget_t *gap = gui_label("");
    gui_set_expand(gap, true);
    gui_add(actions, gap);
    gui_add(actions, tool("Refresh", refresh_clicked));
    gui_add(root, actions);

    static const char *const titles[] = { "Name", "Size", "Type" };
    static const int32_t widths[] = { 300, 100, 100 };
    table = gui_table(3, titles, widths);
    gui_table_on_activate(table, open_selected, NULL);
    gui_set_expand(table, true);
    gui_add(root, table);
    status = gui_label("");
    gui_label_set_dim(status, true);
    gui_add(root, status);

    gui_window_set_root(window, root);
    gui_window_on_key(window, shortcut, NULL);
    gui_window_focus(window, table);
    show(argc > 1 ? argv[1] : getenv("HOME") ? getenv("HOME") : "/", NULL);
    return gui_run(app);
}
