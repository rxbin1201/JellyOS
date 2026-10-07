/*
 * USB core (README section 24: buses).
 *
 *   host controller driver (xHCI)  ─▶  USB core  ─▶  bus "usb"  ─▶  class drivers (HID, hub, storage)
 *
 * A host controller driver detects a device on a root port, gives it an
 * address and hands it to usb_device_attach(). The core reads the
 * descriptors, selects the first configuration, opens its endpoints and
 * registers every interface as a device on the bus "usb"; class drivers
 * bind to interfaces through the ordinary driver model (device ID: vendor,
 * product, interface class and subclass, protocol as prog_if).
 *
 * Hubs are class drivers, too (hub.c): the hub driver watches the hub's
 * ports and asks the host controller driver for a child device on a port
 * (usb_host_ops_t.child_attach); devices form a tree below the root ports.
 *
 * Control and bulk transfers block and may only be used from thread
 * context. Interrupt transfers are asynchronous: their completion runs in
 * interrupt context and usually submits the transfer again.
 *
 * Not yet: isochronous transfers, alternate settings, suspend.
 */

#ifndef DRIVERS_BUS_USB_USB_H
#define DRIVERS_BUS_USB_USB_H

#include "drivers/core/device.h"

#include <stddef.h>

/* Speeds (numbered like the xHCI port speed IDs) */
#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

/* bmRequestType */
#define USB_DIR_IN          0x80
#define USB_TYPE_STANDARD   0x00
#define USB_TYPE_CLASS      0x20
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02
#define USB_RECIP_OTHER     0x03 /* hub ports */

/* Standard requests */
#define USB_REQUEST_GET_STATUS        0
#define USB_REQUEST_CLEAR_FEATURE     1
#define USB_REQUEST_SET_FEATURE       3
#define USB_REQUEST_GET_DESCRIPTOR    6
#define USB_REQUEST_SET_CONFIGURATION 9

#define USB_FEATURE_ENDPOINT_HALT 0

/* Descriptor types */
#define USB_DESCRIPTOR_DEVICE        1
#define USB_DESCRIPTOR_CONFIGURATION 2
#define USB_DESCRIPTOR_STRING        3
#define USB_DESCRIPTOR_INTERFACE     4
#define USB_DESCRIPTOR_ENDPOINT      5

#define USB_CLASS_HID     3
#define USB_CLASS_STORAGE 8
#define USB_CLASS_HUB     9

#define USB_ENDPOINT_IN         0x80
#define USB_ENDPOINT_TYPE_MASK  0x03
#define USB_ENDPOINT_CONTROL    0
#define USB_ENDPOINT_ISOCHRONOUS 1
#define USB_ENDPOINT_BULK       2
#define USB_ENDPOINT_INTERRUPT  3

typedef struct __attribute__((packed)) {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} usb_setup_t;

typedef struct __attribute__((packed)) {
    uint8_t  length;
    uint8_t  type;
    uint16_t usb_version;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet_size0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device_version;
    uint8_t  manufacturer_string;
    uint8_t  product_string;
    uint8_t  serial_string;
    uint8_t  configurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  length;
    uint8_t  type;
    uint16_t total_length;
    uint8_t  interfaces;
    uint8_t  configuration_value;
    uint8_t  configuration_string;
    uint8_t  attributes;
    uint8_t  max_power;
} usb_configuration_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t length;
    uint8_t type;
    uint8_t number;
    uint8_t alternate_setting;
    uint8_t endpoints;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_string;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  length;
    uint8_t  type;
    uint8_t  address;         /* number | USB_ENDPOINT_IN */
    uint8_t  attributes;      /* USB_ENDPOINT_* type in the low bits */
    uint16_t max_packet_size;
    uint8_t  interval;
} usb_endpoint_descriptor_t;

#define USB_MAX_INTERFACES 8
#define USB_MAX_ENDPOINTS  4  /* per interface */
#define USB_CONFIG_MAX     1024
#define USB_MAX_DEPTH      5  /* hubs between a root port and a device (the route string has 5 digits) */

struct usb_device;
struct usb_transfer;

typedef struct usb_interface {
    device_t                          device;      /* on the bus "usb" */
    struct usb_device                *usb;
    const usb_interface_descriptor_t *descriptor;  /* inside the configuration descriptor */
    const uint8_t                    *extra;       /* everything up to the next interface (class descriptors, endpoints) */
    size_t                            extra_size;
    const usb_endpoint_descriptor_t  *endpoints[USB_MAX_ENDPOINTS];
    uint32_t                          endpoint_count;
    bool                              registered;
} usb_interface_t;

