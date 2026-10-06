/*
 * ramfs: a file system that lives entirely in kernel memory.
 *
 * It serves as the root file system ("/" with /volumes and /tmp) and
 * supports everything the VFS offers: files, directories, symbolic links,
 * permissions and renames. Nodes stay in the tree while linked; an unlinked
 * node is freed when its last reference goes away.
 */

#include "fs/vfs/vfs.h"

#include "core/string.h"
#include "memory/heap.h"

typedef struct ram_node {
    vnode_t          vnode;
    struct ram_node *parent;
    list_t           children;
    list_node_t      sibling;
    char             name[VFS_NAME_MAX + 1];
    size_t           name_length;
    uint8_t         *data;      /* file contents or symlink target */
    uint64_t         capacity;
    bool             unlinked;
} ram_node_t;

static uint64_t next_inode = 1;
static const vnode_ops_t ops;

static ram_node_t *node_of(vnode_t *v)
{
    return container_of(v, ram_node_t, vnode);
}

static void free_node(ram_node_t *n)
{
    kfree(n->data);
    kfree(n);
}

static ram_node_t *new_node(filesystem_t *fs, vnode_type_t type, uint32_t mode)
{
    ram_node_t *n = kcalloc(1, sizeof(*n));
    if (!n)
        return NULL;
    vnode_init(&n->vnode, fs, type, &ops);
    n->vnode.mode = mode;
    n->vnode.inode = next_inode++;
    list_init(&n->children);
    return n;
}

static ram_node_t *find_child(ram_node_t *dir, const char *name, size_t length)
{
    list_for_each(node, &dir->children) {
        ram_node_t *child = container_of(node, ram_node_t, sibling);
        if (child->name_length == length && memcmp(child->name, name, length) == 0)
            return child;
    }
    return NULL;
}

static void link_child(ram_node_t *dir, ram_node_t *child, const char *name, size_t length)
{
    memcpy(child->name, name, length);
    child->name[length] = '\0';
    child->name_length = length;
    child->parent = dir;
    list_push_back(&dir->children, &child->sibling);
}

static status_t reserve(ram_node_t *n, uint64_t size)
{
    if (size <= n->capacity)
        return STATUS_SUCCESS;

    uint64_t capacity = n->capacity ? n->capacity : 64;
    while (capacity < size)
        capacity *= 2;
    uint8_t *data = krealloc(n->data, capacity);
    if (!data)
        return STATUS_NO_SPACE;
    memset(data + n->capacity, 0, capacity - n->capacity);
    n->data = data;
    n->capacity = capacity;
    return STATUS_SUCCESS;
}

/* --- Operations ---------------------------------------------------------------- */

static status_t ram_lookup(vnode_t *dir, const char *name, size_t length, vnode_t **result)
{
    ram_node_t *child = find_child(node_of(dir), name, length);
    if (!child)
        return STATUS_NOT_FOUND;
    vnode_retain(&child->vnode);
    *result = &child->vnode;
    return STATUS_SUCCESS;
}

static status_t ram_create(vnode_t *dir, const char *name, size_t length, vnode_type_t type, uint32_t mode,
                           vnode_t **result)
{
    if (find_child(node_of(dir), name, length))
        return STATUS_ALREADY_EXISTS;

    ram_node_t *n = new_node(dir->fs, type, mode);
    if (!n)
        return STATUS_NO_SPACE;
    link_child(node_of(dir), n, name, length);
    *result = &n->vnode;
    return STATUS_SUCCESS;
}

static status_t ram_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    *done = 0;
    if (offset >= v->size)
        return STATUS_SUCCESS;
    if (size > v->size - offset)
        size = v->size - offset;
    memcpy(buffer, node_of(v)->data + offset, size);
    *done = size;
    return STATUS_SUCCESS;
}

