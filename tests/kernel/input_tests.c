/*
 * Kernel tests for Phase 11, input: the PS/2 driver (bytes injected through
 * the i8042 controller), HID reports of a gamepad on their way through the
 * input manager, and USB enumeration behind the xHCI controller.
 */

#include "tests/kernel/ktest.h"

#include "core/arch.h"
#include "core/log.h"
#include "drivers/core/device.h"
#include "drivers/input/hid.h"
#include "drivers/input/ps2.h"
#include "input/input.h"
#include "memory/heap.h"
#include "scheduler/thread.h"

#include <jelly/input.h>
#include <jelly/syscall.h>

/* Wait up to half a second for the next event of this type. */
static bool next_event(object_t *queue, uint32_t type, jelly_input_event_t *event)
{
    for (int i = 0; i < 50; i++) {
        while (input_read(queue, event, 1) == 1) {
            if (event->type == type)
                return true;
        }
        thread_sleep(10000000);
    }
    return false;
}

/* Controller commands 0xD2/0xD3: the next data byte appears as if the keyboard/mouse had sent it. */
static void ps2_inject(bool mouse, uint8_t byte)
{
    for (int i = 0; i < 1000 && (arch_io_read8(0x64) & 0x03); i++)
        thread_sleep(1000000); /* until the driver has taken the previous byte */
    arch_io_write8(0x64, mouse ? 0xD3 : 0xD2);
    for (int i = 0; i < 1000 && (arch_io_read8(0x64) & 0x02); i++)
        ;
    arch_io_write8(0x60, byte);
}

KTEST(ps2_keyboard_scancodes)
{
    object_t *queue;
    jelly_input_event_t event;

    if (!ps2_keyboard_present()) {
        klog_info("ktest: no PS/2 controller, skipped");
        return;
    }
    KASSERT(input_open(&queue) == STATUS_SUCCESS);

    ps2_inject(false, 0x1E); /* A pressed */
    KEXPECT(next_event(queue, JELLY_INPUT_KEY_DOWN, &event) && event.code == JELLY_KEY_A && event.value == 1);
    ps2_inject(false, 0x1E); /* typematic repeat */
    KEXPECT(next_event(queue, JELLY_INPUT_KEY_DOWN, &event) && event.code == JELLY_KEY_A && event.value == 2);
    ps2_inject(false, 0x9E); /* released */
    KEXPECT(next_event(queue, JELLY_INPUT_KEY_UP, &event) && event.code == JELLY_KEY_A);

    ps2_inject(false, 0xE0); /* extended: cursor up */
    ps2_inject(false, 0x48);
    KEXPECT(next_event(queue, JELLY_INPUT_KEY_DOWN, &event) && event.code == JELLY_KEY_UP);
    ps2_inject(false, 0xE0);
    ps2_inject(false, 0xC8);
    KEXPECT(next_event(queue, JELLY_INPUT_KEY_UP, &event) && event.code == JELLY_KEY_UP);

    ps2_inject(false, 0x48); /* without the prefix the same code is keypad 8 */
    KEXPECT(next_event(queue, JELLY_INPUT_KEY_DOWN, &event) && event.code == 72);
    ps2_inject(false, 0xC8);
    KEXPECT(next_event(queue, JELLY_INPUT_KEY_UP, &event));
    object_release(queue);
}

