/*
 * FAT32 file system (README section 26: "Boot should initially use FAT32").
 *
 * Read and write support with long file names (VFAT). Writes go straight to
 * the device (write-through). Live vnodes are cached by the position of
 * their directory entry, so every user of a file sees the same size and
 * clusters. FAT12/16 volumes are not supported.
 *
 * FAT has no owners or permissions: everything belongs to root, files are
 * 0644 (0444 with the read-only attribute) and directories 0755.
 */

#include "fs/vfs/vfs.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define ATTR_READ_ONLY    0x01
#define ATTR_VOLUME_ID    0x08
#define ATTR_DIRECTORY    0x10
#define ATTR_ARCHIVE      0x20
#define ATTR_LFN          0x0F
#define NT_LOWER_BASE     0x08
#define NT_LOWER_EXT      0x10

#define ENTRY_SIZE        32
#define ENTRY_FREE        0xE5
#define ENTRY_END         0x00
#define LFN_LAST          0x40
#define LFN_CHARS         13

#define FAT_MASK          0x0FFFFFFFu
#define FAT_EOC           0x0FFFFFFFu
#define FAT_EOC_MIN       0x0FFFFFF8u
#define FAT32_MIN_CLUSTERS 65525u
#define FSINFO_FREE_COUNT 488
#define FIXED_DATE        (((2026 - 1980) << 9) | (1 << 5) | 1) /* no wall clock yet */
#define MAX_FILE_SIZE     0xFFFFFFFFULL

typedef struct __attribute__((packed)) {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_flags;
    uint8_t  create_tenths;
    uint16_t create_time, create_date, access_date;
    uint16_t cluster_high;
    uint16_t write_time, write_date;
    uint16_t cluster_low;
    uint32_t size;
} dir_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} lfn_entry_t;

typedef struct {
    block_device_t *device;
    uint32_t        sector_size;
    uint32_t        cluster_sectors;
    uint32_t        cluster_size;
    uint32_t        reserved_sectors;
    uint32_t        fat_count;
    uint32_t        fat_sectors;
    uint64_t        data_start;      /* first data sector */
    uint32_t        cluster_count;
    uint32_t        root_cluster;
    uint32_t        fsinfo_sector;
    uint32_t        free_hint;
    bool            fsinfo_stale;
    uint8_t        *sector;          /* scratch sector */
    uint8_t        *cluster;         /* scratch cluster */
    list_t          nodes;           /* live non-root vnodes */
} fat_t;

typedef struct {
    vnode_t     vnode;
    fat_t      *fat;
    uint32_t    first_cluster;
    uint32_t    dir_cluster;   /* first cluster of the containing directory */
    uint32_t    entry_index;   /* index of the short entry in that directory */
    bool        is_root;
    list_node_t cache_node;
} fat_node_t;

/* A directory entry with its decoded name. */
typedef struct {
    dir_entry_t entry;
    uint32_t    index;         /* short entry */
    uint32_t    first_index;   /* first LFN entry (== index without LFN) */
    char        name[VFS_NAME_MAX + 1];
    size_t      length;
} fat_dirent_t;

static const vnode_ops_t ops;

static fat_node_t *node_of(vnode_t *v)
{
    return container_of(v, fat_node_t, vnode);
}

/* --- Clusters and the allocation table ------------------------------------------- */

static bool valid_cluster(const fat_t *f, uint32_t c)
{
    return c >= 2 && c < f->cluster_count + 2;
}

static uint64_t cluster_lba(const fat_t *f, uint32_t c)
{
    return f->data_start + (uint64_t)(c - 2) * f->cluster_sectors;
}

static status_t read_cluster(fat_t *f, uint32_t c, void *buffer)
{
    return block_read(f->device, cluster_lba(f, c), f->cluster_sectors, buffer);
}

static status_t write_cluster(fat_t *f, uint32_t c, const void *buffer)
{
    return block_write(f->device, cluster_lba(f, c), f->cluster_sectors, buffer);
}

static status_t fat_get(fat_t *f, uint32_t c, uint32_t *value)
{
    uint64_t offset = (uint64_t)c * 4;
    status_t status = block_read(f->device, f->reserved_sectors + offset / f->sector_size, 1, f->sector);
    if (!STATUS_IS_ERROR(status)) {
        uint32_t raw;
        memcpy(&raw, f->sector + offset % f->sector_size, 4);
        *value = raw & FAT_MASK;
    }
    return status;
}

