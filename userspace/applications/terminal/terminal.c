/*
 * terminal: a shell in a window.
 *
 * Starts `/bin/sh -i` with pipes as stdin and stdout/stderr. A reader thread
 * copies the shell's output into a buffer and signals an event; the GUI
 * loop (gui_watch) moves it into the character grid. There is no line
 * discipline behind a pipe, so the terminal edits the input line itself
 * (echo, Backspace) and sends it on Enter. Ctrl-D ends the input; closing
 * the window ends the shell. The window is resizable: the visible grid
 * follows its size (up to COLUMNS x MAX_ROWS characters).
 */

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <jelly/input.h>
#include <jelly/os.h>

#include "graphics/gui/gui.h"

#define COLUMNS     160        /* grid width; the window shows as many as fit */
#define START_COLUMNS 80
#define START_ROWS  25
#define MAX_ROWS    100
#define SCROLLBACK  500
#define PADDING     6
#define LINE_MAX    512

static gui_app_t *app;
static gui_window_t *window;
static widget_t *view;

/* Grid of code points: SCROLLBACK lines; the last visible_rows() are the screen. */
static uint32_t grid[SCROLLBACK][COLUMNS];
static int cursor_x, cursor_y;          /* cursor_y indexes grid */
static int scroll_offset;               /* lines scrolled back by the user */

static char line[LINE_MAX];
static size_t line_length;

static jelly_handle_t shell_input = JELLY_HANDLE_INVALID; /* write end of the shell's stdin */
static jelly_handle_t shell_output;                         /* read end of its stdout */
static jelly_handle_t shell_process;
static jelly_handle_t output_ready;                         /* event: the reader added output */

static mtx_t output_lock;
static char output[16384];
static size_t output_length;
static bool shell_exited;

/* --- Grid --------------------------------------------------------------------------- */

static int visible_columns(void)
{
    int n = (gui_widget_rect(view).w - 2 * PADDING) / TEXT_CELL_WIDTH;
    return n < 1 ? START_COLUMNS : n > COLUMNS ? COLUMNS : n;
}

static int visible_rows(void)
{
    int n = (gui_widget_rect(view).h - 2 * PADDING) / TEXT_CELL_HEIGHT;
    return n < 1 ? START_ROWS : n > MAX_ROWS ? MAX_ROWS : n;
}

static void scroll_grid(void)
{
    memmove(grid[0], grid[1], sizeof(grid[0]) * (SCROLLBACK - 1));
    for (int x = 0; x < COLUMNS; x++)
        grid[SCROLLBACK - 1][x] = ' ';
}

static void new_line(void)
{
    cursor_x = 0;
    if (cursor_y == SCROLLBACK - 1)
        scroll_grid();
    else
        cursor_y++;
}

static void put(uint32_t c)
{
    if (cursor_x >= visible_columns())
        new_line();
    grid[cursor_y][cursor_x++] = c;
}

/* Terminal output: UTF-8 with \n, \r, \b, \t; escape sequences are skipped. */
static void write_text(const char *text, size_t length)
{
    static int escape; /* 1 after ESC, 2 inside CSI */
    const char *end = text + length;
    while (text < end) {
        unsigned char c = (unsigned char)*text;
        if (escape == 1) {
            escape = c == '[' ? 2 : 0;
            text++;
            continue;
        }
        if (escape == 2) {
            if (c >= 0x40 && c <= 0x7E)
                escape = 0;
            text++;
            continue;
        }
        if (c >= 0x80) {
            put(utf8_next(&text));
            continue;
        }
        text++;
        switch (c) {
        case '\n': new_line(); break;
        case '\r': cursor_x = 0; break;
        case '\b': if (cursor_x) cursor_x--; break;
        case '\t': do put(' '); while (cursor_x % 8); break;
        case 0x1B: escape = 1; break;
        default:
            if (c >= 0x20)
                put(c);
        }
    }
    scroll_offset = 0;
    gui_custom_redraw(view);
}

static void draw(widget_t *widget, canvas_t *canvas, rect_t area, void *user)
{
    (void)widget;
    (void)user;
    const gui_theme_t *t = gui_window_theme(window);
    color_t background = RGB(0x15, 0x13, 0x22), foreground = RGB(0xE6, 0xE3, 0xF2);
    canvas_fill(canvas, area, background);

    int rows = visible_rows(), columns = visible_columns();
    int first = SCROLLBACK - rows - scroll_offset;
    for (int row = 0; row < rows; row++) {
        char text[COLUMNS * 3 + 1];
        int n = 0;
        for (int x = 0; x < columns; x++)
            n += utf8_encode(grid[first + row][x] ? grid[first + row][x] : ' ', text + n);
        text[n] = '\0';
        canvas_text(canvas, area.x + PADDING, area.y + PADDING + row * TEXT_CELL_HEIGHT, text, foreground, 1);
    }
    /* cursor block */
    int screen_y = cursor_y - first;
    if (scroll_offset == 0 && screen_y >= 0 && screen_y < rows && !shell_exited)
        canvas_fill(canvas,
                    rect_make(area.x + PADDING + cursor_x * TEXT_CELL_WIDTH, area.y + PADDING + screen_y * TEXT_CELL_HEIGHT,
                              TEXT_CELL_WIDTH, TEXT_CELL_HEIGHT),
                    RGBA(0xA7, 0x8B, 0xFA, 0xC0));
    (void)t;
}

/* --- Input ---------------------------------------------------------------------- */

static void send_to_shell(const char *data, size_t length)
{
    size_t done;
    if (shell_input != JELLY_HANDLE_INVALID)
        jelly_write(shell_input, data, length, &done);
}

