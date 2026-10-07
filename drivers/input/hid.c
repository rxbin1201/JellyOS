/*
 * HID report descriptor parser and report decoder. See hid.h.
 *
 * No kernel dependencies: compiled into the kernel and into the host unit tests.
 */

#include "drivers/input/hid.h"

#include <jelly/input.h>
#include <jelly/syscall.h>

/* --- Report descriptor ----------------------------------------------------------- */

typedef struct {
    uint32_t usage_page;
    int32_t  logical_min, logical_max;
    uint32_t logical_max_raw;
    uint32_t report_size, report_count;
    uint8_t  report_id;
} globals_t;

static uint8_t kind_of(uint32_t usage)
{
    switch (usage) {
    case HID_USAGE(HID_PAGE_DESKTOP, 0x06): /* keyboard */
    case HID_USAGE(HID_PAGE_DESKTOP, 0x07): /* keypad */
        return HID_KIND_KEYBOARD;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x01): /* pointer */
    case HID_USAGE(HID_PAGE_DESKTOP, 0x02): /* mouse */
        return HID_KIND_MOUSE;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x04): /* joystick */
    case HID_USAGE(HID_PAGE_DESKTOP, 0x05): /* gamepad */
    case HID_USAGE(HID_PAGE_DESKTOP, 0x08): /* multi-axis controller */
        return HID_KIND_GAMEPAD;
    }
    return 0;
}

static uint32_t *report_bits(hid_descriptor_t *d, uint8_t id)
{
    for (uint32_t i = 0; i < d->report_count; i++) {
        if (d->reports[i].id == id)
            return &d->reports[i].bits;
    }
    if (d->report_count == HID_MAX_REPORTS)
        return NULL;
    d->reports[d->report_count].id = id;
    d->reports[d->report_count].bits = 0;
    return &d->reports[d->report_count++].bits;
}

static bool add_input(hid_descriptor_t *d, const globals_t *g, uint32_t item, const hid_usage_range_t *usages,
                      uint32_t usage_count, uint8_t kind)
{
    uint32_t *bits = report_bits(d, g->report_id);
    if (!bits)
        return false;
    uint64_t total = (uint64_t)g->report_size * g->report_count;
    if (total > 65536)
        return false;
    uint32_t offset = *bits;
    *bits += (uint32_t)total;

    /* Padding, things we do not decode, and what does not fit only take up space. */
    if ((item & 1) || !kind || !usage_count || g->report_size == 0 || g->report_size > 32 || g->report_count == 0 ||
        d->field_count == HID_MAX_FIELDS)
        return true;

    hid_field_t *f = &d->fields[d->field_count++];
    f->report_id = g->report_id;
    f->flags = (uint8_t)(((item & 2) ? HID_FIELD_VARIABLE : 0) | ((item & 4) ? HID_FIELD_RELATIVE : 0));
    f->kind = kind;
    f->usage_count = (uint8_t)usage_count;
    for (uint32_t i = 0; i < usage_count; i++) {
        f->usages[i] = usages[i];
        /* A usage without a page gets the page in effect at the main item. */
        if (!(f->usages[i].min >> 16))
            f->usages[i].min |= g->usage_page << 16;
        if (!(f->usages[i].max >> 16))
            f->usages[i].max |= g->usage_page << 16;
    }
    f->logical_min = g->logical_min;
    f->logical_max = g->logical_max;
    if (f->logical_max < f->logical_min) /* an unsigned maximum written in too few bytes */
        f->logical_max = (int32_t)g->logical_max_raw;
    f->bit_offset = offset;
    f->bit_size = g->report_size;
    f->count = g->report_count;
    d->kinds |= kind;
    return true;
}

