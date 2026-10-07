/*
 * Kernel tests for the exFAT driver (read-only), against the volume that
 * tools/image_builder/mkexfat.py builds: `make test` attaches it as the
 * second VirtIO disk, mounted at /volumes/virtio1.
 */

#include "tests/kernel/ktest.h"

#include "core/log.h"
#include "core/string.h"
#include "fs/block/block.h"
#include "fs/vfs/vfs.h"
#include "memory/heap.h"

#define VOLUME "/volumes/virtio1"

static uint8_t pattern_byte(size_t i)
{
    return (uint8_t)((i * 7 + i / 251) % 256);
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
    char buffer[128];
    size_t size;
    return read_file(path, buffer, sizeof(buffer), &size) == STATUS_SUCCESS && size == strlen(text) &&
           memcmp(buffer, text, size) == 0;
}

static bool present(void)
{
    vfs_stat_t stat;
    if (block_find("virtio1") && vfs_stat(VOLUME "/hello.txt", NULL, true, &stat) == STATUS_SUCCESS)
        return true;
    klog_info("ktest: no exFAT test volume, skipped");
    return false;
}

KTEST(exfat_reads_files)
{
    if (!present())
        return;
    KEXPECT(file_equals(VOLUME "/hello.txt", "Hello from an exFAT volume!\n"));
    KEXPECT(file_equals(VOLUME "/HELLO.TXT", "Hello from an exFAT volume!\n")); /* names compare without case */
    KEXPECT(file_equals(VOLUME "/docs/nested/deep.txt", "deep inside exFAT\n"));
    KEXPECT(file_equals(VOLUME "/file00.txt", "file number 0\n"));
    KEXPECT(file_equals(VOLUME "/file39.txt", "file number 39\n")); /* in a later cluster of the root directory */
    KEXPECT(file_equals(VOLUME "/\xF0\x9F\x98\x80.txt", "smile\n")); /* UTF-16 surrogates to UTF-8 */

    vfs_stat_t stat;
    KEXPECT(vfs_stat(VOLUME "/deleted.txt", NULL, true, &stat) == STATUS_NOT_FOUND);
    KEXPECT(vfs_stat(VOLUME "/missing.txt", NULL, true, &stat) == STATUS_NOT_FOUND);
    KEXPECT(vfs_stat(VOLUME "/big.bin", NULL, true, &stat) == STATUS_SUCCESS && stat.size == 300000 &&
            stat.type == VNODE_FILE);
    KEXPECT(vfs_stat(VOLUME "/docs", NULL, true, &stat) == STATUS_SUCCESS && stat.type == VNODE_DIRECTORY);

    /* A contiguous file ("NoFatChain") and one whose clusters are scattered and need the allocation table */
    uint8_t *data = kmalloc(300001);
    size_t size = 0;
    KASSERT(data != NULL);
    KEXPECT(read_file(VOLUME "/big.bin", data, 300001, &size) == STATUS_SUCCESS && size == 300000);
    bool ok = true;
    for (size_t i = 0; i < 300000 && ok; i++)
        ok = data[i] == pattern_byte(i);
    KEXPECT(ok);

    KEXPECT(read_file(VOLUME "/Fragmented Datei \xC3\xA4\xC3\xB6\xC3\xBC \xE2\x82\xAC.bin", data, 300001, &size) ==
                STATUS_SUCCESS && size == 10000);
    ok = true;
    for (size_t i = 0; i < 10000 && ok; i++)
        ok = data[i] == pattern_byte(i);
    KEXPECT(ok);

    /* Beyond the valid data length a file reads as zeros, whatever is on the disk */
    KEXPECT(read_file(VOLUME "/sparse.bin", data, 300001, &size) == STATUS_SUCCESS && size == 5000);
    ok = true;
    for (size_t i = 0; i < 5000 && ok; i++)
        ok = data[i] == (i < 1000 ? pattern_byte(i) : 0);
    KEXPECT(ok);

    /* Reads at odd offsets across sector and cluster borders */
    file_t *file;
    uint64_t position;
    KASSERT(vfs_open(VOLUME "/big.bin", JELLY_OPEN_READ, 0, NULL, &file) == STATUS_SUCCESS);
    static const struct {
        uint64_t offset;
        size_t   length;
    } spans[] = { { 1, 10 }, { 500, 30 }, { 2040, 20 }, { 2047, 4097 }, { 100000, 70000 }, { 299990, 100 } };
    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        KEXPECT(vfs_seek(file, (int64_t)spans[i].offset, JELLY_SEEK_SET, &position) == STATUS_SUCCESS);
        KEXPECT(vfs_read(file, data, spans[i].length, &size) == STATUS_SUCCESS);
        size_t expected = spans[i].offset + spans[i].length > 300000 ? 300000 - spans[i].offset : spans[i].length;
        KEXPECT(size == expected);
        ok = true;
        for (size_t k = 0; k < size && ok; k++)
            ok = data[k] == pattern_byte(spans[i].offset + k);
        KEXPECT(ok);
    }
    object_release(&file->object);
    kfree(data);
}

KTEST(exfat_lists_directories_and_refuses_writes)
{
    if (!present())
        return;
    file_t *dir, *file;
    vfs_dirent_t entry;
    unsigned files = 0, directories = 0;
    bool umlauts = false, deleted = false;

    KASSERT(vfs_open(VOLUME, JELLY_OPEN_READ | JELLY_OPEN_DIRECTORY, 0, NULL, &dir) == STATUS_SUCCESS);
    while (vfs_readdir(dir, &entry) == STATUS_SUCCESS) {
        if (entry.type == VNODE_DIRECTORY)
            directories++;
        else
            files++;
        umlauts = umlauts || strcmp(entry.name, "Fragmented Datei \xC3\xA4\xC3\xB6\xC3\xBC \xE2\x82\xAC.bin") == 0;
        deleted = deleted || strcmp(entry.name, "deleted.txt") == 0;
    }
    object_release(&dir->object);
    /* hello, big, fragmented, smiley, sparse, file00-39; docs. No label, bitmap, up-case table or deleted file. */
    KEXPECT(files == 45 && directories == 1);
    KEXPECT(umlauts && !deleted);

    /* Read-only: nothing can be created, written or removed */
    KEXPECT(STATUS_IS_ERROR(vfs_open(VOLUME "/new.txt", JELLY_OPEN_WRITE | JELLY_OPEN_CREATE, 0644, NULL, &file)));
    /* The kernel itself (no credentials) may open for writing, but the write is refused; users fail at the mode. */
    static const credentials_t user = { 1000, 1000 };
    size_t done;
    KEXPECT(STATUS_IS_ERROR(vfs_open(VOLUME "/hello.txt", JELLY_OPEN_WRITE, 0, &user, &file)));
    if (vfs_open(VOLUME "/hello.txt", JELLY_OPEN_WRITE, 0, NULL, &file) == STATUS_SUCCESS) {
        KEXPECT(STATUS_IS_ERROR(vfs_write(file, "x", 1, &done)));
        object_release(&file->object);
    }
    KEXPECT(STATUS_IS_ERROR(vfs_unlink(VOLUME "/hello.txt", NULL)));
    KEXPECT(STATUS_IS_ERROR(vfs_mkdir(VOLUME "/newdir", 0755, NULL)));
}
