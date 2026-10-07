/*
 * Block device API (README section 25).
 *
 *   NVMe / AHCI / USB / VirtIO  ->  Block Device API  ->  Partitions  ->  File systems
 *
 * Drivers register whole disks; the partition layer adds one block device
 * per partition, which forwards I/O to its parent with an LBA offset. File
 * systems only see block devices.
 */

#ifndef FS_BLOCK_BLOCK_H
#define FS_BLOCK_BLOCK_H

#include "core/list.h"

#include <jelly/status.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_NAME_MAX 32

struct block_device;

typedef struct {
    /* buffer is a kernel buffer of count * sector_size bytes */
    status_t (*read)(struct block_device *device, uint64_t lba, uint32_t count, void *buffer);
    status_t (*write)(struct block_device *device, uint64_t lba, uint32_t count, const void *buffer);
    status_t (*flush)(struct block_device *device);
} block_ops_t;

typedef enum {
    PARTITION_NONE,
    PARTITION_MBR,
    PARTITION_GPT,
} partition_scheme_t;

typedef struct block_device {
    char                 name[BLOCK_NAME_MAX];
    uint32_t             sector_size;
    uint64_t             sector_count;
    bool                 read_only;
    bool                 removable;    /* a medium the user plugs in (USB stick): its files are everybody's */
    const block_ops_t   *ops;          /* whole disks */
    void                *driver_data;

    /* Partitions */
    struct block_device *parent;
    uint64_t             start_lba;    /* in the parent */
    uint32_t             partition_number;
    partition_scheme_t   scheme;
    uint8_t              type_guid[16]; /* GPT */
    uint8_t              unique_guid[16];
    uint8_t              mbr_type;      /* MBR */

    list_node_t          node;
} block_device_t;

/*
 * May the disks built into the machine (NVMe, SATA) be written? They hold
 * other operating systems and their data, so they are read-only unless the
 * kernel command line says "disks=rw". Drivers of such disks set read_only
 * from this.
 */
bool block_internal_disks_writable(void);

/* Register a disk or partition. Disks are scanned for partitions, then offered to file systems. */
status_t block_register(block_device_t *device);

/*
 * The disk is gone (USB): unmount the file systems on it and on its
 * partitions and forget the devices; the partitions are freed. Returns
 * false if a file system is still in use: then everything stays registered,
 * the driver must keep the disk's block_device_t and fail all I/O.
 */
bool     block_unregister(block_device_t *disk);

status_t block_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer);
status_t block_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer);
status_t block_flush(block_device_t *device);

/* Byte-granular read through a bounce sector (partition tables, superblocks). */
status_t block_read_bytes(block_device_t *device, uint64_t offset, size_t size, void *buffer);

block_device_t *block_find(const char *name);
block_device_t *block_at(size_t index);

/* Partition layer (partition.c): register a child device per partition. Returns the count. */
unsigned partition_scan(block_device_t *disk);

#endif