bool hid_parse(const uint8_t *data, size_t size, hid_descriptor_t *d)
{
    globals_t g = { 0 }, stack[4];
    hid_usage_range_t usages[HID_MAX_USAGES];
    uint32_t usage_count = 0, usage_min = 0, stack_depth = 0, collection_depth = 0;
    uint8_t kind = 0;
    size_t i = 0;

    __builtin_memset(d, 0, sizeof(*d));
    while (i < size) {
        uint8_t prefix = data[i++];
        if (prefix == 0xFE) { /* long item: size, tag, data */
            if (i + 2 > size)
                return false;
            i += 2 + (size_t)data[i];
            continue;
        }
        uint32_t n = prefix & 3;
        if (n == 3)
            n = 4;
        if (i + n > size)
            return false;
        uint32_t u = 0;
        for (uint32_t k = 0; k < n; k++)
            u |= (uint32_t)data[i + k] << (8 * k);
        int32_t s = n == 1 ? (int8_t)u : n == 2 ? (int16_t)u : (int32_t)u;
        i += n;

        switch (prefix & 0xFC) {
        /* Main items */
        case 0x80: /* input */
            if (!add_input(d, &g, u, usages, usage_count, kind))
                return false;
            usage_count = 0;
            break;
        case 0x90: /* output */
        case 0xB0: /* feature */
            usage_count = 0;
            break;
        case 0xA0: /* collection */
            if (collection_depth++ == 0) {
                uint32_t usage = usage_count ? usages[0].min : 0;
                kind = kind_of((usage >> 16) ? usage : usage | (g.usage_page << 16));
            }
            usage_count = 0;
            break;
        case 0xC0: /* end collection */
            if (collection_depth && --collection_depth == 0)
                kind = 0;
            usage_count = 0;
            break;

        /* Global items */
        case 0x04: g.usage_page = u & 0xFFFF; break;
        case 0x14: g.logical_min = s; break;
        case 0x24: g.logical_max = s; g.logical_max_raw = u; break;
        case 0x74: g.report_size = u; break;
        case 0x84: g.report_id = (uint8_t)u; d->uses_report_ids = true; break;
        case 0x94: g.report_count = u; break;
        case 0xA4: /* push */
            if (stack_depth == 4)
                return false;
            stack[stack_depth++] = g;
            break;
        case 0xB4: /* pop */
            if (stack_depth == 0)
                return false;
            g = stack[--stack_depth];
            break;

        /* Local items */
        case 0x08: /* usage */
            if (usage_count < HID_MAX_USAGES)
                usages[usage_count++] = (hid_usage_range_t){ u, u };
            break;
        case 0x18: /* usage minimum */
            usage_min = u;
            break;
        case 0x28: /* usage maximum */
            if (usage_count < HID_MAX_USAGES && u >= usage_min)
                usages[usage_count++] = (hid_usage_range_t){ usage_min, u };
            break;
        }
    }
    return d->field_count > 0;
}

size_t hid_report_size(const hid_descriptor_t *d)
{
    uint32_t bits = 0;
    for (uint32_t i = 0; i < d->report_count; i++) {
        if (d->reports[i].bits > bits)
            bits = d->reports[i].bits;
    }
    return (bits + 7) / 8 + (d->uses_report_ids ? 1 : 0);
}

/* The index-th usage in declaration order; false past the last one. */
static bool usage_at(const hid_field_t *f, uint32_t index, uint32_t *usage)
{
    for (uint32_t i = 0; i < f->usage_count; i++) {
        uint32_t length = f->usages[i].max - f->usages[i].min + 1;
        if (index < length) {
            *usage = f->usages[i].min + index;
            return true;
        }
        index -= length;
    }
    return false;
}

uint32_t hid_field_usage(const hid_field_t *f, uint32_t index)
{
    uint32_t usage = 0;
    if (!usage_at(f, index, &usage) && f->usage_count)
        usage = f->usages[f->usage_count - 1].max; /* the last usage applies to the remaining elements */
    return usage;
}

