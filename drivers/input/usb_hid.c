/*
 * USB HID class driver: keyboards, mice, tablets and gamepads (README section 37).
 *
 * The driver reads the interface's report descriptor, lets hid.c parse it
 * and then keeps one interrupt transfer pending on the IN endpoint. Every
 * report that arrives is decoded into input manager events in the
 * completion handler, which queues the transfer again.
 *
 * Devices stay in report protocol (no boot protocol), so anything the
 * descriptor describes works: extra mouse buttons aside, also absolute
 * pointers and gamepads. Keyboard LEDs are not driven yet.
 */

#include "drivers/bus/usb/usb.h"
#include "drivers/core/module.h"
#include "drivers/input/hid.h"
#include "input/input.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define HID_DESCRIPTOR_HID    0x21
#define HID_DESCRIPTOR_REPORT 0x22
#define HID_REQUEST_SET_IDLE     0x0A
#define HID_REQUEST_SET_PROTOCOL 0x0B
#define HID_REPORT_DESCRIPTOR_MAX 2048

typedef struct {
    usb_interface_t *interface;
    hid_state_t      state;
    uint32_t         input_device;
    uint8_t          endpoint;
    dma_buffer_t     buffer;
    usb_transfer_t   transfer;
    bool             stopped;
} usb_hid_t;

static void emit(void *context, uint32_t type, uint32_t code, int32_t value, int32_t dx, int32_t dy, int32_t x,
                 int32_t y, uint32_t flags)
{
    usb_hid_t *hid = context;
    input_report(hid->input_device, type, code, value, dx, dy, x, y, flags);
}

static void report_complete(usb_transfer_t *transfer, status_t status, uint32_t transferred)
{
    usb_hid_t *hid = transfer->context;

    if (hid->stopped)
        return;
    if (STATUS_IS_ERROR(status)) {
        /* The endpoint is halted or the device is on its way out. */
        klog_debug("usb-hid: %s: report failed, input stops", hid->interface->device.name);
        hid->stopped = true;
        return;
    }
    hid_process(&hid->state, hid->buffer.virt, transferred, emit, hid);
    if (STATUS_IS_ERROR(usb_submit(hid->interface->usb, hid->endpoint, &hid->transfer)))
        hid->stopped = true;
}

static const char *kind_name(uint32_t kinds)
{
    if (kinds & HID_KIND_GAMEPAD)
        return "gamepad";
    if ((kinds & HID_KIND_KEYBOARD) && (kinds & HID_KIND_MOUSE))
        return "keyboard and pointer";
    return (kinds & HID_KIND_KEYBOARD) ? "keyboard" : "pointer";
}

static status_t usb_hid_probe(device_t *device)
{
    usb_interface_t *interface = usb_interface_from_device(device);
    usb_device_t *usb = interface->usb;
    uint8_t number = interface->descriptor->number;
    const usb_endpoint_descriptor_t *endpoint = NULL;
    uint32_t got = 0;

    for (uint32_t i = 0; i < interface->endpoint_count; i++) {
        const usb_endpoint_descriptor_t *e = interface->endpoints[i];
        if ((e->address & USB_ENDPOINT_IN) && (e->attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_INTERRUPT) {
            endpoint = e;
            break;
        }
    }
    /* HID descriptor: ..., bNumDescriptors, then type and length of each class descriptor */
    const uint8_t *hid_descriptor = usb_find_descriptor(interface->extra, interface->extra_size, NULL, HID_DESCRIPTOR_HID);
    if (!endpoint || !hid_descriptor || hid_descriptor[0] < 9 || hid_descriptor[6] != HID_DESCRIPTOR_REPORT)
        return STATUS_NOT_SUPPORTED;
    uint32_t length = (uint32_t)(hid_descriptor[7] | hid_descriptor[8] << 8);
    if (length == 0 || length > HID_REPORT_DESCRIPTOR_MAX)
        return STATUS_NOT_SUPPORTED;

    usb_hid_t *hid = kcalloc(1, sizeof(*hid));
    uint8_t *report_descriptor = kmalloc(length);
    if (!hid || !report_descriptor) {
        kfree(hid);
        kfree(report_descriptor);
        return STATUS_OUT_OF_MEMORY;
    }
    hid->interface = interface;
    hid->endpoint = endpoint->address;

    /* Reports only when something changes (devices may refuse: then they repeat them, which is harmless). */
    usb_control(usb, USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQUEST_SET_IDLE, 0, number, NULL, 0, NULL);
    if (interface->descriptor->interface_subclass == 1) /* boot interface: make sure it is in report protocol */
        usb_control(usb, USB_TYPE_CLASS | USB_RECIP_INTERFACE, HID_REQUEST_SET_PROTOCOL, 1, number, NULL, 0, NULL);

    status_t status = usb_control(usb, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQUEST_GET_DESCRIPTOR,
                                  HID_DESCRIPTOR_REPORT << 8, number, report_descriptor, (uint16_t)length, &got);
    if (!STATUS_IS_ERROR(status) && !hid_state_init(&hid->state, report_descriptor, got))
        status = STATUS_NOT_SUPPORTED; /* nothing we have events for (or a broken descriptor) */
    kfree(report_descriptor);

    size_t report_size = STATUS_IS_ERROR(status) ? 0 : hid_report_size(&hid->state.descriptor);
    size_t packet = endpoint->max_packet_size & 0x7FF;
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, report_size > packet ? report_size : packet, ~0ull, &hid->buffer);
    if (STATUS_IS_ERROR(status)) {
        kfree(hid);
        return status;
    }

    hid->input_device = input_register_device(usb->product);
    klog_info("usb-hid: %s: %s (%s)", device->name, usb->product, kind_name(hid->state.descriptor.kinds));
    hid->transfer = (usb_transfer_t){
        .buffer = hid->buffer.virt,
        .buffer_phys = hid->buffer.phys,
        .length = (uint32_t)(report_size > packet ? report_size : packet),
        .complete = report_complete,
        .context = hid,
    };
    device->driver_data = hid;
    status = usb_submit(usb, hid->endpoint, &hid->transfer);
    if (STATUS_IS_ERROR(status)) {
        dma_free(&hid->buffer);
        kfree(hid);
        device->driver_data = NULL;
    }
    return status;
}

static void usb_hid_remove(device_t *device)
{
    usb_hid_t *hid = device->driver_data;

    /* The controller has dropped the pending transfer; nothing calls report_complete any more. */
    hid->stopped = true;
    hid_release_all(&hid->state, emit, hid); /* no key stays stuck */
    dma_free(&hid->buffer);
    kfree(hid);
}

static const device_match_t usb_hid_ids[] = {
    { 0, 0, USB_CLASS_HID, 0, MATCH_CLASS },
    DEVICE_MATCH_END,
};

static driver_t usb_hid_driver = {
    .name = "usb-hid",
    .bus_name = "usb",
    .version = 1,
    .capabilities = DRIVER_CAP_INPUT,
    .ids = usb_hid_ids,
    .probe = usb_hid_probe,
    .remove = usb_hid_remove,
};

static status_t usb_hid_module_init(void)
{
    return driver_register(&usb_hid_driver);
}

static const char *const usb_hid_dependencies[] = { "usb", NULL };

MODULE(.name = "usb_hid", .description = "USB keyboards, mice and gamepads (HID)", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 11, 0), .dependencies = usb_hid_dependencies,
       .init = usb_hid_module_init);
