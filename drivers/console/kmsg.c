/*
 * /dev/kmsg: the kernel log as a read-only file (what `dmesg` prints).
 *
 * On a real machine there is no serial port to capture, and the screen
 * belongs to the desktop once it runs; this is how to see what the kernel
 * and its drivers reported. The file holds the last 64 KiB of the log.
 */

#include "drivers/core/module.h"
#include "fs/vfs/vfs.h"

#include "core/log.h"

static status_t kmsg_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    (void)v;
    *done = klog_read(offset, buffer, size);
    return STATUS_SUCCESS;
}

static const vnode_ops_t kmsg_ops = {
    .read = kmsg_read,
};

static status_t kmsg_init(void)
{
    return devfs_register_file("kmsg", &kmsg_ops, 0444, NULL);
}

MODULE(.name = "kmsg", .description = "Kernel log as /dev/kmsg", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .init = kmsg_init);