bool hid_field_value(const hid_field_t *f, const uint8_t *data, size_t size, uint32_t index, int32_t *value)
{
    uint64_t first = (uint64_t)f->bit_offset + (uint64_t)index * f->bit_size;
    if ((first + f->bit_size + 7) / 8 > size)
        return false;

    uint64_t raw = 0;
    size_t byte = (size_t)(first / 8);
    uint32_t shift = (uint32_t)(first % 8), needed = shift + f->bit_size;
    for (uint32_t k = 0; k * 8 < needed; k++)
        raw |= (uint64_t)data[byte + k] << (8 * k);
    raw >>= shift;
    if (f->bit_size < 32)
        raw &= (1ull << f->bit_size) - 1;

    if (f->logical_min < 0 && f->bit_size < 32 && (raw & (1ull << (f->bit_size - 1))))
        raw |= ~((1ull << f->bit_size) - 1); /* sign extension */
    *value = (int32_t)(uint32_t)raw;
    return true;
}

/* --- Reports to events ----------------------------------------------------------- */

/* Keyboard page usages 0x00-0x67 as evdev key codes. */
static const uint8_t key_codes[0x68] = {
    0,   0,   0,   0,   30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,  /* 0x00: a-l */
    50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,  2,   3,   /* 0x10: m-z, 1, 2 */
    4,   5,   6,   7,   8,   9,   10,  11,  28,  1,   14,  15,  57,  12,  13,  26,  /* 0x20: 3-0, enter ... */
    27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,  /* 0x30: ... F1-F6 */
    65,  66,  67,  68,  87,  88,  99,  70,  119, 110, 102, 104, 111, 107, 109, 106, /* 0x40: F7-F12 ... right */
    105, 108, 103, 69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,  /* 0x50: left ... keypad */
    72,  73,  82,  83,  86,  127, 116, 117,                                         /* 0x60 */
};

/* 0xE0-0xE7: left control, shift, alt, meta; right control, shift, alt, meta */
static const uint8_t modifier_codes[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

uint32_t hid_key_code(uint32_t usage_id)
{
    if (usage_id < sizeof(key_codes))
        return key_codes[usage_id];
    if (usage_id >= 0xE0 && usage_id <= 0xE7)
        return modifier_codes[usage_id - 0xE0];
    switch (usage_id) {
    case 0x7F: return 113; /* mute */
    case 0x80: return 115; /* volume up */
    case 0x81: return 114; /* volume down */
    }
    return 0;
}

static int32_t scale(int32_t value, int32_t min, int32_t max, int32_t range)
{
    if (max <= min)
        return 0;
    if (value < min)
        value = min;
    if (value > max)
        value = max;
    int64_t span = (int64_t)max - min;
    return (int32_t)((((int64_t)value - min) * range + span / 2) / span);
}

static void axis(hid_state_t *state, uint32_t code, int32_t value, hid_emit_t emit, void *context)
{
    if (state->axis_known[code] && state->axes[code] == value)
        return;
    state->axis_known[code] = true;
    state->axes[code] = value;
    emit(context, JELLY_INPUT_GAMEPAD_AXIS, code, value, 0, 0, 0, 0, 0);
}

static void gamepad_value(hid_state_t *state, const hid_field_t *f, uint32_t usage, int32_t value, hid_emit_t emit,
                          void *context)
{
    static const int8_t hat_x[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int8_t hat_y[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    int code = -1;

    switch (usage) {
    case HID_USAGE(HID_PAGE_DESKTOP, 0x30): code = JELLY_AXIS_LEFT_X; break;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x31): code = JELLY_AXIS_LEFT_Y; break;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x32): code = state->right_stick_rx ? JELLY_AXIS_TRIGGER_L : JELLY_AXIS_RIGHT_X; break;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x33): code = state->right_stick_rx ? JELLY_AXIS_RIGHT_X : JELLY_AXIS_TRIGGER_L; break;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x34): code = state->right_stick_rx ? JELLY_AXIS_RIGHT_Y : JELLY_AXIS_TRIGGER_R; break;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x35): code = state->right_stick_rx ? JELLY_AXIS_TRIGGER_R : JELLY_AXIS_RIGHT_Y; break;
    case HID_USAGE(HID_PAGE_DESKTOP, 0x39): { /* hat switch: 8 (or 4) directions clockwise from up, else centered */
        int32_t x = 0, y = 0;
        if (value >= f->logical_min && value <= f->logical_max) {
            int32_t direction = value - f->logical_min;
            if (f->logical_max - f->logical_min == 3)
                direction *= 2;
            x = hat_x[direction & 7];
            y = hat_y[direction & 7];
        }
        axis(state, JELLY_AXIS_HAT_X, x * 32767, emit, context);
        axis(state, JELLY_AXIS_HAT_Y, y * 32767, emit, context);
        return;
    }
    }
    if (code >= 0)
        axis(state, (uint32_t)code, scale(value, f->logical_min, f->logical_max, 65535) - 32768, emit, context);
}

