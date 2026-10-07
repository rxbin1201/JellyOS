/*
 * USB hub class driver: devices behind hubs.
 *
 * A hub is a USB device whose ports the driver has to manage itself: power
 * them, notice when something is plugged in or pulled out (the hub reports
 * changes on an interrupt endpoint; the ports are also looked at every two
 * seconds, because not every hub is reliable about it), reset the port to
 * find out the device's speed, and then ask the host controller driver for
 * a child device on that port. Each hub has a thread for this, since the
 * requests to the hub block.
 *
 * Both kinds of hubs are handled: USB 2 hubs (low, full and high speed
 * devices) and the SuperSpeed half of USB 3 hubs, which differ in the hub
 * descriptor, the port status bits and the "hub depth" they must be told.
 * Hubs may be stacked up to USB_MAX_DEPTH.
 *
 * Not yet: per-port power switching policy, over-current handling, port
 * indicators, suspend.
 */

#include "drivers/bus/usb/usb.h"
#include "drivers/core/module.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "scheduler/thread.h"

#define HUB_MAX_PORTS 15 /* the route to a device has one hexadecimal digit per hub */

#define HUB_DESCRIPTOR       0x29
#define HUB_DESCRIPTOR_SUPER 0x2A
#define HUB_REQUEST_SET_DEPTH 12

/* Port features */
#define PORT_RESET       4
#define PORT_POWER       8
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE     17
#define C_PORT_SUSPEND    18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET      20
#define C_PORT_LINK_STATE 25
#define C_PORT_CONFIG_ERROR 26
#define C_BH_PORT_RESET   29

/* wPortStatus */
#define STATUS_CONNECTED (1u << 0)
#define STATUS_ENABLED   (1u << 1)
#define STATUS_IN_RESET  (1u << 4)
#define STATUS_LOW_SPEED  (1u << 9)  /* USB 2 hubs */
#define STATUS_HIGH_SPEED (1u << 10)

#define RESCAN_NS 2000000000ull

typedef struct {
    usb_device_t  *usb;
    uint32_t       ports;
    bool           super;
    usb_device_t  *children[HUB_MAX_PORTS + 1];
    bool           failed[HUB_MAX_PORTS + 1]; /* a device we could not set up: left alone until it is replaced */

    uint8_t        endpoint;
    dma_buffer_t   buffer;
    usb_transfer_t transfer;

    wait_queue_t   wakeup;
    bool           changed, stopping, stopped;
} hub_t;

static status_t port_feature(hub_t *hub, uint32_t port, uint16_t feature, bool set)
{
    return usb_control(hub->usb, USB_TYPE_CLASS | USB_RECIP_OTHER, set ? USB_REQUEST_SET_FEATURE : USB_REQUEST_CLEAR_FEATURE,
                       feature, (uint16_t)port, NULL, 0, NULL);
}

static status_t port_status(hub_t *hub, uint32_t port, uint16_t *status, uint16_t *change)
{
    uint16_t data[2] = { 0, 0 };
    uint32_t got = 0;
    status_t result = usb_control(hub->usb, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQUEST_GET_STATUS, 0,
                                  (uint16_t)port, data, sizeof(data), &got);
    if (!STATUS_IS_ERROR(result) && got < sizeof(data))
        result = STATUS_IO_ERROR;
    *status = data[0];
    *change = data[1];
    return result;
}

/* Acknowledge every change the hub reports for the port. */
static void clear_changes(hub_t *hub, uint32_t port, uint16_t change)
{
    static const struct {
        uint16_t bit, feature;
        bool     super;
    } changes[] = {
        { 1u << 0, C_PORT_CONNECTION, false },   { 1u << 1, C_PORT_ENABLE, false },
        { 1u << 2, C_PORT_SUSPEND, false },      { 1u << 3, C_PORT_OVER_CURRENT, false },
        { 1u << 4, C_PORT_RESET, false },        { 1u << 0, C_PORT_CONNECTION, true },
        { 1u << 3, C_PORT_OVER_CURRENT, true },  { 1u << 4, C_PORT_RESET, true },
        { 1u << 5, C_BH_PORT_RESET, true },      { 1u << 6, C_PORT_LINK_STATE, true },
        { 1u << 7, C_PORT_CONFIG_ERROR, true },
    };
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        if (changes[i].super == hub->super && (change & changes[i].bit))
            port_feature(hub, port, changes[i].feature, false);
    }
}

