/*
 * /dev/crash: a kernel panic on request, to see what a panic looks like on
 * a system that is running (a desktop on the screen, a mode set by a
 * driver), which the faults of "crashtest=<kind>" at the start cannot show.
 *
 * The file is only there with "crashtest=device" on the kernel command
 * line, and then everybody may write to it: whoever starts the kernel that
 * way wants to be able to crash it, and on a real machine there is no root
 * shell to do it from (root cannot log in at the screen). A word is written:
 *
 *   echo panic > /dev/crash     panic()
 *   echo assert > /dev/crash    a failed ASSERT()
 */

#include "drivers/core/module.h"
#include "fs/vfs/vfs.h"

#include "core/cmdline.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/string.h"

static status_t crash_write(vnode_t *v, uint64_t offset, const void *buffer, size_t size, size_t *done)
{
    (void)v;
    (void)offset;
    if (size >= 5 && memcmp(buffer, "panic", 5) == 0)
        panic("requested through /dev/crash");
    if (size >= 6 && memcmp(buffer, "assert", 6) == 0)
        ASSERT(size == 0);
    *done = 0;
    return STATUS_INVALID_ARGUMENT;
}

static const vnode_ops_t crash_ops = {
    .write = crash_write,
};

static status_t crash_init(void)
{
    char kind[32];

    if (!cmdline_value("crashtest", kind, sizeof(kind)) || strcmp(kind, "device") != 0)
        return STATUS_SUCCESS;
    klog_warn("crashtest: /dev/crash is there: every user can make the kernel panic");
    return devfs_register_file("crash", &crash_ops, 0222, NULL);
}

MODULE(.name = "crash", .description = "Kernel panic on request (/dev/crash, with crashtest=device)", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .init = crash_init);