/* Update every copy of the table; the top four bits are reserved and kept. */
static status_t fat_set(fat_t *f, uint32_t c, uint32_t value)
{
    uint64_t offset = (uint64_t)c * 4;

    for (uint32_t copy = 0; copy < f->fat_count; copy++) {
        uint64_t lba = f->reserved_sectors + (uint64_t)copy * f->fat_sectors + offset / f->sector_size;
        status_t status = block_read(f->device, lba, 1, f->sector);
        if (STATUS_IS_ERROR(status))
            return status;
        uint32_t raw;
        memcpy(&raw, f->sector + offset % f->sector_size, 4);
        raw = (raw & ~FAT_MASK) | (value & FAT_MASK);
        memcpy(f->sector + offset % f->sector_size, &raw, 4);
        status = block_write(f->device, lba, 1, f->sector);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    return STATUS_SUCCESS;
}

/* The free count in FSInfo becomes unknown (0xFFFFFFFF) once we allocate or free. */
static void invalidate_fsinfo(fat_t *f)
{
    if (f->fsinfo_stale || !f->fsinfo_sector)
        return;
    if (block_read(f->device, f->fsinfo_sector, 1, f->sector) == STATUS_SUCCESS) {
        memset(f->sector + FSINFO_FREE_COUNT, 0xFF, 4);
        block_write(f->device, f->fsinfo_sector, 1, f->sector);
    }
    f->fsinfo_stale = true;
}

static status_t alloc_cluster(fat_t *f, uint32_t previous, uint32_t *result)
{
    for (uint32_t n = 0; n < f->cluster_count; n++) {
        uint32_t c = 2 + (f->free_hint - 2 + n) % f->cluster_count;
        uint32_t value;
        status_t status = fat_get(f, c, &value);
        if (STATUS_IS_ERROR(status))
            return status;
        if (value != 0)
            continue;

        invalidate_fsinfo(f);
        memset(f->cluster, 0, f->cluster_size);
        status = write_cluster(f, c, f->cluster);
        if (!STATUS_IS_ERROR(status))
            status = fat_set(f, c, FAT_EOC);
        if (!STATUS_IS_ERROR(status) && previous)
            status = fat_set(f, previous, c);
        if (STATUS_IS_ERROR(status))
            return status;
        f->free_hint = c + 1 < f->cluster_count + 2 ? c + 1 : 2;
        *result = c;
        return STATUS_SUCCESS;
    }
    return STATUS_NO_SPACE;
}

static status_t free_chain(fat_t *f, uint32_t c)
{
    invalidate_fsinfo(f);
    while (valid_cluster(f, c)) {
        uint32_t next;
        status_t status = fat_get(f, c, &next);
        if (!STATUS_IS_ERROR(status))
            status = fat_set(f, c, 0);
        if (STATUS_IS_ERROR(status))
            return status;
        c = next;
    }
    return STATUS_SUCCESS;
}

/* The index-th cluster of a chain. */
static status_t chain_at(fat_t *f, uint32_t first, uint64_t index, uint32_t *result)
{
    uint32_t c = first;
    for (uint64_t i = 0; i < index; i++) {
        status_t status = fat_get(f, c, &c);
        if (STATUS_IS_ERROR(status))
            return status;
        if (!valid_cluster(f, c))
            return STATUS_NOT_FOUND;
    }
    if (!valid_cluster(f, c))
        return STATUS_NOT_FOUND;
    *result = c;
    return STATUS_SUCCESS;
}

/* Last cluster of a chain and its length. */
static status_t chain_end(fat_t *f, uint32_t first, uint32_t *last, uint64_t *length)
{
    uint32_t c = first;
    *length = 0;
    while (valid_cluster(f, c)) {
        uint32_t next;
        status_t status = fat_get(f, c, &next);
        if (STATUS_IS_ERROR(status))
            return status;
        (*length)++;
        *last = c;
        if (next >= FAT_EOC_MIN || !valid_cluster(f, next))
            break;
        c = next;
    }
    return STATUS_SUCCESS;
}

/* --- Directory entries --------------------------------------------------------------- */

static status_t entry_location(fat_t *f, uint32_t dir_first, uint32_t index, uint64_t *lba, uint32_t *offset)
{
    uint64_t byte = (uint64_t)index * ENTRY_SIZE;
    uint32_t c;
    status_t status = chain_at(f, dir_first, byte / f->cluster_size, &c);
    if (STATUS_IS_ERROR(status))
        return status;
    *lba = cluster_lba(f, c) + (byte % f->cluster_size) / f->sector_size;
    *offset = (byte % f->cluster_size) % f->sector_size;
    return STATUS_SUCCESS;
}

static status_t read_entry(fat_t *f, uint32_t dir_first, uint32_t index, dir_entry_t *entry)
{
    uint64_t lba;
    uint32_t offset;
    status_t status = entry_location(f, dir_first, index, &lba, &offset);
    if (!STATUS_IS_ERROR(status))
        status = block_read(f->device, lba, 1, f->sector);
    if (!STATUS_IS_ERROR(status))
        memcpy(entry, f->sector + offset, sizeof(*entry));
    return status;
}

static status_t write_entry(fat_t *f, uint32_t dir_first, uint32_t index, const void *entry)
{
    uint64_t lba;
    uint32_t offset;
    status_t status = entry_location(f, dir_first, index, &lba, &offset);
    if (!STATUS_IS_ERROR(status))
        status = block_read(f->device, lba, 1, f->sector);
    if (!STATUS_IS_ERROR(status)) {
        memcpy(f->sector + offset, entry, ENTRY_SIZE);
        status = block_write(f->device, lba, 1, f->sector);
    }
    return status;
}

static uint32_t entry_cluster(const dir_entry_t *e)
{
    return ((uint32_t)e->cluster_high << 16) | e->cluster_low;
}

static void set_entry_cluster(dir_entry_t *e, uint32_t c)
{
    e->cluster_high = (uint16_t)(c >> 16);
    e->cluster_low = (uint16_t)c;
}

static uint8_t lfn_checksum(const uint8_t *short_name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

static void short_name_text(const dir_entry_t *e, char *out, size_t *length)
{
    size_t n = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) {
        char c = (char)e->name[i];
        out[n++] = (e->nt_flags & NT_LOWER_BASE) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e->name[i] != ' '; i++) {
            char c = (char)e->name[i];
            out[n++] = (e->nt_flags & NT_LOWER_EXT) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    if (out[0] == 0x05)
        out[0] = (char)0xE5; /* escaped first byte */
    out[n] = '\0';
    *length = n;
}

static void lfn_chars(const lfn_entry_t *l, uint16_t *chars)
{
    memcpy(chars, l->name1, sizeof(l->name1));
    memcpy(chars + 5, l->name2, sizeof(l->name2));
    memcpy(chars + 11, l->name3, sizeof(l->name3));
}

/* Iterate decoded entries starting at *index; NOT_FOUND at the end of the directory. */
static status_t next_dirent(fat_t *f, uint32_t dir_first, uint32_t *index, fat_dirent_t *out)
{
    uint16_t name[260];
    int lfn_count = 0, lfn_expected = 0;
    uint8_t lfn_sum = 0;
    uint32_t lfn_start = 0;
    uint32_t loaded = ~0u, cluster = 0;
    uint32_t per_cluster = f->cluster_size / ENTRY_SIZE;

    for (;; (*index)++) {
        uint32_t cluster_index = *index / per_cluster;
        if (cluster_index != loaded) {
            /* Follow the chain from the previous cluster; only the first one is looked up. */
            status_t status = loaded == ~0u ? chain_at(f, dir_first, cluster_index, &cluster)
                                            : fat_get(f, cluster, &cluster);
            if (status == STATUS_NOT_FOUND || (!STATUS_IS_ERROR(status) && !valid_cluster(f, cluster)))
                return STATUS_NOT_FOUND;
            if (!STATUS_IS_ERROR(status))
                status = read_cluster(f, cluster, f->cluster);
            if (STATUS_IS_ERROR(status))
                return status;
            loaded = cluster_index;
        }
        const dir_entry_t *e = (const dir_entry_t *)(f->cluster + (*index % per_cluster) * ENTRY_SIZE);

        if (e->name[0] == ENTRY_END)
            return STATUS_NOT_FOUND;
        if (e->name[0] == ENTRY_FREE) {
            lfn_count = 0;
            continue;
        }
        if (e->attr == ATTR_LFN) {
            const lfn_entry_t *l = (const lfn_entry_t *)e;
            int order = l->order & 0x1F;
            if (l->order & LFN_LAST) {
                lfn_expected = order;
                lfn_count = 0;
                lfn_sum = l->checksum;
                lfn_start = *index;
                memset(name, 0, sizeof(name));
            }
            if (order < 1 || order > 20 || order != lfn_expected - lfn_count || l->checksum != lfn_sum) {
                lfn_count = 0; /* broken sequence: fall back to the short name */
                lfn_expected = 0;
                continue;
            }
            lfn_chars(l, name + (order - 1) * LFN_CHARS);
            lfn_count++;
            continue;
        }
        if (e->attr & ATTR_VOLUME_ID) {
            lfn_count = 0;
            continue;
        }

        out->entry = *e;
        out->index = *index;
        out->first_index = *index;
        if (lfn_count && lfn_count == lfn_expected && lfn_checksum(e->name) == lfn_sum) {
            size_t n = 0;
            while (n < VFS_NAME_MAX && n < 260 && name[n] && name[n] != 0xFFFF) {
                out->name[n] = name[n] < 0x80 ? (char)name[n] : '?';
                n++;
            }
            out->name[n] = '\0';
            out->length = n;
            out->first_index = lfn_start;
        } else {
            short_name_text(e, out->name, &out->length);
        }
        (*index)++;
        return STATUS_SUCCESS;
    }
}

static bool same_name(const char *a, size_t a_length, const char *b, size_t b_length)
{
    if (a_length != b_length)
        return false;
    for (size_t i = 0; i < a_length; i++) {
        char x = a[i] >= 'a' && a[i] <= 'z' ? (char)(a[i] - 32) : a[i];
        char y = b[i] >= 'a' && b[i] <= 'z' ? (char)(b[i] - 32) : b[i];
        if (x != y)
            return false;
    }
    return true;
}

static status_t find(fat_t *f, uint32_t dir_first, const char *name, size_t length, fat_dirent_t *out)
{
    uint32_t index = 0;
    status_t status;
    while ((status = next_dirent(f, dir_first, &index, out)) == STATUS_SUCCESS) {
        if (same_name(out->name, out->length, name, length))
            return STATUS_SUCCESS;
    }
    return status;
}

/* --- Vnodes ------------------------------------------------------------------------ */

static fat_node_t *cached(fat_t *f, uint32_t dir_cluster, uint32_t index)
{
    list_for_each(node, &f->nodes) {
        fat_node_t *n = container_of(node, fat_node_t, cache_node);
        if (n->dir_cluster == dir_cluster && n->entry_index == index)
            return n;
    }
    return NULL;
}

static status_t get_node(filesystem_t *fs, uint32_t dir_cluster, const fat_dirent_t *d, vnode_t **result)
{
    fat_t *f = fs->data;
    fat_node_t *n = cached(f, dir_cluster, d->index);

    if (n) {
        vnode_retain(&n->vnode);
        *result = &n->vnode;
        return STATUS_SUCCESS;
    }
    n = kcalloc(1, sizeof(*n));
    if (!n)
        return STATUS_OUT_OF_MEMORY;

    bool directory = d->entry.attr & ATTR_DIRECTORY;
    vnode_init(&n->vnode, fs, directory ? VNODE_DIRECTORY : VNODE_FILE, &ops);
    n->fat = f;
    n->first_cluster = entry_cluster(&d->entry);
    n->dir_cluster = dir_cluster;
    n->entry_index = d->index;
    n->vnode.size = directory ? 0 : d->entry.size;
    n->vnode.inode = ((uint64_t)dir_cluster << 32) | d->index;
    n->vnode.mode = directory ? 0755 : (d->entry.attr & ATTR_READ_ONLY) ? 0444 : 0644;
    list_push_back(&f->nodes, &n->cache_node);
    *result = &n->vnode;
    return STATUS_SUCCESS;
}

/* Write size and first cluster back into the node's directory entry. */
static status_t update_entry(fat_node_t *n)
{
    dir_entry_t e;
    if (n->is_root)
        return STATUS_SUCCESS;
    status_t status = read_entry(n->fat, n->dir_cluster, n->entry_index, &e);
    if (STATUS_IS_ERROR(status))
        return status;
    set_entry_cluster(&e, n->first_cluster);
    e.size = n->vnode.type == VNODE_DIRECTORY ? 0 : (uint32_t)n->vnode.size;
    e.write_date = FIXED_DATE;
    return write_entry(n->fat, n->dir_cluster, n->entry_index, &e);
}

/* --- Creating entries ---------------------------------------------------------------- */

static bool strchr_simple(const char *set, unsigned char c)
{
    for (; *set; set++) {
        if ((unsigned char)*set == c)
            return true;
    }
    return false;
}

static bool valid_long_name(const char *name, size_t length)
{
    if (length == 0 || length > VFS_NAME_MAX || name[length - 1] == '.' || name[length - 1] == ' ')
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7F || strchr_simple("\"*/:<>?\\|", c))
            return false;
    }
    return true;
}