static bool key(widget_t *widget, const wm_event_t *e, void *user)
{
    (void)widget;
    (void)user;
    if (e->type == WM_EVENT_MOUSE_WHEEL) {
        scroll_offset += e->wheel * 3;
        if (scroll_offset < 0)
            scroll_offset = 0;
        if (scroll_offset > SCROLLBACK - visible_rows())
            scroll_offset = SCROLLBACK - visible_rows();
        gui_custom_redraw(view);
        return true;
    }
    if (e->type != WM_EVENT_KEY_DOWN || shell_exited)
        return false;

    uint32_t c = e->character;
    if (c == '\n') {
        write_text("\n", 1);
        line[line_length++] = '\n';
        send_to_shell(line, line_length);
        line_length = 0;
    } else if (c == '\b') {
        if (line_length) {
            /* remove one UTF-8 character */
            do
                line_length--;
            while (line_length && ((unsigned char)line[line_length] & 0xC0) == 0x80);
            write_text("\b \b", 3);
        }
    } else if (c == 4) { /* Ctrl-D: end of input */
        if (line_length == 0 && shell_input != JELLY_HANDLE_INVALID) {
            jelly_handle_close(shell_input);
            shell_input = JELLY_HANDLE_INVALID;
        }
    } else if (c == 3) { /* Ctrl-C: drop the line (there are no signals) */
        write_text("^C\n", 3);
        line_length = 0;
        send_to_shell("\n", 1);
    } else if (c >= 0x20 && c != 0x7F) {
        char encoded[4];
        int n = utf8_encode(c, encoded);
        if (line_length + (size_t)n < LINE_MAX - 1) {
            memcpy(line + line_length, encoded, (size_t)n);
            line_length += (size_t)n;
            write_text(encoded, (size_t)n);
        }
    }
    return true;
}

/* --- Shell process -------------------------------------------------------------- */

static int read_output(void *arg)
{
    (void)arg;
    char buffer[2048];
    for (;;) {
        size_t n = 0;
        status_t status = jelly_read(shell_output, buffer, sizeof(buffer), &n);
        mtx_lock(&output_lock);
        if (STATUS_IS_ERROR(status) || n == 0) {
            shell_exited = true;
        } else {
            size_t room = sizeof(output) - output_length;
            if (n > room)
                n = room;
            memcpy(output + output_length, buffer, n);
            output_length += n;
        }
        mtx_unlock(&output_lock);
        jelly_event_signal(output_ready);
        if (shell_exited)
            return 0;
    }
}

static void output_arrived(void *user)
{
    (void)user;
    static char chunk[sizeof(output)];
    jelly_event_reset(output_ready);
    mtx_lock(&output_lock);
    size_t n = output_length;
    memcpy(chunk, output, n);
    output_length = 0;
    bool exited = shell_exited;
    mtx_unlock(&output_lock);
    if (n)
        write_text(chunk, n);
    if (exited) {
        write_text("\n[shell exited]\n", 16);
        gui_window_set_title(window, "Terminal (exited)");
    }
}

static int start_shell(void)
{
    jelly_handle_t stdin_read, stdout_write;
    if (STATUS_IS_ERROR(jelly_pipe_create(&stdin_read, &shell_input)) ||
        STATUS_IS_ERROR(jelly_pipe_create(&shell_output, &stdout_write)))
        return -1;

    char *argv[] = { "sh", "-i", NULL };
    process_options_t options = { .envp = NULL, .stdio = { stdin_read, stdout_write, stdout_write } };
    for (int i = 0; i < 5; i++)
        options.extra[i] = JELLY_HANDLE_INVALID;
    int result = process_spawn("/bin/sh", argv, &options, &shell_process);
    /* The shell has its own copies now. */
    jelly_handle_close(stdin_read);
    jelly_handle_close(stdout_write);
    return result;
}

static void close_window(widget_t *widget, void *user)
{
    (void)widget;
    (void)user;
    if (!shell_exited)
        jelly_process_kill(shell_process, 0);
    gui_quit(app, 0);
}

int main(void)
{
    app = gui_init();
    if (!app) {
        fprintf(stderr, "terminal: no display server\n");
        return 1;
    }
    window = gui_window_create_ex(app, "Terminal", 0, 0, START_COLUMNS * TEXT_CELL_WIDTH + 2 * PADDING,
                                  START_ROWS * TEXT_CELL_HEIGHT + 2 * PADDING, WM_WINDOW_RESIZABLE);
    if (!window)
        return 1;
    gui_window_set_theme(window, gui_theme_dark(1));
    gui_window_keep_theme(window, true);
    for (int y = 0; y < SCROLLBACK; y++)
        for (int x = 0; x < COLUMNS; x++)
            grid[y][x] = ' ';
    /* The screen is the bottom of the grid; the cursor starts at the top of the first screen. */
    cursor_y = SCROLLBACK - START_ROWS;

    gui_custom_t custom = { .draw = draw, .event = key, .min_width = 1, .min_height = 1 };
    view = gui_custom(&custom);
    gui_set_expand(view, true);
    widget_t *root = gui_vbox(0);
    gui_add(root, view);
    gui_window_set_root(window, root);
    gui_window_focus(window, view);
    gui_window_on_close(window, close_window, NULL);

    mtx_init(&output_lock, mtx_plain);
    if (STATUS_IS_ERROR(jelly_event_create(0, &output_ready)) || start_shell()) {
        fprintf(stderr, "terminal: cannot start the shell\n");
        return 1;
    }
    thrd_t reader;
    thrd_create(&reader, read_output, NULL);
    gui_watch(app, output_ready, output_arrived, NULL);
    return gui_run(app);
}
