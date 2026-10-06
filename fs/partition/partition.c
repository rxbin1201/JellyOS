/*
 * Partition layer: GPT (with header and entry-array CRC checks, falling back
 * to the backup header) and classic MBR primary partitions.
 */

#include "fs/block/block.h"

#include "core/crc32.h"
#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define MBR_SIGNATURE      0xAA55
#define MBR_TYPE_GPT       0xEE
#define GPT_SIGNATURE      0x5452415020494645ULL /* "EFI PART" */
#define GPT_MAX_ENTRIES    256
#define MAX_PARTITIONS     32

typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} mbr_entry_t;

typedef struct __attribute__((packed)) {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} gpt_entry_t;

static block_device_t *new_partition(block_device_t *disk, unsigned number, uint64_t start, uint64_t count,
                                     partition_scheme_t scheme)
{
    if (count == 0 || start >= disk->sector_count || count > disk->sector_count - start)
        return NULL;

    block_device_t *p = kcalloc(1, sizeof(*p));
    if (!p)
        return NULL;
    format(p->name, sizeof(p->name), "%sp%u", disk->name, number);
    p->sector_size = disk->sector_size;
    p->sector_count = count;
    p->read_only = disk->read_only;
    p->parent = disk;
    p->start_lba = start;
    p->partition_number = number;
    p->scheme = scheme;
    return p;
}

static bool zero_guid(const uint8_t *guid)
{
    for (int i = 0; i < 16; i++) {
        if (guid[i])
            return false;
    }
    return true;
}

static bool read_gpt_header(block_device_t *disk, uint64_t lba, gpt_header_t *header)
{
    if (STATUS_IS_ERROR(block_read_bytes(disk, lba * disk->sector_size, sizeof(*header), header)))
        return false;
    if (header->signature != GPT_SIGNATURE || header->header_size < 92 || header->header_size > disk->sector_size)
        return false;

    uint32_t expected = header->header_crc;
    header->header_crc = 0;
    bool ok = crc32(header, 92) == expected;
    header->header_crc = expected;
    return ok && header->entry_size >= sizeof(gpt_entry_t) && header->entry_count <= GPT_MAX_ENTRIES;
}

static unsigned scan_gpt(block_device_t *disk)
{
    gpt_header_t header;

    if (!read_gpt_header(disk, 1, &header) && !read_gpt_header(disk, disk->sector_count - 1, &header)) {
        klog_warn("block %s: protective MBR but no valid GPT header", disk->name);
        return 0;
    }

    size_t table_size = (size_t)header.entry_count * header.entry_size;
    uint8_t *table = kmalloc(table_size ? table_size : 1);
    if (!table || STATUS_IS_ERROR(block_read_bytes(disk, header.entries_lba * disk->sector_size, table_size, table)) ||
        crc32(table, table_size) != header.entries_crc) {
        klog_warn("block %s: GPT partition entries are unreadable or corrupt", disk->name);
        kfree(table);
        return 0;
    }

    unsigned count = 0;
    for (uint32_t i = 0; i < header.entry_count && count < MAX_PARTITIONS; i++) {
        const gpt_entry_t *e = (const gpt_entry_t *)(table + (size_t)i * header.entry_size);
        if (zero_guid(e->type_guid) || e->last_lba < e->first_lba)
            continue;
        block_device_t *p = new_partition(disk, i + 1, e->first_lba, e->last_lba - e->first_lba + 1, PARTITION_GPT);
        if (!p)
            continue;
        memcpy(p->type_guid, e->type_guid, 16);
        memcpy(p->unique_guid, e->unique_guid, 16);
        block_register(p);
        count++;
    }
    kfree(table);
    return count;
}

unsigned partition_scan(block_device_t *disk)
{
    uint8_t mbr[512];

    if (disk->sector_size < 512 || STATUS_IS_ERROR(block_read_bytes(disk, 0, sizeof(mbr), mbr)))
        return 0;
    if ((uint16_t)(mbr[510] | mbr[511] << 8) != MBR_SIGNATURE)
        return 0;

    mbr_entry_t entries[4];
    memcpy(entries, mbr + 446, sizeof(entries));
    if (entries[0].type == MBR_TYPE_GPT)
        return scan_gpt(disk);

    /* A FAT boot sector also ends in 0xAA55: require sane partition entries. */
    unsigned count = 0;
    for (unsigned i = 0; i < 4; i++) {
        const mbr_entry_t *e = &entries[i];
        if (!e->type || !e->sectors || !e->lba_first || (e->status != 0 && e->status != 0x80))
            continue;
        block_device_t *p = new_partition(disk, i + 1, e->lba_first, e->sectors, PARTITION_MBR);
        if (!p)
            continue;
        p->mbr_type = e->type;
        block_register(p);
        count++;
    }
    return count;
}