static bool short_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr_simple("$%'-_@~`!(){}^#&", (unsigned char)c);
}

#define SHORT_NUMBERS 1024

/* Mark the ~N numbers already used by short names that start with `prefix` and share the extension. */
static status_t used_numbers(fat_t *f, uint32_t dir_first, const char *base, size_t base_length, const char *ext,
                             size_t ext_length, uint8_t *used)
{
    fat_dirent_t d;
    uint32_t index = 0;
    status_t status;

    while ((status = next_dirent(f, dir_first, &index, &d)) == STATUS_SUCCESS) {
        const uint8_t *n = d.entry.name;
        size_t tilde = 0;
        while (tilde < 8 && n[tilde] != '~')
            tilde++;
        if (tilde == 8 || tilde > base_length || memcmp(n, base, tilde) != 0)
            continue;
        if (memcmp(n + 8, ext, ext_length) != 0 || (ext_length < 3 && n[8 + ext_length] != ' '))
            continue;
        uint32_t number = 0;
        for (size_t i = tilde + 1; i < 8 && n[i] >= '0' && n[i] <= '9'; i++)
            number = number * 10 + (n[i] - '0');
        if (number < SHORT_NUMBERS)
            used[number] = 1;
    }
    return status == STATUS_NOT_FOUND ? STATUS_SUCCESS : status;
}

