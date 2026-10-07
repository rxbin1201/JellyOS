/*
 * PS/2 keyboard and mouse through the i8042 controller (README section 37).
 *
 * Keyboard: scan code set 1 (the controller translates set 2), including
 * the E0-prefixed keys; make codes become KEY_DOWN (the keyboard's own
 * typematic repeat arrives as repeated make codes: value 2), break codes
 * KEY_UP. Mouse: 3-byte packets, or 4 bytes with a wheel once the
 * IntelliMouse sequence is accepted; movement is relative.
 *
 * The bytes arrive by interrupt (ISA IRQ 1 and 12) and are translated into
 * the input manager's standardized events.
 */

#include "drivers/input/ps2.h"

#include "drivers/core/module.h"
#include "input/input.h"

#include "core/arch.h"
#include "core/log.h"
#include "time/clock.h"

#include <jelly/input.h>

#define DATA_PORT    0x60
#define STATUS_PORT  0x64
#define COMMAND_PORT 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_AUX_DATA    0x20

#define CMD_READ_CONFIG    0x20
#define CMD_WRITE_CONFIG   0x60
#define CMD_DISABLE_AUX    0xA7
#define CMD_ENABLE_AUX     0xA8
#define CMD_SELF_TEST      0xAA
#define CMD_DISABLE_KBD    0xAD
#define CMD_ENABLE_KBD     0xAE
#define CMD_WRITE_AUX      0xD4

#define CONFIG_KBD_IRQ     0x01
#define CONFIG_AUX_IRQ     0x02
#define CONFIG_KBD_CLOCK_OFF 0x10
#define CONFIG_AUX_CLOCK_OFF 0x20
#define CONFIG_TRANSLATE   0x40

#define TIMEOUT_NS 50000000ULL

static uint32_t keyboard_device, mouse_device;
static bool keyboard_extended;
static uint8_t key_down[JELLY_KEY_MAX + 1];
static uint8_t packet[4];
static unsigned packet_length = 3, packet_fill;
static uint8_t buttons;
static bool has_keyboard, has_mouse;

/* Scan code set 1 make codes equal the evdev key codes for 0x01-0x58. E0-prefixed keys: */
static const struct {
    uint8_t  scan;
    uint16_t key;
} extended_keys[] = {
    { 0x1C, JELLY_KEY_KPENTER }, { 0x1D, JELLY_KEY_RIGHTCTRL }, { 0x38, JELLY_KEY_RIGHTALT },
    { 0x47, JELLY_KEY_HOME },    { 0x48, JELLY_KEY_UP },        { 0x49, JELLY_KEY_PAGEUP },
    { 0x4B, JELLY_KEY_LEFT },    { 0x4D, JELLY_KEY_RIGHT },     { 0x4F, JELLY_KEY_END },
    { 0x50, JELLY_KEY_DOWN },    { 0x51, JELLY_KEY_PAGEDOWN },  { 0x52, JELLY_KEY_INSERT },
    { 0x53, JELLY_KEY_DELETE },  { 0x5B, JELLY_KEY_LEFTMETA },  { 0x5C, JELLY_KEY_RIGHTMETA },
    { 0x35, 98 /* keypad / */ }, { 0x37, 99 /* print screen */ }, { 0x5D, 127 /* menu */ },
};

bool ps2_keyboard_present(void)
{
    return has_keyboard;
}

unsigned ps2_mouse_packet_length(void)
{
    return has_mouse ? packet_length : 0;
}

static bool wait_input_empty(void)
{
    uint64_t deadline = clock_monotonic_ns() + TIMEOUT_NS;
    while (arch_io_read8(STATUS_PORT) & STATUS_INPUT_FULL) {
        if (clock_monotonic_ns() > deadline)
            return false;
    }
    return true;
}

static bool wait_output_full(void)
{
    uint64_t deadline = clock_monotonic_ns() + TIMEOUT_NS;
    while (!(arch_io_read8(STATUS_PORT) & STATUS_OUTPUT_FULL)) {
        if (clock_monotonic_ns() > deadline)
            return false;
    }
    return true;
}

static void command(uint8_t value)
{
    wait_input_empty();
    arch_io_write8(COMMAND_PORT, value);
}

static void write_data(uint8_t value)
{
    wait_input_empty();
    arch_io_write8(DATA_PORT, value);
}

static int read_data(void)
{
    return wait_output_full() ? arch_io_read8(DATA_PORT) : -1;
}

static void flush(void)
{
    for (int i = 0; i < 32 && (arch_io_read8(STATUS_PORT) & STATUS_OUTPUT_FULL); i++)
        arch_io_read8(DATA_PORT);
}

/* Send a byte to the mouse and wait for its ACK (0xFA). Interrupts for the mouse must be off. */
static bool mouse_command(uint8_t value)
{
    command(CMD_WRITE_AUX);
    write_data(value);
    return read_data() == 0xFA;
}

