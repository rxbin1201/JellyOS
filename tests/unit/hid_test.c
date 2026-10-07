/*
 * Host unit tests for drivers/input/hid.c: report descriptors of a boot
 * keyboard, a mouse, QEMU's USB tablet and gamepads, and the events their
 * reports turn into. Gamepads are only tested here and in the kernel tests:
 * QEMU has no gamepad device.
 */

#include <stdio.h>
#include <string.h>

#include "drivers/input/hid.h"

#include <jelly/input.h>
#include <jelly/syscall.h>

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("unit: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

static const uint8_t keyboard_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,                                     /* desktop, keyboard, application */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, /* modifiers */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                     /* reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* LEDs (output) */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, /* 6 keys */
    0xC0,
};

static const uint8_t mouse_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, /* 3 buttons */
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
};

/* QEMU's usb-tablet (hw/usb/dev-hid.c) */
static const uint8_t tablet_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,                                     /* X, Y absolute 0..32767 */
    0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
};

/* A gamepad with report ID 1: 12 buttons, a hat switch, X/Y and Z/Rz sticks */
static const uint8_t gamepad_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0C, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0C, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0xC0,
};

/* A controller with both sticks on X/Y and Rx/Ry and the triggers on Z/Rz (16-bit signed sticks) */
static const uint8_t controller_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x04,
    0x81, 0x02,
    0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0xC0,
};

typedef struct {
    uint32_t type, code, flags;
    int32_t  value, dx, dy, x, y;
} event_t;

static event_t events[64];
static int event_count;

static void record(void *context, uint32_t type, uint32_t code, int32_t value, int32_t dx, int32_t dy, int32_t x,
                   int32_t y, uint32_t flags)
{
    (void)context;
    if (event_count < 64)
        events[event_count++] = (event_t){ type, code, flags, value, dx, dy, x, y };
}

static void feed(hid_state_t *state, const uint8_t *report, size_t size)
{
    event_count = 0;
    hid_process(state, report, size, record, NULL);
}

static int is(int index, uint32_t type, uint32_t code, int32_t value)
{
    return index < event_count && events[index].type == type && events[index].code == code &&
           events[index].value == value;
}

static void test_keyboard(void)
{
    static hid_state_t s;
    CHECK(hid_state_init(&s, keyboard_descriptor, sizeof(keyboard_descriptor)));
    CHECK(s.descriptor.kinds == HID_KIND_KEYBOARD);
    CHECK(!s.descriptor.uses_report_ids);
    CHECK(hid_report_size(&s.descriptor) == 8);
    CHECK(s.descriptor.field_count == 2); /* the LEDs are an output report */

    feed(&s, (uint8_t[]){ 0x02, 0, 0x04, 0, 0, 0, 0, 0 }, 8); /* left shift + a */
    CHECK(event_count == 2);
    CHECK(is(0, JELLY_INPUT_KEY_DOWN, JELLY_KEY_A, 1) || is(1, JELLY_INPUT_KEY_DOWN, JELLY_KEY_A, 1));
    CHECK(is(0, JELLY_INPUT_KEY_DOWN, JELLY_KEY_LEFTSHIFT, 1) || is(1, JELLY_INPUT_KEY_DOWN, JELLY_KEY_LEFTSHIFT, 1));

    feed(&s, (uint8_t[]){ 0x02, 0, 0x04, 0, 0, 0, 0, 0 }, 8); /* unchanged */
    CHECK(event_count == 0);

    feed(&s, (uint8_t[]){ 0x02, 0, 0x05, 0x04, 0, 0, 0, 0 }, 8); /* b joins, the order in the array is free */
    CHECK(event_count == 1 && is(0, JELLY_INPUT_KEY_DOWN, JELLY_KEY_B, 1));

    feed(&s, (uint8_t[]){ 0x02, 0, 1, 1, 1, 1, 1, 1 }, 8); /* rollover error: nothing changes */
    CHECK(event_count == 0);

    feed(&s, (uint8_t[]){ 0x00, 0, 0x28, 0, 0, 0, 0, 0 }, 8); /* a, b and shift up, enter down: releases first */
    CHECK(event_count == 4);
    CHECK(events[0].type == JELLY_INPUT_KEY_UP && events[1].type == JELLY_INPUT_KEY_UP &&
          events[2].type == JELLY_INPUT_KEY_UP);
    CHECK(is(3, JELLY_INPUT_KEY_DOWN, JELLY_KEY_ENTER, 1));

    feed(&s, (uint8_t[]){ 0x00, 0, 0x28 }, 3); /* a short report changes nothing it does not contain */
    CHECK(event_count == 0);

    event_count = 0;
    hid_release_all(&s, record, NULL);
    CHECK(event_count == 1 && is(0, JELLY_INPUT_KEY_UP, JELLY_KEY_ENTER, 0));

    CHECK(hid_key_code(0x04) == JELLY_KEY_A && hid_key_code(0x1D) == JELLY_KEY_Z && hid_key_code(0x2C) == JELLY_KEY_SPACE);
    CHECK(hid_key_code(0x52) == JELLY_KEY_UP && hid_key_code(0xE4) == JELLY_KEY_RIGHTCTRL && hid_key_code(0x03) == 0);
}

