/*
 * Kernel tests: block devices, partitions, the VFS (ramfs) and FAT32.
 *
 * `make test` attaches a VirtIO disk built by tools/image_builder/mkdisk.sh
 * from tests/storage/disk (GPT, one FAT32 partition, files written by mtools)
 * plus pattern.bin (200000 bytes). It is mounted at /volumes/virtio0p1.
 */

#include "tests/kernel/ktest.h"

#include "core/format.h"
#include "core/string.h"
#include "fs/block/block.h"
#include "fs/vfs/vfs.h"
#include "memory/heap.h"

#define VOLUME     "/volumes/virtio0p1"
#define HOST_TEXT  "Written by the JellyOS FAT32 driver.\n"

static const credentials_t user = { 1000, 1000 };

static uint8_t pattern_byte(size_t i)
{
    return (uint8_t)((i * 7 + i / 251) % 256);
}

static status_t write_file(const char *path, const void *data, size_t size, uint32_t extra_flags)
{
    file_t *file;
    size_t done;
    status_t status = vfs_open(path, JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | extra_flags, 0644, NULL, &file);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_write(file, data, size, &done);
    object_release(&file->object);
    return STATUS_IS_ERROR(status) ? status : done == size ? STATUS_SUCCESS : STATUS_IO_ERROR;
}

static status_t read_file(const char *path, void *buffer, size_t capacity, size_t *size)
{
    file_t *file;
    status_t status = vfs_open(path, JELLY_OPEN_READ, 0, NULL, &file);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_read(file, buffer, capacity, size);
    object_release(&file->object);
    return status;
}

static bool file_equals(const char *path, const char *text)
{
    char buffer[256];
    size_t size;
    return read_file(path, buffer, sizeof(buffer), &size) == STATUS_SUCCESS && size == strlen(text) &&
           memcmp(buffer, text, size) == 0;
}

static bool directory_contains(const char *path, const char *name)
{
    file_t *dir;
    vfs_dirent_t entry;
    bool found = false;

    if (vfs_open(path, JELLY_OPEN_READ | JELLY_OPEN_DIRECTORY, 0, NULL, &dir) != STATUS_SUCCESS)
        return false;
    while (!found && vfs_readdir(dir, &entry) == STATUS_SUCCESS)
        found = strcmp(entry.name, name) == 0;
    object_release(&dir->object);
    return found;
}

/* --- Block layer ------------------------------------------------------------------- */