static void port_connect(hub_t *hub, uint32_t port)
{
    uint16_t status = 0, change = 0;

    thread_sleep(100000000); /* let the plug settle */
    if (STATUS_IS_ERROR(port_status(hub, port, &status, &change)) || !(status & STATUS_CONNECTED))
        return;
    /* SuperSpeed ports are enabled once their link is up; everything else needs a reset. */
    if (!hub->super || !(status & STATUS_ENABLED)) {
        if (STATUS_IS_ERROR(port_feature(hub, port, PORT_RESET, true)))
            return;
        for (int i = 0; i < 50; i++) {
            thread_sleep(20000000);
            if (STATUS_IS_ERROR(port_status(hub, port, &status, &change)))
                return;
            if (!(status & STATUS_IN_RESET) && (status & STATUS_ENABLED))
                break;
        }
        clear_changes(hub, port, change);
        if (!(status & STATUS_ENABLED)) {
            klog_warn("usb: hub %s port %u: reset failed (status 0x%x)", hub->usb->path, port, status);
            hub->failed[port] = true;
            return;
        }
        thread_sleep(20000000); /* reset recovery */
    }
    uint8_t speed = hub->super ? USB_SPEED_SUPER
                    : (status & STATUS_LOW_SPEED) ? USB_SPEED_LOW
                    : (status & STATUS_HIGH_SPEED) ? USB_SPEED_HIGH
                                                   : USB_SPEED_FULL;
    status_t result = hub->usb->host->child_attach(hub->usb, port, speed, &hub->children[port]);
    if (STATUS_IS_ERROR(result)) {
        hub->children[port] = NULL;
        hub->failed[port] = true;
        klog_warn("usb: hub %s port %u: cannot set up the device: %s", hub->usb->path, port, status_name(result));
    }
}

static void port_check(hub_t *hub, uint32_t port)
{
    uint16_t status, change;

    if (STATUS_IS_ERROR(port_status(hub, port, &status, &change)))
        return;
    clear_changes(hub, port, change);
    bool connected = status & STATUS_CONNECTED, replaced = change & 1;

    if (!connected || replaced)
        hub->failed[port] = false;
    if (hub->children[port] && (!connected || replaced)) {
        hub->usb->host->child_detach(hub->children[port]);
        hub->children[port] = NULL;
    }
    if (connected && !hub->children[port] && !hub->failed[port])
        port_connect(hub, port);
}

static void hub_thread(void *context)
{
    hub_t *hub = context;

    for (;;) {
        uint64_t flags = arch_interrupts_save();
        if (!hub->changed && !hub->stopping)
            wait_queue_block_uninterruptible(&hub->wakeup, wait_deadline(RESCAN_NS));
        hub->changed = false;
        bool stop = hub->stopping;
        arch_interrupts_restore(flags);
        if (stop)
            break;
        for (uint32_t port = 1; port <= hub->ports && !hub->stopping; port++)
            port_check(hub, port);
    }
    uint64_t flags = arch_interrupts_save();
    hub->stopped = true;
    wait_queue_wake_all(&hub->wakeup, STATUS_SUCCESS);
    arch_interrupts_restore(flags);
    thread_exit();
}

/* The hub's status change endpoint has news (interrupt context). */
static void status_complete(usb_transfer_t *transfer, status_t status, uint32_t transferred)
{
    hub_t *hub = transfer->context;

    (void)transferred;
    if (STATUS_IS_ERROR(status) || hub->stopping)
        return; /* the hub is going away; the periodic look at the ports remains otherwise */
    hub->changed = true;
    wait_queue_wake_all(&hub->wakeup, STATUS_SUCCESS);
    usb_submit(hub->usb, hub->endpoint, &hub->transfer);
}