static void test_mouse(void)
{
    static hid_state_t s;
    CHECK(hid_state_init(&s, mouse_descriptor, sizeof(mouse_descriptor)));
    CHECK(s.descriptor.kinds == HID_KIND_MOUSE && hid_report_size(&s.descriptor) == 4);

    feed(&s, (uint8_t[]){ 0x01, 5, 0xFD, 0 }, 4); /* left down, moved +5, -3 */
    CHECK(event_count == 2);
    CHECK(events[0].type == JELLY_INPUT_MOUSE_MOVE && events[0].dx == 5 && events[0].dy == -3 && events[0].flags == 0);
    CHECK(is(1, JELLY_INPUT_MOUSE_BUTTON, JELLY_BUTTON_LEFT, 1));

    feed(&s, (uint8_t[]){ 0x04, 0, 0, 0xFF }, 4); /* left up, middle down, wheel one step towards the user */
    CHECK(event_count == 3);
    CHECK(is(0, JELLY_INPUT_MOUSE_BUTTON, JELLY_BUTTON_LEFT, 0));
    CHECK(is(1, JELLY_INPUT_MOUSE_BUTTON, JELLY_BUTTON_MIDDLE, 1));
    CHECK(is(2, JELLY_INPUT_MOUSE_WHEEL, 0, -1));
}

static void test_tablet(void)
{
    static hid_state_t s;
    CHECK(hid_state_init(&s, tablet_descriptor, sizeof(tablet_descriptor)));
    CHECK(s.descriptor.kinds == HID_KIND_MOUSE && hid_report_size(&s.descriptor) == 6);

    feed(&s, (uint8_t[]){ 0x00, 0xFF, 0x7F, 0x00, 0x00, 0 }, 6); /* right edge, top */
    CHECK(event_count == 1);
    CHECK(events[0].type == JELLY_INPUT_MOUSE_MOVE && (events[0].flags & JELLY_INPUT_ABSOLUTE));
    CHECK(events[0].x == 65535 && events[0].y == 0);

    feed(&s, (uint8_t[]){ 0x02, 0x00, 0x40, 0x00, 0x20, 0 }, 6); /* middle, a quarter down, right button */
    CHECK(event_count == 2);
    CHECK(events[0].x == 32769 && events[0].y == 16384); /* 16384 and 8192 of 32767, rounded */
    CHECK(is(1, JELLY_INPUT_MOUSE_BUTTON, JELLY_BUTTON_RIGHT, 1));
}

static int axis_event(uint32_t code, int32_t *value)
{
    for (int i = 0; i < event_count; i++) {
        if (events[i].type == JELLY_INPUT_GAMEPAD_AXIS && events[i].code == code) {
            *value = events[i].value;
            return 1;
        }
    }
    return 0;
}