/* Windows-style basis name "NAME~N.EXT" that is unique in the directory. */
static status_t make_short_name(fat_t *f, uint32_t dir_first, const char *name, size_t length, uint8_t *out)
{
    size_t dot = length;
    for (size_t i = length; i > 0; i--) {
        if (name[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }

    char base[8], ext[3];
    size_t base_length = 0, ext_length = 0;
    for (size_t i = 0; i < dot && base_length < 6; i++) {
        char c = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 32) : name[i];
        if (c == ' ' || c == '.')
            continue;
        base[base_length++] = short_char(c) ? c : '_';
    }
    for (size_t i = dot + 1; i < length && ext_length < 3; i++) {
        char c = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 32) : name[i];
        if (c == ' ')
            continue;
        ext[ext_length++] = short_char(c) ? c : '_';
    }
    if (base_length == 0)
        base[base_length++] = '_';

    uint8_t *used = kcalloc(SHORT_NUMBERS, 1);
    if (!used)
        return STATUS_OUT_OF_MEMORY;
    status_t status = used_numbers(f, dir_first, base, base_length, ext, ext_length, used);

    for (uint32_t number = 1; !STATUS_IS_ERROR(status) && number < SHORT_NUMBERS; number++) {
        if (used[number])
            continue;
        char suffix[8];
        int digits = 0;
        for (uint32_t v = number; v; v /= 10)
            suffix[digits++] = (char)('0' + v % 10);

        size_t keep = base_length;
        if (keep + 1 + (size_t)digits > 8)
            keep = 8 - 1 - (size_t)digits;

        memset(out, ' ', 11);
        memcpy(out, base, keep);
        out[keep] = '~';
        for (int i = 0; i < digits; i++)
            out[keep + 1 + i] = (uint8_t)suffix[digits - 1 - i];
        memcpy(out + 8, ext, ext_length);
        kfree(used);
        return STATUS_SUCCESS;
    }
    kfree(used);
    return STATUS_IS_ERROR(status) ? status : STATUS_NO_SPACE;
}