KTEST(ps2_mouse_packets)
{
    object_t *queue;
    jelly_input_event_t event;
    unsigned length = ps2_mouse_packet_length();

    if (!length) {
        klog_info("ktest: no PS/2 mouse, skipped");
        return;
    }
    KASSERT(input_open(&queue) == STATUS_SUCCESS);

    /* Left button, 5 to the right, 3 up (PS/2 counts up as positive, events count down) */
    ps2_inject(true, 0x09);
    ps2_inject(true, 5);
    ps2_inject(true, 3);
    if (length == 4)
        ps2_inject(true, 0);
    KEXPECT(next_event(queue, JELLY_INPUT_MOUSE_MOVE, &event) && event.dx == 5 && event.dy == -3 &&
            !(event.flags & JELLY_INPUT_ABSOLUTE));
    KEXPECT(next_event(queue, JELLY_INPUT_MOUSE_BUTTON, &event) && event.code == JELLY_BUTTON_LEFT && event.value == 1);

    /* Released, moved left and down (negative X: sign bit 4; negative Y: sign bit 5) */
    ps2_inject(true, 0x08 | 0x10 | 0x20);
    ps2_inject(true, 0xFE);
    ps2_inject(true, 0xFC);
    if (length == 4)
        ps2_inject(true, 0xFF); /* wheel one step away from the user */
    KEXPECT(next_event(queue, JELLY_INPUT_MOUSE_MOVE, &event) && event.dx == -2 && event.dy == 4);
    KEXPECT(next_event(queue, JELLY_INPUT_MOUSE_BUTTON, &event) && event.code == JELLY_BUTTON_LEFT && event.value == 0);
    if (length == 4)
        KEXPECT(next_event(queue, JELLY_INPUT_MOUSE_WHEEL, &event) && event.value == 1);
    object_release(queue);
}

/* 4 buttons, X and Y (8 bits each) */
static const uint8_t pad_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x04, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0xC0,
};

static void to_input_manager(void *context, uint32_t type, uint32_t code, int32_t value, int32_t dx, int32_t dy,
                             int32_t x, int32_t y, uint32_t flags)
{
    input_report(*(uint32_t *)context, type, code, value, dx, dy, x, y, flags);
}

KTEST(gamepad_reports_become_events)
{
    object_t *queue;
    jelly_input_event_t events[8];
    uint32_t device = 9;
    hid_state_t *pad = kmalloc(sizeof(*pad));

    KASSERT(pad != NULL);
    KASSERT(hid_state_init(pad, pad_descriptor, sizeof(pad_descriptor)));
    KEXPECT(pad->descriptor.kinds == HID_KIND_GAMEPAD);
    KASSERT(input_open(&queue) == STATUS_SUCCESS);

    hid_process(pad, (uint8_t[]){ 0x04, 0x00, 0xFF }, 3, to_input_manager, &device); /* button 3, stick left and down */
    KASSERT(input_read(queue, events, 8) == 3);
    KEXPECT(events[0].type == JELLY_INPUT_GAMEPAD_AXIS && events[0].code == JELLY_AXIS_LEFT_X &&
            events[0].value == -32768);
    KEXPECT(events[1].type == JELLY_INPUT_GAMEPAD_AXIS && events[1].code == JELLY_AXIS_LEFT_Y &&
            events[1].value == 32767);
    KEXPECT(events[2].type == JELLY_INPUT_GAMEPAD_BUTTON && events[2].code == 2 && events[2].value == 1 &&
            events[2].device == 9);

    hid_release_all(pad, to_input_manager, &device);
    KASSERT(input_read(queue, events, 8) == 1);
    KEXPECT(events[0].type == JELLY_INPUT_GAMEPAD_BUTTON && events[0].code == 2 && events[0].value == 0);
    object_release(queue);
    kfree(pad);
}

KTEST(usb_devices_enumerate)
{
    /* QEMU's xHCI controller with its HID devices (0627:0001), as `make test` attaches them */
    if (!device_find("pci", 0x1b36, 0x000d, 0)) {
        klog_info("ktest: no QEMU xHCI controller, skipped");
        return;
    }
    device_t *keyboard = NULL, *tablet = NULL;
    for (int i = 0; i < 300 && !(keyboard && tablet && keyboard->state == DEVICE_RUNNING &&
                                 tablet->state == DEVICE_RUNNING); i++) {
        thread_sleep(10000000); /* the controller's thread enumerates the ports */
        keyboard = device_find("usb", 0x0627, 0x0001, 0);
        tablet = device_find("usb", 0x0627, 0x0001, 1);
    }
    KASSERT(keyboard != NULL && tablet != NULL);
    KEXPECT(keyboard->state == DEVICE_RUNNING && tablet->state == DEVICE_RUNNING);
    KEXPECT(keyboard->id.class_code == 3 && tablet->id.class_code == 3);
    KEXPECT(keyboard->driver && tablet->driver);
}
