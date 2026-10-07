/*
 * USB core: enumeration of an addressed device and the bus "usb". See usb.h.
 */

#include "drivers/bus/usb/usb.h"

#include "drivers/core/module.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

static bus_t usb_bus = { .name = "usb" };

const char *usb_speed_name(uint8_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_FULL:  return "full";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    }
    return "unknown";
}

status_t usb_control(usb_device_t *device, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                     void *data, uint16_t length, uint32_t *transferred)
{
    usb_setup_t setup = { request_type, request, value, index, length };
    return device->host->control(device, &setup, data, transferred);
}

status_t usb_submit(usb_device_t *device, uint8_t endpoint_address, usb_transfer_t *transfer)
{
    return device->host->submit(device, endpoint_address, transfer);
}

const uint8_t *usb_find_descriptor(const uint8_t *data, size_t size, const uint8_t *from, uint8_t type)
{
    const uint8_t *end = data + size;
    const uint8_t *p = from ? from : data;

    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == type)
            return p;
        p += p[0];
    }
    return NULL;
}

static status_t get_descriptor(usb_device_t *device, uint8_t type, uint8_t index, uint16_t language, void *data,
                               uint16_t length, uint32_t *transferred)
{
    return usb_control(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQUEST_GET_DESCRIPTOR,
                       (uint16_t)(type << 8 | index), language, data, length, transferred);
}

/* The product name as ASCII; falls back to the IDs. */
static void read_product(usb_device_t *device)
{
    uint8_t raw[2 + 2 * (sizeof(device->product) - 1)];
    uint32_t got = 0;

    format(device->product, sizeof(device->product), "USB device %04x:%04x", device->descriptor.vendor,
              device->descriptor.product);
    if (!device->descriptor.product_string)
        return;
    status_t status = get_descriptor(device, USB_DESCRIPTOR_STRING, device->descriptor.product_string, 0x0409, raw,
                                     sizeof(raw), &got);
    if (STATUS_IS_ERROR(status) || got < 4 || raw[1] != USB_DESCRIPTOR_STRING)
        return;
    if (raw[0] < got)
        got = raw[0];
    size_t n = 0;
    for (uint32_t i = 2; i + 1 < got && n + 1 < sizeof(device->product); i += 2) {
        uint16_t unit = (uint16_t)(raw[i] | raw[i + 1] << 8);
        device->product[n++] = (unit >= 0x20 && unit < 0x7F) ? (char)unit : '?';
    }
    device->product[n] = '\0';
}

/* Split the configuration descriptor into the interfaces (alternate setting 0) and their endpoints. */
static void parse_configuration(usb_device_t *device)
{
    const uint8_t *data = device->configuration, *end = data + device->configuration_size;
    usb_interface_t *current = NULL;

    for (const uint8_t *p = data; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0]) {
        if (p[1] == USB_DESCRIPTOR_INTERFACE && p[0] >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t *d = (const usb_interface_descriptor_t *)p;
            if (current)
                current->extra_size = (size_t)(p - current->extra);
            current = NULL;
            if (d->alternate_setting != 0 || device->interface_count == USB_MAX_INTERFACES)
                continue;
            current = &device->interfaces[device->interface_count++];
            current->usb = device;
            current->descriptor = d;
            current->extra = p;
            current->extra_size = (size_t)(end - p);
        } else if (p[1] == USB_DESCRIPTOR_ENDPOINT && p[0] >= sizeof(usb_endpoint_descriptor_t) && current &&
                   current->endpoint_count < USB_MAX_ENDPOINTS) {
            current->endpoints[current->endpoint_count++] = (const usb_endpoint_descriptor_t *)p;
        }
    }
}