static status_t hub_probe(device_t *device)
{
    usb_interface_t *interface = usb_interface_from_device(device);
    usb_device_t *usb = interface->usb;
    const usb_endpoint_descriptor_t *endpoint = NULL;
    uint8_t descriptor[16] = { 0 };
    uint32_t got = 0;
    thread_t *thread;

    if (usb->depth >= USB_MAX_DEPTH) {
        klog_warn("usb: hub %s: too many hubs in a row", usb->path);
        return STATUS_NOT_SUPPORTED;
    }
    for (uint32_t i = 0; i < interface->endpoint_count; i++) {
        const usb_endpoint_descriptor_t *e = interface->endpoints[i];
        if ((e->address & USB_ENDPOINT_IN) && (e->attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_INTERRUPT)
            endpoint = e;
    }
    hub_t *hub = kcalloc(1, sizeof(*hub));
    if (!hub)
        return STATUS_OUT_OF_MEMORY;
    hub->usb = usb;
    hub->super = usb->speed == USB_SPEED_SUPER;
    wait_queue_init(&hub->wakeup);

    status_t status = STATUS_SUCCESS;
    if (hub->super) /* SuperSpeed hubs route packets by their position in the tree */
        status = usb_control(usb, USB_TYPE_CLASS | USB_RECIP_DEVICE, HUB_REQUEST_SET_DEPTH, usb->depth, 0, NULL, 0, NULL);
    if (!STATUS_IS_ERROR(status))
        status = usb_control(usb, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQUEST_GET_DESCRIPTOR,
                             (uint16_t)((hub->super ? HUB_DESCRIPTOR_SUPER : HUB_DESCRIPTOR) << 8), 0, descriptor,
                             hub->super ? 12 : 9, &got);
    if (!STATUS_IS_ERROR(status) && (got < 7 || descriptor[2] == 0))
        status = STATUS_IO_ERROR;
    if (!STATUS_IS_ERROR(status)) {
        hub->ports = descriptor[2] > HUB_MAX_PORTS ? HUB_MAX_PORTS : descriptor[2];
        uint32_t characteristics = (uint32_t)(descriptor[3] | descriptor[4] << 8);
        status = usb->host->hub_configure(usb, hub->ports, (characteristics >> 5) & 3);
    }
    if (!STATUS_IS_ERROR(status) && endpoint)
        status = dma_alloc(device, 64, ~0ull, &hub->buffer);
    if (!STATUS_IS_ERROR(status))
        status = thread_create_kernel("usb-hub", hub_thread, hub, THREAD_PRIORITY_KERNEL, &thread);
    if (STATUS_IS_ERROR(status)) {
        dma_free(&hub->buffer);
        kfree(hub);
        return status;
    }

    for (uint32_t port = 1; port <= hub->ports; port++)
        port_feature(hub, port, PORT_POWER, true);
    thread_sleep((descriptor[5] * 2 > 100 ? descriptor[5] * 2u : 100u) * 1000000ull); /* power good */

    klog_info("usb: hub %s: %s, %u ports", usb->path, usb->product, hub->ports);
    device->driver_data = hub;
    hub->changed = true; /* what is plugged in already */
    thread_start(thread);
    if (endpoint) {
        uint32_t size = (hub->ports + 8) / 8;
        hub->endpoint = endpoint->address;
        hub->transfer = (usb_transfer_t){
            .buffer = hub->buffer.virt,
            .buffer_phys = hub->buffer.phys,
            .length = size > (endpoint->max_packet_size & 0x7FFu) ? size : (endpoint->max_packet_size & 0x7FFu),
            .complete = status_complete,
            .context = hub,
        };
        if (hub->transfer.length > 64)
            hub->transfer.length = 64;
        usb_submit(usb, hub->endpoint, &hub->transfer);
    }
    return STATUS_SUCCESS;
}

static void hub_remove(device_t *device)
{
    hub_t *hub = device->driver_data;

    /* Stop the thread first: it may be in the middle of setting up a child. */
    uint64_t flags = arch_interrupts_save();
    hub->stopping = true;
    wait_queue_wake_all(&hub->wakeup, STATUS_SUCCESS);
    while (!hub->stopped)
        wait_queue_block_uninterruptible(&hub->wakeup, WAIT_FOREVER);
    arch_interrupts_restore(flags);

    for (uint32_t port = 1; port <= hub->ports; port++) {
        if (hub->children[port])
            hub->usb->host->child_detach(hub->children[port]);
    }
    dma_free(&hub->buffer);
    kfree(hub);
}

static const device_match_t hub_ids[] = {
    { 0, 0, USB_CLASS_HUB, 0, MATCH_CLASS },
    DEVICE_MATCH_END,
};

static driver_t hub_driver = {
    .name = "usb-hub",
    .bus_name = "usb",
    .version = 1,
    .capabilities = DRIVER_CAP_BUS,
    .ids = hub_ids,
    .probe = hub_probe,
    .remove = hub_remove,
};

static status_t hub_module_init(void)
{
    return driver_register(&hub_driver);
}

static const char *const hub_dependencies[] = { "usb", NULL };

MODULE(.name = "usb_hub", .description = "USB hubs", .version = 1, .min_kernel_version = KERNEL_VERSION(0, 12, 0),
       .dependencies = hub_dependencies, .init = hub_module_init);
