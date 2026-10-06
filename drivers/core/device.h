/*
 * Device model and driver manager (README sections 22 and 23).
 *
 *   Bus -> Device -> Driver -> Capabilities
 *
 * Buses discover devices and describe their resources. The device manager
 * owns the device tree, matches devices with drivers and runs the driver
 * lifecycle:
 *
 *   Discover -> Match -> (Load) -> Probe -> Attach -> Running
 *            -> Suspend / Resume -> Detach
 *
 * Device structures are embedded in bus-specific structures (pci_device_t).
 */

#ifndef DRIVERS_CORE_DEVICE_H
#define DRIVERS_CORE_DEVICE_H

#include "core/list.h"

#include <jelly/status.h>
#include <stdbool.h>
#include <stdint.h>

/* --- Resources --------------------------------------------------------------- */

typedef enum {
    RESOURCE_MMIO = 1,
    RESOURCE_IO   = 2,
    RESOURCE_IRQ  = 3,
    RESOURCE_DMA  = 4,
} resource_type_t;

#define RESOURCE_PREFETCHABLE (1u << 0)
#define RESOURCE_64BIT        (1u << 1)

typedef struct {
    resource_type_t type;
    uint32_t        flags;
    uint64_t        start;
    uint64_t        size;
} resource_t;

/* --- Devices ----------------------------------------------------------------- */

typedef enum {
    DEVICE_DISCOVERED, /* known, no driver */
    DEVICE_PROBING,
    DEVICE_RUNNING,
    DEVICE_SUSPENDED,
    DEVICE_FAILED,     /* a driver's probe failed */
} device_state_t;

typedef enum {
    DEVICE_POWER_D0, /* fully on */
    DEVICE_POWER_D3, /* off (D3hot) */
} device_power_t;

typedef struct {
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
} device_id_t;

#define DEVICE_NAME_MAX      48
#define DEVICE_MAX_RESOURCES 8

struct bus;
struct driver;

typedef struct device {
    char            name[DEVICE_NAME_MAX];
    struct bus     *bus;
    struct device  *parent;
    list_t          children;
    list_node_t     sibling_node;
    list_node_t     bus_node;

    device_id_t     id;
    resource_t      resources[DEVICE_MAX_RESOURCES];
    uint32_t        resource_count;

    struct driver  *driver;
    void           *driver_data;
    device_state_t  state;
    device_power_t  power;
} device_t;

/* --- Drivers ----------------------------------------------------------------- */

#define MATCH_VENDOR   (1u << 0)
#define MATCH_DEVICE   (1u << 1)
#define MATCH_CLASS    (1u << 2)
#define MATCH_SUBCLASS (1u << 3)

typedef struct {
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  fields;     /* MATCH_*; 0 terminates the table */
} device_match_t;

#define DEVICE_MATCH_ID(v, d)    { (v), (d), 0, 0, MATCH_VENDOR | MATCH_DEVICE }
#define DEVICE_MATCH_CLASS(c, s) { 0, 0, (c), (s), MATCH_CLASS | MATCH_SUBCLASS }
#define DEVICE_MATCH_END         { 0, 0, 0, 0, 0 }

/* Driver capabilities (what the driver provides to the system) */
#define DRIVER_CAP_BUS     (1u << 0)
#define DRIVER_CAP_STORAGE (1u << 1)
#define DRIVER_CAP_NETWORK (1u << 2)
#define DRIVER_CAP_DISPLAY (1u << 3)
#define DRIVER_CAP_INPUT   (1u << 4)
#define DRIVER_CAP_AUDIO   (1u << 5)
#define DRIVER_CAP_TEST    (1u << 31)

struct module;

typedef struct driver {
    const char           *name;
    const char           *bus_name;
    uint32_t              version;
    uint32_t              capabilities;
    const device_match_t *ids;

    status_t (*probe)(device_t *device);   /* claim and start the device */
    void     (*remove)(device_t *device);  /* stop it; resources are released afterwards */
    status_t (*suspend)(device_t *device); /* optional */
    status_t (*resume)(device_t *device);  /* optional */

    /* Managed by the driver manager */
    struct bus    *bus;
    struct module *module;
    list_node_t    bus_node;
    uint32_t       bound;
} driver_t;

/* --- Buses ------------------------------------------------------------------- */

typedef struct bus {
    const char *name;
    /* Optional: default matching compares device_id_t with the driver's table. */
    bool     (*match)(const device_t *device, const driver_t *driver);
    /* Optional: change the device's power state. */
    status_t (*set_power)(device_t *device, device_power_t power);
    /* Optional: undo bus-level setup a driver left behind (interrupts, bus mastering). */
    void     (*detached)(device_t *device);

    list_t      devices;
    list_t      drivers;
    list_node_t node;
} bus_t;

/* --- Device manager ---------------------------------------------------------- */

void      device_manager_init(void);
device_t *device_root(void);

status_t  bus_register(bus_t *bus);
bus_t    *bus_find(const char *name);

/* Add a device to the tree (parent NULL: root) and its bus, then try to bind a driver. */
status_t  device_register(device_t *device, bus_t *bus, device_t *parent);

/* Add a driver and bind it to every matching unbound device. */
status_t  driver_register(driver_t *driver);

/* Detach all devices of the driver and remove it. */
void      driver_unregister(driver_t *driver);

status_t  device_suspend(device_t *device);
status_t  device_resume(device_t *device);

/* The index-th device on a bus with this vendor/device ID. */
device_t *device_find(const char *bus_name, uint16_t vendor, uint16_t device_id, unsigned index);

bool      device_matches(const device_t *device, const device_match_t *ids);
const char *device_state_name(device_state_t state);

/* Number of drivers registered by a module (unload safety check). */
uint32_t  driver_count_for_module(const struct module *module);

void      device_tree_dump(void);

/* --- Resource management ----------------------------------------------------- */

/* Claim a range for owner; BUSY if it overlaps another owner's claim. */
status_t  resource_claim(const resource_t *resource, const device_t *owner);
void      resource_release_all(const device_t *owner);

/* --- DMA --------------------------------------------------------------------- */

typedef struct {
    void    *virt;
    uint64_t phys;
    uint64_t size;
} dma_buffer_t;

/* Zeroed, physically contiguous, cache-coherent memory below max_address. */
status_t  dma_alloc(device_t *device, uint64_t size, uint64_t max_address, dma_buffer_t *buffer);
void      dma_free(dma_buffer_t *buffer);

#endif
