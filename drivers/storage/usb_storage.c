/*
 * USB mass storage (README section 25): USB sticks, card readers, external disks.
 *
 * Class 08, subclass 06 (SCSI transparent command set), protocol 0x50
 * (bulk-only transport). Every command is three steps on two bulk
 * endpoints: a command block wrapper (CBW) with the SCSI command goes out,
 * the data goes in or out, and a command status wrapper (CSW) comes back.
 *
 * SCSI commands used: INQUIRY, TEST UNIT READY, REQUEST SENSE, READ
 * CAPACITY (10/16), READ and WRITE (10/16), SYNCHRONIZE CACHE.
 *
 * Requests are serialized by a mutex and go through a bounce buffer of
 * 64 KiB that does not cross a 64 KiB boundary (one transfer block for the
 * controller). A stalled endpoint is cleared; anything worse gets the
 * transport's reset. The first unit (LUN 0) with 512-byte blocks becomes a
 * block device "usb<n>", writable.
 *
 * When the device is pulled out, its file systems are unmounted. If files
 * on it are still open, the block device stays behind as a dead one (all
 * I/O fails) until the next start.
 *
 * Not yet: several LUNs (multi-slot card readers show the first slot), UAS,
 * write protection detection, media change in card readers.
 */

#include "drivers/bus/usb/usb.h"
#include "drivers/core/module.h"
#include "fs/block/block.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"

#define SUBCLASS_SCSI      0x06
#define PROTOCOL_BULK_ONLY 0x50

#define REQUEST_RESET       0xFF
#define REQUEST_GET_MAX_LUN 0xFE

#define CBW_SIGNATURE 0x43425355
#define CSW_SIGNATURE 0x53425355
#define CBW_SIZE 31
#define CSW_SIZE 13

#define SCSI_TEST_UNIT_READY   0x00
#define SCSI_REQUEST_SENSE     0x03
#define SCSI_INQUIRY           0x12
#define SCSI_READ_CAPACITY_10  0x25
#define SCSI_READ_10           0x28
#define SCSI_WRITE_10          0x2A
#define SCSI_SYNCHRONIZE_CACHE 0x35
#define SCSI_READ_16           0x88
#define SCSI_WRITE_16          0x8A
#define SCSI_SERVICE_ACTION_IN 0x9E /* with action 0x10: READ CAPACITY (16) */

#define SECTOR_SIZE   512
#define WINDOW_SIZE   (64 * 1024)
#define DATA_TIMEOUT_NS    20000000000ull
#define COMMAND_TIMEOUT_NS 5000000000ull

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;      /* 0x80: data comes in */
    uint8_t  lun;
    uint8_t  command_length;
    uint8_t  command[16];
} cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;     /* 0 passed, 1 failed, 2 phase error */
} csw_t;

typedef struct {
    usb_device_t  *usb;
    uint8_t        interface_number, in, out;
    mutex_t        lock;
    bool           gone;
    uint32_t       tag;
    dma_buffer_t   memory;      /* twice the window: an aligned 64 KiB lies somewhere inside */
    uint8_t       *window;
    uint64_t       window_phys;
    dma_buffer_t   wrappers;    /* CBW at 0, CSW at 64 */
    bool           long_commands; /* more than 2^32 blocks: 16-byte commands */
    block_device_t block;
} ums_t;

static uint32_t disk_count;

/* Bulk-only mass storage reset: the way out of every confused state. */
static void reset_recovery(ums_t *u)
{
    usb_control(u->usb, USB_TYPE_CLASS | USB_RECIP_INTERFACE, REQUEST_RESET, 0, u->interface_number, NULL, 0, NULL);
    usb_clear_halt(u->usb, u->in);
    usb_clear_halt(u->usb, u->out);
}

/*
 * Run one SCSI command with `length` bytes of data in the window.
 * IO_ERROR: the device reported a failure (REQUEST SENSE tells why).
 * u->lock held.
 */
