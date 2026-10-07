/*
 * /dev/kmsg: the kernel log as a read-only file (what `dmesg` prints).
 *
 * On a real machine there is no serial port to capture, and the screen
 * belongs to the desktop once it runs; this is how to see what the kernel
 * and its drivers reported. The file holds the last 64 KiB of the log.
 *
 * "logfile=PATH" on the kernel command line also writes the log to a file,
 * every two seconds while it changes: for the cases in which nothing can be
 * read or typed any more (a graphics driver that leaves the screen dark).
 * With PATH on a USB stick (/volumes/usb0p1/kernel.log) the log can be read
 * on another computer afterwards.
 */

#include "drivers/core/module.h"
#include "fs/vfs/vfs.h"

#include "drivers/console/kmsg.h"

#include "core/cmdline.h"
#include "core/log.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"

#define LOG_BYTES (64u << 10)

static char logfile_path[96];

static mutex_t logfile_lock;
static bool logfile_ready;

void kmsg_logfile_write(void)
{
    static char text[LOG_BYTES];
    static size_t written;
    static uint32_t written_sum;

    if (!logfile_ready)
        return;
    mutex_lock(&logfile_lock);
    size_t length = klog_read(0, text, sizeof(text)), done;
    uint32_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum = sum * 31 + (uint8_t)text[i];
    file_t *file;
    /*
     * The volume may not be there yet (a USB stick appears a moment after the start) or be gone again. The
     * file is overwritten in place and cut to its length afterwards: if the machine stops in the middle, the
     * log of two seconds ago is still mostly there instead of an empty file.
     */
    if ((length != written || sum != written_sum) &&
        vfs_open(logfile_path, JELLY_OPEN_WRITE | JELLY_OPEN_CREATE, 0644, NULL, &file) == STATUS_SUCCESS) {
        if (vfs_write(file, text, length, &done) == STATUS_SUCCESS && done == length) {
            vfs_truncate(file, length);
            written = length;
            written_sum = sum;
        }
        object_release(&file->object);
    }
    mutex_unlock(&logfile_lock);
}

static void logfile_thread(void *argument)
{
    (void)argument;
    for (;;) {
        thread_sleep(2000000000ull);
        kmsg_logfile_write();
    }
}

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
    thread_t *thread;

    mutex_init(&logfile_lock);
    logfile_ready = cmdline_value("logfile", logfile_path, sizeof(logfile_path)) && logfile_path[0] == '/';
    if (logfile_ready &&
        thread_create_kernel("logfile", logfile_thread, NULL, THREAD_PRIORITY_KERNEL, &thread) == STATUS_SUCCESS) {
        klog_info("kmsg: the kernel log is also written to %s", logfile_path);
        thread_start(thread);
        object_release(&thread->object);
    }
    return devfs_register_file("kmsg", &kmsg_ops, 0444, NULL);
}

MODULE(.name = "kmsg", .description = "Kernel log as /dev/kmsg", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .init = kmsg_init);