static void test_gamepad(void)
{
    static hid_state_t s;
    int32_t v;
    CHECK(hid_state_init(&s, gamepad_descriptor, sizeof(gamepad_descriptor)));
    CHECK(s.descriptor.kinds == HID_KIND_GAMEPAD && s.descriptor.uses_report_ids);
    CHECK(hid_report_size(&s.descriptor) == 7 && !s.right_stick_rx);

    /* Centered, hat released (8 is outside 0..7): the first report announces every axis */
    feed(&s, (uint8_t[]){ 1, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80 }, 7);
    CHECK(event_count == 6);
    CHECK(axis_event(JELLY_AXIS_LEFT_X, &v) && v >= -200 && v <= 200);
    CHECK(axis_event(JELLY_AXIS_RIGHT_Y, &v) && v >= -200 && v <= 200);
    CHECK(axis_event(JELLY_AXIS_HAT_X, &v) && v == 0);
    CHECK(axis_event(JELLY_AXIS_HAT_Y, &v) && v == 0);

    feed(&s, (uint8_t[]){ 1, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80 }, 7);
    CHECK(event_count == 0);

    /* Buttons 1 and 10, hat up-right, left stick fully left and down */
    feed(&s, (uint8_t[]){ 1, 0x01, 0x12, 0x00, 0xFF, 0x80, 0x80 }, 7);
    CHECK(axis_event(JELLY_AXIS_HAT_X, &v) && v == 32767);
    CHECK(axis_event(JELLY_AXIS_HAT_Y, &v) && v == -32767);
    CHECK(axis_event(JELLY_AXIS_LEFT_X, &v) && v == -32768);
    CHECK(axis_event(JELLY_AXIS_LEFT_Y, &v) && v == 32767);
    CHECK(!axis_event(JELLY_AXIS_RIGHT_X, &v));
    CHECK(event_count == 6);
    CHECK(is(4, JELLY_INPUT_GAMEPAD_BUTTON, 0, 1) && is(5, JELLY_INPUT_GAMEPAD_BUTTON, 9, 1));

    /* Another report ID is not ours */
    feed(&s, (uint8_t[]){ 2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 7);
    CHECK(event_count == 0);

    event_count = 0;
    hid_release_all(&s, record, NULL);
    CHECK(event_count == 2 && is(0, JELLY_INPUT_GAMEPAD_BUTTON, 0, 0) && is(1, JELLY_INPUT_GAMEPAD_BUTTON, 9, 0));

    /* Sticks on X/Y and Rx/Ry: Z and Rz are the triggers */
    CHECK(hid_state_init(&s, controller_descriptor, sizeof(controller_descriptor)));
    CHECK(s.right_stick_rx && hid_report_size(&s.descriptor) == 11);
    feed(&s, (uint8_t[]){ 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x40, 0xFF, 0x00, 0x80 }, 11);
    CHECK(axis_event(JELLY_AXIS_LEFT_X, &v) && v == -32768);
    CHECK(axis_event(JELLY_AXIS_LEFT_Y, &v) && v == 32767);
    CHECK(axis_event(JELLY_AXIS_RIGHT_X, &v) && v >= -1 && v <= 1);
    CHECK(axis_event(JELLY_AXIS_RIGHT_Y, &v) && v >= 16383 && v <= 16385);
    CHECK(axis_event(JELLY_AXIS_TRIGGER_L, &v) && v == 32767);
    CHECK(axis_event(JELLY_AXIS_TRIGGER_R, &v) && v == -32768);
    CHECK(is(event_count - 1, JELLY_INPUT_GAMEPAD_BUTTON, 7, 1));
}

static void test_malformed(void)
{
    static hid_descriptor_t d;
    CHECK(!hid_parse(NULL, 0, &d));
    CHECK(!hid_parse(keyboard_descriptor, 7, &d));                  /* cut inside an item / no input fields */
    CHECK(!hid_parse((uint8_t[]){ 0x05 }, 1, &d));                  /* data missing */
    CHECK(!hid_parse((uint8_t[]){ 0xB4 }, 1, &d));                  /* pop without push */
    CHECK(!hid_parse((uint8_t[]){ 0xFE, 0x05, 0x00 }, 3, &d));      /* long item past the end */
    /* A report of absurd size is refused instead of overflowing the offsets */
    CHECK(!hid_parse((uint8_t[]){ 0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x30, 0x75, 0x20, 0x96, 0xFF, 0xFF, 0x81,
                                  0x02, 0xC0 },
                     16, &d));
    /* A vendor-defined device parses but has nothing for us */
    static hid_state_t s;
    CHECK(!hid_state_init(&s, (uint8_t[]){ 0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x09, 0x02, 0x15, 0x00, 0x25,
                                           0x7F, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0xC0 },
                          20));
}

int main(void)
{
    test_keyboard();
    test_mouse();
    test_tablet();
    test_gamepad();
    test_malformed();
    printf("unit: hid %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
