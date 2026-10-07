/*
 * VirtIO input devices (keyboard, mouse, tablet): QEMU's virtual input
 * hardware and the first input source of JellyOS. PS/2, USB HID and
 * gamepads follow in Phase 11 behind the same input manager.
 *
 * The device sends Linux evdev events (type, code, value) on queue 0. The
 * driver translates them in its interrupt handler into the standardized
 * events of the input manager:
 *   EV_KEY  keys -> KEY_DOWN / KEY_UP, mouse buttons -> MOUSE_BUTTON
 *   EV_REL  X/Y collected until EV_SYN -> MOUSE_MOVE, wheel -> MOUSE_WHEEL
 *   EV_ABS  X/Y scaled to 0..65535 and sent at EV_SYN -> MOUSE_MOVE (absolute)
 */

#include "drivers/bus/virtio/virtio.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"
#include "input/input.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#include <jelly/input.h>

#define EVENT_SLOTS 64

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08
#define ABS_X     0x00
#define ABS_Y     0x01

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

/* Device configuration space */
#define CONFIG_SELECT   0
#define CONFIG_SUBSEL   1
#define CONFIG_SIZE     2
#define CONFIG_DATA     8
#define CFG_ID_NAME     0x01
#define CFG_ABS_INFO    0x12

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} virtio_input_event_t;

typedef struct {
    virtio_device_t virtio;
    virtqueue_t     events;
    dma_buffer_t    buffers;
    uint32_t        irq;
    uint32_t        device;
    int32_t         abs_max_x, abs_max_y;
    int32_t         rel_x, rel_y;
    int32_t         abs_x, abs_y;
    bool            rel_pending, abs_pending;
} vinput_t;

static void post(vinput_t *v, uint16_t slot)
{
    v->events.desc[slot] = (virtq_desc_t){ v->buffers.phys + slot * sizeof(virtio_input_event_t),
                                           sizeof(virtio_input_event_t), VIRTQ_DESC_F_WRITE, 0 };
    virtio_queue_submit(&v->events, slot);
}

static int32_t scale(int32_t value, int32_t max)
{
    if (max <= 0)
        max = 32767;
    if (value < 0)
        value = 0;
    if (value > max)
        value = max;
    return (int32_t)(((int64_t)value * 65535 + max / 2) / max); /* rounded: no drift through two scalings */
}

static void translate(vinput_t *v, const virtio_input_event_t *e)
{
    int32_t value = (int32_t)e->value;

    switch (e->type) {
    case EV_KEY:
        if (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE) {
            uint32_t button = e->code == BTN_LEFT ? JELLY_BUTTON_LEFT
                              : e->code == BTN_RIGHT ? JELLY_BUTTON_RIGHT
                                                     : JELLY_BUTTON_MIDDLE;
            input_report(v->device, JELLY_INPUT_MOUSE_BUTTON, button, value ? 1 : 0, 0, 0, 0, 0, 0);
        } else if (e->code <= JELLY_KEY_MAX) {
            input_report(v->device, value ? JELLY_INPUT_KEY_DOWN : JELLY_INPUT_KEY_UP, e->code, value, 0, 0, 0, 0, 0);
        }
        break;
    case EV_REL:
        if (e->code == REL_X) {
            v->rel_x += value;
            v->rel_pending = true;
        } else if (e->code == REL_Y) {
            v->rel_y += value;
            v->rel_pending = true;
        } else if (e->code == REL_WHEEL) {
            input_report(v->device, JELLY_INPUT_MOUSE_WHEEL, 0, value, 0, 0, 0, 0, 0);
        }
        break;
    case EV_ABS:
        if (e->code == ABS_X) {
            v->abs_x = scale(value, v->abs_max_x);
            v->abs_pending = true;
        } else if (e->code == ABS_Y) {
            v->abs_y = scale(value, v->abs_max_y);
            v->abs_pending = true;
        }
        break;
    case EV_SYN:
        if (v->rel_pending)
            input_report(v->device, JELLY_INPUT_MOUSE_MOVE, 0, 0, v->rel_x, v->rel_y, 0, 0, 0);
        if (v->abs_pending)
            input_report(v->device, JELLY_INPUT_MOUSE_MOVE, 0, 0, 0, 0, v->abs_x, v->abs_y, JELLY_INPUT_ABSOLUTE);
        v->rel_x = v->rel_y = 0;
        v->rel_pending = v->abs_pending = false;
        break;
    }
}

static void vinput_interrupt(void *context)
{
    vinput_t *v = context;
    uint32_t id, length;
    while (virtio_queue_next_used(&v->events, &id, &length)) {
        if (id >= v->events.size)
            continue;
        if (length >= sizeof(virtio_input_event_t))
            translate(v, (virtio_input_event_t *)v->buffers.virt + id);
        post(v, (uint16_t)id);
    }
}

/* Read a configuration item into buffer; returns its size. */
static uint8_t read_config(vinput_t *v, uint8_t select, uint8_t subsel, void *buffer, uint8_t size)
{
    volatile uint8_t *config = v->virtio.device_config;
    config[CONFIG_SELECT] = select;
    config[CONFIG_SUBSEL] = subsel;
    uint8_t available = config[CONFIG_SIZE];
    uint8_t n = available < size ? available : size;
    for (uint8_t i = 0; i < n; i++)
        ((uint8_t *)buffer)[i] = config[CONFIG_DATA + i];
    return available;
}

static status_t vinput_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    vinput_t *v = kcalloc(1, sizeof(*v));
    char name[64];
    uint32_t abs_info[5];

    if (!v)
        return STATUS_OUT_OF_MEMORY;
    device->driver_data = v;
    status_t status = virtio_init(&v->virtio, pci, 0, device);
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msix(pci, 0, vinput_interrupt, v, &v->irq);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, 0, EVENT_SLOTS, 0, &v->events);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, EVENT_SLOTS * sizeof(virtio_input_event_t), ~0ULL, &v->buffers);
    if (STATUS_IS_ERROR(status)) {
        if (v->virtio.common)
            virtio_reset(&v->virtio);
        pci_disable_msix(pci);
        virtio_queue_free(&v->events);
        dma_free(&v->buffers);
        kfree(v);
        device->driver_data = NULL;
        return status;
    }

    memset(name, 0, sizeof(name));
    read_config(v, CFG_ID_NAME, 0, name, sizeof(name) - 1);
    v->abs_max_x = read_config(v, CFG_ABS_INFO, ABS_X, abs_info, sizeof(abs_info)) ? (int32_t)abs_info[1] : 0;
    v->abs_max_y = read_config(v, CFG_ABS_INFO, ABS_Y, abs_info, sizeof(abs_info)) ? (int32_t)abs_info[1] : 0;
    v->device = input_register_device(name[0] ? name : "virtio input");

    virtio_driver_ok(&v->virtio);
    for (uint16_t i = 0; i < v->events.size; i++)
        post(v, i);
    return STATUS_SUCCESS;
}

static const device_match_t vinput_ids[] = {
    DEVICE_MATCH_ID(VIRTIO_VENDOR, 0x1052),
    DEVICE_MATCH_END,
};

static driver_t vinput_driver = {
    .name = "virtio-input",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_INPUT,
    .ids = vinput_ids,
    .probe = vinput_probe,
};

static status_t vinput_module_init(void)
{
    return driver_register(&vinput_driver);
}

static const char *const vinput_dependencies[] = { "pci", NULL };

MODULE(.name = "virtio_input", .description = "VirtIO keyboards, mice and tablets", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 9, 0), .dependencies = vinput_dependencies,
       .init = vinput_module_init);
