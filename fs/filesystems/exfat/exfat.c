/*
 * exFAT file system, read-only (README section 26).
 *
 * exFAT is what large removable disks and many data partitions are
 * formatted with. Layout: a boot sector, one file allocation table, and a
 * cluster heap. A file is described by a set of 32-byte directory entries:
 * a File entry (attributes), a Stream Extension (first cluster, sizes, and
 * whether the clusters are contiguous so the table is not needed) and File
 * Name entries with 15 UTF-16 characters each.
 *
 * This driver reads: lookup (ASCII letters compare without case), directory
 * listing, file contents with both kinds of cluster chains. Names are
 * converted to UTF-8. It does not write, and it does not check the boot
 * checksum or the up-case table.
 */

#include "fs/vfs/vfs.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define SECTOR_SIZE 512
#define ENTRY_SIZE  32

#define ENTRY_END         0x00
#define ENTRY_LABEL       0x83
#define ENTRY_FILE        0x85
#define ENTRY_STREAM      0xC0
#define ENTRY_NAME        0xC1

#define ATTR_READ_ONLY    0x01
#define ATTR_DIRECTORY    0x10
#define STREAM_CONTIGUOUS 0x02 /* "NoFatChain": the clusters follow each other */

#define CLUSTER_END       0xFFFFFFF8u /* this and above: no further cluster */
#define DIRECTORY_MAX     (256u * 1024 * 1024) /* the format's limit for a directory */

typedef struct {
    block_device_t *device;
    uint32_t        fat_sector;       /* first sector of the allocation table */
    uint32_t        heap_sector;      /* sector of cluster 2 */
    uint32_t        cluster_count;
    uint32_t        cluster_shift;    /* sectors per cluster = 1 << shift */
    uint32_t        root_cluster;
    uint8_t        *sector;           /* cache of one metadata sector */
    uint64_t        sector_lba;       /* UINT64_MAX: nothing cached */
    list_t          nodes;            /* live non-root vnodes */
} exfat_t;

typedef struct {
    vnode_t     vnode;
    exfat_t    *fs;
    uint32_t    first_cluster;
    bool        contiguous;
    bool        is_root;
    uint64_t    valid_size;    /* bytes that were written; the rest up to size reads as zeros */
    uint32_t    dir_cluster;   /* identity: containing directory and entry index */
    uint32_t    entry_index;
    /* Where the last lookup in the cluster chain ended: sequential reads continue from there. */
    uint64_t    walk_index;
    uint32_t    walk_cluster;
    list_node_t cache_node;
} exfat_node_t;

/* One file as its directory entries describe it. */
typedef struct {
    uint32_t index;            /* of the File entry */
    uint32_t next;             /* index after the entry set */
    uint16_t attributes;
    bool     contiguous;
    uint32_t first_cluster;
    uint64_t size, valid_size;
    char     name[VFS_NAME_MAX + 1];
    size_t   length;
} exfat_dirent_t;

static const vnode_ops_t ops;