/* First index of `count` consecutive free entries; grows the directory if needed. */
static status_t find_free_run(fat_t *f, uint32_t dir_first, uint32_t count, uint32_t *start)
{
    uint32_t per_cluster = f->cluster_size / ENTRY_SIZE;
    uint32_t run = 0, base = 0, c = dir_first;

    for (;;) {
        status_t status = read_cluster(f, c, f->cluster);
        if (STATUS_IS_ERROR(status))
            return status;
        for (uint32_t i = 0; i < per_cluster; i++) {
            uint8_t first = f->cluster[i * ENTRY_SIZE];
            if (first != ENTRY_FREE && first != ENTRY_END) {
                run = 0;
                continue;
            }
            if (run++ == 0)
                *start = base + i;
            if (run == count)
                return STATUS_SUCCESS;
        }

        uint32_t next;
        status = fat_get(f, c, &next);
        if (STATUS_IS_ERROR(status))
            return status;
        if (next >= FAT_EOC_MIN || !valid_cluster(f, next)) {
            /* Out of entries: append a zeroed cluster to the directory. */
            status = alloc_cluster(f, c, &next);
            if (STATUS_IS_ERROR(status))
                return status;
        }
        c = next;
        base += per_cluster;
    }
}

/* Write LFN entries plus the short entry for `name`. Returns the short entry's index. */
static status_t add_entry(fat_t *f, uint32_t dir_first, const char *name, size_t length, const dir_entry_t *base,
                          uint32_t *short_index)
{
    dir_entry_t e = *base;
    e.nt_flags = 0; /* the long name carries the case */
    status_t status = make_short_name(f, dir_first, name, length, e.name);
    if (STATUS_IS_ERROR(status))
        return status;

    uint32_t lfn_count = (uint32_t)((length + LFN_CHARS - 1) / LFN_CHARS);
    uint32_t start;
    status = find_free_run(f, dir_first, lfn_count + 1, &start);
    if (STATUS_IS_ERROR(status))
        return status;

    uint8_t sum = lfn_checksum(e.name);
    for (uint32_t i = 0; i < lfn_count; i++) {
        uint32_t order = lfn_count - i; /* stored last part first */
        uint16_t chars[LFN_CHARS];
        for (uint32_t j = 0; j < LFN_CHARS; j++) {
            size_t pos = (order - 1) * LFN_CHARS + j;
            chars[j] = pos < length ? (uint16_t)(unsigned char)name[pos] : pos == length ? 0 : 0xFFFF;
        }
        lfn_entry_t l = { 0 };
        l.order = (uint8_t)(order | (i == 0 ? LFN_LAST : 0));
        l.attr = ATTR_LFN;
        l.checksum = sum;
        memcpy(l.name1, chars, sizeof(l.name1));
        memcpy(l.name2, chars + 5, sizeof(l.name2));
        memcpy(l.name3, chars + 11, sizeof(l.name3));
        status = write_entry(f, dir_first, start + i, &l);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    *short_index = start + lfn_count;
    return write_entry(f, dir_first, *short_index, &e);
}

static status_t remove_entries(fat_t *f, uint32_t dir_first, uint32_t first, uint32_t last)
{
    for (uint32_t index = first; index <= last; index++) {
        dir_entry_t e;
        status_t status = read_entry(f, dir_first, index, &e);
        if (!STATUS_IS_ERROR(status)) {
            e.name[0] = ENTRY_FREE;
            status = write_entry(f, dir_first, index, &e);
        }
        if (STATUS_IS_ERROR(status))
            return status;
    }
    return STATUS_SUCCESS;
}

/* "." and ".." for a new directory; ".." uses 0 for the root directory. */
static status_t init_directory(fat_t *f, uint32_t cluster, uint32_t parent)
{
    dir_entry_t dot = { 0 };
    memset(dot.name, ' ', 11);
    dot.attr = ATTR_DIRECTORY;
    dot.write_date = dot.create_date = FIXED_DATE;

    dot.name[0] = '.';
    set_entry_cluster(&dot, cluster);
    status_t status = write_entry(f, cluster, 0, &dot);
    if (STATUS_IS_ERROR(status))
        return status;

    dot.name[1] = '.';
    set_entry_cluster(&dot, parent == f->root_cluster ? 0 : parent);
    return write_entry(f, cluster, 1, &dot);
}

/* --- Operations ----------------------------------------------------------------------- */

static status_t fat_lookup(vnode_t *dir, const char *name, size_t length, vnode_t **result)
{
    fat_node_t *d = node_of(dir);
    fat_dirent_t entry;
    status_t status = find(d->fat, d->first_cluster, name, length, &entry);
    if (STATUS_IS_ERROR(status))
        return status;
    return get_node(dir->fs, d->first_cluster, &entry, result);
}

static status_t fat_create(vnode_t *dir, const char *name, size_t length, vnode_type_t type, uint32_t mode,
                           vnode_t **result)
{
    fat_node_t *d = node_of(dir);
    fat_t *f = d->fat;
    fat_dirent_t existing;
    uint32_t cluster = 0;

    (void)mode;
    if (type == VNODE_SYMLINK || !valid_long_name(name, length))
        return type == VNODE_SYMLINK ? STATUS_NOT_SUPPORTED : STATUS_INVALID_ARGUMENT;
    status_t status = find(f, d->first_cluster, name, length, &existing);
    if (status == STATUS_SUCCESS)
        return STATUS_ALREADY_EXISTS;
    if (status != STATUS_NOT_FOUND)
        return status;

    if (type == VNODE_DIRECTORY) {
        status = alloc_cluster(f, 0, &cluster);
        if (!STATUS_IS_ERROR(status))
            status = init_directory(f, cluster, d->first_cluster);
        if (STATUS_IS_ERROR(status))
            return status;
    }

    fat_dirent_t created = { 0 };
    created.entry.attr = type == VNODE_DIRECTORY ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    created.entry.create_date = created.entry.write_date = created.entry.access_date = FIXED_DATE;
    set_entry_cluster(&created.entry, cluster);
    status = add_entry(f, d->first_cluster, name, length, &created.entry, &created.index);
    if (STATUS_IS_ERROR(status)) {
        if (cluster)
            free_chain(f, cluster);
        return status;
    }
    return get_node(dir->fs, d->first_cluster, &created, result);
}

static status_t fat_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    fat_node_t *n = node_of(v);
    fat_t *f = n->fat;
    uint8_t *out = buffer;

    *done = 0;
    if (offset >= v->size)
        return STATUS_SUCCESS;
    if (size > v->size - offset)
        size = v->size - offset;

    uint32_t c;
    status_t status = chain_at(f, n->first_cluster, offset / f->cluster_size, &c);
    while (!STATUS_IS_ERROR(status) && *done < size) {
        uint32_t skip = (uint32_t)(offset % f->cluster_size);
        size_t chunk = f->cluster_size - skip < size - *done ? f->cluster_size - skip : size - *done;

        status = read_cluster(f, c, f->cluster);
        if (STATUS_IS_ERROR(status))
            break;
        memcpy(out + *done, f->cluster + skip, chunk);
        *done += chunk;
        offset += chunk;
        if (*done < size)
            status = fat_get(f, c, &c);
    }
    return status == STATUS_NOT_FOUND ? STATUS_IO_ERROR : status;
}