#define usb_interface_from_device(d) container_of((d), usb_interface_t, device)

/* An asynchronous transfer on an interrupt or bulk endpoint. The buffer must come from dma_alloc(). */
typedef struct usb_transfer {
    void    *buffer;
    uint64_t buffer_phys;
    uint32_t length;          /* at most 64 KiB, not crossing a 64 KiB boundary of physical memory */
    /* Interrupt context. Not called when the device disappears with the transfer pending. */
    void   (*complete)(struct usb_transfer *transfer, status_t status, uint32_t transferred);
    void    *context;
} usb_transfer_t;

/* What a host controller driver provides */
typedef struct {
    /* One control transfer on endpoint 0; data direction from setup->request_type. Blocks. */
    status_t (*control)(struct usb_device *device, const usb_setup_t *setup, void *data, uint32_t *transferred);
    /* The maximum packet size of endpoint 0 is now known (full speed devices). */
    status_t (*set_max_packet0)(struct usb_device *device, uint32_t max_packet);
    /* Open the endpoints of the selected configuration. */
    status_t (*configure)(struct usb_device *device, const usb_endpoint_descriptor_t *const *endpoints, uint32_t count);
    /* Queue a transfer on an interrupt or bulk endpoint; one per endpoint at a time. Safe in interrupt context. */
    status_t (*submit)(struct usb_device *device, uint8_t endpoint_address, usb_transfer_t *transfer);
    /* Give up the pending transfer of an endpoint (after a timeout) / restart a halted endpoint (after a stall). */
    status_t (*reset_endpoint)(struct usb_device *device, uint8_t endpoint_address);

    /* Hubs: the device is a hub with this many ports (and, for high speed hubs, this TT think time code). */
    status_t (*hub_configure)(struct usb_device *hub, uint32_t ports, uint32_t think_time);
    /* A device of `speed` is on port `port` (1-based) of the hub and was reset: address and enumerate it. */
    status_t (*child_attach)(struct usb_device *hub, uint32_t port, uint8_t speed, struct usb_device **child);
    /* The child is gone: unbind its drivers and free it. */
    void     (*child_detach)(struct usb_device *child);
} usb_host_ops_t;

typedef struct usb_device {
    const usb_host_ops_t   *host;
    void                   *host_data;
    device_t               *controller;  /* parent in the device tree */
    struct usb_device      *hub;         /* NULL on a root port */
    uint8_t                 port;        /* port of the hub, or root hub port; 1-based */
    uint8_t                 depth;       /* 0 on a root port */
    uint8_t                 speed;       /* USB_SPEED_* */
    char                    path[24];    /* "5", "5.2", "5.2.1": root port, then hub ports */
    usb_device_descriptor_t descriptor;
    char                    product[48];
    uint8_t                *configuration;
    size_t                  configuration_size;
    usb_interface_t         interfaces[USB_MAX_INTERFACES];
    uint32_t                interface_count;
} usb_device_t;

/* An addressed device appeared: read it, configure it, bind class drivers. Thread context. */
status_t usb_device_attach(usb_device_t *device);
/* The device is gone and its transfers are dead: unbind the class drivers. */
void     usb_device_detach(usb_device_t *device);

status_t usb_control(usb_device_t *device, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                     void *data, uint16_t length, uint32_t *transferred);
status_t usb_submit(usb_device_t *device, uint8_t endpoint_address, usb_transfer_t *transfer);

/*
 * One bulk transfer, waiting for its end. The buffer rules of usb_transfer_t
 * apply. TIMEOUT cancels the transfer; IO_ERROR usually means the endpoint
 * stalled (usb_clear_halt() makes it usable again).
 */
status_t usb_bulk(usb_device_t *device, uint8_t endpoint_address, void *buffer, uint64_t buffer_phys, uint32_t length,
                  uint32_t *transferred, uint64_t timeout_ns);
/* Restart a stalled endpoint on both sides. */
status_t usb_clear_halt(usb_device_t *device, uint8_t endpoint_address);

/* The next descriptor of `type` in [data, data + size) at or after `from` (NULL: the start); NULL if none. */
const uint8_t *usb_find_descriptor(const uint8_t *data, size_t size, const uint8_t *from, uint8_t type);

const char *usb_speed_name(uint8_t speed);

#endif
