/*
 * Initramfs (README section 27): a cpio archive in "newc" format, passed by
 * the boot manager as the boot module "initrd" and unpacked into the root
 * ramfs before init starts. It is replaceable without rebuilding the kernel.
 *
 * Supported entries: directories, regular files and symbolic links, with
 * their permission bits and owners. Everything else is skipped.
 */

#include "fs/initramfs/initramfs.h"

#include "fs/vfs/vfs.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define HEADER_SIZE   110
#define MODE_TYPE     0170000
#define MODE_DIR      0040000
#define MODE_FILE     0100000
#define MODE_SYMLINK  0120000

typedef struct {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t name_size;
} entry_t;

static bool hex_field(const char *p, uint32_t *value)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
                       : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                       : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16;
        if (digit == 16)
            return false;
        v = v << 4 | digit;
    }
    *value = v;
    return true;
}

/* Fields in order: ino mode uid gid nlink mtime filesize devmajor devminor rdevmajor rdevminor namesize check */
static bool parse_header(const char *h, entry_t *e)
{
    uint32_t ignored;
    return memcmp(h, "070701", 6) == 0 && hex_field(h + 6, &ignored) && hex_field(h + 14, &e->mode) &&
           hex_field(h + 22, &e->uid) && hex_field(h + 30, &e->gid) && hex_field(h + 54, &e->size) &&
           hex_field(h + 94, &e->name_size);
}

static size_t pad4(size_t value)
{
    return (value + 3) & ~(size_t)3;
}

static status_t write_file(const char *path, const uint8_t *data, size_t size)
{
    file_t *file;
    size_t done;
    status_t status = vfs_open(path, JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | JELLY_OPEN_TRUNCATE, 0644, NULL, &file);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_write(file, data, size, &done);
    object_release(&file->object);
    return STATUS_IS_ERROR(status) ? status : done == size ? STATUS_SUCCESS : STATUS_NO_SPACE;
}

static status_t unpack_entry(const entry_t *e, const char *path, const uint8_t *data)
{
    status_t status;

    if (strcmp(path, "/") == 0)
        return STATUS_SUCCESS; /* the archive's "." entry: the root exists */

    switch (e->mode & MODE_TYPE) {
    case MODE_DIR:
        status = vfs_mkdir(path, e->mode, NULL);
        if (status == STATUS_ALREADY_EXISTS)
            status = STATUS_SUCCESS; /* "/" or a directory listed twice */
        break;
    case MODE_FILE:
        status = write_file(path, data, e->size);
        break;
    case MODE_SYMLINK: {
        char *target = kmalloc(e->size + 1);
        if (!target)
            return STATUS_OUT_OF_MEMORY;
        memcpy(target, data, e->size);
        target[e->size] = '\0';
        status = vfs_symlink(target, path, NULL);
        kfree(target);
        return status; /* links carry no own permissions */
    }
    default:
        klog_warn("initramfs: %s: unsupported entry type, skipped", path);
        return STATUS_SUCCESS;
    }
    if (!STATUS_IS_ERROR(status))
        status = vfs_set_attributes(path, e->uid, e->gid, e->mode);
    return status;
}

status_t initramfs_unpack(const void *archive, size_t size, unsigned *entries)
{
    const uint8_t *base = archive;
    size_t offset = 0;
    char *path = kmalloc(VFS_PATH_MAX);

    *entries = 0;
    if (!path)
        return STATUS_OUT_OF_MEMORY;

    status_t status = STATUS_SUCCESS;
    while (!STATUS_IS_ERROR(status)) {
        entry_t e;
        if (size - offset < HEADER_SIZE || !parse_header((const char *)base + offset, &e)) {
            status = STATUS_INVALID_ARGUMENT;
            break;
        }
        size_t name_offset = offset + HEADER_SIZE;
        size_t data_offset = pad4(name_offset + e.name_size);
        if (e.name_size == 0 || e.name_size > size - name_offset || data_offset > size ||
            e.size > size - data_offset || base[name_offset + e.name_size - 1] != '\0') {
            status = STATUS_INVALID_ARGUMENT;
            break;
        }

        const char *name = (const char *)base + name_offset;
        if (strcmp(name, "TRAILER!!!") == 0)
            break;

        /* Names are relative to the archive root ("bin/shell" or "./bin/shell"). */
        status = vfs_normalize("/", name, e.name_size - 1, path, VFS_PATH_MAX);
        if (!STATUS_IS_ERROR(status))
            status = unpack_entry(&e, path, base + data_offset);
        if (STATUS_IS_ERROR(status))
            klog_error("initramfs: %s: %s", name, status_name(status));
        else
            (*entries)++;
        offset = pad4(data_offset + e.size);
    }
    kfree(path);
    return status;
}