/* Make the chain cover `size` bytes; new clusters are zeroed. */
static status_t ensure_clusters(fat_node_t *n, uint64_t size)
{
    fat_t *f = n->fat;
    uint64_t needed = (size + f->cluster_size - 1) / f->cluster_size;
    uint32_t last = 0;
    uint64_t have = 0;
    status_t status = STATUS_SUCCESS;

    if (n->first_cluster)
        status = chain_end(f, n->first_cluster, &last, &have);
    while (!STATUS_IS_ERROR(status) && have < needed) {
        uint32_t c;
        status = alloc_cluster(f, last, &c);
        if (!STATUS_IS_ERROR(status)) {
            if (!n->first_cluster)
                n->first_cluster = c;
            last = c;
            have++;
        }
    }
    return status;
}

/* Bytes after the end of file in its last cluster may be stale: clear them before growing. */
static status_t zero_tail(fat_node_t *n)
{
    fat_t *f = n->fat;
    uint32_t used = (uint32_t)(n->vnode.size % f->cluster_size);
    uint32_t c;

    if (!n->first_cluster || used == 0)
        return STATUS_SUCCESS;
    status_t status = chain_at(f, n->first_cluster, n->vnode.size / f->cluster_size, &c);
    if (!STATUS_IS_ERROR(status))
        status = read_cluster(f, c, f->cluster);
    if (!STATUS_IS_ERROR(status)) {
        memset(f->cluster + used, 0, f->cluster_size - used);
        status = write_cluster(f, c, f->cluster);
    }
    return status;
}