static status_t scsi(ums_t *u, const uint8_t *command, uint8_t command_length, uint32_t length, bool in,
                     uint32_t *transferred)
{
    cbw_t *cbw = u->wrappers.virt;
    csw_t *csw = (csw_t *)((uint8_t *)u->wrappers.virt + 64);
    uint32_t got = 0;

    if (u->gone)
        return STATUS_NOT_FOUND;
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    cbw->tag = ++u->tag;
    cbw->data_length = length;
    cbw->flags = in ? 0x80 : 0;
    cbw->command_length = command_length;
    memcpy(cbw->command, command, command_length);

    status_t status = usb_bulk(u->usb, u->out, cbw, u->wrappers.phys, CBW_SIZE, NULL, COMMAND_TIMEOUT_NS);
    if (STATUS_IS_ERROR(status)) {
        if (status != STATUS_NOT_FOUND)
            reset_recovery(u);
        return status == STATUS_NOT_FOUND ? status : STATUS_DEVICE_ERROR;
    }
    if (length) {
        status = usb_bulk(u->usb, in ? u->in : u->out, u->window, u->window_phys, length, &got, DATA_TIMEOUT_NS);
        if (status == STATUS_IO_ERROR) {
            usb_clear_halt(u->usb, in ? u->in : u->out); /* a stall: the status follows anyway */
        } else if (STATUS_IS_ERROR(status)) {
            if (status != STATUS_NOT_FOUND)
                reset_recovery(u);
            return status == STATUS_NOT_FOUND ? status : STATUS_DEVICE_ERROR;
        }
    }
    memset(csw, 0, sizeof(*csw));
    status = usb_bulk(u->usb, u->in, csw, u->wrappers.phys + 64, CSW_SIZE, NULL, COMMAND_TIMEOUT_NS);
    if (status == STATUS_IO_ERROR) { /* stalled once: clear and ask again */
        usb_clear_halt(u->usb, u->in);
        status = usb_bulk(u->usb, u->in, csw, u->wrappers.phys + 64, CSW_SIZE, NULL, COMMAND_TIMEOUT_NS);
    }
    if (STATUS_IS_ERROR(status) || csw->signature != CSW_SIGNATURE || csw->tag != u->tag || csw->status > 1) {
        if (status != STATUS_NOT_FOUND)
            reset_recovery(u);
        return status == STATUS_NOT_FOUND ? status : STATUS_DEVICE_ERROR;
    }
    if (transferred)
        *transferred = got;
    return csw->status == 0 ? STATUS_SUCCESS : STATUS_IO_ERROR;
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* --- Block device ---------------------------------------------------------------- */

static status_t transfer(ums_t *u, bool write, uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t *data = buffer;
    status_t status = STATUS_SUCCESS;

    mutex_lock(&u->lock);
    while (count && !STATUS_IS_ERROR(status)) {
        uint32_t chunk = count < WINDOW_SIZE / SECTOR_SIZE ? count : WINDOW_SIZE / SECTOR_SIZE;
        uint32_t bytes = chunk * SECTOR_SIZE, got = 0;
        uint8_t command[16] = { 0 };
        uint8_t length;

        if (u->long_commands) {
            command[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
            put_be32(command + 2, (uint32_t)(lba >> 32));
            put_be32(command + 6, (uint32_t)lba);
            put_be32(command + 10, chunk);
            length = 16;
        } else {
            command[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
            put_be32(command + 2, (uint32_t)lba);
            command[7] = (uint8_t)(chunk >> 8);
            command[8] = (uint8_t)chunk;
            length = 10;
        }
        if (write)
            memcpy(u->window, data, bytes);
        status = scsi(u, command, length, bytes, !write, &got);
        if (!STATUS_IS_ERROR(status) && got != bytes)
            status = STATUS_IO_ERROR;
        if (!STATUS_IS_ERROR(status) && !write)
            memcpy(data, u->window, bytes);
        data += bytes;
        lba += chunk;
        count -= chunk;
    }
    mutex_unlock(&u->lock);
    return status;
}

static status_t ums_read(block_device_t *block, uint64_t lba, uint32_t count, void *buffer)
{
    return transfer(block->driver_data, false, lba, count, buffer);
}

static status_t ums_write(block_device_t *block, uint64_t lba, uint32_t count, const void *buffer)
{
    return transfer(block->driver_data, true, lba, count, (void *)buffer);
}

static status_t ums_flush(block_device_t *block)
{
    ums_t *u = block->driver_data;
    uint8_t command[10] = { SCSI_SYNCHRONIZE_CACHE };

    mutex_lock(&u->lock);
    status_t status = scsi(u, command, sizeof(command), 0, false, NULL);
    mutex_unlock(&u->lock);
    /* Sticks without a write cache refuse the command; that is no failure. */
    return status == STATUS_IO_ERROR ? STATUS_SUCCESS : status;
}

static const block_ops_t ums_ops = {
    .read = ums_read,
    .write = ums_write,
    .flush = ums_flush,
};

/* --- Setup ----------------------------------------------------------------------- */

/* INQUIRY strings are space padded. */
static void scsi_text(char *out, const uint8_t *field, size_t length)
{
    while (length && (field[length - 1] == ' ' || field[length - 1] == '\0'))
        length--;
    for (size_t i = 0; i < length; i++)
        out[i] = field[i] >= 0x20 && field[i] < 0x7F ? (char)field[i] : '?';
    out[length] = '\0';
}

/* Find out what the unit is and how large; fills u->block. u->lock held. */
static status_t identify(ums_t *u, char *model)
{
    uint8_t inquiry[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t ready[6] = { SCSI_TEST_UNIT_READY };
    uint8_t sense[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t capacity[10] = { SCSI_READ_CAPACITY_10 };
    uint32_t got = 0;

    model[0] = '\0';
    status_t status = scsi(u, inquiry, sizeof(inquiry), 36, true, &got);
    if (!STATUS_IS_ERROR(status) && got >= 36) {
        char vendor[9], product[17];
        scsi_text(vendor, u->window + 8, 8);
        scsi_text(product, u->window + 16, 16);
        format(model, 32, "%s %s", vendor, product);
    } else if (status != STATUS_IO_ERROR) {
        return status;
    }

    /* A unit needs a moment after power-on, and reports "not ready" until asked why. */
    for (int tries = 0;; tries++) {
        status = scsi(u, ready, sizeof(ready), 0, false, NULL);
        if (status != STATUS_IO_ERROR)
            break;
        if (STATUS_IS_ERROR(scsi(u, sense, sizeof(sense), 18, true, NULL)) && tries > 2)
            break;
        if (tries == 30)
            return STATUS_TIMEOUT; /* e.g. a card reader without a card */
        thread_sleep(100000000);
    }
    if (STATUS_IS_ERROR(status))
        return status;

    status = scsi(u, capacity, sizeof(capacity), 8, true, &got);
    if (STATUS_IS_ERROR(status) || got < 8)
        return STATUS_IS_ERROR(status) ? status : STATUS_IO_ERROR;
    uint64_t last = be32(u->window);
    uint32_t block_size = be32(u->window + 4);
    if (last == 0xFFFFFFFF) {
        uint8_t capacity16[16] = { SCSI_SERVICE_ACTION_IN, 0x10 };
        capacity16[13] = 32;
        status = scsi(u, capacity16, sizeof(capacity16), 32, true, &got);
        if (STATUS_IS_ERROR(status) || got < 12)
            return STATUS_IS_ERROR(status) ? status : STATUS_IO_ERROR;
        last = (uint64_t)be32(u->window) << 32 | be32(u->window + 4);
        block_size = be32(u->window + 8);
        u->long_commands = true;
    }
    if (block_size != SECTOR_SIZE) {
        klog_warn("usb-storage: %s: blocks of %u bytes are not supported", model, block_size);
        return STATUS_NOT_SUPPORTED;
    }
    u->block.sector_size = SECTOR_SIZE;
    u->block.sector_count = last + 1;
    u->block.removable = true;
    return STATUS_SUCCESS;
}

static status_t ums_probe(device_t *device)
{
    usb_interface_t *interface = usb_interface_from_device(device);
    char model[32];

    if (interface->descriptor->interface_subclass != SUBCLASS_SCSI ||
        interface->descriptor->interface_protocol != PROTOCOL_BULK_ONLY)
        return STATUS_NOT_SUPPORTED;
    ums_t *u = kcalloc(1, sizeof(*u));
    if (!u)
        return STATUS_OUT_OF_MEMORY;
    u->usb = interface->usb;
    u->interface_number = interface->descriptor->number;
    mutex_init(&u->lock);
    for (uint32_t i = 0; i < interface->endpoint_count; i++) {
        const usb_endpoint_descriptor_t *e = interface->endpoints[i];
        if ((e->attributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_BULK)
            continue;
        if (e->address & USB_ENDPOINT_IN)
            u->in = e->address;
        else
            u->out = e->address;
    }
    status_t status = (u->in && u->out) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, 2 * WINDOW_SIZE, ~0ull, &u->memory);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, PAGE_SIZE, ~0ull, &u->wrappers);
    if (!STATUS_IS_ERROR(status)) {
        uint64_t offset = (WINDOW_SIZE - u->memory.phys % WINDOW_SIZE) % WINDOW_SIZE;
        u->window = (uint8_t *)u->memory.virt + offset;
        u->window_phys = u->memory.phys + offset;

        mutex_lock(&u->lock);
        status = identify(u, model);
        mutex_unlock(&u->lock);
    }
    if (STATUS_IS_ERROR(status)) {
        if (status == STATUS_TIMEOUT)
            klog_info("usb-storage: %s: no medium", device->name);
        dma_free(&u->memory);
        dma_free(&u->wrappers);
        kfree(u);
        return status;
    }

    format(u->block.name, sizeof(u->block.name), "usb%u", disk_count++);
    u->block.ops = &ums_ops;
    u->block.driver_data = u;
    klog_info("usb-storage: %s: %s, %lu MiB", device->name, model[0] ? model : "disk",
              u->block.sector_count / (1024 * 1024 / SECTOR_SIZE));
    device->driver_data = u;
    return block_register(&u->block);
}

static void ums_remove(device_t *device)
{
    ums_t *u = device->driver_data;

    /* Wait for a request in progress (its transfers have just failed), then refuse new ones. */
    mutex_lock(&u->lock);
    u->gone = true;
    mutex_unlock(&u->lock);

    if (!block_unregister(&u->block)) {
        klog_warn("usb-storage: %s was removed while files on it were open; they will fail", u->block.name);
        return; /* the block device and its buffers stay: the file systems still point at them */
    }
    dma_free(&u->memory);
    dma_free(&u->wrappers);
    kfree(u);
}

static const device_match_t ums_ids[] = {
    { 0, 0, USB_CLASS_STORAGE, 0, MATCH_CLASS },
    DEVICE_MATCH_END,
};

static driver_t ums_driver = {
    .name = "usb-storage",
    .bus_name = "usb",
    .version = 1,
    .capabilities = DRIVER_CAP_STORAGE,
    .ids = ums_ids,
    .probe = ums_probe,
    .remove = ums_remove,
};

static status_t ums_module_init(void)
{
    return driver_register(&ums_driver);
}

static const char *const ums_dependencies[] = { "usb", NULL };

MODULE(.name = "usb_storage", .description = "USB mass storage (sticks, card readers, disks)", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = ums_dependencies, .init = ums_module_init);