KTEST(block_devices_and_gpt)
{
    static const uint8_t basic_data[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                            0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    uint8_t sector[512];

    block_device_t *disk = block_find("virtio0");
    block_device_t *part = block_find("virtio0p1");
    KASSERT(disk && part);
    KEXPECT(disk->sector_count == 64 * 2048);
    KEXPECT(part->parent == disk && part->scheme == PARTITION_GPT && part->start_lba == 2048);
    KEXPECT(memcmp(part->type_guid, basic_data, 16) == 0);

    KEXPECT(block_read(part, 0, 1, sector) == STATUS_SUCCESS);
    KEXPECT(sector[510] == 0x55 && sector[511] == 0xAA); /* FAT boot sector */
    KEXPECT(block_read(part, part->sector_count, 1, sector) == STATUS_INVALID_ARGUMENT);
    KEXPECT(block_read(part, part->sector_count - 1, 2, sector) == STATUS_INVALID_ARGUMENT);
}

/* --- VFS ----------------------------------------------------------------------------- */

static bool normalizes(const char *base, const char *path, const char *expected)
{
    char out[VFS_PATH_MAX];
    return vfs_normalize(base, path, strlen(path), out, sizeof(out)) == STATUS_SUCCESS && strcmp(out, expected) == 0;
}

KTEST(path_normalization)
{
    char out[VFS_PATH_MAX];

    KEXPECT(normalizes("/", "a/b/../c", "/a/c"));
    KEXPECT(normalizes("/x/y", "../z", "/x/z"));
    KEXPECT(normalizes("/x/y", "/abs", "/abs"));
    KEXPECT(normalizes("/", "/../..", "/"));
    KEXPECT(normalizes("/", "//a///b/./", "/a/b"));
    KEXPECT(vfs_normalize("/", "a\0b", 3, out, sizeof(out)) == STATUS_INVALID_ARGUMENT);
    KEXPECT(vfs_normalize("/", "", 0, out, sizeof(out)) == STATUS_INVALID_ARGUMENT);

    char long_name[300];
    memset(long_name, 'n', sizeof(long_name));
    KEXPECT(vfs_normalize("/", long_name, sizeof(long_name), out, sizeof(out)) == STATUS_INVALID_ARGUMENT);
}

KTEST(ramfs_files_and_directories)
{
    char buffer[64];
    size_t size;
    vfs_stat_t stat;
    file_t *file;

    KASSERT(vfs_mkdir("/tmp/t", 0755, NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_mkdir("/tmp/t", 0755, NULL) == STATUS_ALREADY_EXISTS);
    KEXPECT(write_file("/tmp/t/file", "jellyfish", 9, 0) == STATUS_SUCCESS);
    KEXPECT(file_equals("/tmp/t/file", "jellyfish"));
    KEXPECT(vfs_stat("/tmp/t/file", NULL, true, &stat) == STATUS_SUCCESS && stat.size == 9 &&
            stat.type == JELLY_FILE_TYPE_FILE);

    /* Seek, overwrite in the middle, append. */
    KASSERT(vfs_open("/tmp/t/file", JELLY_OPEN_READ | JELLY_OPEN_WRITE, 0, NULL, &file) == STATUS_SUCCESS);
    uint64_t position;
    KEXPECT(vfs_seek(file, 5, JELLY_SEEK_SET, &position) == STATUS_SUCCESS && position == 5);
    KEXPECT(vfs_write(file, "FISH", 4, &size) == STATUS_SUCCESS);
    KEXPECT(vfs_seek(file, -2, JELLY_SEEK_END, &position) == STATUS_SUCCESS && position == 7);
    KEXPECT(vfs_seek(file, -100, JELLY_SEEK_CURRENT, &position) == STATUS_INVALID_ARGUMENT);
    object_release(&file->object);
    KEXPECT(file_equals("/tmp/t/file", "jellyFISH"));
    KEXPECT(write_file("/tmp/t/file", "!", 1, JELLY_OPEN_APPEND) == STATUS_SUCCESS);
    KEXPECT(file_equals("/tmp/t/file", "jellyFISH!"));
    KEXPECT(write_file("/tmp/t/file", "new", 3, JELLY_OPEN_TRUNCATE) == STATUS_SUCCESS);
    KEXPECT(file_equals("/tmp/t/file", "new"));

    /* Errors */
    KEXPECT(vfs_open("/tmp/t/missing", JELLY_OPEN_READ, 0, NULL, &file) == STATUS_NOT_FOUND);
    KEXPECT(vfs_open("/tmp/t", JELLY_OPEN_WRITE, 0, NULL, &file) == STATUS_IS_DIRECTORY);
    KEXPECT(vfs_open("/tmp/t/file", JELLY_OPEN_READ | JELLY_OPEN_DIRECTORY, 0, NULL, &file) == STATUS_NOT_DIRECTORY);
    KEXPECT(vfs_open("/tmp/t/file/x", JELLY_OPEN_READ, 0, NULL, &file) == STATUS_NOT_DIRECTORY);
    KEXPECT(vfs_open("/tmp/t/file", JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | JELLY_OPEN_EXCLUSIVE, 0644, NULL, &file) ==
            STATUS_ALREADY_EXISTS);
    KEXPECT(read_file("/tmp/t", buffer, sizeof(buffer), &size) == STATUS_IS_DIRECTORY);

    /* Rename, listing, removal */
    KEXPECT(vfs_rename("/tmp/t/file", "/tmp/t/renamed", NULL) == STATUS_SUCCESS);
    KEXPECT(directory_contains("/tmp/t", "renamed") && !directory_contains("/tmp/t", "file"));
    KEXPECT(vfs_rename("/tmp/t", "/tmp/t/inside", NULL) == STATUS_INVALID_ARGUMENT);
    KEXPECT(vfs_unlink("/tmp/t", NULL) == STATUS_NOT_EMPTY);
    KEXPECT(vfs_unlink("/tmp/t/renamed", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_unlink("/tmp/t", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_stat("/tmp/t", NULL, true, &stat) == STATUS_NOT_FOUND);
}

KTEST(symbolic_links)
{
    char target[64];
    size_t length;
    vnode_t *v;

    KASSERT(vfs_mkdir("/tmp/s", 0755, NULL) == STATUS_SUCCESS);
    KEXPECT(write_file("/tmp/s/real", "linked", 6, 0) == STATUS_SUCCESS);
    KEXPECT(vfs_symlink("/tmp/s/real", "/tmp/s/absolute", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_symlink("real", "/tmp/s/relative", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_symlink("/tmp/s", "/tmp/dirlink", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_symlink("/tmp/loop-b", "/tmp/loop-a", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_symlink("/tmp/loop-a", "/tmp/loop-b", NULL) == STATUS_SUCCESS);

    KEXPECT(file_equals("/tmp/s/absolute", "linked"));
    KEXPECT(file_equals("/tmp/s/relative", "linked"));
    KEXPECT(file_equals("/tmp/dirlink/real", "linked"));
    KEXPECT(vfs_readlink("/tmp/s/relative", NULL, target, sizeof(target), &length) == STATUS_SUCCESS &&
            length == 4 && memcmp(target, "real", 4) == 0);
    KEXPECT(vfs_lookup("/tmp/loop-a", NULL, true, &v) == STATUS_LIMIT_EXCEEDED);
    KEXPECT(vfs_lookup("/tmp/loop-a", NULL, false, &v) == STATUS_SUCCESS && v->type == VNODE_SYMLINK);
    vnode_release(v);

    vfs_unlink("/tmp/loop-a", NULL);
    vfs_unlink("/tmp/loop-b", NULL);
    vfs_unlink("/tmp/dirlink", NULL);
    vfs_unlink("/tmp/s/relative", NULL);
    vfs_unlink("/tmp/s/absolute", NULL);
    vfs_unlink("/tmp/s/real", NULL);
    KEXPECT(vfs_unlink("/tmp/s", NULL) == STATUS_SUCCESS);
}

KTEST(permissions)
{
    file_t *file;
    vfs_stat_t stat;

    KASSERT(vfs_open("/tmp/secret", JELLY_OPEN_WRITE | JELLY_OPEN_CREATE, 0600, NULL, &file) == STATUS_SUCCESS);
    object_release(&file->object);
    KEXPECT(vfs_open("/tmp/secret", JELLY_OPEN_READ, 0, &user, &file) == STATUS_ACCESS_DENIED);
    KEXPECT(vfs_stat("/tmp/secret", &user, true, &stat) == STATUS_SUCCESS && stat.mode == 0600 && stat.uid == 0);

    KASSERT(vfs_mkdir("/tmp/private", 0700, NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_stat("/tmp/private/anything", &user, true, &stat) == STATUS_ACCESS_DENIED);
    KEXPECT(vfs_mkdir("/tmp/private/x", 0755, &user) == STATUS_ACCESS_DENIED);

    /* A user's own file in a world-writable directory */
    KEXPECT(vfs_open("/tmp/mine", JELLY_OPEN_WRITE | JELLY_OPEN_CREATE, 0600, &user, &file) == STATUS_SUCCESS);
    object_release(&file->object);
    KEXPECT(vfs_stat("/tmp/mine", NULL, true, &stat) == STATUS_SUCCESS && stat.uid == 1000);
    KEXPECT(vfs_open("/tmp/mine", JELLY_OPEN_READ, 0, &user, &file) == STATUS_SUCCESS);
    object_release(&file->object);

    vfs_unlink("/tmp/mine", NULL);
    vfs_unlink("/tmp/private", NULL);
    KEXPECT(vfs_unlink("/tmp/secret", NULL) == STATUS_SUCCESS);
}

/* --- FAT32 ------------------------------------------------------------------------------ */

KTEST(fat_reads_files_from_the_image)
{
    KEXPECT(file_equals(VOLUME "/hello.txt", "Hello from the JellyOS test disk!\n"));
    KEXPECT(file_equals(VOLUME "/HELLO.TXT", "Hello from the JellyOS test disk!\n")); /* case-insensitive */
    KEXPECT(file_equals(VOLUME "/A long file name for testing.txt",
                        "This file has a long name with spaces and mixed Case.\n"));
    KEXPECT(file_equals(VOLUME "/docs/nested/deep.txt", "deep inside\n"));
    KEXPECT(directory_contains(VOLUME, "A long file name for testing.txt"));
    KEXPECT(directory_contains(VOLUME "/docs", "nested"));

    /* 200000 bytes over ~390 clusters of 512 bytes */
    uint8_t *data = kmalloc(200001);
    size_t size;
    KASSERT(data != NULL);
    KEXPECT(read_file(VOLUME "/pattern.bin", data, 200001, &size) == STATUS_SUCCESS && size == 200000);
    bool ok = true;
    for (size_t i = 0; i < 200000 && ok; i++)
        ok = data[i] == pattern_byte(i);
    KEXPECT(ok);
    kfree(data);
}

KTEST(fat_writes_files)
{
    const char *path = VOLUME "/written-in-kernel.bin";
    uint8_t *data = kmalloc(6000);
    uint8_t *back = kmalloc(6000);
    file_t *file;
    size_t size;
    vfs_stat_t stat;
    KASSERT(data && back);

    for (size_t i = 0; i < 3000; i++)
        data[i] = (uint8_t)(i * 31);
    KEXPECT(write_file(path, data, 3000, 0) == STATUS_SUCCESS);
    KEXPECT(read_file(path, back, 6000, &size) == STATUS_SUCCESS && size == 3000 && memcmp(back, data, 3000) == 0);

    /* Overwrite across a cluster boundary, then grow past the end. */
    KASSERT(vfs_open(path, JELLY_OPEN_READ | JELLY_OPEN_WRITE, 0, NULL, &file) == STATUS_SUCCESS);
    uint64_t position;
    vfs_seek(file, 500, JELLY_SEEK_SET, &position);
    memset(data + 500, 0xAB, 100);
    KEXPECT(vfs_write(file, data + 500, 100, &size) == STATUS_SUCCESS && size == 100);
    vfs_seek(file, 4000, JELLY_SEEK_SET, &position); /* leaves a hole: must read as zeros */
    KEXPECT(vfs_write(file, "end", 3, &size) == STATUS_SUCCESS);
    object_release(&file->object);

    KEXPECT(read_file(path, back, 6000, &size) == STATUS_SUCCESS && size == 4003);
    KEXPECT(memcmp(back, data, 3000) == 0);
    bool hole = true;
    for (size_t i = 3000; i < 4000; i++)
        hole &= back[i] == 0;
    KEXPECT(hole && memcmp(back + 4000, "end", 3) == 0);

    /* Truncate down and up again: the regrown part reads as zeros. */
    KASSERT(vfs_open(path, JELLY_OPEN_WRITE, 0, NULL, &file) == STATUS_SUCCESS);
    KEXPECT(vfs_truncate(file, 100) == STATUS_SUCCESS);
    KEXPECT(vfs_truncate(file, 1500) == STATUS_SUCCESS);
    object_release(&file->object);
    KEXPECT(vfs_stat(path, NULL, true, &stat) == STATUS_SUCCESS && stat.size == 1500);
    KEXPECT(read_file(path, back, 6000, &size) == STATUS_SUCCESS && size == 1500);
    bool zeros = memcmp(back, data, 100) == 0;
    for (size_t i = 100; i < 1500; i++)
        zeros &= back[i] == 0;
    KEXPECT(zeros);

    KEXPECT(vfs_unlink(path, NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_stat(path, NULL, true, &stat) == STATUS_NOT_FOUND);
    kfree(data);
    kfree(back);
}

KTEST(fat_directories_and_renames)
{
    vfs_stat_t stat;

    KASSERT(vfs_mkdir(VOLUME "/dir1", 0755, NULL) == STATUS_SUCCESS);
    KASSERT(vfs_mkdir(VOLUME "/dir1/sub", 0755, NULL) == STATUS_SUCCESS);
    KEXPECT(write_file(VOLUME "/dir1/sub/a file.txt", "abc", 3, 0) == STATUS_SUCCESS);
    KEXPECT(vfs_rename(VOLUME "/dir1/sub/a file.txt", VOLUME "/dir1/sub/renamed.txt", NULL) == STATUS_SUCCESS);
    KEXPECT(file_equals(VOLUME "/dir1/sub/renamed.txt", "abc"));
    KEXPECT(vfs_unlink(VOLUME "/dir1", NULL) == STATUS_NOT_EMPTY);

    /* Move a directory to another parent; its ".." follows. */
    KEXPECT(vfs_rename(VOLUME "/dir1/sub", VOLUME "/moved", NULL) == STATUS_SUCCESS);
    KEXPECT(file_equals(VOLUME "/moved/renamed.txt", "abc"));
    KEXPECT(vfs_stat(VOLUME "/dir1/sub", NULL, true, &stat) == STATUS_NOT_FOUND);
    KEXPECT(file_equals(VOLUME "/moved/../hello.txt", "Hello from the JellyOS test disk!\n"));

    /* 40 entries of 3 slots each need more than one 512-byte cluster: the directory grows. */
    char name[96];
    for (int i = 0; i < 40; i++) {
        format(name, sizeof(name), VOLUME "/dir1/entry with a long name %02d", i);
        KEXPECT(write_file(name, "x", 1, 0) == STATUS_SUCCESS);
    }
    KEXPECT(directory_contains(VOLUME "/dir1", "entry with a long name 39"));
    for (int i = 0; i < 40; i++) {
        format(name, sizeof(name), VOLUME "/dir1/entry with a long name %02d", i);
        vfs_unlink(name, NULL);
    }

    vfs_unlink(VOLUME "/moved/renamed.txt", NULL);
    KEXPECT(vfs_unlink(VOLUME "/moved", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_unlink(VOLUME "/dir1", NULL) == STATUS_SUCCESS);
    KEXPECT(vfs_symlink("/x", VOLUME "/link", NULL) == STATUS_NOT_SUPPORTED);
}

KTEST(fat_open_files_and_mounts)
{
    file_t *file;

    KASSERT(vfs_open(VOLUME "/hello.txt", JELLY_OPEN_READ, 0, NULL, &file) == STATUS_SUCCESS);
    KEXPECT(vfs_unlink(VOLUME "/hello.txt", NULL) == STATUS_BUSY);  /* open */
    KEXPECT(vfs_unmount(VOLUME) == STATUS_BUSY);                    /* open file */
    object_release(&file->object);

    KEXPECT(vfs_unlink(VOLUME, NULL) == STATUS_BUSY);               /* mount point */
    KEXPECT(vfs_rename(VOLUME "/hello.txt", "/tmp/hello.txt", NULL) == STATUS_NOT_SUPPORTED); /* other fs */
}

KTEST(fat_data_survives_remount)
{
    /* Also read by the host after the run (Makefile: HOST_CHECK_PATH). */
    vfs_mkdir(VOLUME "/jellyos", 0755, NULL);
    KEXPECT(write_file(VOLUME "/jellyos/written.txt", HOST_TEXT, strlen(HOST_TEXT), JELLY_OPEN_TRUNCATE) ==
            STATUS_SUCCESS);
    KEXPECT(vfs_sync() == STATUS_SUCCESS);

    KEXPECT(vfs_unmount(VOLUME) == STATUS_SUCCESS);
    KEXPECT(!file_equals(VOLUME "/jellyos/written.txt", HOST_TEXT)); /* plain ramfs directory now */
    KEXPECT(vfs_mount(VOLUME, block_find("virtio0p1"), "fat32") == STATUS_SUCCESS);
    KEXPECT(file_equals(VOLUME "/jellyos/written.txt", HOST_TEXT));
}