static status_t ram_write(vnode_t *v, uint64_t offset, const void *buffer, size_t size, size_t *done)
{
    ram_node_t *n = node_of(v);
    *done = 0;

    if (offset + size < offset)
        return STATUS_INVALID_ARGUMENT;
    status_t status = reserve(n, offset + size);
    if (STATUS_IS_ERROR(status))
        return status;
    memcpy(n->data + offset, buffer, size);
    if (offset + size > v->size)
        v->size = offset + size;
    *done = size;
    return STATUS_SUCCESS;
}

static status_t ram_truncate(vnode_t *v, uint64_t size)
{
    ram_node_t *n = node_of(v);

    if (size > v->size) {
        status_t status = reserve(n, size);
        if (STATUS_IS_ERROR(status))
            return status;
    } else if (n->data) {
        memset(n->data + size, 0, v->size - size);
    }
    v->size = size;
    return STATUS_SUCCESS;
}

static status_t ram_unlink(vnode_t *dir, const char *name, size_t length)
{
    ram_node_t *child = find_child(node_of(dir), name, length);
    if (!child)
        return STATUS_NOT_FOUND;
    if (!list_empty(&child->children))
        return STATUS_NOT_EMPTY;

    list_remove(&child->sibling);
    child->parent = NULL;
    child->unlinked = true;
    if (child->vnode.refs == 0)
        free_node(child);
    return STATUS_SUCCESS;
}

static status_t ram_rename(vnode_t *from_dir, const char *from, size_t from_length,
                           vnode_t *to_dir, const char *to, size_t to_length)
{
    ram_node_t *n = find_child(node_of(from_dir), from, from_length);
    if (!n)
        return STATUS_NOT_FOUND;
    list_remove(&n->sibling);
    link_child(node_of(to_dir), n, to, to_length);
    return STATUS_SUCCESS;
}

static status_t ram_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *entry)
{
    uint64_t index = 0;

    list_for_each(node, &node_of(dir)->children) {
        if (index++ != *cookie)
            continue;
        ram_node_t *child = container_of(node, ram_node_t, sibling);
        entry->type = child->vnode.type;
        entry->inode = child->vnode.inode;
        entry->name_length = (uint32_t)child->name_length;
        memcpy(entry->name, child->name, child->name_length + 1);
        (*cookie)++;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}

static status_t ram_symlink(vnode_t *dir, const char *name, size_t length, const char *target, size_t target_length)
{
    vnode_t *v;
    status_t status = ram_create(dir, name, length, VNODE_SYMLINK, 0777, &v);
    if (STATUS_IS_ERROR(status))
        return status;

    size_t done;
    status = ram_write(v, 0, target, target_length, &done);
    vnode_release(v);
    return status;
}

static status_t ram_readlink(vnode_t *v, char *buffer, size_t size, size_t *length)
{
    if (v->size > size)
        return STATUS_BUFFER_TOO_SMALL;
    memcpy(buffer, node_of(v)->data, v->size);
    *length = v->size;
    return STATUS_SUCCESS;
}

static void ram_release(vnode_t *v)
{
    ram_node_t *n = node_of(v);
    if (n->unlinked)
        free_node(n); /* linked nodes stay in the tree without references */
}

static const vnode_ops_t ops = {
    .lookup = ram_lookup,
    .create = ram_create,
    .read = ram_read,
    .write = ram_write,
    .truncate = ram_truncate,
    .unlink = ram_unlink,
    .rename = ram_rename,
    .readdir = ram_readdir,
    .symlink = ram_symlink,
    .readlink = ram_readlink,
    .release = ram_release,
};

filesystem_t *ramfs_create(void)
{
    filesystem_t *fs = kcalloc(1, sizeof(*fs));
    if (!fs)
        return NULL;

    ram_node_t *root = new_node(fs, VNODE_DIRECTORY, 0755);
    if (!root) {
        kfree(fs);
        return NULL;
    }
    fs->root = &root->vnode;
    return fs;
}