static exfat_node_t *node_of(vnode_t *v)
{
    return container_of(v, exfat_node_t, vnode);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* --- Clusters -------------------------------------------------------------------- */

static bool valid_cluster(const exfat_t *f, uint32_t cluster)
{
    return cluster >= 2 && cluster - 2 < f->cluster_count;
}

static uint64_t cluster_lba(const exfat_t *f, uint32_t cluster)
{
    return f->heap_sector + ((uint64_t)(cluster - 2) << f->cluster_shift);
}

static status_t metadata_sector(exfat_t *f, uint64_t lba)
{
    if (f->sector_lba == lba)
        return STATUS_SUCCESS;
    f->sector_lba = UINT64_MAX;
    status_t status = block_read(f->device, lba, 1, f->sector);
    if (!STATUS_IS_ERROR(status))
        f->sector_lba = lba;
    return status;
}

static status_t next_cluster(exfat_t *f, uint32_t cluster, uint32_t *next)
{
    status_t status = metadata_sector(f, f->fat_sector + (uint64_t)cluster * 4 / SECTOR_SIZE);
    if (STATUS_IS_ERROR(status))
        return status;
    *next = le32(f->sector + (cluster * 4) % SECTOR_SIZE);
    return STATUS_SUCCESS;
}

/* The cluster holding cluster number `index` of the node's data. NOT_FOUND past the end of the chain. */
static status_t cluster_at(exfat_node_t *n, uint64_t index, uint32_t *result)
{
    exfat_t *f = n->fs;

    if (n->contiguous) {
        uint64_t cluster = (uint64_t)n->first_cluster + index;
        if (cluster > UINT32_MAX || !valid_cluster(f, (uint32_t)cluster))
            return STATUS_NOT_FOUND;
        *result = (uint32_t)cluster;
        return STATUS_SUCCESS;
    }
    if (!n->walk_cluster || index < n->walk_index) {
        n->walk_index = 0;
        n->walk_cluster = n->first_cluster;
    }
    while (n->walk_index < index) {
        uint32_t next;
        if (!valid_cluster(f, n->walk_cluster))
            return STATUS_NOT_FOUND;
        status_t status = next_cluster(f, n->walk_cluster, &next);
        if (STATUS_IS_ERROR(status))
            return status;
        if (next >= CLUSTER_END || !valid_cluster(f, next)) {
            n->walk_cluster = 0; /* start over next time */
            return STATUS_NOT_FOUND;
        }
        n->walk_cluster = next;
        n->walk_index++;
    }
    if (!valid_cluster(f, n->walk_cluster))
        return STATUS_NOT_FOUND;
    *result = n->walk_cluster;
    return STATUS_SUCCESS;
}

/* --- Directories ------------------------------------------------------------------- */

/* Directory entry `index` of a directory; NOT_FOUND past its end. */
static status_t read_entry(exfat_node_t *dir, uint32_t index, uint8_t *entry)
{
    exfat_t *f = dir->fs;
    uint64_t offset = (uint64_t)index * ENTRY_SIZE, cluster_bytes = (uint64_t)SECTOR_SIZE << f->cluster_shift;
    uint32_t cluster;

    if (offset >= DIRECTORY_MAX || (!dir->is_root && offset >= dir->vnode.size))
        return STATUS_NOT_FOUND;
    status_t status = cluster_at(dir, offset / cluster_bytes, &cluster);
    if (!STATUS_IS_ERROR(status))
        status = metadata_sector(f, cluster_lba(f, cluster) + (offset % cluster_bytes) / SECTOR_SIZE);
    if (STATUS_IS_ERROR(status))
        return status;
    memcpy(entry, f->sector + offset % SECTOR_SIZE, ENTRY_SIZE);
    return STATUS_SUCCESS;
}

/* Append a UTF-16 code unit sequence position as UTF-8. Returns false if the name gets too long. */
static bool append_utf8(exfat_dirent_t *out, uint32_t code_point)
{
    char bytes[4];
    size_t n;

    if (code_point < 0x80) {
        bytes[0] = (char)code_point;
        n = 1;
    } else if (code_point < 0x800) {
        bytes[0] = (char)(0xC0 | code_point >> 6);
        bytes[1] = (char)(0x80 | (code_point & 0x3F));
        n = 2;
    } else if (code_point < 0x10000) {
        bytes[0] = (char)(0xE0 | code_point >> 12);
        bytes[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (code_point & 0x3F));
        n = 3;
    } else {
        bytes[0] = (char)(0xF0 | code_point >> 18);
        bytes[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (code_point & 0x3F));
        n = 4;
    }
    if (out->length + n > VFS_NAME_MAX)
        return false;
    memcpy(out->name + out->length, bytes, n);
    out->length += n;
    return true;
}

/*
 * The next file at or after *index. Entry sets that are damaged, or whose
 * name does not fit into a VFS name, are skipped. NOT_FOUND at the end.
 */
static status_t next_dirent(exfat_node_t *dir, uint32_t *index, exfat_dirent_t *out)
{
    uint8_t entry[ENTRY_SIZE];

    for (;;) {
        status_t status = read_entry(dir, *index, entry);
        if (STATUS_IS_ERROR(status))
            return status;
        if (entry[0] == ENTRY_END)
            return STATUS_NOT_FOUND;
        if (entry[0] != ENTRY_FILE) {
            (*index)++;
            continue;
        }
        uint32_t secondaries = entry[1], first = *index;
        out->index = first;
        out->next = first + 1 + secondaries;
        out->attributes = le16(entry + 4);
        out->length = 0;
        *index = out->next;
        if (secondaries < 2 || STATUS_IS_ERROR(read_entry(dir, first + 1, entry)) || entry[0] != ENTRY_STREAM)
            continue;
        out->contiguous = entry[1] & STREAM_CONTIGUOUS;
        uint32_t name_units = entry[3];
        out->valid_size = le64(entry + 8);
        out->first_cluster = le32(entry + 20);
        out->size = le64(entry + 24);
        if (out->valid_size > out->size)
            out->valid_size = out->size;

        bool ok = name_units > 0;
        uint32_t high = 0; /* pending high surrogate */
        for (uint32_t unit = 0; ok && unit < name_units; unit++) {
            if (unit % 15 == 0) {
                uint32_t part = 2 + unit / 15;
                ok = part <= secondaries && !STATUS_IS_ERROR(read_entry(dir, first + part, entry)) &&
                     entry[0] == ENTRY_NAME;
                if (!ok)
                    break;
            }
            uint32_t c = le16(entry + 2 + 2 * (unit % 15));
            if (c >= 0xD800 && c < 0xDC00) {
                high = c;
                continue;
            }
            if (c >= 0xDC00 && c < 0xE000 && high)
                c = 0x10000 + ((high - 0xD800) << 10) + (c - 0xDC00);
            high = 0;
            ok = c != 0 && c != '/' && append_utf8(out, c);
        }
        if (!ok)
            continue;
        out->name[out->length] = '\0';
        return STATUS_SUCCESS;
    }
}

/* Names compare without case for ASCII letters (the up-case table would cover all of Unicode). */
static bool same_name(const char *a, size_t a_length, const char *b, size_t b_length)
{
    if (a_length != b_length)
        return false;
    for (size_t i = 0; i < a_length; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z')
            x = (char)(x - 32);
        if (y >= 'a' && y <= 'z')
            y = (char)(y - 32);
        if (x != y)
            return false;
    }
    return true;
}

/* --- Vnodes ---------------------------------------------------------------------- */

static status_t get_node(filesystem_t *fs, exfat_node_t *dir, const exfat_dirent_t *d, vnode_t **result)
{
    exfat_t *f = fs->data;

    list_for_each(node, &f->nodes) {
        exfat_node_t *n = container_of(node, exfat_node_t, cache_node);
        if (n->dir_cluster == dir->first_cluster && n->entry_index == d->index) {
            vnode_retain(&n->vnode);
            *result = &n->vnode;
            return STATUS_SUCCESS;
        }
    }
    exfat_node_t *n = kcalloc(1, sizeof(*n));
    if (!n)
        return STATUS_OUT_OF_MEMORY;
    bool directory = d->attributes & ATTR_DIRECTORY;
    vnode_init(&n->vnode, fs, directory ? VNODE_DIRECTORY : VNODE_FILE, &ops);
    n->fs = f;
    n->first_cluster = d->first_cluster;
    n->contiguous = d->contiguous;
    n->valid_size = d->valid_size;
    n->dir_cluster = dir->first_cluster;
    n->entry_index = d->index;
    n->vnode.size = d->size;
    n->vnode.inode = ((uint64_t)dir->first_cluster << 32) | d->index;
    n->vnode.mode = directory ? 0555 : 0444; /* read-only file system */
    list_push_back(&f->nodes, &n->cache_node);
    *result = &n->vnode;
    return STATUS_SUCCESS;
}

static status_t exfat_lookup(vnode_t *dir, const char *name, size_t length, vnode_t **result)
{
    exfat_node_t *d = node_of(dir);
    exfat_dirent_t *entry = kmalloc(sizeof(*entry));
    uint32_t index = 0;

    if (!entry)
        return STATUS_OUT_OF_MEMORY;
    status_t status;
    while ((status = next_dirent(d, &index, entry)) == STATUS_SUCCESS) {
        if (same_name(entry->name, entry->length, name, length)) {
            status = get_node(dir->fs, d, entry, result);
            break;
        }
    }
    kfree(entry);
    return status;
}

static status_t exfat_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out)
{
    exfat_node_t *d = node_of(dir);
    exfat_dirent_t *entry = kmalloc(sizeof(*entry));
    uint32_t index = (uint32_t)*cookie;

    if (!entry)
        return STATUS_OUT_OF_MEMORY;
    status_t status = next_dirent(d, &index, entry);
    if (!STATUS_IS_ERROR(status)) {
        *cookie = index;
        out->type = (entry->attributes & ATTR_DIRECTORY) ? VNODE_DIRECTORY : VNODE_FILE;
        out->inode = ((uint64_t)d->first_cluster << 32) | entry->index;
        out->name_length = (uint32_t)entry->length;
        memcpy(out->name, entry->name, entry->length + 1);
    }
    kfree(entry);
    return status;
}