static status_t fat_write(vnode_t *v, uint64_t offset, const void *buffer, size_t size, size_t *done)
{
    fat_node_t *n = node_of(v);
    fat_t *f = n->fat;
    const uint8_t *in = buffer;

    *done = 0;
    if (size == 0)
        return STATUS_SUCCESS;
    if (offset + size > MAX_FILE_SIZE || offset + size < offset)
        return STATUS_NO_SPACE;

    status_t status = STATUS_SUCCESS;
    if (offset + size > v->size) {
        status = zero_tail(n);
        if (!STATUS_IS_ERROR(status))
            status = ensure_clusters(n, offset + size);
        if (STATUS_IS_ERROR(status))
            return status;
    }

    uint32_t c;
    status = chain_at(f, n->first_cluster, offset / f->cluster_size, &c);
    while (!STATUS_IS_ERROR(status) && *done < size) {
        uint32_t skip = (uint32_t)(offset % f->cluster_size);
        size_t chunk = f->cluster_size - skip < size - *done ? f->cluster_size - skip : size - *done;

        if (chunk < f->cluster_size)
            status = read_cluster(f, c, f->cluster);
        if (STATUS_IS_ERROR(status))
            break;
        memcpy(f->cluster + skip, in + *done, chunk);
        status = write_cluster(f, c, f->cluster);
        if (STATUS_IS_ERROR(status))
            break;
        *done += chunk;
        offset += chunk;
        if (*done < size)
            status = fat_get(f, c, &c);
    }

    if (offset > v->size)
        v->size = offset;
    status_t update = update_entry(n);
    return STATUS_IS_ERROR(status) ? status : update;
}

static status_t fat_truncate(vnode_t *v, uint64_t size)
{
    fat_node_t *n = node_of(v);
    fat_t *f = n->fat;
    status_t status = STATUS_SUCCESS;

    if (size > MAX_FILE_SIZE)
        return STATUS_NO_SPACE;
    if (size > v->size) {
        status = zero_tail(n);
        if (!STATUS_IS_ERROR(status))
            status = ensure_clusters(n, size);
    } else if (size < v->size && n->first_cluster) {
        uint64_t keep = (size + f->cluster_size - 1) / f->cluster_size;
        if (keep == 0) {
            status = free_chain(f, n->first_cluster);
            n->first_cluster = 0;
        } else {
            uint32_t last, rest;
            status = chain_at(f, n->first_cluster, keep - 1, &last);
            if (!STATUS_IS_ERROR(status))
                status = fat_get(f, last, &rest);
            if (!STATUS_IS_ERROR(status))
                status = fat_set(f, last, FAT_EOC);
            if (!STATUS_IS_ERROR(status) && rest < FAT_EOC_MIN)
                status = free_chain(f, rest);
        }
    }
    if (STATUS_IS_ERROR(status))
        return status;
    v->size = size;
    return update_entry(n);
}

static status_t directory_empty(fat_t *f, uint32_t first)
{
    fat_dirent_t d;
    uint32_t index = 0;
    status_t status;
    while ((status = next_dirent(f, first, &index, &d)) == STATUS_SUCCESS) {
        bool dot = (d.length == 1 && d.name[0] == '.') || (d.length == 2 && d.name[0] == '.' && d.name[1] == '.');
        if (!dot)
            return STATUS_NOT_EMPTY;
    }
    return status == STATUS_NOT_FOUND ? STATUS_SUCCESS : status;
}

static status_t fat_unlink(vnode_t *dir, const char *name, size_t length)
{
    fat_node_t *d = node_of(dir);
    fat_t *f = d->fat;
    fat_dirent_t entry;

    status_t status = find(f, d->first_cluster, name, length, &entry);
    if (STATUS_IS_ERROR(status))
        return status;

    fat_node_t *open = cached(f, d->first_cluster, entry.index);
    if (open && open->vnode.refs > 0)
        return STATUS_BUSY; /* in use: its clusters must not be freed */

    uint32_t first = entry_cluster(&entry.entry);
    if (entry.entry.attr & ATTR_DIRECTORY) {
        status = directory_empty(f, first);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    status = remove_entries(f, d->first_cluster, entry.first_index, entry.index);
    if (!STATUS_IS_ERROR(status) && first)
        status = free_chain(f, first);
    return status;
}

static status_t fat_rename(vnode_t *from_dir, const char *from, size_t from_length,
                           vnode_t *to_dir, const char *to, size_t to_length)
{
    fat_node_t *src = node_of(from_dir), *dst = node_of(to_dir);
    fat_t *f = src->fat;
    fat_dirent_t entry;
    uint32_t new_index;

    if (!valid_long_name(to, to_length))
        return STATUS_INVALID_ARGUMENT;
    status_t status = find(f, src->first_cluster, from, from_length, &entry);
    if (STATUS_IS_ERROR(status))
        return status;

    status = add_entry(f, dst->first_cluster, to, to_length, &entry.entry, &new_index);
    if (STATUS_IS_ERROR(status))
        return status;

    /* add_entry may have reused slots; the old entry is found again by name. */
    fat_dirent_t old;
    status = find(f, src->first_cluster, from, from_length, &old);
    if (status == STATUS_SUCCESS && !(src == dst && old.index == new_index))
        status = remove_entries(f, src->first_cluster, old.first_index, old.index);
    if (STATUS_IS_ERROR(status))
        return status;

    /* A moved directory's ".." must point to its new parent. */
    uint32_t first = entry_cluster(&entry.entry);
    if ((entry.entry.attr & ATTR_DIRECTORY) && src != dst && first) {
        dir_entry_t dotdot;
        status = read_entry(f, first, 1, &dotdot);
        if (!STATUS_IS_ERROR(status)) {
            set_entry_cluster(&dotdot, dst->first_cluster == f->root_cluster ? 0 : dst->first_cluster);
            status = write_entry(f, first, 1, &dotdot);
        }
    }

    fat_node_t *moved = cached(f, src->first_cluster, entry.index);
    if (moved) {
        moved->dir_cluster = dst->first_cluster;
        moved->entry_index = new_index;
    }
    return status;
}

static status_t fat_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out)
{
    fat_node_t *d = node_of(dir);
    fat_dirent_t entry;
    uint32_t index = (uint32_t)*cookie;

    status_t status = next_dirent(d->fat, d->first_cluster, &index, &entry);
    if (STATUS_IS_ERROR(status))
        return status;
    *cookie = index;
    out->type = (entry.entry.attr & ATTR_DIRECTORY) ? VNODE_DIRECTORY : VNODE_FILE;
    out->inode = ((uint64_t)d->first_cluster << 32) | entry.index;
    out->name_length = (uint32_t)entry.length;
    memcpy(out->name, entry.name, entry.length + 1);
    return STATUS_SUCCESS;
}