status_t usb_device_attach(usb_device_t *device)
{
    usb_configuration_descriptor_t header;
    const usb_endpoint_descriptor_t *endpoints[USB_MAX_INTERFACES * USB_MAX_ENDPOINTS];
    uint32_t got = 0, endpoint_count = 0;

    /* The first 8 bytes fit into any packet size and hold the real one. */
    status_t status = get_descriptor(device, USB_DESCRIPTOR_DEVICE, 0, 0, &device->descriptor, 8, &got);
    if (STATUS_IS_ERROR(status) || got < 8)
        return STATUS_IS_ERROR(status) ? status : STATUS_IO_ERROR;
    if (device->speed == USB_SPEED_FULL && device->descriptor.max_packet_size0 != 8) {
        status = device->host->set_max_packet0(device, device->descriptor.max_packet_size0);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    status = get_descriptor(device, USB_DESCRIPTOR_DEVICE, 0, 0, &device->descriptor, sizeof(device->descriptor), &got);
    if (STATUS_IS_ERROR(status) || got < sizeof(device->descriptor))
        return STATUS_IS_ERROR(status) ? status : STATUS_IO_ERROR;
    read_product(device);

    status = get_descriptor(device, USB_DESCRIPTOR_CONFIGURATION, 0, 0, &header, sizeof(header), &got);
    if (STATUS_IS_ERROR(status) || got < sizeof(header) || header.type != USB_DESCRIPTOR_CONFIGURATION)
        return STATUS_IS_ERROR(status) ? status : STATUS_IO_ERROR;
    size_t total = header.total_length;
    if (total < sizeof(header) || total > USB_CONFIG_MAX)
        return STATUS_NOT_SUPPORTED;
    device->configuration = kmalloc(total);
    if (!device->configuration)
        return STATUS_OUT_OF_MEMORY;
    status = get_descriptor(device, USB_DESCRIPTOR_CONFIGURATION, 0, 0, device->configuration, (uint16_t)total, &got);
    if (STATUS_IS_ERROR(status) || got < sizeof(header)) {
        kfree(device->configuration);
        device->configuration = NULL;
        return STATUS_IS_ERROR(status) ? status : STATUS_IO_ERROR;
    }
    device->configuration_size = got;
    parse_configuration(device);

    for (uint32_t i = 0; i < device->interface_count; i++) {
        for (uint32_t k = 0; k < device->interfaces[i].endpoint_count; k++)
            endpoints[endpoint_count++] = device->interfaces[i].endpoints[k];
    }
    /* The controller learns the endpoints before the device switches them on. */
    status = device->host->configure(device, endpoints, endpoint_count);
    if (!STATUS_IS_ERROR(status))
        status = usb_control(device, USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQUEST_SET_CONFIGURATION,
                             header.configuration_value, 0, NULL, 0, NULL);
    if (STATUS_IS_ERROR(status)) {
        kfree(device->configuration);
        device->configuration = NULL;
        device->interface_count = 0;
        return status;
    }

    klog_info("usb: port %u: %s (%04x:%04x, %s speed, %u interface%s)", device->port, device->product,
              device->descriptor.vendor, device->descriptor.product, usb_speed_name(device->speed),
              device->interface_count, device->interface_count == 1 ? "" : "s");

    for (uint32_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *interface = &device->interfaces[i];
        device_t *d = &interface->device;
        format(d->name, sizeof(d->name), "usb%u.%u", device->port, interface->descriptor->number);
        d->id = (device_id_t){
            .vendor = device->descriptor.vendor,
            .device = device->descriptor.product,
            .class_code = interface->descriptor->interface_class,
            .subclass = interface->descriptor->interface_subclass,
            .prog_if = interface->descriptor->interface_protocol,
        };
        interface->registered = true;
        device_register(d, &usb_bus, device->controller);
    }
    return STATUS_SUCCESS;
}

void usb_device_detach(usb_device_t *device)
{
    for (uint32_t i = 0; i < device->interface_count; i++) {
        if (device->interfaces[i].registered)
            device_unregister(&device->interfaces[i].device);
        device->interfaces[i].registered = false;
    }
    device->interface_count = 0;
    kfree(device->configuration);
    device->configuration = NULL;
}

static status_t usb_init(void)
{
    return bus_register(&usb_bus);
}

MODULE(.name = "usb", .description = "USB core", .version = 1, .min_kernel_version = KERNEL_VERSION(0, 11, 0),
       .init = usb_init);

EXPORT_SYMBOL(usb_device_attach);
EXPORT_SYMBOL(usb_device_detach);
EXPORT_SYMBOL(usb_control);
EXPORT_SYMBOL(usb_submit);
EXPORT_SYMBOL(usb_find_descriptor);