static status_t exfat_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    exfat_node_t *n = node_of(v);
    exfat_t *f = n->fs;
    uint64_t cluster_bytes = (uint64_t)SECTOR_SIZE << f->cluster_shift;
    uint8_t *out = buffer;

    *done = 0;
    if (v->type != VNODE_FILE)
        return STATUS_IS_DIRECTORY;
    if (offset >= v->size)
        return STATUS_SUCCESS;
    if (size > v->size - offset)
        size = (size_t)(v->size - offset);

    while (*done < size) {
        uint64_t position = offset + *done;
        size_t left = size - *done;

        /* Allocated but never written: zeros. */
        if (position >= n->valid_size) {
            memset(out + *done, 0, left);
            *done = size;
            break;
        }
        if (left > n->valid_size - position)
            left = (size_t)(n->valid_size - position);

        uint32_t cluster;
        status_t status = cluster_at(n, position / cluster_bytes, &cluster);
        if (STATUS_IS_ERROR(status))
            return *done ? STATUS_SUCCESS : STATUS_IO_ERROR; /* the chain is shorter than the size says */
        uint64_t in_cluster = position % cluster_bytes;
        uint64_t lba = cluster_lba(f, cluster) + in_cluster / SECTOR_SIZE;
        uint32_t in_sector = (uint32_t)(in_cluster % SECTOR_SIZE);
        size_t n_bytes;

        if (in_sector || left < SECTOR_SIZE) {
            /* A partial sector goes through the scratch sector. */
            status = metadata_sector(f, lba);
            n_bytes = SECTOR_SIZE - in_sector < left ? SECTOR_SIZE - in_sector : left;
            if (!STATUS_IS_ERROR(status))
                memcpy(out + *done, f->sector + in_sector, n_bytes);
        } else {
            /* Whole sectors straight into the caller's buffer, up to the end of the cluster. */
            uint64_t sectors = left / SECTOR_SIZE, in_this_cluster = (cluster_bytes - in_cluster) / SECTOR_SIZE;
            if (sectors > in_this_cluster)
                sectors = in_this_cluster;
            if (sectors > 2048)
                sectors = 2048;
            status = block_read(f->device, lba, (uint32_t)sectors, out + *done);
            n_bytes = (size_t)sectors * SECTOR_SIZE;
        }
        if (STATUS_IS_ERROR(status))
            return *done ? STATUS_SUCCESS : status;
        *done += n_bytes;
    }
    return STATUS_SUCCESS;
}