static void fat_release(vnode_t *v)
{
    fat_node_t *n = node_of(v);
    if (n->is_root)
        return; /* freed by unmount */
    list_remove(&n->cache_node);
    kfree(n);
}

static const vnode_ops_t ops = {
    .lookup = fat_lookup,
    .create = fat_create,
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .unlink = fat_unlink,
    .rename = fat_rename,
    .readdir = fat_readdir,
    .release = fat_release,
};

/* --- Mounting --------------------------------------------------------------------------- */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static status_t fat_mount(block_device_t *device, filesystem_t **result)
{
    uint8_t boot[512];

    if (!device || device->sector_size != 512 || STATUS_IS_ERROR(block_read_bytes(device, 0, sizeof(boot), boot)))
        return STATUS_NOT_SUPPORTED;

    uint32_t sector_size = le16(boot + 11);
    uint32_t cluster_sectors = boot[13];
    uint32_t reserved = le16(boot + 14);
    uint32_t fats = boot[16];
    uint32_t root_entries = le16(boot + 17);
    uint32_t total = le16(boot + 19) ? le16(boot + 19) : le32(boot + 32);
    uint32_t fat16_size = le16(boot + 22);
    uint32_t fat_size = le32(boot + 36);

    if (le16(boot + 510) != 0xAA55 || sector_size != device->sector_size || cluster_sectors == 0 ||
        (cluster_sectors & (cluster_sectors - 1)) || reserved == 0 || fats == 0 || total == 0 ||
        total > device->sector_count)
        return STATUS_NOT_SUPPORTED;
    if (root_entries != 0 || fat16_size != 0 || fat_size == 0)
        return STATUS_NOT_SUPPORTED; /* FAT12/16 */

    uint64_t data_start = reserved + (uint64_t)fats * fat_size;
    if (data_start >= total)
        return STATUS_NOT_SUPPORTED;
    uint32_t clusters = (uint32_t)((total - data_start) / cluster_sectors);
    if (clusters < FAT32_MIN_CLUSTERS)
        return STATUS_NOT_SUPPORTED;

    filesystem_t *fs = kcalloc(1, sizeof(*fs));
    fat_t *f = kcalloc(1, sizeof(*f));
    fat_node_t *root = kcalloc(1, sizeof(*root));
    if (f) {
        f->sector = kmalloc(sector_size);
        f->cluster = kmalloc(cluster_sectors * sector_size);
    }
    if (!fs || !f || !root || !f->sector || !f->cluster) {
        if (f) {
            kfree(f->sector);
            kfree(f->cluster);
        }
        kfree(fs);
        kfree(f);
        kfree(root);
        return STATUS_OUT_OF_MEMORY;
    }

    f->device = device;
    f->sector_size = sector_size;
    f->cluster_sectors = cluster_sectors;
    f->cluster_size = cluster_sectors * sector_size;
    f->reserved_sectors = reserved;
    f->fat_count = fats;
    f->fat_sectors = fat_size;
    f->data_start = data_start;
    f->cluster_count = clusters;
    f->root_cluster = le32(boot + 44);
    f->fsinfo_sector = le16(boot + 48);
    f->free_hint = 2;
    list_init(&f->nodes);
    fs->data = f;

    vnode_init(&root->vnode, fs, VNODE_DIRECTORY, &ops);
    root->fat = f;
    root->is_root = true;
    root->first_cluster = f->root_cluster;
    root->vnode.inode = f->root_cluster;
    fs->root = &root->vnode;

    char label[12];
    memcpy(label, boot + 71, 11);
    label[11] = '\0';
    klog_info("fat: %s: FAT32 \"%s\", %u clusters of %u bytes", device->name, label, clusters, f->cluster_size);
    *result = fs;
    return STATUS_SUCCESS;
}

static status_t fat_unmount(filesystem_t *fs)
{
    fat_t *f = fs->data;

    if (!list_empty(&f->nodes))
        return STATUS_BUSY;
    kfree(node_of(fs->root));
    kfree(f->sector);
    kfree(f->cluster);
    kfree(f);
    kfree(fs);
    return STATUS_SUCCESS;
}

fs_type_t fat_type = {
    .name = "fat32",
    .mount = fat_mount,
    .unmount = fat_unmount,
};
