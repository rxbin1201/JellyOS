#include "drivers/core/device.h"

#include "drivers/core/module.h"

#include "core/export.h"
#include "core/log.h"
#include "core/string.h"

static list_t buses;
static device_t root;

const char *device_state_name(device_state_t state)
{
    switch (state) {
    case DEVICE_DISCOVERED: return "discovered";
    case DEVICE_PROBING:    return "probing";
    case DEVICE_RUNNING:    return "running";
    case DEVICE_SUSPENDED:  return "suspended";
    case DEVICE_FAILED:     return "failed";
    }
    return "unknown";
}

void device_manager_init(void)
{
    list_init(&buses);
    memset(&root, 0, sizeof(root));
    memcpy(root.name, "root", 5);
    list_init(&root.children);
    root.state = DEVICE_RUNNING;
}

device_t *device_root(void)
{
    return &root;
}

/* --- Matching and lifecycle ---------------------------------------------------- */

bool device_matches(const device_t *device, const device_match_t *ids)
{
    for (const device_match_t *m = ids; m && m->fields; m++) {
        if ((m->fields & MATCH_VENDOR) && m->vendor != device->id.vendor)
            continue;
        if ((m->fields & MATCH_DEVICE) && m->device != device->id.device)
            continue;
        if ((m->fields & MATCH_CLASS) && m->class_code != device->id.class_code)
            continue;
        if ((m->fields & MATCH_SUBCLASS) && m->subclass != device->id.subclass)
            continue;
        return true;
    }
    return false;
}

static bool matches(const device_t *device, const driver_t *driver)
{
    if (device->bus->match)
        return device->bus->match(device, driver);
    return device_matches(device, driver->ids);
}