static void keyboard_byte(uint8_t scan)
{
    if (scan == 0xE0) {
        keyboard_extended = true;
        return;
    }
    if (scan == 0xE1) /* Pause: the rest of its sequence is ignored as unknown codes */
        return;
    bool release = scan & 0x80;
    uint8_t code = scan & 0x7F;
    uint32_t key = 0;

    if (keyboard_extended) {
        keyboard_extended = false;
        for (size_t i = 0; i < sizeof(extended_keys) / sizeof(extended_keys[0]); i++) {
            if (extended_keys[i].scan == code)
                key = extended_keys[i].key;
        }
    } else if (code >= 0x01 && code <= 0x58) {
        key = code;
    }
    if (!key)
        return;

    if (release) {
        key_down[key] = 0;
        input_report(keyboard_device, JELLY_INPUT_KEY_UP, key, 0, 0, 0, 0, 0, 0);
    } else {
        int32_t value = key_down[key] ? 2 : 1; /* typematic repeat */
        key_down[key] = 1;
        input_report(keyboard_device, JELLY_INPUT_KEY_DOWN, key, value, 0, 0, 0, 0, 0);
    }
}

static void mouse_byte(uint8_t byte)
{
    /* Bit 3 of the first byte is always set: resynchronize on it. */
    if (packet_fill == 0 && !(byte & 0x08))
        return;
    packet[packet_fill++] = byte;
    if (packet_fill < packet_length)
        return;
    packet_fill = 0;

    uint8_t state = packet[0];
    if (state & 0xC0)
        return; /* overflow: the deltas are garbage */
    int32_t dx = packet[1] - ((state & 0x10) ? 256 : 0);
    int32_t dy = packet[2] - ((state & 0x20) ? 256 : 0);
    if (dx || dy)
        input_report(mouse_device, JELLY_INPUT_MOUSE_MOVE, 0, 0, dx, -dy, 0, 0, 0); /* PS/2 counts up as positive */

    static const uint8_t button_codes[3] = { JELLY_BUTTON_LEFT, JELLY_BUTTON_RIGHT, JELLY_BUTTON_MIDDLE };
    uint8_t now = state & 0x07;
    for (int i = 0; i < 3; i++) {
        if ((now ^ buttons) & (1u << i))
            input_report(mouse_device, JELLY_INPUT_MOUSE_BUTTON, button_codes[i], (now >> i) & 1, 0, 0, 0, 0, 0);
    }
    buttons = now;
    if (packet_length == 4 && (int8_t)packet[3])
        input_report(mouse_device, JELLY_INPUT_MOUSE_WHEEL, 0, -(int8_t)packet[3], 0, 0, 0, 0, 0);
}

static void ps2_interrupt(void *context)
{
    (void)context;
    /* Either interrupt drains everything that is waiting. */
    for (int i = 0; i < 16; i++) {
        uint8_t status = arch_io_read8(STATUS_PORT);
        if (!(status & STATUS_OUTPUT_FULL))
            break;
        uint8_t byte = arch_io_read8(DATA_PORT);
        if (status & STATUS_AUX_DATA) {
            if (has_mouse)
                mouse_byte(byte);
        } else {
            keyboard_byte(byte);
        }
    }
}

static bool controller_present(void)
{
    /* A floating bus reads 0xFF. */
    if (arch_io_read8(STATUS_PORT) == 0xFF)
        return false;
    command(CMD_DISABLE_KBD);
    command(CMD_DISABLE_AUX);
    flush();
    command(CMD_SELF_TEST);
    return read_data() == 0x55;
}

static void setup_mouse(void)
{
    command(CMD_ENABLE_AUX);
    flush();
    if (!mouse_command(0xF6)) /* defaults */
        return;
    /* IntelliMouse: sample rates 200, 100, 80, then the ID becomes 3 and packets have a wheel byte. */
    static const uint8_t magic[] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) {
        mouse_command(0xF3);
        mouse_command(magic[i]);
    }
    if (mouse_command(0xF2) && read_data() == 3)
        packet_length = 4;
    has_mouse = mouse_command(0xF4); /* enable reporting */
}

static status_t ps2_init(void)
{
    uint32_t irq, gsi;

    if (!controller_present()) {
        klog_info("ps2: no i8042 controller");
        return STATUS_SUCCESS;
    }
    setup_mouse();

    command(CMD_READ_CONFIG);
    int config = read_data();
    if (config < 0)
        config = CONFIG_TRANSLATE;
    config |= CONFIG_KBD_IRQ | CONFIG_TRANSLATE | (has_mouse ? CONFIG_AUX_IRQ : 0);
    config &= ~(CONFIG_KBD_CLOCK_OFF | (has_mouse ? CONFIG_AUX_CLOCK_OFF : 0));
    command(CMD_WRITE_CONFIG);
    write_data((uint8_t)config);
    command(CMD_ENABLE_KBD);
    flush();

    keyboard_device = input_register_device("PS/2 keyboard");
    status_t status = arch_irq_allocate(ps2_interrupt, NULL, &irq);
    if (!STATUS_IS_ERROR(status))
        status = arch_irq_route_isa(1, irq, &gsi);
    if (STATUS_IS_ERROR(status))
        return status;
    has_keyboard = true;
    if (has_mouse) {
        mouse_device = input_register_device(packet_length == 4 ? "PS/2 mouse with wheel" : "PS/2 mouse");
        status = arch_irq_allocate(ps2_interrupt, NULL, &irq);
        if (!STATUS_IS_ERROR(status))
            status = arch_irq_route_isa(12, irq, &gsi);
    }
    return status;
}

MODULE(.name = "ps2", .description = "PS/2 keyboard and mouse (i8042)", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 11, 0), .init = ps2_init);
