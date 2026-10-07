/*
 * HID report descriptors and reports (USB HID 1.11, section 6.2.2).
 *
 * hid_parse() turns a report descriptor into the list of input fields;
 * hid_process() decodes a report with it and emits JellyOS input events for
 * keyboards, mice (relative or absolute) and gamepads/joysticks.
 *
 * This file and hid.c use nothing from the kernel, so the host unit tests
 * (tests/unit/hid_test.c) compile them directly.
 */

#ifndef DRIVERS_INPUT_HID_H
#define DRIVERS_INPUT_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_MAX_FIELDS  48
#define HID_MAX_USAGES  8  /* usage entries (single usages or ranges) per field */
#define HID_MAX_REPORTS 32

#define HID_FIELD_CONSTANT (1u << 0)
#define HID_FIELD_VARIABLE (1u << 1) /* otherwise an array of usage indexes */
#define HID_FIELD_RELATIVE (1u << 2)

/* Usages are 32 bits: page << 16 | id */
#define HID_USAGE(page, id) (((uint32_t)(page) << 16) | (id))
#define HID_PAGE_DESKTOP  0x01
#define HID_PAGE_KEYBOARD 0x07
#define HID_PAGE_BUTTON   0x09

/* What a field's top-level application collection is */
#define HID_KIND_KEYBOARD (1u << 0)
#define HID_KIND_MOUSE    (1u << 1)
#define HID_KIND_GAMEPAD  (1u << 2)

typedef struct {
    uint32_t min, max;
} hid_usage_range_t;

typedef struct {
    uint8_t           report_id;
    uint8_t           flags;       /* HID_FIELD_* */
    uint8_t           kind;        /* HID_KIND_* or 0 */
    uint8_t           usage_count;
    hid_usage_range_t usages[HID_MAX_USAGES];
    int32_t           logical_min, logical_max;
    uint32_t          bit_offset;  /* in the report, after the ID byte */
    uint32_t          bit_size;    /* of one element */
    uint32_t          count;       /* elements */
} hid_field_t;

typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    uint32_t    field_count;
    bool        uses_report_ids;
    uint32_t    kinds;             /* HID_KIND_* of all fields */
    struct {
        uint8_t  id;
        uint32_t bits;
    } reports[HID_MAX_REPORTS];
    uint32_t    report_count;
} hid_descriptor_t;

/* Parse a report descriptor. False if it is malformed or has no input fields. */
bool     hid_parse(const uint8_t *data, size_t size, hid_descriptor_t *descriptor);

/* Bytes of the longest input report, including the ID byte. */
size_t   hid_report_size(const hid_descriptor_t *descriptor);

/* The usage of element `index` of a variable field. */
uint32_t hid_field_usage(const hid_field_t *field, uint32_t index);

/* The value of element `index`; false if the report is too short. */
bool     hid_field_value(const hid_field_t *field, const uint8_t *data, size_t size, uint32_t index, int32_t *value);

/* --- Reports to input events ---------------------------------------------------- */

/* Same parameters as input_report() after the device. */
typedef void (*hid_emit_t)(void *context, uint32_t type, uint32_t code, int32_t value, int32_t dx, int32_t dy,
                           int32_t x, int32_t y, uint32_t flags);

#define HID_AXES 8

typedef struct {
    hid_descriptor_t descriptor;
    uint8_t  keys[32];           /* pressed keyboard usages (bitmap) */
    uint32_t mouse_buttons;
    uint32_t pad_buttons;
    int32_t  axes[HID_AXES];     /* last values sent */
    bool     axis_known[HID_AXES];
    bool     right_stick_rx;     /* Rx/Ry are the right stick (then Z/Rz are the triggers) */
} hid_state_t;

/* Parse the descriptor and reset the state. False as hid_parse(), or if no field is one we handle. */
bool hid_state_init(hid_state_t *state, const uint8_t *descriptor, size_t size);

/* Decode one input report (with its ID byte, if the descriptor uses IDs). */
void hid_process(hid_state_t *state, const uint8_t *report, size_t size, hid_emit_t emit, void *context);

/* The device is gone: release everything that is still pressed. */
void hid_release_all(hid_state_t *state, hid_emit_t emit, void *context);

/* JellyOS key code (evdev numbering) of a keyboard-page usage, 0 if none. */
uint32_t hid_key_code(uint32_t usage_id);

#endif