static status_t claim_resources(device_t *device)
{
    for (uint32_t i = 0; i < device->resource_count; i++) {
        status_t status = resource_claim(&device->resources[i], device);
        if (STATUS_IS_ERROR(status)) {
            resource_release_all(device);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

static status_t attach(device_t *device, driver_t *driver)
{
    klog_debug("device %s: matched driver %s v%u", device->name, driver->name, driver->version);
    device->state = DEVICE_PROBING;

    status_t status = claim_resources(device);
    if (STATUS_IS_ERROR(status)) {
        klog_error("device %s: resource conflict, driver %s not attached", device->name, driver->name);
        device->state = DEVICE_FAILED;
        return status;
    }
    if (device->bus->set_power && device->power != DEVICE_POWER_D0)
        device->bus->set_power(device, DEVICE_POWER_D0);

    device->driver = driver;
    status = driver->probe(device);
    if (STATUS_IS_ERROR(status)) {
        klog_error("device %s: driver %s probe failed: %s", device->name, driver->name, status_name(status));
        if (device->bus->detached)
            device->bus->detached(device);
        device->driver = NULL;
        device->driver_data = NULL;
        resource_release_all(device);
        device->state = DEVICE_FAILED;
        return status;
    }

    driver->bound++;
    device->state = DEVICE_RUNNING;
    klog_info("device %s: attached to %s", device->name, driver->name);
    return STATUS_SUCCESS;
}

static void detach(device_t *device)
{
    driver_t *driver = device->driver;

    if (driver->remove)
        driver->remove(device);
    if (device->bus->detached)
        device->bus->detached(device);
    resource_release_all(device);

    device->driver = NULL;
    device->driver_data = NULL;
    device->state = DEVICE_DISCOVERED;
    driver->bound--;
    klog_info("device %s: detached from %s", device->name, driver->name);
}

static bool bindable(const device_t *device)
{
    return !device->driver && (device->state == DEVICE_DISCOVERED || device->state == DEVICE_FAILED);
}

static void try_bind_device(device_t *device)
{
    list_for_each(node, &device->bus->drivers) {
        driver_t *driver = container_of(node, driver_t, bus_node);
        if (matches(device, driver) && attach(device, driver) == STATUS_SUCCESS)
            return;
    }
}

/* --- Registration -------------------------------------------------------------- */

status_t bus_register(bus_t *bus)
{
    if (bus_find(bus->name))
        return STATUS_BUSY;
    list_init(&bus->devices);
    list_init(&bus->drivers);
    list_push_back(&buses, &bus->node);
    klog_debug("bus %s: registered", bus->name);
    return STATUS_SUCCESS;
}

bus_t *bus_find(const char *name)
{
    list_for_each(node, &buses) {
        bus_t *bus = container_of(node, bus_t, node);
        if (strcmp(bus->name, name) == 0)
            return bus;
    }
    return NULL;
}

status_t device_register(device_t *device, bus_t *bus, device_t *parent)
{
    if (!parent)
        parent = &root;

    list_init(&device->children);
    device->bus = bus;
    device->parent = parent;
    device->driver = NULL;
    device->state = DEVICE_DISCOVERED;
    list_push_back(&parent->children, &device->sibling_node);
    if (bus)
        list_push_back(&bus->devices, &device->bus_node);

    klog_debug("device %s: discovered (%04x:%04x class %02x.%02x)", device->name, device->id.vendor,
               device->id.device, device->id.class_code, device->id.subclass);
    if (bus)
        try_bind_device(device);
    return STATUS_SUCCESS;
}

void device_unregister(device_t *device)
{
    if (device->driver)
        detach(device);
    list_remove(&device->sibling_node);
    if (device->bus)
        list_remove(&device->bus_node);
    klog_debug("device %s: removed", device->name);
}

status_t driver_register(driver_t *driver)
{
    bus_t *bus = bus_find(driver->bus_name);
    if (!bus || !driver->probe) {
        klog_error("driver %s: unknown bus '%s' or no probe function", driver->name, driver->bus_name);
        return STATUS_INVALID_ARGUMENT;
    }

    driver->bus = bus;
    driver->module = module_current();
    driver->bound = 0;
    list_push_back(&bus->drivers, &driver->bus_node);
    klog_info("driver %s v%u registered on %s%s%s", driver->name, driver->version, bus->name,
              driver->module ? " by module " : "", driver->module ? driver->module->name : "");

    list_for_each(node, &bus->devices) {
        device_t *device = container_of(node, device_t, bus_node);
        if (bindable(device) && matches(device, driver))
            attach(device, driver);
    }
    return STATUS_SUCCESS;
}

void driver_unregister(driver_t *driver)
{
    list_for_each(node, &driver->bus->devices) {
        device_t *device = container_of(node, device_t, bus_node);
        if (device->driver == driver)
            detach(device);
    }
    list_remove(&driver->bus_node);
    klog_info("driver %s unregistered", driver->name);

    /* Devices it served may have another driver. */
    list_for_each(node, &driver->bus->devices) {
        device_t *device = container_of(node, device_t, bus_node);
        if (device->state == DEVICE_DISCOVERED)
            try_bind_device(device);
    }
}

uint32_t driver_count_for_module(const struct module *module)
{
    uint32_t count = 0;
    list_for_each(bus_node, &buses) {
        bus_t *bus = container_of(bus_node, bus_t, node);
        list_for_each(node, &bus->drivers) {
            if (container_of(node, driver_t, bus_node)->module == module)
                count++;
        }
    }
    return count;
}

/* --- Power --------------------------------------------------------------------- */

status_t device_suspend(device_t *device)
{
    if (device->state != DEVICE_RUNNING)
        return STATUS_INVALID_ARGUMENT;

    if (device->driver->suspend) {
        status_t status = device->driver->suspend(device);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    if (device->bus->set_power && device->bus->set_power(device, DEVICE_POWER_D3) == STATUS_SUCCESS)
        device->power = DEVICE_POWER_D3;
    device->state = DEVICE_SUSPENDED;
    klog_info("device %s: suspended (D%u)", device->name, device->power == DEVICE_POWER_D3 ? 3u : 0u);
    return STATUS_SUCCESS;
}

status_t device_resume(device_t *device)
{
    if (device->state != DEVICE_SUSPENDED)
        return STATUS_INVALID_ARGUMENT;

    if (device->power != DEVICE_POWER_D0 && device->bus->set_power) {
        status_t status = device->bus->set_power(device, DEVICE_POWER_D0);
        if (STATUS_IS_ERROR(status))
            return status;
        device->power = DEVICE_POWER_D0;
    }
    if (device->driver->resume) {
        status_t status = device->driver->resume(device);
        if (STATUS_IS_ERROR(status)) {
            device->state = DEVICE_FAILED;
            return status;
        }
    }
    device->state = DEVICE_RUNNING;
    klog_info("device %s: resumed", device->name);
    return STATUS_SUCCESS;
}

/* --- Queries and diagnostics --------------------------------------------------- */

device_t *device_find(const char *bus_name, uint16_t vendor, uint16_t device_id, unsigned index)
{
    bus_t *bus = bus_find(bus_name);
    if (!bus)
        return NULL;
    list_for_each(node, &bus->devices) {
        device_t *device = container_of(node, device_t, bus_node);
        if (device->id.vendor == vendor && device->id.device == device_id && index-- == 0)
            return device;
    }
    return NULL;
}

static void dump(const device_t *device, unsigned depth)
{
    char indent[2 * 8 + 1];
    unsigned n = depth < 8 ? depth : 8;
    memset(indent, ' ', 2 * n);
    indent[2 * n] = '\0';

    if (depth == 0)
        klog_info("devices: %s", device->name);
    else
        klog_info("devices: %s%s [%04x:%04x %02x.%02x] %s%s%s", indent, device->name, device->id.vendor,
                  device->id.device, device->id.class_code, device->id.subclass, device_state_name(device->state),
                  device->driver ? " -> " : "", device->driver ? device->driver->name : "");

    list_for_each(node, &device->children)
        dump(container_of(node, device_t, sibling_node), depth + 1);
}

void device_tree_dump(void)
{
    dump(&root, 0);
}

EXPORT_SYMBOL(driver_register);
EXPORT_SYMBOL(driver_unregister);
EXPORT_SYMBOL(device_register);
EXPORT_SYMBOL(device_unregister);
EXPORT_SYMBOL(device_find);
