/*
 * gamepad: shows what a connected gamepad or joystick reports (Phase 11).
 *
 * The display server sends the gamepad events of the input manager to the
 * window with the focus; this program draws the sticks, triggers, the
 * directional pad and the buttons, and prints every button event.
 */

#include <stdio.h>
#include <string.h>

#include <jelly/input.h>
#include <jelly/os.h>

#include "graphics/gui/gui.h"

#define BUTTONS 16

static gui_app_t *app;
static gui_window_t *window;
static widget_t *view, *status;
static int32_t axes[8];
static bool buttons[32];
static bool seen;

static void draw_stick(canvas_t *canvas, const gui_theme_t *t, rect_t box, int32_t x, int32_t y)
{
    canvas_fill_rounded(canvas, box, 8, t->surface);
    canvas_outline_rounded(canvas, box, 8, t->border);
    int32_t cx = box.x + box.w / 2 + (int32_t)((int64_t)x * (box.w / 2 - 10) / 32768);
    int32_t cy = box.y + box.h / 2 + (int32_t)((int64_t)y * (box.h / 2 - 10) / 32768);
    canvas_fill_rounded(canvas, rect_make(cx - 8, cy - 8, 16, 16), 8, t->accent);
}

static void draw_trigger(canvas_t *canvas, const gui_theme_t *t, rect_t box, int32_t value)
{
    canvas_fill_rounded(canvas, box, 4, t->surface);
    canvas_outline_rounded(canvas, box, 4, t->border);
    int32_t filled = (int32_t)(((int64_t)value + 32768) * (box.w - 4) / 65535);
    if (filled > 0)
        canvas_fill_rounded(canvas, rect_make(box.x + 2, box.y + 2, filled, box.h - 4), 3, t->accent);
}

static void draw(widget_t *widget, canvas_t *canvas, rect_t area, void *user)
{
    (void)user;
    const gui_theme_t *t = gui_window_theme(gui_widget_window(widget));
    int32_t x = area.x, y = area.y;
    char label[8];

    draw_stick(canvas, t, rect_make(x, y, 110, 110), axes[JELLY_AXIS_LEFT_X], axes[JELLY_AXIS_LEFT_Y]);
    draw_stick(canvas, t, rect_make(x + 130, y, 110, 110), axes[JELLY_AXIS_RIGHT_X], axes[JELLY_AXIS_RIGHT_Y]);
    draw_stick(canvas, t, rect_make(x + 260, y, 110, 110), axes[JELLY_AXIS_HAT_X], axes[JELLY_AXIS_HAT_Y]);
    canvas_text(canvas, x + 4, y + 114, "left stick", t->text_dim, 1);
    canvas_text(canvas, x + 134, y + 114, "right stick", t->text_dim, 1);
    canvas_text(canvas, x + 264, y + 114, "directional pad", t->text_dim, 1);

    draw_trigger(canvas, t, rect_make(x, y + 140, 175, 16), axes[JELLY_AXIS_TRIGGER_L]);
    draw_trigger(canvas, t, rect_make(x + 195, y + 140, 175, 16), axes[JELLY_AXIS_TRIGGER_R]);

    for (int i = 0; i < BUTTONS; i++) {
        rect_t r = rect_make(x + (i % 8) * 47, y + 170 + (i / 8) * 34, 40, 28);
        canvas_fill_rounded(canvas, r, 6, buttons[i] ? t->accent : t->surface);
        canvas_outline_rounded(canvas, r, 6, t->border);
        snprintf(label, sizeof(label), "%d", i + 1);
        canvas_text(canvas, r.x + (r.w - text_width(label, 1)) / 2, r.y + 6, label, buttons[i] ? t->accent_text : t->text,
                    1);
    }
}

static void on_gamepad(gui_window_t *win, const wm_event_t *e, void *user)
{
    (void)win;
    (void)user;
    if (!seen) {
        seen = true;
        gui_label_set_text(status, "Gamepad connected.");
    }
    if (e->type == WM_EVENT_GAMEPAD_BUTTON && e->button < 32) {
        buttons[e->button] = e->x != 0;
        printf("gamepad: button %u %s\n", e->button + 1, e->x ? "pressed" : "released");
        fflush(stdout);
    } else if (e->type == WM_EVENT_GAMEPAD_AXIS && e->button < 8) {
        axes[e->button] = e->x;
    }
    gui_custom_redraw(view);
}

int main(void)
{
    app = gui_init();
    if (!app) {
        fprintf(stderr, "gamepad: no display server\n");
        return 1;
    }
    window = gui_window_create(app, "Gamepad", 400, 300);
    if (!window)
        return 1;
    axes[JELLY_AXIS_TRIGGER_L] = axes[JELLY_AXIS_TRIGGER_R] = -32768;

    widget_t *root = gui_vbox(8);
    gui_box_set_padding(root, 14);
    status = gui_label("Connect a USB gamepad and press a button.");
    gui_label_set_dim(status, true);
    gui_add(root, status);
    gui_custom_t custom = { .draw = draw, .min_width = 372, .min_height = 240 };
    view = gui_custom(&custom);
    gui_set_expand(view, true);
    gui_add(root, view);
    gui_window_set_root(window, root);
    gui_window_on_gamepad(window, on_gamepad, NULL);
    printf("gamepad: ready\n");
    fflush(stdout);
    return gui_run(app);
}
