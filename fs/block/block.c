#include "fs/block/block.h"

#include "fs/vfs/vfs.h"

#include "core/cmdline.h"
#include "core/export.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

static list_t devices = { { &devices.head, &devices.head } };

static bool in_range(const block_device_t *device, uint64_t lba, uint32_t count)
{
    return count && lba < device->sector_count && count <= device->sector_count - lba;
}

bool block_internal_disks_writable(void)
{
    static bool announced;
    char value[8];
    bool writable = cmdline_value("disks", value, sizeof(value)) && strcmp(value, "rw") == 0;

    if (!writable && !announced) {
        announced = true;
        klog_info("block: internal disks are read-only (boot with disks=rw to allow writing)");
    }
    return writable;
}

status_t block_register(block_device_t *device)
{
    if (!device->sector_size || !device->sector_count || (!device->parent && !device->ops))
        return STATUS_INVALID_ARGUMENT;

    list_push_back(&devices, &device->node);
    klog_info("block %s: %lu sectors of %u bytes (%lu MiB)%s", device->name, device->sector_count,
              device->sector_size, device->sector_count * device->sector_size >> 20,
              device->read_only ? ", read-only" : "");

    /* A disk with partitions is not mounted itself; its partitions are. */
    if (!device->parent && partition_scan(device) > 0)
        return STATUS_SUCCESS;
    vfs_automount(device);
    return STATUS_SUCCESS;
}

status_t block_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    if (!in_range(device, lba, count))
        return STATUS_INVALID_ARGUMENT;
    if (device->parent)
        return block_read(device->parent, device->start_lba + lba, count, buffer);
    return device->ops->read(device, lba, count, buffer);
}

status_t block_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!in_range(device, lba, count))
        return STATUS_INVALID_ARGUMENT;
    if (device->read_only)
        return STATUS_ACCESS_DENIED;
    if (device->parent)
        return block_write(device->parent, device->start_lba + lba, count, buffer);
    return device->ops->write(device, lba, count, buffer);
}

status_t block_flush(block_device_t *device)
{
    while (device->parent)
        device = device->parent;
    return device->ops->flush ? device->ops->flush(device) : STATUS_SUCCESS;
}

status_t block_read_bytes(block_device_t *device, uint64_t offset, size_t size, void *buffer)
{
    uint32_t sector_size = device->sector_size;
    uint8_t *sector = kmalloc(sector_size);
    uint8_t *out = buffer;
    status_t status = sector ? STATUS_SUCCESS : STATUS_OUT_OF_MEMORY;

    while (!STATUS_IS_ERROR(status) && size) {
        uint64_t lba = offset / sector_size;
        size_t skip = offset % sector_size;
        size_t chunk = sector_size - skip < size ? sector_size - skip : size;

        status = block_read(device, lba, 1, sector);
        if (!STATUS_IS_ERROR(status)) {
            memcpy(out, sector + skip, chunk);
            out += chunk;
            offset += chunk;
            size -= chunk;
        }
    }
    kfree(sector);
    return status;
}

block_device_t *block_find(const char *name)
{
    list_for_each(node, &devices) {
        block_device_t *device = container_of(node, block_device_t, node);
        if (strcmp(device->name, name) == 0)
            return device;
    }
    return NULL;
}

block_device_t *block_at(size_t index)
{
    list_for_each(node, &devices) {
        if (index-- == 0)
            return container_of(node, block_device_t, node);
    }
    return NULL;
}

EXPORT_SYMBOL(block_register);
EXPORT_SYMBOL(block_internal_disks_writable);
EXPORT_SYMBOL(block_read);
EXPORT_SYMBOL(block_write);