static void exfat_release(vnode_t *v)
{
    exfat_node_t *n = node_of(v);
    if (n->is_root)
        return; /* freed by unmount */
    list_remove(&n->cache_node);
    kfree(n);
}

static const vnode_ops_t ops = {
    .lookup = exfat_lookup,
    .read = exfat_read,
    .readdir = exfat_readdir,
    .release = exfat_release,
};

/* --- Mounting -------------------------------------------------------------------- */

static void read_label(exfat_node_t *root, char *label, size_t size)
{
    uint8_t entry[ENTRY_SIZE];

    label[0] = '\0';
    for (uint32_t index = 0; index < 64; index++) {
        if (STATUS_IS_ERROR(read_entry(root, index, entry)) || entry[0] == ENTRY_END)
            return;
        if (entry[0] != ENTRY_LABEL)
            continue;
        size_t n = 0;
        for (uint32_t i = 0; i < entry[1] && i < 11 && n + 1 < size; i++) {
            uint16_t c = le16(entry + 2 + 2 * i);
            label[n++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
        }
        label[n] = '\0';
        return;
    }
}

static status_t exfat_mount(block_device_t *device, filesystem_t **result)
{
    uint8_t boot[SECTOR_SIZE];
    char label[16];

    if (!device || device->sector_size != SECTOR_SIZE ||
        STATUS_IS_ERROR(block_read_bytes(device, 0, sizeof(boot), boot)))
        return STATUS_NOT_SUPPORTED;
    if (memcmp(boot + 3, "EXFAT   ", 8) != 0 || le16(boot + 510) != 0xAA55)
        return STATUS_NOT_SUPPORTED;

    uint32_t fat_sector = le32(boot + 80), fat_length = le32(boot + 84), heap_sector = le32(boot + 88);
    uint32_t cluster_count = le32(boot + 92), root_cluster = le32(boot + 96);
    uint32_t sector_shift = boot[108], cluster_shift = boot[109];

    if (sector_shift != 9) {
        klog_warn("exfat: %s: sectors of %u bytes are not supported", device->name, 1u << (sector_shift & 31));
        return STATUS_NOT_SUPPORTED;
    }
    if (cluster_shift > 16 || !fat_sector || !fat_length || heap_sector < fat_sector + fat_length ||
        !cluster_count || heap_sector + ((uint64_t)cluster_count << cluster_shift) > device->sector_count ||
        (uint64_t)fat_length * SECTOR_SIZE / 4 < (uint64_t)cluster_count + 2 || root_cluster < 2 ||
        root_cluster - 2 >= cluster_count) {
        klog_warn("exfat: %s: inconsistent boot sector, not mounted", device->name);
        return STATUS_NOT_SUPPORTED;
    }

    filesystem_t *fs = kcalloc(1, sizeof(*fs));
    exfat_t *f = kcalloc(1, sizeof(*f));
    exfat_node_t *root = kcalloc(1, sizeof(*root));
    if (f)
        f->sector = kmalloc(SECTOR_SIZE);
    if (!fs || !f || !root || !f->sector) {
        if (f)
            kfree(f->sector);
        kfree(fs);
        kfree(f);
        kfree(root);
        return STATUS_OUT_OF_MEMORY;
    }
    f->device = device;
    f->fat_sector = fat_sector;
    f->heap_sector = heap_sector;
    f->cluster_count = cluster_count;
    f->cluster_shift = cluster_shift;
    f->root_cluster = root_cluster;
    f->sector_lba = UINT64_MAX;
    list_init(&f->nodes);
    fs->data = f;

    vnode_init(&root->vnode, fs, VNODE_DIRECTORY, &ops);
    root->fs = f;
    root->is_root = true;
    root->first_cluster = root_cluster;
    root->vnode.inode = root_cluster;
    root->vnode.mode = 0555;
    fs->root = &root->vnode;

    read_label(root, label, sizeof(label));
    klog_info("exfat: %s: exFAT \"%s\", %u clusters of %u KiB, read-only", device->name, label, cluster_count,
              (SECTOR_SIZE << cluster_shift) / 1024);
    *result = fs;
    return STATUS_SUCCESS;
}

static status_t exfat_unmount(filesystem_t *fs)
{
    exfat_t *f = fs->data;

    if (!list_empty(&f->nodes))
        return STATUS_BUSY;
    kfree(node_of(fs->root));
    kfree(f->sector);
    kfree(f);
    kfree(fs);
    return STATUS_SUCCESS;
}

fs_type_t exfat_type = {
    .name = "exfat",
    .mount = exfat_mount,
    .unmount = exfat_unmount,
};