static void buttons_changed(uint32_t *state, uint32_t now, uint32_t type, const uint8_t *codes, uint32_t count,
                            hid_emit_t emit, void *context)
{
    uint32_t changed = *state ^ now;
    for (uint32_t i = 0; i < count; i++) {
        if (changed & (1u << i))
            emit(context, type, codes ? codes[i] : i, (now >> i) & 1, 0, 0, 0, 0, 0);
    }
    *state = now;
}

static void keys_changed(hid_state_t *state, const uint8_t *now, hid_emit_t emit, void *context)
{
    /* Releases first: a report that swaps two keys never shows both as held. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t usage = 0; usage < 256; usage++) {
            bool was = state->keys[usage / 8] & (1u << (usage % 8));
            bool is = now[usage / 8] & (1u << (usage % 8));
            uint32_t code = hid_key_code(usage);
            if (was == is || !code || is != (pass == 1))
                continue;
            emit(context, is ? JELLY_INPUT_KEY_DOWN : JELLY_INPUT_KEY_UP, code, is ? 1 : 0, 0, 0, 0, 0, 0);
        }
    }
    __builtin_memcpy(state->keys, now, sizeof(state->keys));
}

static const uint8_t mouse_button_codes[3] = { JELLY_BUTTON_LEFT, JELLY_BUTTON_RIGHT, JELLY_BUTTON_MIDDLE };

bool hid_state_init(hid_state_t *state, const uint8_t *descriptor, size_t size)
{
    bool rx = false, ry = false;

    __builtin_memset(state, 0, sizeof(*state));
    if (!hid_parse(descriptor, size, &state->descriptor))
        return false;
    for (uint32_t i = 0; i < state->descriptor.field_count; i++) {
        const hid_field_t *f = &state->descriptor.fields[i];
        if (f->kind != HID_KIND_GAMEPAD || !(f->flags & HID_FIELD_VARIABLE))
            continue;
        for (uint32_t k = 0; k < f->count; k++) {
            rx = rx || hid_field_usage(f, k) == HID_USAGE(HID_PAGE_DESKTOP, 0x33);
            ry = ry || hid_field_usage(f, k) == HID_USAGE(HID_PAGE_DESKTOP, 0x34);
        }
    }
    state->right_stick_rx = rx && ry;
    return state->descriptor.kinds != 0;
}

void hid_process(hid_state_t *state, const uint8_t *report, size_t size, hid_emit_t emit, void *context)
{
    const hid_descriptor_t *d = &state->descriptor;
    uint8_t id = 0, keys[32] = { 0 };
    bool keyboard = false, rollover = false, mouse = false, pad = false, absolute = false;
    uint32_t mouse_buttons = 0, pad_buttons = 0;
    int32_t dx = 0, dy = 0, wheel = 0, x = 0, y = 0;

    if (d->uses_report_ids) {
        if (size == 0)
            return;
        id = report[0];
        report++;
        size--;
    }

    for (uint32_t i = 0; i < d->field_count; i++) {
        const hid_field_t *f = &d->fields[i];
        if (f->report_id != id)
            continue;
        for (uint32_t k = 0; k < f->count; k++) {
            int32_t value;
            uint32_t usage;
            if (!hid_field_value(f, report, size, k, &value))
                break;
            if (f->flags & HID_FIELD_VARIABLE) {
                usage = hid_field_usage(f, k);
            } else {
                /* Array: the value selects the usage that is active. */
                if (value < f->logical_min || value > f->logical_max ||
                    !usage_at(f, (uint32_t)(value - f->logical_min), &usage))
                    continue;
                value = 1;
            }
            uint32_t page = usage >> 16, number = usage & 0xFFFF;

            switch (f->kind) {
            case HID_KIND_KEYBOARD:
                if (page != HID_PAGE_KEYBOARD)
                    break;
                keyboard = true;
                if (number == 1 && value)
                    rollover = true; /* too many keys: the report says nothing */
                else if (number >= 4 && number < 256 && value)
                    keys[number / 8] |= (uint8_t)(1u << (number % 8));
                break;
            case HID_KIND_MOUSE:
                if (page == HID_PAGE_BUTTON && (f->flags & HID_FIELD_VARIABLE)) {
                    mouse = true;
                    if (value && number >= 1 && number <= 3)
                        mouse_buttons |= 1u << (number - 1);
                } else if (usage == HID_USAGE(HID_PAGE_DESKTOP, 0x30) || usage == HID_USAGE(HID_PAGE_DESKTOP, 0x31)) {
                    bool is_x = number == 0x30;
                    if (f->flags & HID_FIELD_RELATIVE) {
                        *(is_x ? &dx : &dy) += value;
                    } else {
                        absolute = true;
                        *(is_x ? &x : &y) = scale(value, f->logical_min, f->logical_max, 65535);
                    }
                } else if (usage == HID_USAGE(HID_PAGE_DESKTOP, 0x38)) {
                    wheel += value;
                }
                break;
            case HID_KIND_GAMEPAD:
                if (page == HID_PAGE_BUTTON && (f->flags & HID_FIELD_VARIABLE)) {
                    pad = true;
                    if (value && number >= 1 && number <= 32)
                        pad_buttons |= 1u << (number - 1);
                } else if (f->flags & HID_FIELD_VARIABLE) {
                    gamepad_value(state, f, usage, value, emit, context);
                }
                break;
            }
        }
    }

    if (keyboard && !rollover)
        keys_changed(state, keys, emit, context);
    /* The pointer moves before its buttons change, so a click lands on the new position. */
    if (dx || dy)
        emit(context, JELLY_INPUT_MOUSE_MOVE, 0, 0, dx, dy, 0, 0, 0);
    if (absolute)
        emit(context, JELLY_INPUT_MOUSE_MOVE, 0, 0, 0, 0, x, y, JELLY_INPUT_ABSOLUTE);
    if (mouse)
        buttons_changed(&state->mouse_buttons, mouse_buttons, JELLY_INPUT_MOUSE_BUTTON, mouse_button_codes, 3, emit,
                        context);
    if (wheel)
        emit(context, JELLY_INPUT_MOUSE_WHEEL, 0, wheel, 0, 0, 0, 0, 0);
    if (pad)
        buttons_changed(&state->pad_buttons, pad_buttons, JELLY_INPUT_GAMEPAD_BUTTON, NULL, 32, emit, context);
}

void hid_release_all(hid_state_t *state, hid_emit_t emit, void *context)
{
    static const uint8_t none[32];
    keys_changed(state, none, emit, context);
    buttons_changed(&state->mouse_buttons, 0, JELLY_INPUT_MOUSE_BUTTON, mouse_button_codes, 3, emit, context);
    buttons_changed(&state->pad_buttons, 0, JELLY_INPUT_GAMEPAD_BUTTON, NULL, 32, emit, context);
}
