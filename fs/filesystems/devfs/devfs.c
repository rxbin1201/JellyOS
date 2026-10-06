/*
 * devfs: the device directory mounted at /dev.
 *
 * Drivers register character devices by name with their own vnode
 * operations (read/write). Device vnodes are streams: the VFS calls them
 * without its lock, so they may block (console input) and must do their own
 * locking. Built in: "null" and "zero".
 */

#include "fs/vfs/vfs.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define DEVICE_NAME_MAX 32

typedef struct {
    vnode_t     vnode;
    char        name[DEVICE_NAME_MAX];
    size_t      length;
    list_node_t node;
} device_node_t;

static filesystem_t devfs;
static vnode_t root;
static list_t devices = { { &devices.head, &devices.head } };
static bool mounted;
static uint64_t next_inode = 2;

/* --- Directory --------------------------------------------------------------- */

static status_t dev_lookup(vnode_t *dir, const char *name, size_t length, vnode_t **result)
{
    (void)dir;
    list_for_each(node, &devices) {
        device_node_t *d = container_of(node, device_node_t, node);
        if (d->length == length && memcmp(d->name, name, length) == 0) {
            vnode_retain(&d->vnode);
            *result = &d->vnode;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

static status_t dev_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *entry)
{
    uint64_t index = 0;

    (void)dir;
    list_for_each(node, &devices) {
        if (index++ != *cookie)
            continue;
        device_node_t *d = container_of(node, device_node_t, node);
        entry->type = VNODE_DEVICE;
        entry->inode = d->vnode.inode;
        entry->name_length = (uint32_t)d->length;
        memcpy(entry->name, d->name, d->length + 1);
        (*cookie)++;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}

static const vnode_ops_t directory_ops = {
    .lookup = dev_lookup,
    .readdir = dev_readdir,
};

/* --- null and zero --------------------------------------------------------- */

static status_t null_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    (void)v, (void)offset, (void)buffer, (void)size;
    *done = 0; /* always at end of file */
    return STATUS_SUCCESS;
}

static status_t null_write(vnode_t *v, uint64_t offset, const void *buffer, size_t size, size_t *done)
{
    (void)v, (void)offset, (void)buffer;
    *done = size; /* discard everything */
    return STATUS_SUCCESS;
}

static status_t zero_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    (void)v, (void)offset;
    memset(buffer, 0, size);
    *done = size;
    return STATUS_SUCCESS;
}

static const vnode_ops_t null_ops = { .read = null_read, .write = null_write };
static const vnode_ops_t zero_ops = { .read = zero_read, .write = null_write };

/* --- Registration and mounting ------------------------------------------------ */

static void init_once(void);

status_t devfs_register(const char *name, const vnode_ops_t *ops, uint32_t mode, void *data)
{
    init_once();
    size_t length = strlen(name);
    if (length == 0 || length >= DEVICE_NAME_MAX)
        return STATUS_INVALID_ARGUMENT;

    device_node_t *d = kcalloc(1, sizeof(*d));
    if (!d)
        return STATUS_OUT_OF_MEMORY;

    /* The initial reference is never dropped: device nodes are permanent. */
    vnode_init(&d->vnode, &devfs, VNODE_DEVICE, ops);
    d->vnode.mode = mode & 0777;
    d->vnode.inode = next_inode++;
    d->vnode.data = data;
    memcpy(d->name, name, length + 1);
    d->length = length;
    list_push_back(&devices, &d->node);
    klog_debug("devfs: /dev/%s registered", name);
    return STATUS_SUCCESS;
}

static void init_once(void)
{
    if (devfs.root)
        return;
    vnode_init(&root, &devfs, VNODE_DIRECTORY, &directory_ops);
    root.inode = 1;
    devfs.root = &root;
    devfs_register("null", &null_ops, 0666, NULL);
    devfs_register("zero", &zero_ops, 0666, NULL);
}

static status_t devfs_mount(block_device_t *device, filesystem_t **fs)
{
    if (device)
        return STATUS_NOT_SUPPORTED;
    if (mounted)
        return STATUS_BUSY; /* one instance */
    init_once();
    mounted = true;
    *fs = &devfs;
    return STATUS_SUCCESS;
}

static status_t devfs_unmount(filesystem_t *fs)
{
    (void)fs;
    return STATUS_BUSY; /* device nodes stay referenced */
}

fs_type_t devfs_type = {
    .name = "devfs",
    .mount = devfs_mount,
    .unmount = devfs_unmount,
};
